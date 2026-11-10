/*
 * The Expeed 6 H.264 encoder at 0x24070000
 *
 * register file, interrupt side and the access units it writes
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"

#define H264_BASE 0x24070000ULL
#define H264_SIZE 0x1000

#define H264_GIC_IRQ 110
#define H264_STATUS  0xc28
#define H264_ACK     0xc30
#define H264_IRQ_EN  0xc24
#define H264_ENABLE  0x454
#define H264_CMD     0xc00
#define H264_ST_SYNC 0x80

#define TYPE_EXPEED6_H264 "expeed6-h264"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6H264State, EXPEED6_H264)

struct Expeed6H264State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t reg[H264_SIZE / 4];
    uint32_t status;
    qemu_irq irq;
    QEMUTimer *sync;
    int64_t frame_ns;
    bool fe_rate;
    bool oneshot;
    bool ending;
    bool encode;
    uint32_t frames;
    uint32_t bank_off, ring_off;
    uint32_t bank_b, ring_b;
    uint32_t in_addr[64];
    AddressSpace *as;
};

static Expeed6H264State *s_h264_master;

bool expeed6_h264_is_input_plane(uint32_t addr)
{
    if (!s_h264_master || !addr) {
        return false;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s_h264_master->in_addr); i += 2) {
        if (s_h264_master->in_addr[i] == addr) {
            return true;
        }
    }
    return false;
}

static void expeed6_h264_update(Expeed6H264State *s)
{
    uint32_t en = s->reg[H264_IRQ_EN / 4];
    qemu_set_irq(s->irq, (s->status & (en ? en : ~0u)) != 0);
}

static const uint8_t h264_aud[] = { 0, 0, 0, 1, 0x09, 0xf0 };

#define H264_W_MB 120
#define H264_H_MB 68

typedef struct {
    GByteArray *b;
    uint32_t acc;
    int n;
} H264Bits;

static void h264_u(H264Bits *w, uint32_t v, int bits)
{
    for (int i = bits - 1; i >= 0; i--) {
        w->acc = w->acc << 1 | ((v >> i) & 1);
        if (++w->n == 8) {
            uint8_t c = w->acc;
            g_byte_array_append(w->b, &c, 1);
            w->acc = 0;
            w->n = 0;
        }
    }
}

static void h264_ue(H264Bits *w, uint32_t v)
{
    uint64_t x = (uint64_t)v + 1;
    int len = 0;
    while ((x >> len) > 1) {
        len++;
    }
    h264_u(w, 0, len);
    h264_u(w, (uint32_t)x, len + 1);
}

static void h264_se(H264Bits *w, int32_t v)
{
    h264_ue(w, v <= 0 ? (uint32_t)(-2 * v) : (uint32_t)(2 * v - 1));
}

static void h264_align_zero(H264Bits *w)
{
    while (w->n) {
        h264_u(w, 0, 1);
    }
}

static void h264_trailing(H264Bits *w)
{
    h264_u(w, 1, 1);
    h264_align_zero(w);
}

static void h264_nal(GByteArray *out, uint8_t header, const GByteArray *rbsp)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    g_byte_array_append(out, sc, 4);
    g_byte_array_append(out, &header, 1);
    int zeros = 0;
    for (guint i = 0; i < rbsp->len; i++) {
        uint8_t c = rbsp->data[i];
        if (zeros >= 2 && c <= 3) {
            static const uint8_t ep = 3;
            g_byte_array_append(out, &ep, 1);
            zeros = 0;
        }
        g_byte_array_append(out, &c, 1);
        zeros = c ? 0 : zeros + 1;
    }
}

static void h264_sps_rbsp(H264Bits *w)
{
    h264_u(w, 100, 8);  /* profile_idc: High */
    h264_u(w, 0, 8);    /* constraint flags */
    h264_u(w, 40, 8);   /* level_idc 4.0 */
    h264_ue(w, 0);      /* seq_parameter_set_id */
    h264_ue(w, 1);      /* chroma_format_idc 4:2:0 */
    h264_ue(w, 0);      /* bit_depth_luma_minus8 */
    h264_ue(w, 0);      /* bit_depth_chroma_minus8 */
    h264_u(w, 0, 1);    /* qpprime_y_zero_transform_bypass_flag */
    h264_u(w, 0, 1);    /* seq_scaling_matrix_present_flag */
    h264_ue(w, 0);      /* log2_max_frame_num_minus4 */
    h264_ue(w, 0);      /* pic_order_cnt_type */
    h264_ue(w, 4);      /* log2_max_pic_order_cnt_lsb_minus4: 8 bits */
    h264_ue(w, 1);      /* max_num_ref_frames */
    h264_u(w, 0, 1);    /* gaps_in_frame_num_value_allowed_flag */
    h264_ue(w, H264_W_MB - 1);
    h264_ue(w, H264_H_MB - 1);
    h264_u(w, 1, 1);    /* frame_mbs_only_flag */
    h264_u(w, 1, 1);    /* direct_8x8_inference_flag */
    h264_u(w, 1, 1);    /* frame_cropping_flag: 1088 -> 1080 */
    h264_ue(w, 0);
    h264_ue(w, 0);
    h264_ue(w, 0);
    h264_ue(w, 4);      /* frame_crop_bottom_offset, in 2-line units */
    h264_u(w, 0, 1);    /* vui_parameters_present_flag */
    h264_trailing(w);
}

