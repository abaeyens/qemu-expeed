/*
 * The Expeed 6 system control block clock gate array at 0x04280000
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"

#define SYSCTL_BASE  0x04280000
#define SYSCTL_SIZE  0x1000
#define SYSCTL_GATE0 0x74
#define SYSCTL_GATEN 8

#define TYPE_EXPEED6_SYSCTL "expeed6-sysctl"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6SysctlState, EXPEED6_SYSCTL)

struct Expeed6SysctlState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t gate[SYSCTL_GATEN];
};

static bool sysctl_is_gate(hwaddr off)
{
    return off >= SYSCTL_GATE0 && off < SYSCTL_GATE0 + SYSCTL_GATEN * 4 && !(off & 3);
}

static uint64_t sysctl_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6SysctlState *s = opaque;
    if (sysctl_is_gate(off)) {
        return s->gate[(off - SYSCTL_GATE0) / 4];
    }
    if (off == 0) {
        return 0x30000018;
    }
    if (off < 0x74 || (off >= 0x94 && off < 0x100)) {
        switch (off & ~3) {
        case 0x08:
            return 0x20000000;
        case 0x24:
            return 0x00000001;
        case 0x2c:
            return 0x00000011;
        case 0x30:
            return 0x00000410;
        case 0x34:
            return 0x50000000;
        case 0x38:
            return 0x00620000;
        case 0x5c:
            return 0x00030000;
        default:
            return 0;
        }
    }
    return allones_read((void *)(uintptr_t)SYSCTL_BASE, off, size);
}

static void sysctl_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6SysctlState *s = opaque;
    if (sysctl_is_gate(off)) {
        s->gate[(off - SYSCTL_GATE0) / 4] = (uint32_t)val;
        return;
    }
    allones_write((void *)(uintptr_t)SYSCTL_BASE, off, val, size);
}

static const MemoryRegionOps sysctl_ops = {
    .read = sysctl_read,
    .write = sysctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void sysctl_realize(DeviceState *dev, Error **errp)
{
    Expeed6SysctlState *s = EXPEED6_SYSCTL(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sysctl_ops, s, "expeed6.sysctl", SYSCTL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void sysctl_reset_enter(Object *obj, ResetType type)
{
    Expeed6SysctlState *s = EXPEED6_SYSCTL(obj);

    memset(s->gate, 0, sizeof(s->gate));
}

static const VMStateDescription vmstate_sysctl = {
    .name = TYPE_EXPEED6_SYSCTL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_UINT32_ARRAY(gate, Expeed6SysctlState, SYSCTL_GATEN),
                                      VMSTATE_END_OF_LIST() }
};

static void sysctl_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = sysctl_realize;
    dc->vmsd = &vmstate_sysctl;
    rc->phases.enter = sysctl_reset_enter;
}

static const TypeInfo sysctl_info = {
    .name = TYPE_EXPEED6_SYSCTL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6SysctlState),
    .class_init = sysctl_class_init,
};

static void sysctl_register_types(void)
{
    type_register_static(&sysctl_info);
}

type_init(sysctl_register_types)

void expeed6_install_sysctl(MemoryRegion *root)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_SYSCTL);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, SYSCTL_BASE);
}
