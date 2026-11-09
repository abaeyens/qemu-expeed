/*
 * The Expeed 6 external-interrupt controller (EXIU) at 0x0426f000
 *
 * detects GPIO edges
 *
 * References:
 * - GoPro GP2 Linux 4.19.124: drivers/irqchip/irq-sni-exiu.c
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"

#define EXTINT_BASE 0x0426F000
#define EXTINT_SPI0 33

#define TYPE_EXPEED6_EXTINT "expeed6-extint"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6ExtIntState, EXPEED6_EXTINT)

struct Expeed6ExtIntState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t pending;
    uint32_t polarity;
    uint32_t reg[0x400];
    qemu_irq irq[EXTINT_N];
    int32_t level[EXTINT_N];
};

static void extint_update(Expeed6ExtIntState *s)
{
    uint32_t live = s->pending & ~s->reg[0];
    for (int i = 0; i < EXTINT_N; i++) {
        int l = (live >> i) & 1;
        if (l != s->level[i]) {
            s->level[i] = l;
            qemu_set_irq(s->irq[i], l);
        }
    }
}

void extint_raise(Expeed6ExtIntState *s, int idx)
{
    s->pending |= 1u << idx;
    extint_update(s);
}

void extint_input_edge(Expeed6ExtIntState *s, int idx, bool level)
{
    if (((s->polarity >> idx) & 1) == level) {
        extint_raise(s, idx);
    }
}

void gpio_echo_fire_extint(void *opaque, int src)
{
    extint_raise(opaque, src);
}

static uint64_t extint_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6ExtIntState *s = opaque;
    uint64_t v = (off == 0x08)   ? (s->pending & ~s->reg[0])
                 : (off == 0x0c) ? s->pending
                 : (off == 0x14) ? s->polarity
                                 : (off / 4 < 0x400 ? s->reg[off / 4] : 0);
    return v;
}

static void extint_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6ExtIntState *s = opaque;
    if (off == 0x10) {                       /* write-1-to-clear */
        s->pending &= ~(uint32_t)val;
        extint_update(s);
    } else if (off == 0x14) {
        s->polarity = val;
    } else if (off / 4 < 0x400) {
        s->reg[off / 4] = val;
        if (off == 0x00) {
            extint_update(s);
        }
    }
}

static const MemoryRegionOps extint_ops = {
    .read = extint_read,
    .write = extint_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void extint_realize(DeviceState *dev, Error **errp)
{
    Expeed6ExtIntState *s = EXPEED6_EXTINT(dev);

    for (int i = 0; i < EXTINT_N; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
    memory_region_init_io(&s->mmio, OBJECT(dev), &extint_ops, s, "expeed6.extint", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void extint_reset_enter(Object *obj, ResetType type)
{
    Expeed6ExtIntState *s = EXPEED6_EXTINT(obj);

    s->pending = 0;
    s->polarity = 0;
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->level, 0, sizeof(s->level));
}

static void extint_reset_hold(Object *obj, ResetType type)
{
    Expeed6ExtIntState *s = EXPEED6_EXTINT(obj);

    for (int i = 0; i < EXTINT_N; i++) {
        qemu_set_irq(s->irq[i], 0);
    }
}

static const VMStateDescription vmstate_extint = { .name = TYPE_EXPEED6_EXTINT,
                                                   .version_id = 1,
                                                   .minimum_version_id = 1,
                                                   .fields = (const VMStateField[]){
                                                       VMSTATE_UINT32(pending, Expeed6ExtIntState),
                                                       VMSTATE_UINT32(polarity, Expeed6ExtIntState),
                                                       VMSTATE_UINT32_ARRAY(reg, Expeed6ExtIntState, 0x400),
                                                       VMSTATE_INT32_ARRAY(level, Expeed6ExtIntState, EXTINT_N),
                                                       VMSTATE_END_OF_LIST() } };

static void extint_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = extint_realize;
    dc->vmsd = &vmstate_extint;
    rc->phases.enter = extint_reset_enter;
    rc->phases.hold = extint_reset_hold;
}

static const TypeInfo extint_info = {
    .name = TYPE_EXPEED6_EXTINT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6ExtIntState),
    .class_init = extint_class_init,
};

static void extint_register_types(void)
{
    type_register_static(&extint_info);
}

type_init(extint_register_types)

    Expeed6ExtIntState *expeed6_install_extint(MemoryRegion *root, DeviceState *gic)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_EXTINT);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, EXTINT_BASE);
    for (int i = 0; i < EXTINT_N; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, qdev_get_gpio_in(gic, EXTINT_SPI0 + i));
    }
    return EXPEED6_EXTINT(dev);
}