static void h264_pps_rbsp(H264Bits *w)
{
    h264_ue(w, 0);      /* pic_parameter_set_id */
    h264_ue(w, 0);      /* seq_parameter_set_id */
    h264_u(w, 0, 1);    /* entropy_coding_mode_flag: CAVLC */
    h264_u(w, 0, 1);    /* bottom_field_pic_order_in_frame_present_flag */
    h264_ue(w, 0);      /* num_slice_groups_minus1 */
    h264_ue(w, 0);      /* num_ref_idx_l0_default_active_minus1 */
    h264_ue(w, 0);      /* num_ref_idx_l1_default_active_minus1 */
    h264_u(w, 0, 1);    /* weighted_pred_flag */
    h264_u(w, 0, 2);    /* weighted_bipred_idc */
    h264_se(w, 0);      /* pic_init_qp_minus26 */
    h264_se(w, 0);      /* pic_init_qs_minus26 */
    h264_se(w, 0);      /* chroma_qp_index_offset */
    h264_u(w, 1, 1);    /* deblocking_filter_control_present_flag */
    h264_u(w, 0, 1);    /* constrained_intra_pred_flag */
    h264_u(w, 0, 1);    /* redundant_pic_cnt_present_flag */
    h264_trailing(w);
}

static void h264_slice_rbsp(H264Bits *w, uint32_t k, uint32_t idr_id, const uint8_t *y, const uint8_t *c)
{
    h264_ue(w, 0);              /* first_mb_in_slice */
    h264_ue(w, k ? 5 : 7);      /* slice_type: all P / all I */
    h264_ue(w, 0);              /* pic_parameter_set_id */
    h264_u(w, k ? 1 : 0, 4);    /* frame_num: the non-reference P frames follow the IDR */
    if (!k) {
        h264_ue(w, idr_id & 0xffff);    /* idr_pic_id */
    }
    h264_u(w, (2 * k) & 0xff, 8);       /* pic_order_cnt_lsb */
    if (k) {
        h264_u(w, 0, 1);        /* num_ref_idx_active_override_flag */
        h264_u(w, 0, 1);        /* ref_pic_list_modification_flag_l0 */
    } else {
        h264_u(w, 0, 1);        /* no_output_of_prior_pics_flag */
        h264_u(w, 0, 1);        /* long_term_reference_flag */
    }
    h264_se(w, 0);              /* slice_qp_delta */
    h264_ue(w, 1);              /* disable_deblocking_filter_idc: off */
    if (k) {
        h264_ue(w, H264_W_MB * H264_H_MB);  /* mb_skip_run: the whole picture */
        h264_trailing(w);
        return;
    }
    for (int my = 0; my < H264_H_MB; my++) {
        for (int mx = 0; mx < H264_W_MB; mx++) {
            h264_ue(w, 25); /* mb_type I_PCM */
            h264_align_zero(w);
            uint8_t mb[384];
            for (int r = 0; r < 16; r++) {
                int yy = MIN(my * 16 + r, 1079);
                memcpy(mb + r * 16, y + yy * 1920 + mx * 16, 16);
            }
            for (int r = 0; r < 8; r++) {
                int cy = MIN(my * 8 + r, 539);
                for (int i = 0; i < 8; i++) {
                    mb[256 + r * 8 + i] = c[cy * 1920 + (mx * 8 + i) * 2];      /* Cb */
                    mb[320 + r * 8 + i] = c[cy * 1920 + (mx * 8 + i) * 2 + 1];  /* Cr */
                }
            }
            g_byte_array_append(w->b, mb, sizeof(mb));
        }
    }
    h264_trailing(w);
}

