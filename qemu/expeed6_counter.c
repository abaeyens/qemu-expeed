/*
 * The Expeed 6 free-running 64-bit counter at 0x04266000
 *
 * Runs at 48 MHz on the Z 6II
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"

#define FREERUN_BASE         0x04266000
#define FREERUN_HZ           48000000
#define TYPE_EXPEED6_COUNTER "expeed6-counter"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6CounterState, EXPEED6_COUNTER)

struct Expeed6CounterState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
};

static uint64_t freerun_read(void *opaque, hwaddr off, unsigned size)
{
    if (off == 0x08 || off == 0x0c) {
        uint64_t ticks = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), FREERUN_HZ, NANOSECONDS_PER_SECOND);
        if (off == 0x08 && size == 8) {
            return ticks;
        }
        return (off == 0x08 ? ticks : ticks >> 32) & 0xffffffffULL;
    }
    return (size >= 8) ? ~0ULL : ((1ULL << (size * 8)) - 1);
}

static void freerun_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    allones_write((void *)(uintptr_t)FREERUN_BASE, off, val, size);
}

static const MemoryRegionOps freerun_ops = {
    .read = freerun_read,
    .write = freerun_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

static void counter_realize(DeviceState *dev, Error **errp)
{
    Expeed6CounterState *s = EXPEED6_COUNTER(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &freerun_ops, s, "expeed6.freerun", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void counter_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = counter_realize;
}

static const TypeInfo counter_info = {
    .name = TYPE_EXPEED6_COUNTER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6CounterState),
    .class_init = counter_class_init,
};

static void counter_register_types(void)
{
    type_register_static(&counter_info);
}

type_init(counter_register_types)

    void expeed6_install_counter(MemoryRegion *root)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_COUNTER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, FREERUN_BASE);
}
