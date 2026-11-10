/*
 * The Expeed 6 IDMAC at 0x20070000
 *
 * Two 2-D DMA channels: the memory-clear engine and the DualEngine's data mover
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"

#define IDMAC_SPI0         71
#define IDMAC_MAX          (64u << 20)
#define TYPE_EXPEED6_IDMAC "expeed6-idmac"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6DmaState, EXPEED6_IDMAC)

struct Expeed6DmaState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[0x40];
    AddressSpace *as;
    qemu_irq irq[2];
};

Expeed6DmaState *s_master_idmac, *s_slave_idmac;

AddressSpace *expeed6_idmac_as(Expeed6DmaState *d)
{
    return d->as;
}


static void idmac_update_irq(Expeed6DmaState *s, int ch)
{
    uint32_t *r = &s->regs[ch * 0x10];
    qemu_set_irq(s->irq[ch], (r[0x08 / 4] & r[0x04 / 4] & 0x11) != 0);
}

static void idmac_run(Expeed6DmaState *s, int ch)
{
    uint32_t *r = &s->regs[ch * 0x10];
    bool fill = r[0x14 / 4] & 1;
    uint32_t spitch = r[0x1c / 4] & 0xffff, dpitch = r[0x20 / 4] & 0xffff;
    uint32_t width = r[0x24 / 4] & 0xffff, rows = r[0x28 / 4] & 0xffff;
    uint32_t src = r[0x2c / 4], dst = r[0x30 / 4];
    uint32_t code = (r[0x18 / 4] >> 24) & 3;
    uint8_t buf[0x4000];

    if ((uint64_t)width * rows > IDMAC_MAX) {
        warn_report("expeed6.idmac: ch%d %s %ux%u refused (model bound %u bytes)", ch, fill ? "fill" : "copy",
                    width, rows, IDMAC_MAX);
        r[0x08 / 4] |= 0x10;
        return;
    }
    if (fill) {
        if (code) {
            qemu_log_mask(LOG_UNIMP, "expeed6.idmac: ch%d fill with pixel code %u filled bytes\n", ch, code);
        }
        memset(buf, r[0x18 / 4] & 0xff, MIN(width, sizeof(buf)));
    }
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < width; x += sizeof(buf)) {
            uint32_t n = MIN(width - x, (uint32_t)sizeof(buf));
            if (!fill) {
                address_space_read(s->as, src + y * spitch + x, MEMTXATTRS_UNSPECIFIED, buf, n);
            }
            address_space_write(s->as, dst + y * dpitch + x, MEMTXATTRS_UNSPECIFIED, buf, n);
        }
    }
    r[0x08 / 4] |= 1;
}

static uint64_t dma_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6DmaState *s = opaque;
    if ((off & 0x3f) == 0x10 && off < 0x80) {
        return 2;
    }
    return (off / 4 < 0x40) ? s->regs[off / 4] : 0;
}

static void dma_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6DmaState *s = opaque;
    if (off < 0x80) {
        int ch = off / 0x40;
        uint32_t *r = &s->regs[ch * 0x10];
        switch (off & 0x3f) {
        case 0x08:
            r[0x08 / 4] &= ~val;
            break;
        case 0x10:
            r[0x10 / 4] = val;
            if (val & 1) {
                idmac_run(s, ch);
            }
            break;
        default:
            r[(off & 0x3f) / 4] = val;
            break;
        }
        idmac_update_irq(s, ch);
        return;
    }
    if (off / 4 < 0x40) {
        s->regs[off / 4] = val;
    }
}

static const MemoryRegionOps dma_ops = {
    .read = dma_read,
    .write = dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void idmac_realize(DeviceState *dev, Error **errp)
{
    Expeed6DmaState *s = EXPEED6_IDMAC(dev);

    for (int i = 0; i < 2; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
    memory_region_init_io(&s->mmio, OBJECT(dev), &dma_ops, s, "expeed6.dma", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void idmac_reset_enter(Object *obj, ResetType type)
{
    memset(EXPEED6_IDMAC(obj)->regs, 0, sizeof(EXPEED6_IDMAC(obj)->regs));
}

static void idmac_reset_hold(Object *obj, ResetType type)
{
    Expeed6DmaState *s = EXPEED6_IDMAC(obj);

    for (int i = 0; i < 2; i++) {
        qemu_set_irq(s->irq[i], 0);
    }
}

static const VMStateDescription vmstate_idmac = {
    .name = TYPE_EXPEED6_IDMAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_UINT32_ARRAY(regs, Expeed6DmaState, 0x40), VMSTATE_END_OF_LIST() }
};

static void idmac_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = idmac_realize;
    dc->vmsd = &vmstate_idmac;
    rc->phases.enter = idmac_reset_enter;
    rc->phases.hold = idmac_reset_hold;
}

static const TypeInfo idmac_info = {
    .name = TYPE_EXPEED6_IDMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6DmaState),
    .class_init = idmac_class_init,
};

static void idmac_register_types(void)
{
    type_register_static(&idmac_info);
}

type_init(idmac_register_types)

Expeed6DmaState *expeed6_install_idmac(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_IDMAC);
    Expeed6DmaState *dma = EXPEED6_IDMAC(dev);
    dma->as = as;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, 0x20070000);
    if (master) {
        s_master_idmac = dma;
    } else {
        s_slave_idmac = dma;
    }
    for (int i = 0; i < 2; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, qdev_get_gpio_in(gic, IDMAC_SPI0 + i));
    }
    return dma;
}

void expeed6_idmac_set_as(Expeed6DmaState *d, AddressSpace *as)
{
    d->as = as;
}