static void expeed6_h264_put(Expeed6H264State *s, uint32_t addr, const void *buf, uint32_t len)
{
    if (expeed6_dma_guard_ok(addr, len, "the H.264 model")) {
        address_space_write(s->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
    }
}

static uint32_t h264_other(const uint32_t *base, unsigned cur)
{
    return base[cur ^ 1] ? cur ^ 1 : cur;
}

static void expeed6_h264_encode_frame(Expeed6H264State *s)
{
    uint32_t bank[2] = { s->reg[0x470 / 4] ? s->reg[0x470 / 4] : s->reg[0x490 / 4], s->reg[0x474 / 4] };
    uint32_t bank_size[2] = { s->reg[0x494 / 4], s->reg[0x49c / 4] };
    uint32_t ring[2] = { s->reg[0x4b0 / 4], s->reg[0x4b4 / 4] },
             ring_size[2] = { s->reg[0x4c4 / 4], s->reg[0x4c8 / 4] };
    uint32_t gop = s->reg[0xe10 / 4] & 0xfff;
    if (!bank[0] || !bank_size[0] || !ring[0] || !ring_size[0]) {
        return;
    }
    if (!bank_size[1]) {
        bank[1] = 0;
    }
    if (!ring_size[1]) {
        ring[1] = 0;
    }
    if (!bank[s->bank_b] || s->bank_off >= bank_size[s->bank_b]) {
        s->bank_b = 0;
        s->bank_off = 0;
    }
    if (!ring[s->ring_b]) {
        s->ring_b = 0;
        s->ring_off = 0;
    }
    if (!gop) {
        gop = 30;
    }
    bool idr = (s->frames % gop) == 0;
    uint32_t k = s->frames % gop;
    g_autoptr(GByteArray) au = g_byte_array_new();
    struct {
        uint32_t off, len;
        uint8_t type;
    } nal[4];
    unsigned n = 0;
    g_byte_array_append(au, h264_aud, sizeof(h264_aud));
    nal[n++] = (typeof(nal[0])){ 0, sizeof(h264_aud), 9 };
    g_autofree uint8_t *y = NULL;
    g_autofree uint8_t *c = NULL;
    if (idr) {
        for (int t = 0; t < 2; t++) {
            g_autoptr(GByteArray) rb = g_byte_array_new();
            H264Bits w = { rb, 0, 0 };
            uint32_t at = au->len;
            if (t == 0) {
                h264_sps_rbsp(&w);
                h264_nal(au, 0x67, rb);
            } else {
                h264_pps_rbsp(&w);
                h264_nal(au, 0x68, rb);
            }
            nal[n++] = (typeof(nal[0])){ at, au->len - at, t ? 8 : 7 };
        }
        unsigned idx = ((s->frames / 3) & 1) * 0x20 + 4 * (s->frames % 3);
        uint32_t ya = s->in_addr[idx & 0x3f], ca = s->in_addr[(idx + 1) & 0x3f];
        y = g_malloc0(1920 * 1080);
        c = g_malloc0(1920 * 540);
        if (ya && ca) {
            address_space_read(s->as, ya, MEMTXATTRS_UNSPECIFIED, y, 1920 * 1080);
            address_space_read(s->as, ca, MEMTXATTRS_UNSPECIFIED, c, 1920 * 540);
        } else {
            memset(c, 0x80, 1920 * 540);
        }
    }
    {
        g_autoptr(GByteArray) rb = g_byte_array_new();
        H264Bits w = { rb, 0, 0 };
        uint32_t at = au->len;
        h264_slice_rbsp(&w, k, s->frames / gop, y, c);
        h264_nal(au, idr ? 0x65 : 0x01, rb);
        nal[n++] = (typeof(nal[0])){ at, au->len - at, idr ? 5 : 1 };
    }
    uint32_t a = bank[s->bank_b] + s->bank_off, first = MIN(au->len, bank_size[s->bank_b] - s->bank_off);
    expeed6_h264_put(s, a, au->data, first);
    s->bank_off += first;
    unsigned next = s->bank_b;
    if (s->bank_off == bank_size[s->bank_b]) {
        next = h264_other(bank, s->bank_b);
        s->bank_off = 0;
    }
    s->bank_b = next;
    if (first < au->len) {
        expeed6_h264_put(s, bank[next], au->data + first, au->len - first);
        s->bank_off = au->len - first;
    }
    for (unsigned i = 0; i < n; i++) {
        uint8_t rec[8];
        stl_le_p(rec, nal[i].off < first ? a + nal[i].off : bank[next] + (nal[i].off - first));
        rec[4] = nal[i].len & 0xff;
        rec[5] = (nal[i].len >> 8) & 0xff;
        rec[6] = (nal[i].len >> 16) & 0xff;
        rec[7] = (uint8_t)(nal[i].type << 3 | ((nal[i].len >> 24) & 7));
        if (s->ring_off + 8 > ring_size[s->ring_b]) {
            s->ring_b = h264_other(ring, s->ring_b);
            s->ring_off = 0;
        }
        expeed6_h264_put(s, ring[s->ring_b] + s->ring_off, rec, sizeof(rec));
        s->ring_off += 8;
    }
    s->reg[0xea4 / 4] = n;
    s->status |= 0x1000;
    if (s->frames % 3 == 2) {
        s->status |= ((s->frames / 3) & 1) ? 0x20000 : 0x10000;
    }
    s->frames++;
}

static int64_t expeed6_h264_period(Expeed6H264State *s)
{
    return s->fe_rate ? expeed6_fe_frame_ns(s->as, s->frame_ns) : s->frame_ns;
}

static void expeed6_h264_sync(void *opaque)
{
    Expeed6H264State *s = opaque;
    s->status |= H264_ST_SYNC;
    if (s->ending) {
        s->reg[0x4ac / 4] = s->bank_off;
        s->status |= 0x8000;
        s->ending = false;
    } else if (s->encode && s->as) {
        expeed6_h264_encode_frame(s);
    }
    expeed6_h264_update(s);
    if (s->oneshot) {
        s->oneshot = false;
        return;
    }
    timer_mod(s->sync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + expeed6_h264_period(s));
}

static uint64_t expeed6_h264_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6H264State *s = opaque;
    uint64_t v = (off / 4 < H264_SIZE / 4) ? s->reg[off / 4] : 0;
    if (s->frame_ns && off == H264_STATUS) {
        v = s->status;
    }
    return v;
}

