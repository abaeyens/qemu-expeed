/*
 * The Expeed 6 RAW stage block at 0x20204000, GIC 121
 *
 * a still's raw copy and its NEF compression (qemu/nef_model.c)
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"
#include "mipu_model.h"
#include "nef_model.h"

#define RAWB_BASE        0x20204000
#define RAWB_GIC_IRQ     121
#define RAWB_JOB_US      1000
#define TYPE_EXPEED6_RAW "expeed6-raw"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6RawState, EXPEED6_RAW)

struct Expeed6RawState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t reg[0x1000 / 4];
    uint32_t status;
    qemu_irq irq;
    QEMUTimer *done;
    AddressSpace *as;
};

static void rawb_copy(Expeed6RawState *s)
{
    uint32_t *r = s->reg;
    uint32_t src = r[0x400 / 4], dst = r[0x420 / 4];
    uint32_t sp = r[0x404 / 4] & 0xffff, dp = r[0x424 / 4] & 0xffff, h = r[0x50 / 4] & 0x7fff;
    uint32_t row = MIN(sp, dp);
    g_autofree uint8_t *buf = g_malloc(row ? row : 1);
    for (uint32_t y = 0; y < h && row; y++) {
        address_space_read(s->as, src + (uint64_t)y * sp, MEMTXATTRS_UNSPECIFIED, buf, row);
        if (expeed6_dma_guard_ok(dst + (uint64_t)y * dp, row, "the RAW model")) {
            address_space_write(s->as, dst + (uint64_t)y * dp, MEMTXATTRS_UNSPECIFIED, buf, row);
        }
    }
}

static void rawb_update(Expeed6RawState *s)
{
    qemu_set_irq(s->irq, s->status != 0);
}

static void rawb_done(void *opaque)
{
    Expeed6RawState *s = opaque;
    s->status |= 0x100;
    rawb_update(s);
}

static uint64_t rawb_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6RawState *s = opaque;
    return off == 0x2c ? s->status : s->reg[off / 4];
}

static void rawb_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6RawState *s = opaque;
    if (off == 0x2c) {
        s->status &= ~(uint32_t)val;
        rawb_update(s);
        return;
    }
    s->reg[off / 4] = val;
    if (off == 0x08 && (val & 1)) {
        uint32_t *r = s->reg;
        if ((r[0x44 / 4] & 3) == 2) {
            rawb_copy(s);
        } else if ((r[0x44 / 4] & 3) == 0) {
            NefJob j = { .src = r[0x400 / 4],
                         .dst = r[0x420 / 4],
                         .spitch = r[0x404 / 4] & 0xffff,
                         .w = r[0x4c / 4] & 0x7fff,
                         .h = r[0x50 / 4] & 0x7fff,
                         .limit = r[0xd4 / 4],
                         .pred0 = r[0x58 / 4] & 0x3fff };
            j.bits = j.w && (uint64_t)j.spitch * 8 / j.w < 14 ? 12 : 14;
            for (int k = 0; k < NEF_MODEL_SYMS; k++) {
                j.code[k] = r[(0x5c + 4 * k) / 4] & 0x3fff;
                j.len[k] = r[(0x98 + 4 * k) / 4] & 0xf;
            }
            j.lossy = !(r[0x40 / 4] & 0x10000);
            for (int k = 0; k < 256; k++) {
                j.lut[k] = (r[0x200 / 4 + k / 2] >> (16 * (k & 1))) & 0x3fff;
            }
            long n = nef_model_encode_lossless(&j, (MipuMem){ s->as, mipu_mem_read, mipu_mem_write });
            if (n >= 0) {
                r[0x198 / 4] = (uint32_t)n;
            }
        } else {
            fprintf(stderr, "expeed6.raw: mode %u (a compressed NEF) not modelled: window 1 left as it is\n",
                    r[0x44 / 4] & 3);
        }
        timer_mod(s->done, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + RAWB_JOB_US);
    }
}

static const MemoryRegionOps rawb_ops = {
    .read = rawb_read,
    .write = rawb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void rawb_realize(DeviceState *dev, Error **errp)
{
    Expeed6RawState *s = EXPEED6_RAW(dev);

    s->done = timer_new_us(QEMU_CLOCK_VIRTUAL, rawb_done, s);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    memory_region_init_io(&s->mmio, OBJECT(dev), &rawb_ops, s, "expeed6.raw", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void rawb_reset_enter(Object *obj, ResetType type)
{
    Expeed6RawState *s = EXPEED6_RAW(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->status = 0;
    timer_del(s->done);
}

static void rawb_reset_hold(Object *obj, ResetType type)
{
    rawb_update(EXPEED6_RAW(obj));
}

static const VMStateDescription vmstate_rawb = {
    .name = TYPE_EXPEED6_RAW,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]){ VMSTATE_UINT32_ARRAY(reg, Expeed6RawState, 0x1000 / 4),
                                      VMSTATE_UINT32(status, Expeed6RawState),
                                      VMSTATE_TIMER_PTR(done, Expeed6RawState), VMSTATE_END_OF_LIST() }
};

static void rawb_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = rawb_realize;
    dc->vmsd = &vmstate_rawb;
    rc->phases.enter = rawb_reset_enter;
    rc->phases.hold = rawb_reset_hold;
}

static const TypeInfo rawb_info = {
    .name = TYPE_EXPEED6_RAW,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6RawState),
    .class_init = rawb_class_init,
};

static void rawb_register_types(void)
{
    type_register_static(&rawb_info);
}

type_init(rawb_register_types)

    void expeed6_install_rawb(MemoryRegion *root, AddressSpace *as, DeviceState *gic)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_RAW);
    EXPEED6_RAW(dev)->as = as;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, RAWB_GIC_IRQ - 32));
    memory_region_add_subregion_overlap(root, RAWB_BASE, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0), 3);
}
