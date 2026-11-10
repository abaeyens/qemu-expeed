/*
 * The Expeed 6 HDMI block's hot-plug detect at 0x2016c010
 *
 * Note: HDMI is a trademark or registered trademark of HDMI Licensing, LLC.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"

#define HDMI_BASE    0x2016c000
#define HDMI_SIZE    0x1000
#define HDMI_HPD     0x10
#define HDMI_HPD_BIT 0x02

#define TYPE_EXPEED6_HDMI "expeed6-hdmi"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6HdmiState, EXPEED6_HDMI)

struct Expeed6HdmiState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
};

static uint64_t hdmi_read(void *opaque, hwaddr off, unsigned size)
{
    if (off == HDMI_HPD) {
        return 0;
    }
    return allones_read((void *)(uintptr_t)HDMI_BASE, off, size);
}

static void hdmi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    allones_write((void *)(uintptr_t)HDMI_BASE, off, val, size);
}

static const MemoryRegionOps hdmi_ops = {
    .read = hdmi_read,
    .write = hdmi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void hdmi_realize(DeviceState *dev, Error **errp)
{
    Expeed6HdmiState *s = EXPEED6_HDMI(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &hdmi_ops, s, "expeed6.hdmi", HDMI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void hdmi_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = hdmi_realize;
}

static const TypeInfo hdmi_info = {
    .name = TYPE_EXPEED6_HDMI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6HdmiState),
    .class_init = hdmi_class_init,
};

static void hdmi_register_types(void)
{
    type_register_static(&hdmi_info);
}

type_init(hdmi_register_types)

void expeed6_install_hdmi(MemoryRegion *root)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_HDMI);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, HDMI_BASE);
}
