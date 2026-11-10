/*
 * The Expeed 6 MVD stage block at 0x20201000, GIC 116
 *
 * a job sequence on the movie path
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"

#define MVD_BASE     0x20201000
#define MVD_GIC_IRQ  116
#define MVD_JOB_US   1000
#define MVD_ST_INPUT 0x04
#define MVD_ST_DONE  0x13b

#define TYPE_EXPEED6_MVD "expeed6-mvd"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6MvdState, EXPEED6_MVD)

struct Expeed6MvdState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t reg[0x1000 / 4];
    qemu_irq irq;
    QEMUTimer *timer;
    uint32_t pending;
};

static void mvd_raise(void *opaque)
{
    Expeed6MvdState *s = opaque;
    s->reg[0x02c / 4] |= s->pending;
    s->pending = 0;
    qemu_set_irq(s->irq, 1);
}

static void mvd_schedule(Expeed6MvdState *s, uint32_t bits)
{
    int64_t at = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + MVD_JOB_US;
    s->pending |= bits;
    timer_mod(s->timer, at);
}

static uint64_t mvd_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6MvdState *s = opaque;
    return s->reg[off / 4];
}

static void mvd_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6MvdState *s = opaque;
    if (off == 0x02c) {
        s->reg[off / 4] &= ~(uint32_t)val;
        if (!s->reg[off / 4]) {
            qemu_set_irq(s->irq, 0);
        }
        return;
    }
    if (off == 0x004 && (val & 1)) {
        mvd_schedule(s, MVD_ST_INPUT);
    }
    s->reg[off / 4] = (uint32_t)val;
    if (off == 0x014 && (val & 1)) {
        mvd_schedule(s, MVD_ST_DONE);
    }
}

static const MemoryRegionOps mvd_ops = {
    .read = mvd_read,
    .write = mvd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void mvd_realize(DeviceState *dev, Error **errp)
{
    Expeed6MvdState *s = EXPEED6_MVD(dev);

    s->timer = timer_new_us(QEMU_CLOCK_VIRTUAL, mvd_raise, s);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    memory_region_init_io(&s->mmio, OBJECT(dev), &mvd_ops, s, "expeed6.mvd", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void mvd_reset_enter(Object *obj, ResetType type)
{
    Expeed6MvdState *s = EXPEED6_MVD(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->pending = 0;
    timer_del(s->timer);
}

static void mvd_reset_hold(Object *obj, ResetType type)
{
    Expeed6MvdState *s = EXPEED6_MVD(obj);

    qemu_set_irq(s->irq, 0);
}

static const VMStateDescription vmstate_mvd = {
    .name = TYPE_EXPEED6_MVD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_UINT32_ARRAY(reg, Expeed6MvdState, 0x1000 / 4),
                                      VMSTATE_TIMER_PTR(timer, Expeed6MvdState),
                                      VMSTATE_UINT32(pending, Expeed6MvdState), VMSTATE_END_OF_LIST() }
};

static void mvd_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mvd_realize;
    dc->vmsd = &vmstate_mvd;

    rc->phases.enter = mvd_reset_enter;
    rc->phases.hold = mvd_reset_hold;
}

static const TypeInfo mvd_info = {
    .name = TYPE_EXPEED6_MVD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6MvdState),
    .class_init = mvd_class_init,
};

static void mvd_register_types(void)
{
    type_register_static(&mvd_info);
}

type_init(mvd_register_types)

void expeed6_install_mvd(MemoryRegion *root, DeviceState *gic)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_MVD);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, MVD_GIC_IRQ - 32));
    memory_region_add_subregion_overlap(root, MVD_BASE, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0), 2);
}
