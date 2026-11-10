/*
 * The Expeed 6 IIP imaging block at 0x20100000
 *
 * The block the image processor live view, the develops and the stills' outputs run through.
 * For the processing itself, see qemu/iip_model.c.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "expeed6.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "iip_model.h"

#define IIP_BASE        0x20100000
#define IIP_SIZE        IIP_REGS_SIZE
#define IIP_LV_OUT_BUFS 64
#define IIP_JOB_US      1000

#define TYPE_EXPEED6_IIP "expeed6-iip"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6IipState, EXPEED6_IIP)

struct Expeed6IipState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    bool master;
    uint32_t status[3];
    uint32_t reg[IIP_SIZE / 4];
    qemu_irq irq;
    int32_t level;
    uint32_t unit_busy;
    QEMUTimer *job_timer;
    bool pixels;
    uint8_t started[IIP_UNIT_IDS];
    AddressSpace *as;
    int64_t lv_ns;
    QEMUTimer *lv_timer;
    uint32_t lv_frames[2];
    uint32_t lv_out_addr[16][IIP_LV_OUT_BUFS];
    uint32_t lv_out_gen[16][IIP_LV_OUT_BUFS];
    uint64_t lv_out_geom[16][IIP_LV_OUT_BUFS];
};

static void iip_mem_read(void *opaque, uint64_t addr, void *buf, size_t len)
{
    address_space_read(opaque, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

static void iip_mem_write(void *opaque, uint64_t addr, const void *buf, size_t len)
{
    if (expeed6_dma_guard_ok(addr, len, "the IIP model")) {
        address_space_write(opaque, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
    }
}

static void iip_update(Expeed6IipState *s)
{
    int l = (s->status[0] || s->status[1] || s->status[2]) ? 1 : 0;
    if (l != s->level) {
        s->level = l;
        qemu_set_irq(s->irq, l);
    }
}

#define IIP_LV_ROWS     4
#define IIP_LV_ROW_SIZE 0x40
#define IIP_LV_SLOTS    9

static const uint32_t *iip_ring_row(Expeed6IipState *s, unsigned r)
{
    return &s->reg[(IIP_RING_REG + r / IIP_LV_ROWS * IIP_RING_STRIDE + r % IIP_LV_ROWS * IIP_LV_ROW_SIZE) / 4];
}

static MipuMem iip_mem(Expeed6IipState *s)
{
    return (MipuMem){ s->as, iip_mem_read, iip_mem_write };
}

static int iip_unit_at(hwaddr off)
{
    return off >= IIP_UNIT_START_REG ? iip_unit_at_block(off) : -1;
}

static void iip_ch_row_clear(Expeed6IipState *s, int ch)
{
    memset(&s->reg[(IIP_CH_ROW_REG + ch * IIP_CH_ROW_STRIDE) / 4], 0, IIP_WIN_EN_INST * 4); /* a slot per window */
}

static void iip_lv_outputs(Expeed6IipState *s, int ch, const uint32_t *row, bool only_enc)
{
    uint8_t units[IIP_UNIT_IDS];
    uint32_t a = IIP_GETI(s->reg, CH_MASK_A, ch), b = IIP_GETI(s->reg, CH_MASK_B, ch);
    Expeed6LvDevelop d;
    bool have = expeed6_mipu_lv_develop(&d);
    for (unsigned id = 0; id < IIP_UNIT_IDS; id++) {
        units[id] = id < 0x18 || iip_unit_in_masks(id, a, b);
    }
    IipEval *ev = iip_eval_new(s->reg, units, row, iip_mem(s));
    if (have) {
        iip_eval_stream(ev, d.y, d.c, d.w, d.h);
    }
    IipOutPath path[IIP_UNIT_OUTS];
    int traced[IIP_UNIT_OUTS];
    for (unsigned k = 0; k < IIP_UNIT_OUTS; k++) {
        unsigned out = IIP_UNIT_OUT0 + k;
        traced[k] = units[out] ? !iip_out_trace(ev, out, &path[k]) : 0;
    }
    for (unsigned k = 0; k < IIP_UNIT_OUTS; k++) {
        IipOutPath p = path[k];
        unsigned out = IIP_UNIT_OUT0 + k;
        if (!traced[k]) {
            continue;
        }
        unsigned n = p.win;
        uint32_t buf = (uint32_t)p.dst.y;
        if (only_enc && !expeed6_h264_is_input_plane(buf)) {
            continue;
        }
        int j = 0;
        while (j < IIP_LV_OUT_BUFS && s->lv_out_addr[n][j] && s->lv_out_addr[n][j] != buf) {
            j++;
        }
        if (j == IIP_LV_OUT_BUFS) {
            memset(s->lv_out_addr[n], 0, sizeof(s->lv_out_addr[n]));
            j = 0;
        }
        uint64_t geom =
            p.dst.w | (uint64_t)p.dst.h << 16 | (uint64_t)p.dst.ypitch << 32 | (uint64_t)p.dst.cpitch << 48;
        geom ^= (p.dst.c - p.dst.y) * 0x9e3779b97f4a7c15ull ^ (uint64_t)out << 56;
        uint32_t gen = have ? d.gen : 0;
        if (s->lv_out_addr[n][j] == buf && s->lv_out_gen[n][j] == gen && s->lv_out_geom[n][j] == geom) {
            continue;
        }
        s->lv_out_addr[n][j] = buf;
        s->lv_out_gen[n][j] = gen;
        s->lv_out_geom[n][j] = geom;
        iip_out_run(ev, out, &p);
    }
    iip_eval_free(ev);
}