static void expeed6_h264_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6H264State *s = opaque;
    if (off / 4 < H264_SIZE / 4) {
        s->reg[off / 4] = (uint32_t)val;
    }
    if (off == 0x43c) {
        s->in_addr[s->reg[0x438 / 4] & 0x3f] = (uint32_t)val;
    }
    if (s->frame_ns) {
        if (off == 0x4fc && (val & 0x1000)) {
            s->reg[0x4fc / 4] |= 0x8000;
        }
        if (off == H264_ACK) {
            s->status &= ~(uint32_t)val;
            expeed6_h264_update(s);
        } else if (off == H264_IRQ_EN) {
            expeed6_h264_update(s);
        } else if (off == H264_ENABLE && val == 1) {
            s->oneshot = false;
            timer_mod(s->sync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
        } else if (off == H264_ENABLE && val == 2) {
            timer_del(s->sync);
        } else if (off == 0x440 && (val & 1)) {
            s->oneshot = false;
            s->ending = false;
            s->frames = 0;
            s->bank_b = s->ring_b = 0;
            s->bank_off = s->ring_off = 0;
            timer_mod(s->sync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + expeed6_h264_period(s));
        } else if (off == H264_CMD && val == 0x2000) {
            s->oneshot = true;
            s->ending = true;
            memset(s->in_addr, 0, sizeof(s->in_addr));
            timer_mod(s->sync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
        }
    }
}

static const MemoryRegionOps expeed6_h264_ops = {
    .read = expeed6_h264_read,
    .write = expeed6_h264_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void h264_realize(DeviceState *dev, Error **errp)
{
    Expeed6H264State *s = EXPEED6_H264(dev);

    s->sync = timer_new_ns(QEMU_CLOCK_VIRTUAL, expeed6_h264_sync, s);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    memory_region_init_io(&s->mmio, OBJECT(dev), &expeed6_h264_ops, s, "expeed6.h264", H264_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void h264_reset_enter(Object *obj, ResetType type)
{
    Expeed6H264State *s = EXPEED6_H264(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->status = 0;
    s->oneshot = s->ending = false;
    s->frames = 0;
    s->bank_off = s->ring_off = 0;
    s->bank_b = s->ring_b = 0;
    memset(s->in_addr, 0, sizeof(s->in_addr));
    timer_del(s->sync);
}

static void h264_reset_hold(Object *obj, ResetType type)
{
    qemu_set_irq(EXPEED6_H264(obj)->irq, 0);
}

static const VMStateDescription vmstate_h264 = {
    .name = TYPE_EXPEED6_H264,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields =
        (const VMStateField[]){
            VMSTATE_UINT32_ARRAY(reg, Expeed6H264State, H264_SIZE / 4), VMSTATE_UINT32(status, Expeed6H264State),
            VMSTATE_TIMER_PTR(sync, Expeed6H264State), VMSTATE_BOOL(oneshot, Expeed6H264State),
            VMSTATE_BOOL(ending, Expeed6H264State), VMSTATE_UINT32(frames, Expeed6H264State),
            VMSTATE_UINT32(bank_off, Expeed6H264State), VMSTATE_UINT32(ring_off, Expeed6H264State),
            VMSTATE_UINT32(bank_b, Expeed6H264State), VMSTATE_UINT32(ring_b, Expeed6H264State),
            VMSTATE_UINT32_ARRAY(in_addr, Expeed6H264State, 64), VMSTATE_END_OF_LIST() }
};

static void h264_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = h264_realize;
    dc->vmsd = &vmstate_h264;
    rc->phases.enter = h264_reset_enter;
    rc->phases.hold = h264_reset_hold;
}

static const TypeInfo h264_info = {
    .name = TYPE_EXPEED6_H264,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6H264State),
    .class_init = h264_class_init,
};

static void h264_register_types(void)
{
    type_register_static(&h264_info);
}

type_init(h264_register_types)

void expeed6_install_h264(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_H264);
    Expeed6H264State *h264 = EXPEED6_H264(dev);
    h264->as = as;
    h264->encode = getenv("EXPEED_H264_ENCODE") && atoi(getenv("EXPEED_H264_ENCODE"));
    if (master) {
        s_h264_master = h264;
    }
    const char *rate = master ? getenv("EXPEED_H264") : NULL;
    if (rate && (!strcmp(rate, "fe") || strtod(rate, NULL) > 0)) {
        h264->fe_rate = true;
        h264->frame_ns = (int64_t)((!strcmp(rate, "fe") ? 33.367 : strtod(rate, NULL)) * SCALE_MS);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, H264_GIC_IRQ - 32));
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, H264_BASE);
}
