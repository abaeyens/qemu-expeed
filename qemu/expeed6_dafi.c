/*
 * The Expeed 6 DAFI stage block at 0x20202000, GIC 123
 *
 * Used in live view zoom
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"

#define DAFI_BASE         0x20202000
#define DAFI_GIC_IRQ      123
#define DAFI_STATUS       0x2c
#define DAFI_MODE         0x200
#define DAFI_JOB_US       2000
#define TYPE_EXPEED6_DAFI "expeed6-dafi"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6DafiState, EXPEED6_DAFI)

struct Expeed6DafiState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t reg[0x1000 / 4];
    uint32_t status;
    qemu_irq irq;
    QEMUTimer *done;
};

static void dafi_done(void *opaque)
{
    Expeed6DafiState *s = opaque;
    s->reg[0x08 / 4] = 0;
    s->status |= s->reg[DAFI_MODE / 4] ? 0x2 : 0x10000;
    qemu_set_irq(s->irq, 1);
}

static uint64_t dafi_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6DafiState *s = opaque;
    return off == DAFI_STATUS ? s->status : s->reg[off / 4];
}

static void dafi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6DafiState *s = opaque;
    if (off == DAFI_STATUS) {
        s->status &= ~(uint32_t)val; /* write back to clear */
        qemu_set_irq(s->irq, s->status != 0);
        return;
    }
    s->reg[off / 4] = val;
    if (off == 0x08 && (val & 1)) {
        timer_mod(s->done, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + DAFI_JOB_US);
    }
}

static const MemoryRegionOps dafi_ops = {
    .read = dafi_read,
    .write = dafi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void dafi_realize(DeviceState *dev, Error **errp)
{
    Expeed6DafiState *s = EXPEED6_DAFI(dev);

    s->done = timer_new_us(QEMU_CLOCK_VIRTUAL, dafi_done, s);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    memory_region_init_io(&s->mmio, OBJECT(dev), &dafi_ops, s, "expeed6.dafi", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void dafi_reset_enter(Object *obj, ResetType type)
{
    Expeed6DafiState *s = EXPEED6_DAFI(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->status = 0;
    timer_del(s->done);
}

static void dafi_reset_hold(Object *obj, ResetType type)
{
    Expeed6DafiState *s = EXPEED6_DAFI(obj);

    qemu_set_irq(s->irq, s->status != 0);
}

static const VMStateDescription vmstate_dafi = {
    .name = TYPE_EXPEED6_DAFI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_UINT32_ARRAY(reg, Expeed6DafiState, 0x1000 / 4),
                                      VMSTATE_UINT32(status, Expeed6DafiState),
                                      VMSTATE_TIMER_PTR(done, Expeed6DafiState), VMSTATE_END_OF_LIST() }
};


static void dafi_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = dafi_realize;
    dc->vmsd = &vmstate_dafi;
    rc->phases.enter = dafi_reset_enter;
    rc->phases.hold = dafi_reset_hold;
}

static const TypeInfo dafi_info = {
    .name = TYPE_EXPEED6_DAFI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6DafiState),
    .class_init = dafi_class_init,
};

static void dafi_register_types(void)
{
    type_register_static(&dafi_info);
}

type_init(dafi_register_types)

    void expeed6_install_dafi(MemoryRegion *root, DeviceState *gic)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_DAFI);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, DAFI_GIC_IRQ - 32));
    memory_region_add_subregion_overlap(root, DAFI_BASE, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0), 3);
}