static void expeed6_iip_lv_frame(void *opaque)
{
    Expeed6IipState *s = opaque;
    uint32_t bits = 0x400;
    uint32_t outs = 0;
    for (int ch = 0; ch < 2; ch++) {
        bits |= 0x114u << ch;
        if (!IIP_GETI(s->reg, CH_START, ch) || !IIP_GETI(s->reg, IN_MODE, ch)) {
            s->lv_frames[ch] = 0;
            iip_ch_row_clear(s, ch);
            bits |= 0x40u << ch; /* unit 4 every frame */
            continue;
        }
        uint32_t f = s->lv_frames[ch]++;
        unsigned row = f % (2 * IIP_LV_ROWS);
        const uint32_t *r = iip_ring_row(s, row);
        memcpy(&s->reg[(IIP_CH_ROW_REG + ch * IIP_CH_ROW_STRIDE) / 4], r, IIP_LV_SLOTS * 4);
        s->reg[(IIP_CH_RUNNING_REG + ch * IIP_CH_RUNNING_STRIDE) / 4] |= 1u << IIP_CH_RUNNING_SHIFT;
        iip_lv_outputs(s, ch, r, false);
        for (unsigned rr = 0; rr < 2 * IIP_LV_ROWS; rr++) {
            iip_lv_outputs(s, ch, iip_ring_row(s, rr), true);
        }
        if (row % IIP_LV_ROWS == IIP_LV_ROWS - 1) {
            bits |= 0x40u << ch;
        }
        for (unsigned id = IIP_UNIT_OUT0; id < IIP_UNIT_OUT0 + IIP_UNIT_OUTS; id++) {
            if (iip_unit_in_masks(id, IIP_GETI(s->reg, CH_MASK_A, ch), IIP_GETI(s->reg, CH_MASK_B, ch))) {
                outs |= 1u << (id - IIP_UNIT_OUT0);
            }
        }
    }
    s->reg[IIP_CH1_FRAMES_REG / 4]++;
    s->status[2] |= bits;
    s->status[1] |= outs & IIP_GET(s->reg, IRQ_EN_UNITS);
    iip_update(s);
    timer_mod(s->lv_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + expeed6_fe_frame_ns(s->as, s->lv_ns));
}

static void expeed6_iip_job_done(void *opaque)
{
    Expeed6IipState *s = opaque;
    uint8_t units[IIP_UNIT_IDS];
    for (unsigned id = 0; id < IIP_UNIT_IDS; id++) {
        units[id] = id < 0x18 || s->started[id];
    }
    bool run = s->pixels || s->lv_ns;
    IipEval *ev = iip_eval_new(s->reg, units, NULL, iip_mem(s));
    IipOutPath path[IIP_UNIT_OUTS];
    int traced[IIP_UNIT_OUTS];
    for (unsigned k = 0; k < IIP_UNIT_OUTS; k++) {
        traced[k] = s->started[IIP_UNIT_OUT0 + k] && !iip_out_trace(ev, IIP_UNIT_OUT0 + k, &path[k]);
    }
    for (unsigned k = 0; k < IIP_UNIT_OUTS; k++) {
        if (traced[k] && run) {
            IipOutPath q;
            iip_out_run(ev, IIP_UNIT_OUT0 + k, &q);
        }
    }
    iip_eval_free(ev);
    memset(s->started, 0, sizeof(s->started));
    s->status[1] |= s->unit_busy;
    s->unit_busy = 0;
    iip_update(s);
}

static uint64_t iip_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6IipState *s = opaque;
    uint64_t v = (off == IIP_STATUS_FLAG_REG)    ? s->status[0]
                 : (off == IIP_STATUS_UNITS_REG) ? s->status[1]
                 : (off == IIP_STATUS_LV_REG)    ? s->status[2]
                                                 : s->reg[off / 4];
    return v;
}

