/*
 * The Expeed 6 card detect block at 0x2b000000, GIC 237 (`CD`)
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

#define CD_BASE    0x2b000000
#define CD_SIZE    0x1000
#define CD_STATUS  0x08
#define CD_PRESENT 0x10

#define TYPE_EXPEED6_CD "expeed6-cd"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6CdState, EXPEED6_CD)

struct Expeed6CdState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t status;
    uint32_t present;
    qemu_irq irq;
};

static uint64_t cd_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6CdState *s = opaque;
    switch (off) {
    case CD_STATUS:
        return s->status;
    case CD_PRESENT:
        return s->present;
    default:
        return allones_read((void *)(uintptr_t)CD_BASE, off, size);
    }
}

static void cd_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6CdState *s = opaque;
    if (off == CD_STATUS) {
        s->status &= ~(uint32_t)val;
        qemu_set_irq(s->irq, s->status != 0);
        return;
    }
    allones_write((void *)(uintptr_t)CD_BASE, off, val, size);
}

static const MemoryRegionOps cd_ops = {
    .read = cd_read,
    .write = cd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static Expeed6CdState *s_cd;

void cd_present_prop_set(Object *obj, const char *value, Error **errp)
{
    if (!s_cd) {
        error_setg(errp, "cd-present: no card-detect block on this machine");
        return;
    }
    s_cd->present = (uint32_t)strtoul(value, NULL, 0);
}

void cd_insert_prop_set(Object *obj, const char *value, Error **errp)
{
    if (!s_cd) {
        error_setg(errp, "cd-insert: no card-detect block on this machine");
        return;
    }
    s_cd->status |= (uint32_t)strtoul(value, NULL, 0);
    qemu_set_irq(s_cd->irq, s_cd->status != 0);
}

void sd_card_present_prop_set(Object *obj, const char *value, Error **errp)
{
    if (!s_master_gpio) {
        error_setg(errp, "sd-card-present: no master GPIO block on this machine");
        return;
    }
    bool present = strtoul(value, NULL, 0) != 0;
    gpio_set_sd_card(s_master_gpio, present);
    if (s_slave_gpio) {
        gpio_set_sd_card(s_slave_gpio, present);
    }
}

static void cd_realize(DeviceState *dev, Error **errp)
{
    Expeed6CdState *s = EXPEED6_CD(dev);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    memory_region_init_io(&s->mmio, OBJECT(dev), &cd_ops, s, "expeed6.cd", CD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void cd_reset_enter(Object *obj, ResetType type)
{
    EXPEED6_CD(obj)->status = 0;
}

static void cd_reset_hold(Object *obj, ResetType type)
{
    qemu_set_irq(EXPEED6_CD(obj)->irq, 0);
}

static const VMStateDescription vmstate_cd = {
    .name = TYPE_EXPEED6_CD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_UINT32(status, Expeed6CdState),
                                      VMSTATE_UINT32(present, Expeed6CdState), VMSTATE_END_OF_LIST() }
};

static Property cd_properties[] = {
    DEFINE_PROP_UINT32("present", Expeed6CdState, present, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void cd_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = cd_realize;
    dc->vmsd = &vmstate_cd;
    device_class_set_props(dc, cd_properties);
    rc->phases.enter = cd_reset_enter;
    rc->phases.hold = cd_reset_hold;
}

static const TypeInfo cd_info = {
    .name = TYPE_EXPEED6_CD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6CdState),
    .class_init = cd_class_init,
};

static void cd_register_types(void)
{
    type_register_static(&cd_info);
}

type_init(cd_register_types)

    void expeed6_install_cd(MemoryRegion *root, DeviceState *gic, bool master)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_CD);
    uint32_t present = 0;
    if (!master || getenv("Z6II_XQD") != NULL) {
        present |= 1u << 10; /* slot 3: the PCIe device is present (see expeed6_install_pcie) */
    }
    qdev_prop_set_uint32(dev, "present", present);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    if (master) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, 237 - 32)); /* GIC 237 = CD */
        s_cd = EXPEED6_CD(dev);
    }
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, CD_BASE);
}
