/*
 * The Expeed 6 JPEG block (0x24072000)
 *
 * encodes and decodes jobs through qemu/jpeg_model.c
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"
#include "jpeg_model.h"

#define JPEGCO_BASE      0x24072000
#define JPEGCO_GIC_IRQ   105
#define JPEGCO_ST_END    (1u << 27)
#define JPEGCO_ST_ODMA   (1u << 12)
#define JPEGCO_GO        (1u << 14)
#define JPEGCO_ST_DEC_A  (1u << 15)
#define JPEGCO_ST_DEC_B  (1u << 14)
#define JPEGCO_JOB_US    1000
#define JPEGCO_DEC_ALIGN 32

#define TYPE_EXPEED6_JPEG "expeed6-jpeg"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6JpegState, EXPEED6_JPEG)

struct Expeed6JpegState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t reg[0x1000 / 4];
    AddressSpace *as;
    qemu_irq irq;
    QEMUTimer *done;
    uint32_t len;
    uint32_t n_jobs;
    bool decode;
    uint32_t dec_h;
};

static void jpegco_mem_read(void *opaque, uint64_t addr, void *buf, size_t len)
{
    address_space_read(opaque, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

static void jpegco_mem_write(void *opaque, uint64_t addr, const void *buf, size_t len)
{
    if (expeed6_dma_guard_ok(addr, len, "the JPEG codec model")) {
        address_space_write(opaque, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
    }
}

static void jpegco_update_irq(Expeed6JpegState *s)
{
    qemu_set_irq(s->irq, s->reg[0x480 / 4] != 0);
}

static void jpegco_run(Expeed6JpegState *s)
{
    uint32_t *r = s->reg;
    JpegJob j = {
        .y = r[0x430 / 4],
        .c = r[0x434 / 4],
        .ypitch = r[0x410 / 4],
        .cpitch = r[0x414 / 4],
        .w = r[0x418 / 4] & 0xffff,
        .h = r[0x41c / 4] & 0xffff,
        .shift = (r[0x400 / 4] >> 4) & 3,
    };
    const uint32_t dst = r[0x440 / 4];
    const uint64_t limit = (uint64_t)r[0x458 / 4] * 512;
    MipuMem mem = { s->as, jpegco_mem_read, jpegco_mem_write };
    const char *err = NULL;
    size_t len = 0;

    if ((r[0] & 7) != 1) {
        qemu_log_mask(LOG_UNIMP, "expeed6.jpegco: format %u encoded as 4:2:2\n", r[0] & 7);
    }
    for (int t = 0; t < 2; t++) {
        unsigned sel = (r[0x00c / 4] >> (2 * t)) & 3;
        for (int k = 0; k < 64; k++) {
            j.qtab[t][k] = r[(0x100 + sel * 0x40 + (k & ~3)) / 4] >> (24 - 8 * (k & 3));
        }
    }
    uint8_t *out = jpeg_model_encode(&j, mem, &len, &err);
    if (!out) {
        qemu_log_mask(LOG_GUEST_ERROR, "expeed6.jpegco: job refused: %s\n", err);
        len = 0;
    } else if (limit && len > limit) {
        warn_report("expeed6.jpegco: %zu-byte stream cut to the %" PRIu64 "-byte destination", len, limit);
        len = limit;
    }
    if (out && expeed6_dma_guard_ok(dst, len, "the JPEG codec model")) {
        address_space_write(s->as, dst, MEMTXATTRS_UNSPECIFIED, out, len);
    }
    free(out);
    s->n_jobs++;
    s->len = len;
    timer_mod(s->done, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + JPEGCO_JOB_US);
}

static void jpegco_done(void *opaque)
{
    Expeed6JpegState *s = opaque;
    s->reg[0x020 / 4] = s->len;
    s->reg[0x02c / 4] |= 0x80000000u;
    if (s->decode) {
        s->reg[0x470 / 4] = s->dec_h;
        s->reg[0x480 / 4] |= JPEGCO_ST_END | JPEGCO_ST_DEC_A | JPEGCO_ST_DEC_B;
    } else {
        s->reg[0x480 / 4] |= JPEGCO_ST_END | JPEGCO_ST_ODMA;
    }
    jpegco_update_irq(s);
}

static void jpegco_decode(Expeed6JpegState *s)
{
    uint32_t *r = s->reg;
    JpegDecJob j = {
        .src = r[0x440 / 4],
        .max_len = 8u << 20,
        .y = r[0x430 / 4],
        .c = 0,
        .pitch_align = JPEGCO_DEC_ALIGN,
        .shift = (r[0x400 / 4] >> 4) & 3,
    };
    MipuMem mem = { s->as, jpegco_mem_read, jpegco_mem_write };
    const char *err = NULL;
    uint32_t w = 0, h = 0, used = 0;
    if (jpeg_model_decode(&j, mem, &w, &h, &used, &err) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "expeed6.jpegco: decode refused: %s\n", err);
    }
    s->decode = true;
    s->dec_h = h;
    s->len = used;
    s->n_jobs++;
    timer_mod(s->done, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + JPEGCO_JOB_US);
}

static uint64_t jpegco_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6JpegState *s = opaque;
    uint64_t v = off == 0x488 ? 0 : s->reg[off / 4];
    return v;
}

static void jpegco_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6JpegState *s = opaque;
    uint32_t old = s->reg[off / 4];
    switch (off) {
    case 0x02c:
        s->reg[off / 4] &= val;
        return;
    case 0x480:
        s->reg[off / 4] &= ~val;
        jpegco_update_irq(s);
        return;
    }
    s->reg[off / 4] = val;
    if (off == 0x404 && (val & JPEGCO_GO) && !(old & JPEGCO_GO) && (s->reg[0x004 / 4] & 1)) {
        if (s->reg[0] & 8) {
            jpegco_decode(s);
        } else {
            s->decode = false;
            jpegco_run(s);
        }
    }
}

static const MemoryRegionOps jpegco_ops = {
    .read = jpegco_read,
    .write = jpegco_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void jpegco_realize(DeviceState *dev, Error **errp)
{
    Expeed6JpegState *s = EXPEED6_JPEG(dev);

    s->done = timer_new_us(QEMU_CLOCK_VIRTUAL, jpegco_done, s);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    memory_region_init_io(&s->mmio, OBJECT(dev), &jpegco_ops, s, "expeed6.jpegco", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void jpegco_reset_enter(Object *obj, ResetType type)
{
    Expeed6JpegState *s = EXPEED6_JPEG(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->len = 0;
    s->decode = false;
    s->dec_h = 0;
    timer_del(s->done);
}

static void jpegco_reset_hold(Object *obj, ResetType type)
{
    jpegco_update_irq(EXPEED6_JPEG(obj));
}

static const VMStateDescription vmstate_jpegco = {
    .name = TYPE_EXPEED6_JPEG,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields =
        (const VMStateField[]){ VMSTATE_UINT32_ARRAY(reg, Expeed6JpegState, 0x1000 / 4),
                                VMSTATE_TIMER_PTR(done, Expeed6JpegState), VMSTATE_UINT32(len, Expeed6JpegState),
                                VMSTATE_UINT32(n_jobs, Expeed6JpegState), VMSTATE_BOOL(decode, Expeed6JpegState),
                                VMSTATE_UINT32(dec_h, Expeed6JpegState), VMSTATE_END_OF_LIST() }
};

static void jpegco_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = jpegco_realize;
    dc->vmsd = &vmstate_jpegco;
    rc->phases.enter = jpegco_reset_enter;
    rc->phases.hold = jpegco_reset_hold;
}

static const TypeInfo jpegco_info = {
    .name = TYPE_EXPEED6_JPEG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6JpegState),
    .class_init = jpegco_class_init,
};

static void jpegco_register_types(void)
{
    type_register_static(&jpegco_info);
}

type_init(jpegco_register_types)

    void expeed6_install_jpegco(MemoryRegion *root, AddressSpace *as, DeviceState *gic)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_JPEG);
    EXPEED6_JPEG(dev)->as = as;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, JPEGCO_GIC_IRQ - 32));
    memory_region_add_subregion_overlap(root, JPEGCO_BASE, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0), 2);
}
