/*
 * The Expeed 6 sensor interface front end (SIF) at 0x20060000
 *
 * The SDES receiver's status, the PPC pin-change block and its DMA channels,
 * and the front end's raw outputs.
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

#define SIF_BASE       0x20060000ULL
#define SIF_SIZE       0x10000
#define SIF_FRAME_STOP 0x5504
#define SIF_RUN_FLAG   0x00a8

#define SIF_STATUS0   0x38
#define SIF_SPI0      54
#define SIF_STATUS1   0x44
#define SIF_SPI1      55
#define SIF_STATUS6   0x5508
#define SIF_SPI6      60
#define PPC_STATUS(g) (0x8210 + (g) * 0x20)
#define PPC_SPI0      62

#define TYPE_EXPEED6_SIF "expeed6-sif"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6SifState, EXPEED6_SIF)

static const struct {
    int g;
    uint32_t mask;
    int64_t delay_us;
} ppc_frame_edges[] = {
    { 0, 0x69, 15000 },
    { 7, 0x1, 0 },
};
#define PPC_FRAME_EDGES ARRAY_SIZE(ppc_frame_edges)

typedef struct {
    Expeed6SifState *s;
    int i;
} Expeed6PpcEdge;

struct Expeed6SifState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    Expeed6PpcEdge edge[PPC_FRAME_EDGES];
    uint32_t reg[SIF_SIZE / 4];
    qemu_irq irq0;
    qemu_irq irq1;
    qemu_irq irq6;
    qemu_irq irqppc[5];
    QEMUTimer *frame;
    uint32_t ch1_size;
    bool ch1_final;
    uint64_t fe_a_key;
    uint64_t fe_a_prog;
    bool fe_a_still;
    QEMUTimer *ppc_timer[PPC_FRAME_EDGES];
    QEMUTimer *mode_tick;
    uint32_t units_pending;
    uint32_t req_pending[8];
    QEMUTimer *stop_ack;
    uint32_t sif1_pending;
    QEMUTimer *sif_stop_timer;
    AddressSpace *as;
    Expeed6DmaState *idmac;
    bool stats_model;
    bool stats_ready;
    uint16_t stats_cells[20 * 32];
    uint16_t stats_rgb[3][40 * 64];
    float *stats_px;
    double *stats_pre;
    uint32_t stats_off[3 * 40 * 64 + 20 * 32 + 1];
    double stats_k;
    bool units_stopped;
    bool fe_stopped;
    int64_t fe_resume_ns;
};

#define SIF_FRAME_MS    33
#define SIF_FRAME_MASK  0x101
#define SIF_FRAME_MASK1 0x40490
#define SIF_FRAME_MASK6 0x3

static int expeed6_ppc_line(int g)
{
    static const int line[8] = { 0, 0, 1, 2, 2, 3, 3, 4 };
    return line[g & 7];
}

static void expeed6_ppc_assert(Expeed6SifState *s, int i)
{
    const int g = ppc_frame_edges[i].g;
    s->reg[PPC_STATUS(g) / 4] |= ppc_frame_edges[i].mask;
    qemu_set_irq(s->irqppc[expeed6_ppc_line(g)], 1);
}

static void expeed6_ppc_delayed(void *opaque)
{
    Expeed6PpcEdge *e = opaque;
    if (e->s->fe_stopped || qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < e->s->fe_resume_ns) {
        return;
    }
    if (e->s->units_stopped) {
        return;
    }
    expeed6_ppc_assert(e->s, e->i);
}

#define PPC_COMMIT      0x8100
#define PPC_STOP        0x8060
#define PPC_STOP_ACK_US 100
static const struct {
    uint32_t code;
    int g;
    uint32_t bit;
} ppc_stop_pins[] = {
    { 0x08, 0, 0x10000000 }, { 0x04, 1, 0x00000001 }, { 0x10, 1, 0x00001000 },
    { 0x03, 1, 0x00000110 }, { 0x60, 7, 0x00000030 },
};
#define PPC_UNIT_STOP       0x800c
#define PPC_ENABLE(g)       (0x8200 + (g) * 0x20)
#define PPC_FORCED_STOP_PIN 0x80
static const struct {
    uint32_t cls;
    int g;
    uint32_t bit;
} ppc_unit_stop_pins[] = {
    { 0x001, 0, 0x00000200 }, { 0x002, 7, 0x00000008 },

    { 0x004, 0, 0x00002000 }, { 0x008, 0, 0x00008000 }, { 0x010, 0, 0x80000000 },
    { 0x020, 0, 0x08000000 }, { 0x040, 0, 0x00020000 }, { 0x080, 0, 0x00080000 },
    { 0x200, 1, 0x00000080 }, { 0x400, 1, 0x00000008 }, { 0x800, 1, 0x00000800 },
};
static const struct {
    uint32_t commit;
    int g;
    uint32_t bit;
} ppc_unit_edges[] = {
    { 0x004, 0, 0x10 }, { 0x200, 4, 0x10000 }, { 0x008, 3, 0x20 },
    { 0x010, 3, 0x01 }, { 0x080, 4, 0x80 },    { 0x100, 3, 0x40000000 },
};

static void expeed6_ppc_stop_ack(void *opaque)
{
    Expeed6SifState *s = opaque;
    for (int g = 0; g < 8; g++) {
        if (s->req_pending[g]) {
            s->reg[PPC_STATUS(g) / 4] |= s->req_pending[g];
            qemu_set_irq(s->irqppc[expeed6_ppc_line(g)], 1);
            s->req_pending[g] = 0;
        }
    }
}

#define SDES_TRIGGER0     0x5104
#define SDES_TRIGGER1     0x5504
#define SDES_TRIGGER_MASK 0xc000u
#define SIF1_STOP_BITS    (0x400u | 0x40000u)

static void expeed6_sif_stop_ack(void *opaque)
{
    Expeed6SifState *s = opaque;
    if (s->sif1_pending) {
        s->reg[SIF_STATUS1 / 4] |= s->sif1_pending;
        qemu_set_irq(s->irq1, 1);
        s->sif1_pending = 0;
    }
}

static void expeed6_ppc_units_tick(Expeed6SifState *s)
{
    for (size_t i = 0; i < ARRAY_SIZE(ppc_unit_edges); i++) {
        if (s->units_pending & ppc_unit_edges[i].commit) {
            int g = ppc_unit_edges[i].g;
            s->reg[PPC_STATUS(g) / 4] |= ppc_unit_edges[i].bit;
            qemu_set_irq(s->irqppc[expeed6_ppc_line(g)], 1);
        }
    }
    s->units_pending = 0;
    for (int g = 0; g < 8; g++) {
        if (s->req_pending[g]) {
            s->reg[PPC_STATUS(g) / 4] |= s->req_pending[g];
            qemu_set_irq(s->irqppc[expeed6_ppc_line(g)], 1);
            s->req_pending[g] = 0;
        }
    }
}

static bool expeed6_ppc_w1c(int g)
{
    return g == 0 || g == 1 || g == 3 || g == 4 || g == 7;
}

#define PPC_CH1_SIZE  0xa580
#define PPC_CH1_GROUP 3
#define PPC_CH1_BIT   0x80

static void expeed6_ppc_ch1_tick(Expeed6SifState *s)
{
    if (s->ch1_size || s->ch1_final) {
        s->ch1_final = false;
        s->reg[PPC_STATUS(PPC_CH1_GROUP) / 4] |= PPC_CH1_BIT;
        qemu_set_irq(s->irqppc[expeed6_ppc_line(PPC_CH1_GROUP)], 1);
    }
}

#define STATS_RGB_ID(c, i) ((c) * 40 * 64 + (i))
#define STATS_CELL_ID(i)   (3 * 40 * 64 + (i))
#define STATS_IDS          (3 * 40 * 64 + 20 * 32)

static double expeed6_ppc_stats_mean(const Expeed6SifState *s, int id, double k)
{
    const uint32_t a = s->stats_off[id], n = s->stats_off[id + 1] - a;
    if (!n) {
        return 0;
    }
    const float thr = (float)(SENSOR_WHITE_LIN / k);
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (s->stats_px[a + mid] < thr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (k * s->stats_pre[a + id + lo] + (double)(n - lo) * SENSOR_WHITE_LIN) / n;
}

static void expeed6_ppc_stats_apply(Expeed6SifState *s, double k)
{
    static const float gain[3] = { 0.1368f, 0.1907f, 0.2332f }, ofs[3] = { 0.6f, -21.7f, -4.1f };
    if (k == s->stats_k) {
        return;
    }
    for (int c = 0; c < 3; c++) {
        for (int gy = 0; gy < 40; gy++) {
            for (int gx = 0; gx < 60; gx++) {
                const int i = gy * 64 + gx;
                const double v = expeed6_ppc_stats_mean(s, STATS_RGB_ID(c, i), k) * gain[c] + ofs[c];
                s->stats_rgb[c][i] = v <= 0 ? 0 : v >= 65535 ? 65535 : (uint16_t)(v + 0.5);
            }
        }
    }
    for (int gy = 0; gy < 20; gy++) {
        for (int gx = 0; gx < 30; gx++) {
            const int i = gy * 32 + gx;
            const double v = expeed6_ppc_stats_mean(s, STATS_CELL_ID(i), k) * 0.7315 - 36.3;
            s->stats_cells[i] = v <= 0 ? 0 : v >= 65535 ? 65535 : (uint16_t)(v + 0.5);
        }
    }
    s->stats_k = k;
}

static int expeed6_ppc_stats_cmp(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void expeed6_ppc_stats_prepare(Expeed6SifState *s)
{
    const uint32_t rows = 1006, W = MIPU_DPCM6_CODES;
    if (s->stats_ready || !s_scene.px) {
        return;
    }
    float *f = g_new(float, (size_t)rows *W);
    const uint32_t *fe = s->reg + FE_OFF / 4;
    const uint32_t iw = FE_GET(fe, IN_W) ? FE_GET(fe, IN_W) : 2020,
                   ih = FE_GET(fe, IN_H) ? FE_GET(fe, IN_H) : 1342;
    float *in = g_new(float, (size_t)iw *ih), *fr = g_new(float, (size_t)1514 * rows);
    sensor_readout(&s_scene, iw, ih, 0, ih, 1.0f, in);
    fe_resize(in, iw, ih, fr, 1514, rows);
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < W; x++) {
            const float v = fr[(size_t)y * 1514 + (x < 1514 ? x : 1512 + (x & 1))];
            f[(size_t)y * W + x] = v > 1008.0f ? v - 1008.0f : 0.0f;
        }
    }
    g_free(in);
    g_free(fr);
    const uint32_t x0 = 2, y0 = 2, qw = 1510 / 2, qh = 1002 / 2;
    uint32_t fill[STATS_IDS];
    for (int pass = 0; pass < 2; pass++) {
        memset(fill, 0, sizeof(fill));
        for (int gy = 0; gy < 40; gy++) {
            for (int gx = 0; gx < 60; gx++) {
                for (uint32_t qy = gy * qh / 40; qy < (gy + 1) * qh / 40; qy++) {
                    const float *r0 = f + (size_t)(y0 + 2 * qy) * W, *r1 = r0 + W;
                    for (uint32_t qx = gx * qw / 60; qx < (gx + 1) * qw / 60; qx++) {
                        const uint32_t x = x0 + 2 * qx;
                        const float v[3] = { r0[x], (r0[x + 1] + r1[x]) * 0.5f, r1[x + 1] };
                        for (int c = 0; c < 3; c++) {
                            const int id = STATS_RGB_ID(c, gy * 64 + gx);
                            if (pass) {
                                s->stats_px[s->stats_off[id] + fill[id]] = v[c];
                            }
                            fill[id]++;
                        }
                    }
                }
            }
        }
        for (int gy = 0; gy < 20; gy++) {
            for (int gx = 0; gx < 30; gx++) {
                const int id = STATS_CELL_ID(gy * 32 + gx);
                for (uint32_t qy = gy * qh / 20; qy < (gy + 1) * qh / 20; qy++) {
                    const float *r0 = f + (size_t)(y0 + 2 * qy) * W, *r1 = r0 + W;
                    for (uint32_t qx = gx * qw / 30; qx < (gx + 1) * qw / 30; qx++) {
                        if (pass) {
                            s->stats_px[s->stats_off[id] + fill[id]] =
                                (r0[x0 + 2 * qx + 1] + r1[x0 + 2 * qx]) * 0.5f;
                        }
                        fill[id]++;
                    }
                }
            }
        }
        if (!pass) {
            s->stats_off[0] = 0;
            for (int id = 0; id < STATS_IDS; id++) {
                s->stats_off[id + 1] = s->stats_off[id] + fill[id];
            }
            s->stats_px = g_new(float, s->stats_off[STATS_IDS]);
            s->stats_pre = g_new(double, s->stats_off[STATS_IDS] + STATS_IDS);
        }
    }
    for (int id = 0; id < STATS_IDS; id++) {
        const uint32_t a = s->stats_off[id], n = s->stats_off[id + 1] - a;
        qsort(s->stats_px + a, n, sizeof(float), expeed6_ppc_stats_cmp);
        double *pre = s->stats_pre + a + id;
        pre[0] = 0;
        for (uint32_t j = 0; j < n; j++) {
            pre[j + 1] = pre[j] + s->stats_px[a + j];
        }
    }
    g_free(f);
    s->stats_k = 0;
    expeed6_ppc_stats_apply(s, 1.0);
    s->stats_ready = true;
}

static void expeed6_ppc_stats_tick(Expeed6SifState *s)
{
    expeed6_ppc_stats_prepare(s);
    if (!s->stats_ready) {
        return;
    }
    const uint32_t *fe = s->reg + FE_OFF / 4;
    const double dg =
        s_scene.px && FE_GET(fe, DGAIN1) ? (FE_GET(fe, DGAIN1) + FE_GET(fe, DGAIN2)) / 2 / 4096.0 : 1.0;
    expeed6_ppc_stats_apply(s, sensor_exposure_factor() * dg);
    for (int ch = 1; ch <= 4; ch++) {
        uint32_t base = s->reg[(0xa400 + 0x100 * ch + 0x50) / 4];
        uint32_t stride = s->reg[(0xa400 + 0x100 * ch + 0x80) / 4];
        if (!base || !stride || stride > 0x40000) {
            continue;
        }
        for (int k = 0; k < 6; k++) {
            uint32_t addr = base + k * stride;
            if (ch == 1) {
                address_space_write(s->as, addr, MEMTXATTRS_UNSPECIFIED, s->stats_cells, sizeof(s->stats_cells));
            } else {
                address_space_write(s->as, addr, MEMTXATTRS_UNSPECIFIED, s->stats_rgb[ch - 2],
                                    sizeof(s->stats_rgb[0]));
            }
        }
    }
}

#define SIF_TG0_HTOTAL 0x3000
#define SIF_TG0_VTOTAL 0x3004
static struct {
    AddressSpace *as;
    Expeed6SifState *s;
} sif_of_as[2];

static int64_t expeed6_sif_tg_period(const Expeed6SifState *s)
{
    const uint64_t h = (s->reg[SIF_TG0_HTOTAL / 4] & 0xffff) + 1;
    const uint64_t v = (uint64_t)s->reg[SIF_TG0_VTOTAL / 4] + 1;
    const int64_t ns = h * v * 1000 / 72;
    return ns >= 1000000000 / 240 && ns <= 1000000000 ? ns : 0;
}

int64_t expeed6_fe_frame_ns(AddressSpace *as, int64_t fallback_ns)
{
    for (int i = 0; i < ARRAY_SIZE(sif_of_as); i++) {
        if (sif_of_as[i].as == as) {
            const int64_t ns = expeed6_sif_tg_period(sif_of_as[i].s);
            return ns ? ns : fallback_ns;
        }
    }
    return fallback_ns;
}

static int64_t expeed6_sif_fe_period(Expeed6SifState *s)
{
    return expeed6_fe_frame_ns(s->as, SIF_FRAME_MS * SCALE_MS);
}

#define SIF_ST1_MODE 0x10
static void expeed6_sif_mode_tick(void *opaque)
{
    Expeed6SifState *s = opaque;
    s->reg[SIF_STATUS1 / 4] |= SIF_ST1_MODE;
    qemu_set_irq(s->irq1, 1);
    timer_mod(s->mode_tick, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + SIF_FRAME_MS);
}

static void expeed6_sif_frame_tick(void *opaque)
{
    Expeed6SifState *s = opaque;
    if (s->stats_model) {
        expeed6_ppc_stats_tick(s);
    }
    expeed6_ppc_ch1_tick(s);
    expeed6_ppc_units_tick(s);
    bool held = s->fe_stopped || qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->fe_resume_ns;
    s->reg[SIF_STATUS0 / 4] |= SIF_FRAME_MASK;
    qemu_set_irq(s->irq0, 1);
    s->reg[SIF_STATUS1 / 4] |= SIF_FRAME_MASK1 & ~SIF_ST1_MODE;
    qemu_set_irq(s->irq1, 1);
    s->reg[SIF_STATUS6 / 4] |= SIF_FRAME_MASK6;
    qemu_set_irq(s->irq6, 1);
    for (int i = 0; i < PPC_FRAME_EDGES; i++) {
        if (held || s->units_stopped) {
            continue;
        }
        if (ppc_frame_edges[i].delay_us) {
            timer_mod(s->ppc_timer[i], qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + ppc_frame_edges[i].delay_us);
        } else {
            expeed6_ppc_assert(s, i);
        }
    }
    timer_mod(s->frame, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + expeed6_sif_fe_period(s));
}

static uint64_t expeed6_sif_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6SifState *s = opaque;
    uint64_t v = (off / 4 < SIF_SIZE / 4) ? s->reg[off / 4] : 0;
    return v;
}

#define FE_RAW_RUN (FE_OFF + FE_RAW_RUN_REG)

static uint64_t expeed6_fe_raw_prog(const uint32_t *fe)
{
    const uint32_t regs[8] = { FE_GET(fe, RAWA_W0),    FE_GET(fe, RAWA_SPLIT) ? FE_GET(fe, RAWA_W1) : 0,
                               FE_GET(fe, RAWA_Y0),    FE_GET(fe, RAWA_YEND),
                               FE_GET(fe, RAWA_DEPTH), FE_GET(fe, OUT0_ADDR0),
                               FE_GET(fe, OUT0_PITCH), FE_GET(fe, OUT1_ADDR0) };
    return fe_hash(0xcbf29ce484222325ull, regs, sizeof(regs));
}

static void expeed6_fe_raw_out(Expeed6SifState *s, bool force, double exposure, float dgain)
{
    const uint32_t *fe = s->reg + FE_OFF / 4;
    const uint32_t w0 = FE_GET(fe, RAWA_W0), w1 = FE_GET(fe, RAWA_SPLIT) ? FE_GET(fe, RAWA_W1) : 0;
    const uint32_t y0w = FE_GET(fe, RAWA_Y0), yend = FE_GET(fe, RAWA_YEND);
    const uint32_t rows = yend > y0w ? yend - y0w : 0;
    if (!s_scene.px || !rows || !w0) {
        return;
    }
    uint16_t table[FE_COMPAND_N];
    for (int i = 0; i < FE_COMPAND_N; i++) {
        table[i] = FE_TABLE(fe, COMPAND, i);
    }
    FeOutJob j = { .in_w = s_scene.w,
                   .in_h = s_scene.h,
                   .win_y0 = y0w,
                   .win_h = rows,
                   .out_w = w0 + w1,
                   .out_h = rows,
                   .depth = FE_GET(fe, RAWA_DEPTH),
                   .dpcm = FE_GET(fe, RAWA_DPCM),
                   .pitch = ((w0 + w1) * (FE_GET(fe, RAWA_DEPTH) == 1 ? 14 : 12) + 7) / 8,
                   .compand = table,
                   .exposure =
                       roundf((float)(exposure > 0 ? exposure : sensor_exposure_factor()) * 200.0f) / 200.0f };
    fe_dgains(fe, j.dgain);
    for (int c = 0; dgain > 0 && c < 4; c++) {
        j.dgain[c] = dgain;
    }
    const uint64_t prog = expeed6_fe_raw_prog(fe);
    const uint64_t key = fe_hash(fe_hash(prog, &j.exposure, sizeof(j.exposure)), j.dgain, sizeof(j.dgain));
    if (!force && key == s->fe_a_key) {
        return;
    }
    s->fe_a_key = key;
    s->fe_a_prog = prog;
    const size_t n = j.dpcm ? 0 : fe_out_bytes(&j);
    if (!n) {
        fprintf(stderr,
                "expeed6.sif%s: raw output A (%ux%u (+%u) from %ux%u, depth %d, DPCM %d) cannot be made from "
                "the scene; nothing written (EXPEED_SCENE)\n",
                cluster_tag(), w0, rows, w1, j.in_w, j.in_h, j.depth, j.dpcm);
        return;
    }
    uint8_t *made = g_malloc(n);
    fe_render(&s_scene, &j, made);
    const uint8_t *src = made;
    const size_t pitch = j.pitch;
    const uint32_t bits = j.depth == 1 ? 14 : 12;
    AddressSpace *as = s->idmac ? expeed6_idmac_as(s->idmac) : s->as;
    const uint32_t width[2] = { w0, w1 }, skip[2] = { 0, w0 * bits / 8 };
    const uint32_t dsts[2][4] = { { FE_GET(fe, OUT0_ADDR0), FE_GET(fe, OUT0_ADDR1), FE_GET(fe, OUT0_ADDR2),
                                    FE_GET(fe, OUT0_ADDR3) },
                                  { FE_GET(fe, OUT1_ADDR0) } };
    const uint32_t dpitches[2] = { FE_GET(fe, OUT0_PITCH), FE_GET(fe, OUT1_PITCH) };
    for (int k = 0; k < 2 && width[k]; k++) {
        const uint32_t dpitch = dpitches[k];
        const uint32_t nb = (width[k] * bits + 7) / 8;
        for (int r = 0; r < 4; r++) {
            const uint32_t dst = dsts[k][r];
            if (r && (!dst || dst == dsts[k][0])) {
                continue;
            }
            if (!dst || dpitch < nb || skip[k] + nb > pitch) {
                fprintf(stderr, "expeed6.sif%s: raw output %d (%08x pitch %u, %u bytes a row) not written\n",
                        cluster_tag(), k, dst, dpitch, nb);
                continue;
            }
            for (uint32_t y = 0; y < rows; y++) {
                address_space_write(as, dst + (uint64_t)y * dpitch, MEMTXATTRS_UNSPECIFIED,
                                    src + y * pitch + skip[k], nb);
            }
        }
    }
    g_free(made);
}

static void expeed6_fe_raw_refresh(void *opaque, uint64_t in_addr)
{
    Expeed6SifState *s = opaque;
    const uint32_t *fe = s->reg + FE_OFF / 4;
    const uint32_t a[5] = { FE_GET(fe, OUT0_ADDR0), FE_GET(fe, OUT0_ADDR1), FE_GET(fe, OUT0_ADDR2),
                            FE_GET(fe, OUT0_ADDR3), FE_GET(fe, OUT1_ADDR0) };
    bool reads_a = false;
    int ring = 0;
    for (int k = 0; k < 5; k++) {
        reads_a |= a[k] && a[k] == in_addr;
    }
    for (int k = 1; k < 4; k++) {
        ring += a[k] && a[k] != a[0];
    }
    if (s_scene.px && reads_a && ring && (s->reg[FE_RAW_RUN / 4] & 1)) {
        expeed6_fe_raw_out(s, false, 0, 0);
    }
}

static void expeed6_fe_still_done(void *opaque, double f, float dg)
{
    Expeed6SifState *s = opaque;
    if (s_scene.px && s->fe_a_still) {
        s->fe_a_still = false;
        expeed6_fe_raw_out(s, true, f, dg);
    }
}

static void expeed6_sif_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6SifState *s = opaque;
    for (int g = 0; g < 8; g++) {
        if (expeed6_ppc_w1c(g) && off == PPC_STATUS(g)) {
            s->reg[off / 4] &= ~(uint32_t)val;
            int line = expeed6_ppc_line(g), pending = 0;
            for (int h = 0; h < 8; h++) {
                if (expeed6_ppc_line(h) == line && expeed6_ppc_w1c(h) && s->reg[PPC_STATUS(h) / 4]) {
                    pending = 1;
                }
            }
            if (!pending) {
                qemu_set_irq(s->irqppc[line], 0);
            }
            return;
        }
    }
    if (off == SIF_STATUS0 || off == SIF_STATUS1 || off == SIF_STATUS6) {
        s->reg[off / 4] &= ~(uint32_t)val;
        if (!s->reg[off / 4]) {
            qemu_set_irq(off == SIF_STATUS0 ? s->irq0 : off == SIF_STATUS1 ? s->irq1 : s->irq6, 0);
        }
    } else if (off / 4 < SIF_SIZE / 4) {
        if ((off == SIF_FRAME_STOP && ((val >> 14) & 3)) || (off == SIF_RUN_FLAG && val)) {
            bool stop = off == SIF_FRAME_STOP && ((val >> 14) & 3) == 2;
            if (stop != s->fe_stopped) {
                if (!stop) {
                    s->fe_resume_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SIF_FRAME_MS * SCALE_MS;
                }
            }
            s->fe_stopped = stop;
        }
        if (off == PPC_STOP) {
            for (size_t i = 0; i < ARRAY_SIZE(ppc_stop_pins); i++) {
                if (val == ppc_stop_pins[i].code) {
                    s->req_pending[ppc_stop_pins[i].g] |= ppc_stop_pins[i].bit;
                    timer_mod(s->stop_ack, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + PPC_STOP_ACK_US);
                }
            }
        }
        if ((off == PPC_UNIT_STOP && val) || (off == PPC_COMMIT && val)) {
            s->units_stopped = off == PPC_UNIT_STOP;
        }
        if (off == PPC_UNIT_STOP && val) {
            uint32_t known = 0;
            for (size_t i = 0; i < ARRAY_SIZE(ppc_unit_stop_pins); i++) {
                if (val & ppc_unit_stop_pins[i].cls) {
                    s->req_pending[ppc_unit_stop_pins[i].g] |= ppc_unit_stop_pins[i].bit;
                    known |= ppc_unit_stop_pins[i].cls;
                }
            }
            bool forced = s->reg[PPC_ENABLE(0) / 4] & PPC_FORCED_STOP_PIN;
            if (forced) {
                s->req_pending[0] |= PPC_FORCED_STOP_PIN;
            }
            if (known || forced) {
                timer_mod(s->stop_ack, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + PPC_STOP_ACK_US);
            }
            if (val & ~known) {
                fprintf(stderr,
                        "expeed6.ppc: unit stop %#" PRIx64 " at +0x0c: no acknowledge pin known for %#" PRIx64
                        "\n",
                        val, val & ~known);
            }
        }
        if ((off == SDES_TRIGGER0 || off == SDES_TRIGGER1) && (s->reg[off / 4] & SDES_TRIGGER_MASK) &&
            !(val & SDES_TRIGGER_MASK)) {
            s->sif1_pending |= SIF_FRAME_MASK1 & SIF1_STOP_BITS;
            timer_mod(s->sif_stop_timer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + PPC_STOP_ACK_US);
        }
        bool raw_start = off == FE_RAW_RUN && (val & 1) && !(s->reg[off / 4] & 1);
        s->reg[off / 4] = (uint32_t)val;
        if (off == FE_RAW_RUN && (val & 1) && !raw_start && s_scene.px &&
            expeed6_fe_raw_prog(s->reg + FE_OFF / 4) != s->fe_a_prog) {
            raw_start = true;
        }
        if (raw_start && s_scene.px) {
            expeed6_fe_raw_out(s, true, 0, 0);
            s->fe_a_still = true;
        }
        if (off == PPC_COMMIT) {
            s->units_pending |= (uint32_t)val;
        }
        if (off == PPC_CH1_SIZE) {
            if (!val && s->ch1_size) {
                s->ch1_final = true;
            }
            s->ch1_size = (uint32_t)val;
        }
    }
}

static const MemoryRegionOps expeed6_sif_ops = {
    .read = expeed6_sif_read,
    .write = expeed6_sif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void sif_realize(DeviceState *dev, Error **errp)
{
    Expeed6SifState *s = EXPEED6_SIF(dev);

    for (int i = 0; i < PPC_FRAME_EDGES; i++) {
        s->edge[i] = (Expeed6PpcEdge){ .s = s, .i = i };
        s->ppc_timer[i] = timer_new_us(QEMU_CLOCK_VIRTUAL, expeed6_ppc_delayed, &s->edge[i]);
    }
    s->stop_ack = timer_new_us(QEMU_CLOCK_VIRTUAL, expeed6_ppc_stop_ack, s);
    s->frame = timer_new_ns(QEMU_CLOCK_VIRTUAL, expeed6_sif_frame_tick, s);
    s->mode_tick = timer_new_ms(QEMU_CLOCK_VIRTUAL, expeed6_sif_mode_tick, s);
    s->sif_stop_timer = timer_new_us(QEMU_CLOCK_VIRTUAL, expeed6_sif_stop_ack, s);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq0);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq1);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq6);
    for (int i = 0; i < 5; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irqppc[i]);
    }
    memory_region_init_io(&s->mmio, OBJECT(dev), &expeed6_sif_ops, s, "expeed6.sif", SIF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void sif_reset_enter(Object *obj, ResetType type)
{
    Expeed6SifState *s = EXPEED6_SIF(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->ch1_size = 0;
    s->ch1_final = false;
    s->fe_a_key = 0;
    s->fe_a_prog = 0;
    s->fe_a_still = false;
    s->units_pending = 0;
    memset(s->req_pending, 0, sizeof(s->req_pending));
    s->sif1_pending = 0;
    s->units_stopped = s->fe_stopped = false;
    s->fe_resume_ns = 0;
    s->stats_ready = false;
    for (int i = 0; i < PPC_FRAME_EDGES; i++) {
        timer_del(s->ppc_timer[i]);
    }
    timer_del(s->stop_ack);
    timer_del(s->sif_stop_timer);
    timer_del(s->frame);
    timer_del(s->mode_tick);
    int64_t first_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + SIF_FRAME_MS;
    timer_mod(s->frame, SCALE_MS * first_ms);
    timer_mod(s->mode_tick, first_ms);
}

static void sif_reset_hold(Object *obj, ResetType type)
{
    Expeed6SifState *s = EXPEED6_SIF(obj);

    qemu_set_irq(s->irq0, 0);
    qemu_set_irq(s->irq1, 0);
    qemu_set_irq(s->irq6, 0);
    for (int i = 0; i < 5; i++) {
        qemu_set_irq(s->irqppc[i], 0);
    }
}

static const VMStateDescription vmstate_sif = {
    .name = TYPE_EXPEED6_SIF,
    .version_id = 4,
    .minimum_version_id = 4,
    .fields =
        (const VMStateField[]){
            VMSTATE_UINT32_ARRAY(reg, Expeed6SifState, SIF_SIZE / 4), VMSTATE_TIMER_PTR(frame, Expeed6SifState),

            VMSTATE_UINT32(ch1_size, Expeed6SifState), VMSTATE_BOOL(ch1_final, Expeed6SifState),
            VMSTATE_UINT64(fe_a_key, Expeed6SifState), VMSTATE_UINT64(fe_a_prog, Expeed6SifState),
            VMSTATE_BOOL(fe_a_still, Expeed6SifState),
            VMSTATE_TIMER_PTR_ARRAY(ppc_timer, Expeed6SifState, PPC_FRAME_EDGES),
            VMSTATE_TIMER_PTR(mode_tick, Expeed6SifState), VMSTATE_UINT32(units_pending, Expeed6SifState),
            VMSTATE_UINT32_ARRAY(req_pending, Expeed6SifState, 8), VMSTATE_TIMER_PTR(stop_ack, Expeed6SifState),
            VMSTATE_UINT32(sif1_pending, Expeed6SifState), VMSTATE_TIMER_PTR(sif_stop_timer, Expeed6SifState),
            VMSTATE_BOOL(units_stopped, Expeed6SifState), VMSTATE_BOOL(fe_stopped, Expeed6SifState),
            VMSTATE_INT64(fe_resume_ns, Expeed6SifState), VMSTATE_END_OF_LIST() }
};

static void sif_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = sif_realize;
    dc->vmsd = &vmstate_sif;
    rc->phases.enter = sif_reset_enter;
    rc->phases.hold = sif_reset_hold;
}

static const TypeInfo sif_info = {
    .name = TYPE_EXPEED6_SIF,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6SifState),
    .class_init = sif_class_init,
};

static void sif_register_types(void)
{
    type_register_static(&sif_info);
}

type_init(sif_register_types)

    void expeed6_install_sif(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master,
                             Expeed6DmaState *dma, Expeed6MipuState *mipu)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_SIF);
    Expeed6SifState *sif = EXPEED6_SIF(dev);
    sif->as = as;
    sif->idmac = dma;
    sif_of_as[master ? 0 : 1].as = as;
    sif_of_as[master ? 0 : 1].s = sif;
    expeed6_mipu_connect_fe(mipu, sif->reg + FE_OFF / 4, expeed6_fe_raw_refresh, sif);
    if (master && !(getenv("Z6II_SLAVE") && *getenv("Z6II_SLAVE"))) {
        s_sensor.still_done = expeed6_fe_still_done;
        s_sensor.still_opaque = sif;
    }
    {
        const char *sc = getenv("EXPEED_SCENE");
        if (sc && *sc) {
            sif->stats_model = true;
        }
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, SIF_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, SIF_SPI0));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1, qdev_get_gpio_in(gic, SIF_SPI1));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 2, qdev_get_gpio_in(gic, SIF_SPI6));
    for (int i = 0; i < 5; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 3 + i, qdev_get_gpio_in(gic, PPC_SPI0 + i));
    }
}
