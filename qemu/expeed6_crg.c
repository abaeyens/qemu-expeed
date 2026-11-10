/*
 * The Expeed 6 clock-and-reset generator at 0x04281000 and the event FIFO at 0x04287000
 *
 * References:
 * - GoPro GP2 Linux 4.19.124: drivers/clk/clk-milbeaut.c
 * - Stubby bootloader 1.60.1: configs/config-sc2000.h, src/m8m-clk-io.c
 * - Nikon Z 9 Linux 4.14.224: drivers/clk/nikon/clk-cgn.c
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"

#define CRG_BASE     0x04281000
#define CRG_CLKSEL_N 10
#define CRG_GROUPS   6

static const uint32_t crg_clksel_write_enable[CRG_CLKSEL_N] = {
    0x00448880, 0x82808888, 0x80201088, 0x10108888, 0x88100010,
    0x00880008, 0x00000010, 0x10101010, 0x00081010, 0x00000000,
};
#define EVFIFO_BASE 0x04287000
#define EVFIFO_SPI  218

#define TYPE_EXPEED6_CRG "expeed6-crg"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6EvState, EXPEED6_CRG)

struct Expeed6EvState {
    SysBusDevice parent_obj;
    MemoryRegion crg_mmio, fifo_mmio;
    uint32_t clksel[CRG_CLKSEL_N];
    uint32_t release[CRG_GROUPS];
    uint32_t stopped[CRG_GROUPS];
    uint32_t acquire[CRG_GROUPS];
    uint32_t ctl[0x400];
    uint32_t fifo_cfg;
    uint32_t fifo_done;
    uint32_t fifo_pending;
    uint32_t fifo[0x400];
    qemu_irq irq;
    int32_t level;
};

static void ev_update(Expeed6EvState *s)
{
    int l = s->fifo_pending ? 1 : 0;
    if (l != s->level) {
        s->level = l;
        qemu_set_irq(s->irq, l);
    }
}

static uint64_t crg_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6EvState *s = opaque;
    uint64_t v;

    if (off < CRG_CLKSEL_N * 4) {
        v = s->clksel[off / 4] & ~crg_clksel_write_enable[off / 4];
    } else if (off >= 0x38 && off < 0x38 + CRG_GROUPS * 4) {
        v = s->stopped[(off - 0x38) / 4];
    } else if (off >= 0x138 && off < 0x138 + CRG_GROUPS * 4) {
        v = 0;
    } else if (off >= 0x238 && off < 0x238 + CRG_GROUPS * 4) {
        v = 0;
    } else {
        v = s->ctl[off / 4];
    }
    return v;
}

static void crg_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6EvState *s = opaque;

    if (off == 0x24) {
    } else if (off < CRG_CLKSEL_N * 4) {
        s->clksel[off / 4] = val;
    } else if (off >= 0x138 && off < 0x138 + CRG_GROUPS * 4) {
        s->release[(off - 0x138) / 4] = val;
        s->stopped[(off - 0x138) / 4] |= val;
    } else if (off >= 0x238 && off < 0x238 + CRG_GROUPS * 4) {
        unsigned g = (off - 0x238) / 4;
        s->acquire[g] = val;
        s->stopped[g] &= ~(uint32_t)val;
    } else {
        s->ctl[off / 4] = val;
    }
}

static const MemoryRegionOps crg_ops = {
    .read = crg_read,
    .write = crg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t evfifo_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6EvState *s = opaque;
    uint64_t v = (off == 0x00)   ? s->fifo_cfg
                 : (off == 0x04) ? s->fifo_done
                 : (off == 0x0c) ? s->fifo_pending
                                 : s->fifo[off / 4];
    return v;
}

static void evfifo_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6EvState *s = opaque;

    if (off == 0x00) {
        s->fifo_cfg = val;
    } else if (off == 0x04) {
        if (val & 1) {
            s->fifo_done = 0;
        }
        s->fifo_pending = 0;
        ev_update(s);
    } else if (off == 0x10) {
        s->fifo[off / 4] = val;
        s->fifo_done = 1;
        if (s->fifo_cfg & 0x100) {
            s->fifo_pending |= 1u << ((s->fifo_cfg >> 12) & 7);
            ev_update(s);
        }
    } else {
        s->fifo[off / 4] = val;
    }
}

static const MemoryRegionOps evfifo_ops = {
    .read = evfifo_read,
    .write = evfifo_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void crg_realize(DeviceState *dev, Error **errp)
{
    Expeed6EvState *ev = EXPEED6_CRG(dev);

    memory_region_init_io(&ev->crg_mmio, OBJECT(dev), &crg_ops, ev, "expeed6.crg", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &ev->crg_mmio);
    memory_region_init_io(&ev->fifo_mmio, OBJECT(dev), &evfifo_ops, ev, "expeed6.evfifo", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &ev->fifo_mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &ev->irq);
}

static void crg_reset_enter(Object *obj, ResetType type)
{
    Expeed6EvState *ev = EXPEED6_CRG(obj);

    memset(ev->clksel, 0, sizeof(ev->clksel));
    memset(ev->release, 0, sizeof(ev->release));
    memset(ev->stopped, 0, sizeof(ev->stopped));
    memset(ev->acquire, 0, sizeof(ev->acquire));
    memset(ev->ctl, 0, sizeof(ev->ctl));
    memset(ev->fifo, 0, sizeof(ev->fifo));
    ev->fifo_cfg = ev->fifo_done = ev->fifo_pending = 0;
    ev->level = 0;
}

static void crg_reset_hold(Object *obj, ResetType type)
{
    Expeed6EvState *ev = EXPEED6_CRG(obj);

    qemu_set_irq(ev->irq, ev->level);
}

static const VMStateDescription vmstate_crg = {
    .name = TYPE_EXPEED6_CRG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_UINT32_ARRAY(clksel, Expeed6EvState, CRG_CLKSEL_N),
                                      VMSTATE_UINT32_ARRAY(release, Expeed6EvState, CRG_GROUPS),
                                      VMSTATE_UINT32_ARRAY(stopped, Expeed6EvState, CRG_GROUPS),
                                      VMSTATE_UINT32_ARRAY(acquire, Expeed6EvState, CRG_GROUPS),
                                      VMSTATE_UINT32_ARRAY(ctl, Expeed6EvState, 0x400),
                                      VMSTATE_UINT32(fifo_cfg, Expeed6EvState),
                                      VMSTATE_UINT32(fifo_done, Expeed6EvState),
                                      VMSTATE_UINT32(fifo_pending, Expeed6EvState),
                                      VMSTATE_UINT32_ARRAY(fifo, Expeed6EvState, 0x400),
                                      VMSTATE_INT32(level, Expeed6EvState), VMSTATE_END_OF_LIST() }
};

static void crg_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = crg_realize;
    dc->vmsd = &vmstate_crg;
    rc->phases.enter = crg_reset_enter;
    rc->phases.hold = crg_reset_hold;
}

static const TypeInfo crg_info = {
    .name = TYPE_EXPEED6_CRG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6EvState),
    .class_init = crg_class_init,
};

static void crg_register_types(void)
{
    type_register_static(&crg_info);
}

type_init(crg_register_types)

void expeed6_install_crg(MemoryRegion *root, DeviceState *gic)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_CRG);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, CRG_BASE);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 1, EVFIFO_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, EVFIFO_SPI));
}
