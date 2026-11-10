/*
 * The Expeed 6 FASP XDMAC at 0x04251000
 *
 * The 4-channel DMA controller the boot ROM copies run through
 *
 * References:
 * - mainline Linux: drivers/dma/milbeaut-xdmac.c
 * - Fujifilm's X-H2 Linux 4.9 patches: patch 0004, the descriptor's BT bit
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"

#define XDMAC_BASE   0x04251000
#define XDMAC_SIZE   0x1000
#define XDMAC_LEN    0x10
#define XDMAC_SRC    0x14
#define XDMAC_DST    0x18
#define XDMAC_GO     0x28
#define XDMAC_STAT   0x30
#define XDMAC_OK     0x8
#define XDMAC_MAXLEN (32u << 20)
#define XDMAC_BPS    100000000
#define XDMAC_STRIDE 0x30
#define XDMAC_CHANS  4
#define XDMAC_XDACS  0x00
#define XDACS_XE     (1u << 28)
#define XDDES_CE     (1u << 28)
#define XDDES_SE     (1u << 24)
#define XDDES_TF     (0xfu << 20)
#define XDDES_INTEN  0x3
#define XDDSD_IS     0xf
#define XDMAC_SPI0   13

#define TYPE_EXPEED6_XDMAC "expeed6-xdmac"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6XdmacState, EXPEED6_XDMAC)

typedef struct {
    Expeed6XdmacState *s;
    int n;
    QEMUTimer *t;
} Expeed6XdmacDone;

struct Expeed6XdmacState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t reg[XDMAC_SIZE / 4];
    qemu_irq irq[XDMAC_CHANS];
    AddressSpace *as;
    Expeed6XdmacDone done[XDMAC_CHANS];
};

static int xdmac_chan_of(hwaddr off, hwaddr reg)
{
    for (int n = 0; n < XDMAC_CHANS; n++) {
        if (off == reg + n * XDMAC_STRIDE) {
            return n;
        }
    }
    return -1;
}

static uint32_t *xdmac_ch(Expeed6XdmacState *s, int n, hwaddr reg)
{
    return &s->reg[(reg + n * XDMAC_STRIDE) / 4];
}

static void xdmac_update_irq(Expeed6XdmacState *s, int n)
{
    qemu_set_irq(s->irq[n], (*xdmac_ch(s, n, XDMAC_STAT) & XDDSD_IS) && (*xdmac_ch(s, n, XDMAC_GO) & XDDES_INTEN));
}

static uint64_t xdmac_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6XdmacState *s = opaque;
    return s->reg[off / 4];
}

static void xdmac_complete(Expeed6XdmacState *s, int n)
{
    *xdmac_ch(s, n, XDMAC_STAT) = (*xdmac_ch(s, n, XDMAC_STAT) & ~XDDSD_IS) | XDMAC_OK;
    *xdmac_ch(s, n, XDMAC_GO) &= ~XDDES_CE;
    xdmac_update_irq(s, n);
}

static void xdmac_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6XdmacState *s = opaque;
    int n = xdmac_chan_of(off, XDMAC_GO);
    uint32_t old = s->reg[off / 4];
    s->reg[off / 4] = (uint32_t)val;
    if (n < 0) {
        n = xdmac_chan_of(off, XDMAC_STAT);
        if (n >= 0) {
            xdmac_update_irq(s, n);
        }
        return;
    }

    uint32_t start = XDDES_CE | XDDES_SE;
    if ((val & start) != start || !(val & XDDES_TF) || (old & XDDES_CE)) {
        if ((val & XDDES_CE) && !(old & XDDES_CE)) {
            qemu_log_mask(LOG_UNIMP,
                          "expeed6.xdmac: ch%d started without a software request "
                          "(XDDES=0x%08x) -- hardware-request transfers are not modelled\n",
                          n, (uint32_t)val);
        }
        xdmac_update_irq(s, n);
        return;
    }
    if (!(s->reg[XDMAC_XDACS / 4] & XDACS_XE)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "expeed6.xdmac: ch%d started with XDACS.XE clear -- "
                      "ignored\n",
                      n);
        s->reg[off / 4] &= ~XDDES_CE;
        return;
    }

    uint32_t len = *xdmac_ch(s, n, XDMAC_LEN) + 1;
    uint32_t src = *xdmac_ch(s, n, XDMAC_SRC);
    uint32_t dst = *xdmac_ch(s, n, XDMAC_DST);
    if (len && len <= XDMAC_MAXLEN) {
        g_autofree uint8_t *buf = g_malloc(len);
        address_space_read(s->as, src, MEMTXATTRS_UNSPECIFIED, buf, len);
        address_space_write(s->as, dst, MEMTXATTRS_UNSPECIFIED, buf, len);
    } else {
        qemu_log_mask(LOG_UNIMP,
                      "expeed6.xdmac: ch%d transfer of %u bytes exceeds the "
                      "model's %u-byte bound -- not copied\n",
                      n, len, XDMAC_MAXLEN);
    }
    uint64_t ns = muldiv64(len, NANOSECONDS_PER_SECOND, XDMAC_BPS);
    timer_mod(s->done[n].t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void xdmac_done_cb(void *opaque)
{
    Expeed6XdmacDone *d = opaque;
    xdmac_complete(d->s, d->n);
}

static const MemoryRegionOps xdmac_ops = {
    .read = xdmac_read,
    .write = xdmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void xdmac_realize(DeviceState *dev, Error **errp)
{
    Expeed6XdmacState *s = EXPEED6_XDMAC(dev);

    for (int n = 0; n < XDMAC_CHANS; n++) {
        s->done[n].s = s;
        s->done[n].n = n;
        s->done[n].t = timer_new_ns(QEMU_CLOCK_VIRTUAL, xdmac_done_cb, &s->done[n]);
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[n]);
    }
    memory_region_init_io(&s->mmio, OBJECT(dev), &xdmac_ops, s, "expeed6.xdmac", XDMAC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void xdmac_reset_enter(Object *obj, ResetType type)
{
    Expeed6XdmacState *s = EXPEED6_XDMAC(obj);

    memset(s->reg, 0, sizeof(s->reg));
    for (int n = 0; n < XDMAC_CHANS; n++) {
        timer_del(s->done[n].t);
    }
}

static void xdmac_reset_hold(Object *obj, ResetType type)
{
    Expeed6XdmacState *s = EXPEED6_XDMAC(obj);

    for (int n = 0; n < XDMAC_CHANS; n++) {
        qemu_set_irq(s->irq[n], 0);
    }
}

static const VMStateDescription vmstate_xdmac_done = {
    .name = TYPE_EXPEED6_XDMAC "/done",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_TIMER_PTR(t, Expeed6XdmacDone), VMSTATE_END_OF_LIST() }
};

static const VMStateDescription vmstate_xdmac = { .name = TYPE_EXPEED6_XDMAC,
                                                  .version_id = 1,
                                                  .minimum_version_id = 1,
                                                  .fields = (const VMStateField[]){
                                                      VMSTATE_UINT32_ARRAY(reg, Expeed6XdmacState, XDMAC_SIZE / 4),
                                                      VMSTATE_STRUCT_ARRAY(done, Expeed6XdmacState, XDMAC_CHANS, 1,
                                                                           vmstate_xdmac_done, Expeed6XdmacDone),
                                                      VMSTATE_END_OF_LIST() } };


static void xdmac_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = xdmac_realize;
    dc->vmsd = &vmstate_xdmac;
    rc->phases.enter = xdmac_reset_enter;
    rc->phases.hold = xdmac_reset_hold;
}

static const TypeInfo xdmac_info = {
    .name = TYPE_EXPEED6_XDMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6XdmacState),
    .class_init = xdmac_class_init,
};

static void xdmac_register_types(void)
{
    type_register_static(&xdmac_info);
}

type_init(xdmac_register_types)

void expeed6_install_xdmac(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_XDMAC);
    EXPEED6_XDMAC(dev)->as = as;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, XDMAC_BASE);
    for (int i = 0; i < XDMAC_CHANS; i++) { /* SPI 13-16 = GIC 45-48 = FASP_XDMAC_0..3 */
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, qdev_get_gpio_in(gic, XDMAC_SPI0 + i));
    }
}
