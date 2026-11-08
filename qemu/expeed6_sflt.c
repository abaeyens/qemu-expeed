/*
 * The Expeed 6 SFLT stage block at 0x20203000, GIC 124
 *
 * the downscaled images it creates for the MIPU
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"

#define SFLT_BASE         0x20203000
#define SFLT_GIC_IRQ      124
#define SFLT_STATUS       0x2c
#define SFLT_ST_DONE      0x1
#define SFLT_ST_FRAME     (0x400 | 0x8)
#define SFLT_JOB_US       2000
#define SFLT_FRAME_MS     33
#define TYPE_EXPEED6_SFLT "expeed6-sflt"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6SfltState, EXPEED6_SFLT)

struct Expeed6SfltState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t reg[0x1000 / 4];
    uint32_t status;
    qemu_irq irq;
    QEMUTimer *done;
    QEMUTimer *frame;
    AddressSpace *as;
    bool streaming;
};

static int64_t sflt_step_ns(Expeed6SfltState *s)
{
    return expeed6_fe_frame_ns(s->as, SFLT_FRAME_MS * SCALE_MS);
}

static void sflt_update(Expeed6SfltState *s)
{
    qemu_set_irq(s->irq, s->status != 0);
}

static void sflt_done(void *opaque)
{
    Expeed6SfltState *s = opaque;
    s->status |= SFLT_ST_DONE;
    sflt_update(s);
}

static void sflt_frame(void *opaque)
{
    Expeed6SfltState *s = opaque;
    if (!s->streaming) {
        return;
    }
    s->status |= SFLT_ST_FRAME;
    sflt_update(s);
    timer_mod(s->frame, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + sflt_step_ns(s));
}

static uint64_t sflt_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6SfltState *s = opaque;
    return off == SFLT_STATUS ? s->status : s->reg[off / 4];
}

static void sflt_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6SfltState *s = opaque;
    if (off == SFLT_STATUS) {
        s->status &= ~(uint32_t)val;
        sflt_update(s);
        return;
    }
    s->reg[off / 4] = val;
    if (off == 0x08 && (val & 1) && s->streaming) {
        s->streaming = false;
        timer_del(s->frame);
        return;
    }
    if (off != 0x04) {
        return;
    }
    if (val & 1) {
        if (s->reg[0x54 / 4] & 1) { /* continuous */
            s->streaming = true;
            timer_mod(s->frame, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + sflt_step_ns(s));
        } else {
            timer_mod(s->done, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + SFLT_JOB_US);
        }
    } else if (s->streaming) { /* stop */
        s->streaming = false;
        timer_del(s->frame);
        timer_mod(s->done, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + SFLT_JOB_US);
    }
}

static const MemoryRegionOps sflt_ops = {
    .read = sflt_read,
    .write = sflt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sflt_realize(DeviceState *dev, Error **errp)
{
    Expeed6SfltState *s = EXPEED6_SFLT(dev);

    s->done = timer_new_us(QEMU_CLOCK_VIRTUAL, sflt_done, s);
    s->frame = timer_new_ns(QEMU_CLOCK_VIRTUAL, sflt_frame, s);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    memory_region_init_io(&s->mmio, OBJECT(dev), &sflt_ops, s, "expeed6.sflt", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void sflt_reset_enter(Object *obj, ResetType type)
{
    Expeed6SfltState *s = EXPEED6_SFLT(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->status = 0;
    s->streaming = false;
    timer_del(s->done);
    timer_del(s->frame);
}

static void sflt_reset_hold(Object *obj, ResetType type)
{
    sflt_update(EXPEED6_SFLT(obj));
}

static const VMStateDescription vmstate_sflt = {
    .name = TYPE_EXPEED6_SFLT,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]){ VMSTATE_UINT32_ARRAY(reg, Expeed6SfltState, 0x1000 / 4),
                                      VMSTATE_UINT32(status, Expeed6SfltState),
                                      VMSTATE_TIMER_PTR(done, Expeed6SfltState),
                                      VMSTATE_TIMER_PTR(frame, Expeed6SfltState),
                                      VMSTATE_BOOL(streaming, Expeed6SfltState), VMSTATE_END_OF_LIST() }
};

static void sflt_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = sflt_realize;
    dc->vmsd = &vmstate_sflt;
    rc->phases.enter = sflt_reset_enter;
    rc->phases.hold = sflt_reset_hold;
}

static const TypeInfo sflt_info = {
    .name = TYPE_EXPEED6_SFLT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6SfltState),
    .class_init = sflt_class_init,
};

static void sflt_register_types(void)
{
    type_register_static(&sflt_info);
}

type_init(sflt_register_types)

    void expeed6_install_sflt(MemoryRegion *root, DeviceState *gic, AddressSpace *as)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_SFLT);
    Expeed6SfltState *s = EXPEED6_SFLT(dev);
    s->as = as;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, SFLT_GIC_IRQ - 32));
    memory_region_add_subregion_overlap(root, SFLT_BASE, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0), 3);
}
