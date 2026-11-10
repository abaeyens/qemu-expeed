/*
 * The Expeed 6 vector graphics engine (GEVG) at 0x20180000
 *
 * runs the render command list through qemu/gevg_model.c
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "gevg_model.h"
#include "migration/vmstate.h"

#define GEVG_BASE       0x20180000
#define GEVG_SIZE       0x2000
#define GEVG_SPI0       97
#define GEVG_REG_STATUS 0x58
#define GEVG_REG_GO_A   0x10d0
#define GEVG_REG_GO_B   0x10e0
#define GEVG_REG_GO_C   0x1c
static const uint32_t gevg_ack_reg[3] = { 0x1020, 0xc18, 0x1c };

#define TYPE_EXPEED6_GEVG "expeed6-gevg"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6GevgState, EXPEED6_GEVG)

typedef struct {
    uint8_t *cov;
    uint32_t len;
} GevgCovSave;

struct Expeed6GevgState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    GevgCovSave cov_save[GEVG_MODEL_MASKS];
    GevgModel model;                 /* qemu/gevg_model.c */
    qemu_irq irq[3];
    QEMUTimer *done;

    AddressSpace *as;
};

static void expeed6_gevg_mem_read(void *opaque, uint32_t addr, void *buf, uint32_t len)
{
    address_space_read(((Expeed6GevgState *)opaque)->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

static void expeed6_gevg_mem_write(void *opaque, uint32_t addr, const void *buf, uint32_t len)
{
    if (expeed6_dma_guard_ok(addr, len, "the GEVG model")) {
        address_space_write(((Expeed6GevgState *)opaque)->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
    }
}

static void expeed6_gevg_done(void *opaque)
{
    Expeed6GevgState *s = opaque;
    for (int i = 0; i < 3; i++) {
        s->model.reg[gevg_ack_reg[i] / 4] = 1;
        qemu_set_irq(s->irq[i], 1);
    }
}

static uint64_t expeed6_gevg_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6GevgState *s = opaque;
    uint64_t v = gevg_model_read(&s->model, off);
    return v;
}

static void expeed6_gevg_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6GevgState *s = opaque;
    int start = gevg_model_write(&s->model, off, val);
    for (int i = 0; i < 3; i++) {
        if (off == gevg_ack_reg[i] && val == 0) {
            qemu_set_irq(s->irq[i], 0);
        }
    }
    if (start) {
        timer_mod(s->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
    }
}

static const MemoryRegionOps expeed6_gevg_ops = {
    .read = expeed6_gevg_read,
    .write = expeed6_gevg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void gevg_realize(DeviceState *dev, Error **errp)
{
    Expeed6GevgState *s = EXPEED6_GEVG(dev);

    s->done = timer_new_ns(QEMU_CLOCK_VIRTUAL, expeed6_gevg_done, s);
    for (int i = 0; i < 3; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
    memory_region_init_io(&s->mmio, OBJECT(dev), &expeed6_gevg_ops, s, "expeed6.gevg", GEVG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void gevg_reset_enter(Object *obj, ResetType type)
{
    Expeed6GevgState *s = EXPEED6_GEVG(obj);

    timer_del(s->done);
    gevg_model_free(&s->model);
    gevg_model_init(&s->model, (GevgMem){ s, expeed6_gevg_mem_read, expeed6_gevg_mem_write });
}

static void gevg_reset_hold(Object *obj, ResetType type)
{
    Expeed6GevgState *s = EXPEED6_GEVG(obj);

    for (int i = 0; i < 3; i++) {
        qemu_set_irq(s->irq[i], 0);
    }
}

static int gevg_pre_save(void *opaque)
{
    Expeed6GevgState *s = opaque;

    for (int i = 0; i < GEVG_MODEL_MASKS; i++) {
        GevgMask *m = &s->model.masks[i];
        s->cov_save[i].cov = m->cov;
        s->cov_save[i].len = m->cov ? m->w * m->h : 0;
    }
    return 0;
}

static int gevg_post_load(void *opaque, int version_id)
{
    Expeed6GevgState *s = opaque;

    for (int i = 0; i < GEVG_MODEL_MASKS; i++) {
        GevgMask *m = &s->model.masks[i];
        if (m->cov != s->cov_save[i].cov) {
            free(m->cov);
        }
        m->cov = s->cov_save[i].cov;
        m->cap = s->cov_save[i].len;
    }
    return 0;
}

static const VMStateDescription vmstate_gevg_mask = {
    .name = TYPE_EXPEED6_GEVG "/mask",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (const VMStateField[]){ VMSTATE_UINT32(addr, GevgMask), VMSTATE_UINT32(w, GevgMask),
                                VMSTATE_UINT32(h, GevgMask), VMSTATE_UINT32(stamp, GevgMask),
                                VMSTATE_UINT32(x0, GevgMask), VMSTATE_UINT32(y0, GevgMask),
                                VMSTATE_UINT32(x1, GevgMask), VMSTATE_UINT32(y1, GevgMask), VMSTATE_END_OF_LIST() }
};

static const VMStateDescription vmstate_gevg_cov = {
    .name = TYPE_EXPEED6_GEVG "/coverage",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_UINT32(len, GevgCovSave),
                                      VMSTATE_VBUFFER_ALLOC_UINT32(cov, GevgCovSave, 0, NULL, len),
                                      VMSTATE_END_OF_LIST() }
};

static const VMStateDescription vmstate_gevg = {
    .name = TYPE_EXPEED6_GEVG,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = gevg_pre_save,
    .post_load = gevg_post_load,
    .fields =
        (const VMStateField[]){
            VMSTATE_UINT32_ARRAY(model.reg, Expeed6GevgState, GEVG_MODEL_REGS),
            VMSTATE_UINT32_ARRAY(model.vreg, Expeed6GevgState, GEVG_MODEL_VREGS),
            VMSTATE_BUFFER_UNSAFE(model.matrix, Expeed6GevgState, 0, sizeof(float) * 9),
            VMSTATE_STRUCT_ARRAY(model.masks, Expeed6GevgState, GEVG_MODEL_MASKS, 1, vmstate_gevg_mask, GevgMask),
            VMSTATE_STRUCT_ARRAY(cov_save, Expeed6GevgState, GEVG_MODEL_MASKS, 1, vmstate_gevg_cov, GevgCovSave),
            VMSTATE_UINT32(model.stamp, Expeed6GevgState),
            VMSTATE_UINT32_2DARRAY(model.lut, Expeed6GevgState, 2, 256), VMSTATE_TIMER_PTR(done, Expeed6GevgState),
            VMSTATE_END_OF_LIST() }
};

static void gevg_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = gevg_realize;
    dc->vmsd = &vmstate_gevg;
    rc->phases.enter = gevg_reset_enter;
    rc->phases.hold = gevg_reset_hold;
}

static const TypeInfo gevg_info = {
    .name = TYPE_EXPEED6_GEVG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6GevgState),
    .class_init = gevg_class_init,
};

static void gevg_register_types(void)
{
    type_register_static(&gevg_info);
}

type_init(gevg_register_types)

void expeed6_install_gevg(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_GEVG);
    Expeed6GevgState *gevg = EXPEED6_GEVG(dev);
    gevg->as = as;
    gevg_model_init(&gevg->model, (GevgMem){ gevg, expeed6_gevg_mem_read, expeed6_gevg_mem_write });
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, GEVG_BASE);
    for (int i = 0; i < 3; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, qdev_get_gpio_in(gic, GEVG_SPI0 + i));
    }
}
