/*
 * The Expeed 6 MIPU image processor (0x20380000)
 *
 * its jobs and live-view streams, developed by mipu_model.c.
 *
 * TODO remove the dependency on z6ii.h, shouldn't be there at all, this is Expeed part
 * TODO throw out the scene stand-in and use the actual sensor data
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"
#include "z6ii.h"
#include "mipu_model.h"
#include "fe_model.h"
#include "fe_regs.h"

#define MIPU_BASE      0x20380000
#define MIPU_CH1       0x00040000
#define MIPU_STATUS    0x24
#define MIPU_SPI0      67
#define MIPU_REGS      MIPU_MODEL_REGS
#define MIPU_JOB_US    1000
#define MIPU_STREAM_MS 33

#define TYPE_EXPEED6_MIPU "expeed6-mipu"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6MipuState, EXPEED6_MIPU)

struct Expeed6MipuState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t status[2];
    qemu_irq irq[2];
    int32_t level[2];
    uint32_t regs[2][MIPU_REGS / 4];
    QEMUTimer *job_timer[2];
    AddressSpace *as;
    bool streaming[2];
    bool stream_halt[2];
    bool halt_drain[2];
    QEMUTimer *stream_timer[2];
    bool keep_develop;
    bool unfed_noted;
    uint8_t *lv_y, *lv_c;
    uint32_t lv_w, lv_h;
    uint32_t lv_gen;
    MipuJob *dev_job[2];
    uint32_t planes_addr[2][4];
    uint32_t planes_gen[2][4];
    const uint32_t *fe_regs;
    uint64_t fe_key;
    uint64_t fe_addr[4];
    bool fe_noted;
    void (*fe_refresh)(void *, uint64_t);
    void *fe_refresh_opaque;
    MipuJob dev_job_saved[2];
    bool dev_job_valid[2];
    uint32_t lv_ylen, lv_clen;
};

void fe_dgains(const uint32_t *fe, float *g)
{
    const float green = (FE_GET(fe, DGAIN1) + FE_GET(fe, DGAIN2)) / 2 / 4096.0f;
    for (int c = 0; c < 4; c++) {
        g[c] = green;
    }
}

uint64_t fe_hash(uint64_t h, const void *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull;
    }
    return h;
}

static bool mipu_fe_produce(Expeed6MipuState *s)
{
    if (!s_scene.px || !s->fe_regs) {
        return false;
    }
    const uint32_t *fe = s->fe_regs;
    uint16_t table[FE_COMPAND_N];
    for (int i = 0; i < FE_COMPAND_N; i++) {
        table[i] = FE_TABLE(fe, COMPAND, i);
    }
    FeOutJob j = { .in_w = FE_GET(fe, IN_W),
                   .in_h = FE_GET(fe, IN_H),
                   .out_w = FE_GET(fe, RAWB_W),
                   .out_h = FE_GET(fe, RAWB_H),
                   .depth = FE_GET(fe, RAWB_DEPTH),
                   .dpcm = FE_GET(fe, RAWB_DPCM),
                   .pitch = FE_GET(fe, OUT2_PITCH),
                   .compand = table,
                   .exposure = roundf((float)sensor_exposure_factor() * 200.0f) / 200.0f };
    const uint64_t addr[4] = { FE_GET(fe, OUT2_ADDR0), FE_GET(fe, OUT2_ADDR1), FE_GET(fe, OUT2_ADDR2),
                               FE_GET(fe, OUT2_ADDR3) };
    const uint32_t f[8] = { j.in_w, j.in_h, j.out_w, j.out_h, j.depth, j.dpcm, j.pitch, 0 };
    uint64_t geom = fe_hash(0xcbf29ce484222325ull, f, sizeof(f));
    geom = fe_hash(geom, table, sizeof(table));
    geom = fe_hash(geom, addr, sizeof(addr));
    fe_dgains(fe, j.dgain);
    const uint64_t key = fe_hash(fe_hash(geom, &j.exposure, sizeof(j.exposure)), j.dgain, sizeof(j.dgain));
    if (key == s->fe_key) {
        return false;
    }
    s->fe_key = key;
    memset(s->fe_addr, 0, sizeof(s->fe_addr));
    const size_t n = fe_out_bytes(&j);
    if (!n) {
        if (!s->fe_noted && j.out_w) {
            s->fe_noted = true;
            fprintf(stderr,
                    "expeed6.fe%s: output B (%ux%u from %ux%u, depth %d, DPCM %d, pitch %u) cannot be made from "
                    "the scene; nothing written (EXPEED_SCENE)\n",
                    cluster_tag(), j.out_w, j.out_h, j.in_w, j.in_h, j.depth, j.dpcm, j.pitch);
        }
        return false;
    }
    uint8_t *buf = g_malloc(n);
    fe_render(&s_scene, &j, buf);
    for (int k = 0; k < 4; k++) {
        if (addr[k] && expeed6_dma_guard_ok(addr[k], n, "the front end's output B")) {
            address_space_write(s->as, addr[k], MEMTXATTRS_UNSPECIFIED, buf, n);
            s->fe_addr[k] = addr[k];
        }
    }
    g_free(buf);
    return true;
}

static bool mipu_fe_covers(const Expeed6MipuState *s, uint64_t a)
{
    for (int k = 0; k < 4; k++) {
        if (a && s->fe_addr[k] == a) {
            return true;
        }
    }
    return false;
}


static Expeed6MipuState *s_mipu_lv;

bool expeed6_mipu_lv_develop(Expeed6LvDevelop *d)
{
    Expeed6MipuState *m = s_mipu_lv;
    if (!m || !m->lv_y) {
        return false;
    }
    *d = (Expeed6LvDevelop){ m->lv_y, m->lv_c, m->lv_w, m->lv_h, m->lv_gen };
    return true;
}

#define MIPU_HOST_Y 0x1000000000ull
#define MIPU_HOST_C 0x2000000000ull
typedef struct {
    AddressSpace *as;
    uint8_t *y, *c;
    size_t ylen, clen;
} MipuHostOut;

static void mipu_host_read(void *opaque, uint64_t addr, void *buf, size_t len)
{
    MipuHostOut *h = opaque;
    address_space_read(h->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

static void mipu_host_write(void *opaque, uint64_t addr, const void *buf, size_t len)
{
    MipuHostOut *h = opaque;
    if (addr >= MIPU_HOST_C && addr - MIPU_HOST_C + len <= h->clen) {
        memcpy(h->c + (addr - MIPU_HOST_C), buf, len);
    } else if (addr >= MIPU_HOST_Y && addr - MIPU_HOST_Y + len <= h->ylen) {
        memcpy(h->y + (addr - MIPU_HOST_Y), buf, len);
    }
}

static void mipu_update(Expeed6MipuState *s, int ch);

#define MIPU_ST_UNITS_STOPPED 0x14a0000
#define MIPU_ST_JOB_DONE      0x3

static void mipu_host_develop(Expeed6MipuState *s, int ch, const MipuJob *j)
{
    if (!s->dev_job[ch]) {
        s->dev_job[ch] = g_new0(MipuJob, 1);
    }
    *s->dev_job[ch] = *j;
    MipuJob k = *j;
    k.out_stream = 0;
    k.y_addr = MIPU_HOST_Y;
    k.c_addr = MIPU_HOST_C;
    k.out_pitch = k.w;
    MipuHostOut h = { s->as, NULL, NULL, (size_t)k.w * k.h, (size_t)k.w * (k.h / 2) };
    h.y = g_malloc0(h.ylen);
    h.c = g_malloc0(h.clen);
    if (mipu_model_develop(&k, (MipuMem){ &h, mipu_host_read, mipu_host_write }) == 0) {
        if (s->keep_develop) {
            g_free(s->lv_y);
            g_free(s->lv_c);
            s->lv_y = g_memdup2(h.y, h.ylen);
            s->lv_c = g_memdup2(h.c, h.clen);
            s->lv_w = k.w;
            s->lv_h = k.h;
            s->lv_gen++;
        }
    }
    g_free(h.y);
    g_free(h.c);
}

static void mipu_stream_arm(Expeed6MipuState *s, int ch)
{
    timer_mod(s->stream_timer[ch],
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + expeed6_fe_frame_ns(s->as, MIPU_STREAM_MS * SCALE_MS));
}

static void mipu_job_run(Expeed6MipuState *s, int ch)
{
    MipuJob j;
    bool drain = s->halt_drain[ch];
    s->halt_drain[ch] = false;
    if (drain && MIPU_GET(s->regs[ch], SLOTA_W7) == 0x200 &&
        (MIPU_GET(s->regs[ch], RAW_PITCH) == 0x20 || MIPU_GET(s->regs[ch], RAW_PITCH) == 0x40)) {
        s->status[ch] |= MIPU_ST_UNITS_STOPPED | 0x2;
        mipu_update(s, ch);
        return;
    }
    memset(&j, 0, sizeof(j));
    mipu_job_from_regs(&j, s->regs[ch]);
    const bool scene = s_scene.px != NULL;
    bool fed = false;
    if (scene) {
        mipu_fe_produce(s);
        fed = mipu_fe_covers(s, j.in_addr);
        if (s->fe_refresh) {
            s->fe_refresh(s->fe_refresh_opaque, j.in_addr);
        }
    }
    int r = mipu_model_develop(&j, (MipuMem){ s->as, mipu_mem_read, mipu_mem_write });
    if (j.rawout_addr) {
        mipu_model_raw_out(&j, (MipuMem){ s->as, mipu_mem_read, mipu_mem_write });
    }
    g_free(s->dev_job[ch]);
    s->dev_job[ch] = NULL;
    if (r == 1 && s->keep_develop) {
        mipu_host_develop(s, ch, &j);
    }
    static const char *const src[] = { "registers", "not programmed",
                                       "registers (IMPLAUSIBLE values, used as programmed)" };
    if (r < 0 || r == 2 || j.matrix_src == MIPU_SRC_IMPLAUSIBLE || j.tone_src == MIPU_SRC_IMPLAUSIBLE ||
        j.rggb_src == MIPU_SRC_IMPLAUSIBLE) {
        fprintf(stderr,
                "expeed6.mipu: ch%d job %s: raw %#" PRIx64 " pitch %u %d-bit, %ux%u at "
                "(%u,%u) -> Y %#" PRIx64 " C %#" PRIx64 " pitch %u; matrix %s, tone %s, site gains %s%s\n",
                ch,
                r == 1   ? "not developed (outputs are stream markers: nothing written)"
                : r == 2 ? "not developed (a stage the camera always programs is missing)"
                : r      ? "not developed (implausible geometry)"
                         : "developed",
                j.in_addr, j.in_pitch, j.in_bits, j.w, j.h, j.x0, j.y0, j.y_addr, j.c_addr, j.out_pitch,
                src[j.matrix_src], src[j.tone_src], src[j.rggb_src],
                j.in_bits != MIPU_IN_DPCM6          ? ""
                : j.decompand_src == MIPU_SRC_REGS  ? ", decompanding table registers (+0x2100)"
                : j.decompand_src == MIPU_SRC_UNSET ? ", decompanding table not programmed"
                                                    : ", decompanding table IMPLAUSIBLE (used as programmed)");
    }
    bool ring = MIPU_GET(s->regs[ch], RAW_ADDR1) &&
                MIPU_GET(s->regs[ch], RAW_ADDR1) != MIPU_GET(s->regs[ch], RAW_ADDR0) &&
                (MIPU_GET(s->regs[ch], IN_MODE) & 2);
    bool stream = r == 1 || (r == 0 && ring);
    if (stream && r == 0 && s->keep_develop && fed) {
        mipu_host_develop(s, ch, &j);
    } else if (stream && r == 0 && s->keep_develop && !s->unfed_noted) {
        s->unfed_noted = true;
        fprintf(stderr,
                "expeed6.mipu: ch%d stream job %ux%u %d-bit: no stand-in for its input, the IIP's outputs keep "
                "the last develop\n",
                ch, j.w, j.h, j.in_bits);
    }
    if (stream) {
        s->streaming[ch] = true;
        s->stream_halt[ch] = false;
        mipu_stream_arm(s, ch);
        return;
    }
    s->status[ch] |= MIPU_ST_JOB_DONE;
    mipu_update(s, ch);
}

static void mipu_job_cb0(void *opaque)
{
    mipu_job_run(opaque, 0);
}
static void mipu_job_cb1(void *opaque)
{
    mipu_job_run(opaque, 1);
}

#define MIPU_ST_FRAME_DONE 0x1000
#define MIPU_ST_STOPPED    0x1
#define MIPU_ST_HALTED     (MIPU_ST_UNITS_STOPPED | 0x2)

static void mipu_stream_end(Expeed6MipuState *s, int ch);

static void mipu_stream_redevelop(Expeed6MipuState *s, int ch)
{
    if (!s->dev_job[ch] || !s->keep_develop) {
        return;
    }
    MipuJob j;
    memset(&j, 0, sizeof(j));
    mipu_job_from_regs(&j, s->regs[ch]);
    const bool fresh = mipu_fe_produce(s) && mipu_fe_covers(s, j.in_addr);
    j.y_addr = s->dev_job[ch]->y_addr;
    j.c_addr = s->dev_job[ch]->c_addr;
    j.out_pitch = s->dev_job[ch]->out_pitch;
    j.out_stream = s->dev_job[ch]->out_stream;
    if (!fresh && !memcmp(&j, s->dev_job[ch], sizeof(j))) {
        return;
    }
    mipu_host_develop(s, ch, &j);
}

static void mipu_stream_planes_out(Expeed6MipuState *s, int ch)
{
    const MipuJob *d = s->dev_job[ch];
    const uint32_t *r = s->regs[ch];
    if (!d || d->out_stream || !s->lv_y || s->lv_w != d->w || s->lv_h != d->h) {
        return;
    }
    const uint32_t ring[4][2] = {
        { MIPU_GET(r, OUT_Y_ADDR), MIPU_GET(r, OUT_C_ADDR) },
        { MIPU_GET(r, OUT_Y_ADDR1), MIPU_GET(r, OUT_C_ADDR1) },
        { MIPU_GET(r, OUT_Y_ADDR2), MIPU_GET(r, OUT_C_ADDR2) },
        { MIPU_GET(r, OUT_Y_ADDR3), MIPU_GET(r, OUT_C_ADDR3) },
    };
    uint32_t pitch = MIPU_GET(r, OUT_PITCH);
    for (int e = 0; e < 4; e++) {
        uint32_t ya = ring[e][0], ca = ring[e][1];
        if (ya < 0x80000000u || (ya & 0xfff00000u) == 0x80000000u || pitch < d->w) {
            continue;
        }
        int k = 0;
        while (k < 4 && s->planes_addr[ch][k] && s->planes_addr[ch][k] != ya) {
            k++;
        }
        k = k == 4 ? 0 : k;
        if (s->planes_addr[ch][k] == ya && s->planes_gen[ch][k] == s->lv_gen) {
            continue;
        }
        s->planes_addr[ch][k] = ya;
        s->planes_gen[ch][k] = s->lv_gen;
        if (!expeed6_dma_guard_ok(ya, (size_t)pitch * d->h, "the MIPU stream's planes")) {
            continue;
        }
        bool cok =
            ca >= 0x80000000u && expeed6_dma_guard_ok(ca, (size_t)pitch * (d->h / 2), "the MIPU stream's planes");
        for (uint32_t y = 0; y < d->h; y++) {
            address_space_write(s->as, ya + (uint64_t)y * pitch, MEMTXATTRS_UNSPECIFIED,
                                s->lv_y + (size_t)y * d->w, d->w);
            if (cok && y < d->h / 2) {
                address_space_write(s->as, ca + (uint64_t)y * pitch, MEMTXATTRS_UNSPECIFIED,
                                    s->lv_c + (size_t)y * d->w, d->w);
            }
        }
    }
}

static void mipu_stream_tick(Expeed6MipuState *s, int ch)
{
    if (!s->streaming[ch]) {
        return;
    }
    if (s->stream_halt[ch]) {
        s->stream_halt[ch] = false;
        s->halt_drain[ch] = true;
        mipu_stream_end(s, ch);
        s->status[ch] |= MIPU_ST_HALTED;
        mipu_update(s, ch);
        return;
    }
    s->status[ch] |= MIPU_ST_FRAME_DONE;
    mipu_update(s, ch);
    mipu_stream_redevelop(s, ch);
    mipu_stream_planes_out(s, ch);
    mipu_stream_arm(s, ch);
}

static void mipu_stream_cb0(void *opaque)
{
    mipu_stream_tick(opaque, 0);
}
static void mipu_stream_cb1(void *opaque)
{
    mipu_stream_tick(opaque, 1);
}

static void mipu_stream_end(Expeed6MipuState *s, int ch)
{
    if (!s->streaming[ch]) {
        return;
    }
    s->streaming[ch] = false;
    timer_del(s->stream_timer[ch]);
}

static void mipu_update(Expeed6MipuState *s, int ch)
{
    int l = s->status[ch] ? 1 : 0;
    if (l != s->level[ch]) {
        s->level[ch] = l;
        qemu_set_irq(s->irq[ch], l);
    }
}

static uint64_t mipu_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6MipuState *s = opaque;
    int ch = (off >= MIPU_CH1) ? 1 : 0;
    hwaddr o = off - (ch ? MIPU_CH1 : 0);
    uint64_t v = (o == MIPU_STATUS) ? s->status[ch] : (o < MIPU_REGS) ? s->regs[ch][o / 4] : ~0ULL;
    return v;
}

static void mipu_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6MipuState *s = opaque;
    int ch = (off >= MIPU_CH1) ? 1 : 0;
    hwaddr o = off - (ch ? MIPU_CH1 : 0);
    if (o == MIPU_STATUS) {
        s->status[ch] &= ~(uint32_t)val;
    } else {
        if (o < MIPU_REGS) {
            s->regs[ch][o / 4] = val;
        }
        if (o == 0x08 && (val & 1)) {
            timer_mod(s->job_timer[ch], qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + MIPU_JOB_US);
        }
        if (o == 0x0c && (val & 1) && s->streaming[ch]) {
            mipu_stream_end(s, ch);
            s->status[ch] |= MIPU_ST_STOPPED;
        } else if (o == 0x00 && val == 3) {
            mipu_stream_end(s, ch);
        } else if (o == 0x84 && val == 0 && s->streaming[ch]) {
            s->stream_halt[ch] = true;
        }
    }
    mipu_update(s, ch);
}

static const MemoryRegionOps mipu_ops = {
    .read = mipu_read,
    .write = mipu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void mipu_realize(DeviceState *dev, Error **errp)
{
    Expeed6MipuState *s = EXPEED6_MIPU(dev);

    s->job_timer[0] = timer_new_us(QEMU_CLOCK_VIRTUAL, mipu_job_cb0, s);
    s->job_timer[1] = timer_new_us(QEMU_CLOCK_VIRTUAL, mipu_job_cb1, s);
    s->stream_timer[0] = timer_new_ns(QEMU_CLOCK_VIRTUAL, mipu_stream_cb0, s);
    s->stream_timer[1] = timer_new_ns(QEMU_CLOCK_VIRTUAL, mipu_stream_cb1, s);
    for (int i = 0; i < 2; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
    memory_region_init_io(&s->mmio, OBJECT(dev), &mipu_ops, s, "expeed6.mipu", MIPU_CH1 + MIPU_REGS);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void mipu_reset_enter(Object *obj, ResetType type)
{
    Expeed6MipuState *s = EXPEED6_MIPU(obj);

    memset(s->status, 0, sizeof(s->status));
    memset(s->level, 0, sizeof(s->level));
    memset(s->regs, 0, sizeof(s->regs));
    for (int ch = 0; ch < 2; ch++) {
        timer_del(s->job_timer[ch]);
        timer_del(s->stream_timer[ch]);
        s->streaming[ch] = s->stream_halt[ch] = s->halt_drain[ch] = false;
        g_free(s->dev_job[ch]);
        s->dev_job[ch] = NULL;
    }
    g_free(s->lv_y);
    g_free(s->lv_c);
    s->lv_y = s->lv_c = NULL;
    s->lv_w = s->lv_h = 0;
    s->lv_gen = 0;
    memset(s->planes_addr, 0, sizeof(s->planes_addr));
    memset(s->planes_gen, 0, sizeof(s->planes_gen));
    s->fe_key = 0;
    memset(s->fe_addr, 0, sizeof(s->fe_addr));
}

static void mipu_reset_hold(Object *obj, ResetType type)
{
    Expeed6MipuState *s = EXPEED6_MIPU(obj);

    for (int i = 0; i < 2; i++) {
        qemu_set_irq(s->irq[i], 0);
    }
}

static int mipu_pre_save(void *opaque)
{
    Expeed6MipuState *s = opaque;

    for (int ch = 0; ch < 2; ch++) {
        s->dev_job_valid[ch] = s->dev_job[ch] != NULL;
        if (s->dev_job[ch]) {
            s->dev_job_saved[ch] = *s->dev_job[ch];
        }
    }
    s->lv_ylen = s->lv_y ? s->lv_w * s->lv_h : 0;
    s->lv_clen = s->lv_c ? s->lv_w * (s->lv_h / 2) : 0;
    return 0;
}

static int mipu_post_load(void *opaque, int version_id)
{
    Expeed6MipuState *s = opaque;

    for (int ch = 0; ch < 2; ch++) {
        g_free(s->dev_job[ch]);
        s->dev_job[ch] = s->dev_job_valid[ch] ? g_memdup2(&s->dev_job_saved[ch], sizeof(MipuJob)) : NULL;
    }
    return 0;
}

static const VMStateDescription vmstate_mipu = {
    .name = TYPE_EXPEED6_MIPU,
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_save = mipu_pre_save,
    .post_load = mipu_post_load,
    .fields =
        (const VMStateField[]){ VMSTATE_UINT32_ARRAY(status, Expeed6MipuState, 2),
                                VMSTATE_INT32_ARRAY(level, Expeed6MipuState, 2),
                                VMSTATE_UINT32_2DARRAY(regs, Expeed6MipuState, 2, MIPU_REGS / 4),
                                VMSTATE_TIMER_PTR_ARRAY(job_timer, Expeed6MipuState, 2),
                                VMSTATE_BOOL_ARRAY(streaming, Expeed6MipuState, 2),
                                VMSTATE_BOOL_ARRAY(stream_halt, Expeed6MipuState, 2),
                                VMSTATE_BOOL_ARRAY(halt_drain, Expeed6MipuState, 2),
                                VMSTATE_TIMER_PTR_ARRAY(stream_timer, Expeed6MipuState, 2),
                                VMSTATE_UINT32(lv_w, Expeed6MipuState),
                                VMSTATE_UINT32(lv_h, Expeed6MipuState),
                                VMSTATE_UINT32(lv_gen, Expeed6MipuState),
                                VMSTATE_UINT32(lv_ylen, Expeed6MipuState),
                                VMSTATE_UINT32(lv_clen, Expeed6MipuState),
                                VMSTATE_VBUFFER_ALLOC_UINT32(lv_y, Expeed6MipuState, 0, NULL, lv_ylen),
                                VMSTATE_VBUFFER_ALLOC_UINT32(lv_c, Expeed6MipuState, 0, NULL, lv_clen),
                                VMSTATE_BOOL_ARRAY(dev_job_valid, Expeed6MipuState, 2),
                                VMSTATE_BUFFER_UNSAFE(dev_job_saved, Expeed6MipuState, 0, sizeof(MipuJob) * 2),
                                VMSTATE_UINT32_2DARRAY(planes_addr, Expeed6MipuState, 2, 4),
                                VMSTATE_UINT32_2DARRAY(planes_gen, Expeed6MipuState, 2, 4),
                                VMSTATE_UINT64(fe_key, Expeed6MipuState),
                                VMSTATE_UINT64_ARRAY(fe_addr, Expeed6MipuState, 4),
                                VMSTATE_END_OF_LIST() }
};

static void mipu_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mipu_realize;
    dc->vmsd = &vmstate_mipu;
    rc->phases.enter = mipu_reset_enter;
    rc->phases.hold = mipu_reset_hold;
}

static const TypeInfo mipu_info = {
    .name = TYPE_EXPEED6_MIPU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6MipuState),
    .class_init = mipu_class_init,
};

static void mipu_register_types(void)
{
    type_register_static(&mipu_info);
}

type_init(mipu_register_types)

Expeed6MipuState *expeed6_install_mipu(MemoryRegion *root, AddressSpace *as, bool master)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_MIPU);
    Expeed6MipuState *mipu = EXPEED6_MIPU(dev);
    mipu->as = as;
    if (master) {
        mipu->keep_develop = true;
        s_mipu_lv = mipu;
    }
    const char *sc = getenv("EXPEED_SCENE");
    if (sc && *sc && !s_scene.px) {
        if (sensor_scene_load(&s_scene, sc)) {
            error_report("EXPEED_SCENE: %s is not a scene file (scripts/fe_raw.py --scene NEF OUT)", sc);
            exit(1);
        }
    }
    z6ii_sensor_config(as, master);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, MIPU_BASE);
    return mipu;
}

void expeed6_mipu_connect(Expeed6MipuState *s, DeviceState *gic)
{
    for (int i = 0; i < 2; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s), i, qdev_get_gpio_in(gic, MIPU_SPI0 + i));
    }
}

void expeed6_mipu_connect_fe(Expeed6MipuState *s, const uint32_t *fe_regs, void (*refresh)(void *, uint64_t),
                             void *opaque)
{
    s->fe_regs = fe_regs;
    s->fe_refresh = refresh;
    s->fe_refresh_opaque = opaque;
}