static void iip_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6IipState *s = opaque;

    if (off == IIP_STATUS_FLAG_REG || off == IIP_STATUS_UNITS_REG || off == IIP_STATUS_LV_REG) {
        s->status[(off - IIP_STATUS_FLAG_REG) / 4] &= ~(uint32_t)val; /* write-back-to-clear */
    } else {
        s->reg[off / 4] = val;
        for (int ch = 0; ch < 2; ch++) {
            if (off == IIP_CH_START_REG + ch * IIP_CH_START_STRIDE && !(val & (1u << IIP_CH_START_SHIFT))) {
                iip_ch_row_clear(s, ch);
            }
        }
        if (off < 0x1000) {
            s->status[0] |= 1;
        }
        int id = (val & (1u << IIP_UNIT_START_SHIFT)) ? iip_unit_at(off) : -1;
        if (id >= 0) {
            s->started[id] = 1;
            if (id >= IIP_UNIT_OUT0 && id < IIP_UNIT_OUT0 + IIP_UNIT_OUTS) {
                s->unit_busy |= 1u << (id - IIP_UNIT_OUT0);
                if (!timer_pending(s->job_timer)) {
                    timer_mod(s->job_timer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + IIP_JOB_US);
                }
            }
        }
    }
    iip_update(s);
}

static const MemoryRegionOps iip_ops = {
    .read = iip_read,
    .write = iip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void iip_realize(DeviceState *dev, Error **errp)
{
    Expeed6IipState *iip = EXPEED6_IIP(dev);

    iip->lv_ns = iip->master ? 33 * SCALE_MS : 0;
    iip->job_timer = timer_new_us(QEMU_CLOCK_VIRTUAL, expeed6_iip_job_done, iip);
    iip->lv_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, expeed6_iip_lv_frame, iip);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &iip->irq);
    memory_region_init_io(&iip->mmio, OBJECT(dev), &iip_ops, iip, "expeed6.iip", IIP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &iip->mmio);
}

static void iip_reset_enter(Object *obj, ResetType type)
{
    Expeed6IipState *s = EXPEED6_IIP(obj);

    memset(s->status, 0, sizeof(s->status));
    memset(s->reg, 0, sizeof(s->reg));
    s->level = 0;
    s->unit_busy = 0;
    memset(s->started, 0, sizeof(s->started));
    memset(s->lv_frames, 0, sizeof(s->lv_frames));
    memset(s->lv_out_addr, 0, sizeof(s->lv_out_addr));
    memset(s->lv_out_gen, 0, sizeof(s->lv_out_gen));
    memset(s->lv_out_geom, 0, sizeof(s->lv_out_geom));
    timer_del(s->job_timer);
    timer_del(s->lv_timer);
    if (s->lv_ns) {
        timer_mod(s->lv_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->lv_ns);
    }
}

static void iip_reset_hold(Object *obj, ResetType type)
{
    qemu_set_irq(EXPEED6_IIP(obj)->irq, 0);
}

static const VMStateDescription vmstate_iip = {
    .name = TYPE_EXPEED6_IIP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (const VMStateField[]){
            VMSTATE_UINT32_ARRAY(status, Expeed6IipState, 3),
            VMSTATE_UINT32_ARRAY(reg, Expeed6IipState, IIP_SIZE / 4), VMSTATE_INT32(level, Expeed6IipState),
            VMSTATE_UINT32(unit_busy, Expeed6IipState), VMSTATE_TIMER_PTR(job_timer, Expeed6IipState),
            VMSTATE_UINT8_ARRAY(started, Expeed6IipState, IIP_UNIT_IDS),
            VMSTATE_TIMER_PTR(lv_timer, Expeed6IipState), VMSTATE_UINT32_ARRAY(lv_frames, Expeed6IipState, 2),
            VMSTATE_UINT32_2DARRAY(lv_out_addr, Expeed6IipState, 16, IIP_LV_OUT_BUFS),
            VMSTATE_UINT32_2DARRAY(lv_out_gen, Expeed6IipState, 16, IIP_LV_OUT_BUFS),
            VMSTATE_UINT64_2DARRAY(lv_out_geom, Expeed6IipState, 16, IIP_LV_OUT_BUFS), VMSTATE_END_OF_LIST() }
};

static Property iip_properties[] = {
    DEFINE_PROP_BOOL("pixels", Expeed6IipState, pixels, false),
    DEFINE_PROP_BOOL("master", Expeed6IipState, master, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void iip_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = iip_realize;
    dc->vmsd = &vmstate_iip;
    device_class_set_props(dc, iip_properties);
    rc->phases.enter = iip_reset_enter;
    rc->phases.hold = iip_reset_hold;
}

static const TypeInfo iip_info = {
    .name = TYPE_EXPEED6_IIP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6IipState),
    .class_init = iip_class_init,
};

static void iip_register_types(void)
{
    type_register_static(&iip_info);
}

type_init(iip_register_types)

Expeed6IipState *expeed6_iip_new(MemoryRegion *root, AddressSpace *as, bool master)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_IIP);

    EXPEED6_IIP(dev)->as = as;
    qdev_prop_set_bit(dev, "master", master);
    qdev_prop_set_bit(dev, "pixels", getenv("EXPEED_IIP_PIXELS") && atoi(getenv("EXPEED_IIP_PIXELS")));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, IIP_BASE);
    return EXPEED6_IIP(dev);
}

void expeed6_iip_connect(Expeed6IipState *iip, qemu_irq irq, bool master)
{
    sysbus_connect_irq(SYS_BUS_DEVICE(iip), 0, irq);
}
