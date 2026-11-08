/*
 * MIPU (0x20380000/0x203c0000)
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "mipu_model.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIPU_MAX_DIM 16384

void mipu_job_from_regs(MipuJob *j, const uint32_t *regs)
{
    j->in_addr = MIPU_GET(regs, RAW_ADDR0);
    j->in_pitch = MIPU_GET(regs, RAW_PITCH);
    const uint32_t fmt = MIPU_GET(regs, RAW_FMT);
    j->in_bits = fmt == 1 ? 14 : fmt == 0 ? MIPU_IN_DPCM6 : 12;
    j->y0 = MIPU_GET(regs, CROP_Y0);
    j->h = MIPU_GET(regs, CROP_H);
    j->w = MIPU_GET(regs, CROP_W);
    j->x0 = MIPU_GET(regs, CROP_X0);
    j->y_addr = MIPU_GET(regs, OUT_Y_ADDR);
    j->c_addr = MIPU_GET(regs, OUT_C_ADDR);
    j->out_pitch = MIPU_GET(regs, OUT_PITCH);
    j->frame_w = MIPU_GET(regs, FRAME_W);
    j->frame_h = MIPU_GET(regs, FRAME_H);
    j->rawout_addr = MIPU_GET(regs, RAWOUT_CTL) & 1 ? MIPU_GET(regs, RAWOUT_ADDR) : 0;
    j->rawout_pitch = MIPU_GET(regs, RAWOUT_PITCH);
    j->out_stream = (j->y_addr >= MIPU_STREAM_LO && j->y_addr < MIPU_STREAM_HI) ||
                    (j->c_addr >= MIPU_STREAM_LO && j->c_addr < MIPU_STREAM_HI);

    /* the RGB primary transform matrix CMAT_0..8, row-major */
    const int16_t m[9] = { MIPU_GETS(regs, CMAT_0), MIPU_GETS(regs, CMAT_1), MIPU_GETS(regs, CMAT_2),
                           MIPU_GETS(regs, CMAT_3), MIPU_GETS(regs, CMAT_4), MIPU_GETS(regs, CMAT_5),
                           MIPU_GETS(regs, CMAT_6), MIPU_GETS(regs, CMAT_7), MIPU_GETS(regs, CMAT_8) };
    uint32_t any = 0;
    for (int i = 0; i < 9; i++) {
        j->matrix[i] = m[i];
        any |= m[i];
    }
    j->matrix_src = any ? MIPU_SRC_REGS : MIPU_SRC_UNSET;
    for (int r = 0; any && r < 3; r++) {
        int sum = j->matrix[3 * r] + j->matrix[3 * r + 1] + j->matrix[3 * r + 2];
        if (sum < 512 || sum > 2048) {
            j->matrix_src = MIPU_SRC_IMPLAUSIBLE;
        }
    }

    /* the tone path: TONE_LUT_R/G/B and Y2Y_GAIN_TABLE_A/B and Y2C_GAIN_TABLE_A/B */
    static const unsigned tone_reg[3] = { MIPU_TONE_LUT_R_REG, MIPU_TONE_LUT_G_REG, MIPU_TONE_LUT_B_REG };
    int set = 0, ok = 1;
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < MIPU_TONE_N; i++) {
            j->tone[c][i] = mipu_regs_table(regs, tone_reg[c], i);
            set |= j->tone[c][i];
            ok &= i == 0 || j->tone[c][i] >= j->tone[c][i - 1];
        }
        ok &= j->tone[c][MIPU_TONE_N - 1] > j->tone[c][0];
    }
    static const unsigned y2y_gain_reg[2] = { MIPU_Y2Y_GAIN_TABLE_A_REG, MIPU_Y2Y_GAIN_TABLE_B_REG };
    static const unsigned y2c_gain_reg[2] = { MIPU_Y2C_GAIN_TABLE_A_REG, MIPU_Y2C_GAIN_TABLE_B_REG };
    int yset = 0;
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < MIPU_Y2X_GAIN_N; i++) {
            j->y2y_gain[t][i] = mipu_regs_table(regs, y2y_gain_reg[t], i);
            j->y2c_gain[t][i] = mipu_regs_table(regs, y2c_gain_reg[t], i);
            yset |= j->y2y_gain[t][i];
        }
    }
    j->y2x_gain_weight[0] = MIPU_GET(regs, Y2X_GAIN_TABLE_WEIGHT_A);
    j->y2x_gain_weight[1] = MIPU_GET(regs, Y2X_GAIN_TABLE_WEIGHT_B);
    const uint32_t wsum = j->y2x_gain_weight[0] + j->y2x_gain_weight[1];
    j->tone_src = !set                                     ? MIPU_SRC_UNSET
                  : ok && yset && wsum >= 1 && wsum <= 512 ? MIPU_SRC_REGS
                                                           : MIPU_SRC_IMPLAUSIBLE;
    /* the RGB to YCC matrix, row-major */
    const int ycc[9] = { MIPU_GETS(regs, RGB2YCC_MAT_0), MIPU_GETS(regs, RGB2YCC_MAT_1),
                         MIPU_GETS(regs, RGB2YCC_MAT_2), MIPU_GETS(regs, RGB2YCC_MAT_3),
                         MIPU_GETS(regs, RGB2YCC_MAT_4), MIPU_GETS(regs, RGB2YCC_MAT_5),
                         MIPU_GETS(regs, RGB2YCC_MAT_6), MIPU_GETS(regs, RGB2YCC_MAT_7),
                         MIPU_GETS(regs, RGB2YCC_MAT_8) };
    for (int i = 0; i < 9; i++) {
        j->ycc[i] = ycc[i] / 512.0f;
    }

    /* The RGGB_* per-site gains and offsets:
     * gains R/Gr/Gb/B in 1/256 with the base gain included,
     * apply the 13-bit signed "in" offsets before and the "out" ones after. */
    j->rggb_gain[0] = MIPU_GET(regs, RGGB_GAIN_R);
    j->rggb_gain[1] = MIPU_GET(regs, RGGB_GAIN_GR);
    j->rggb_gain[2] = MIPU_GET(regs, RGGB_GAIN_GB);
    j->rggb_gain[3] = MIPU_GET(regs, RGGB_GAIN_B);
    j->rggb_in_offset[0] = MIPU_GETS(regs, RGGB_IN_OFFSET_R);
    j->rggb_in_offset[1] = MIPU_GETS(regs, RGGB_IN_OFFSET_GR);
    j->rggb_in_offset[2] = MIPU_GETS(regs, RGGB_IN_OFFSET_GB);
    j->rggb_in_offset[3] = MIPU_GETS(regs, RGGB_IN_OFFSET_B);
    j->rggb_out_offset[0] = MIPU_GET(regs, RGGB_OUT_OFFSET_R);
    j->rggb_out_offset[1] = MIPU_GET(regs, RGGB_OUT_OFFSET_GR);
    j->rggb_out_offset[2] = MIPU_GET(regs, RGGB_OUT_OFFSET_GB);
    j->rggb_out_offset[3] = MIPU_GET(regs, RGGB_OUT_OFFSET_B);
    j->tone_offset = MIPU_GET(regs, TONE_OFFSET);
    /* flag when a gain is outside 1/16 .. 16 */
    int wset = 0, wok = 1;
    for (int c = 0; c < 4; c++) {
        wset |= j->rggb_gain[c];
        wok &= j->rggb_gain[c] >= 16 && j->rggb_gain[c] <= 4096;
    }
    j->rggb_src = !wset ? MIPU_SRC_UNSET : wok ? MIPU_SRC_REGS : MIPU_SRC_IMPLAUSIBLE;

    /* live view decompanding LUT */
    int dset = 0, dmono = 1;
    for (int i = 0; i < MIPU_DECOMPAND_N; i++) {
        j->decompand[i] = MIPU_TABLE(regs, LV_DECOMPAND, i);
        dset |= j->decompand[i];
        dmono &= i == 0 || j->decompand[i] >= j->decompand[i - 1];
    }
    j->decompand_src = !dset                                                           ? MIPU_SRC_UNSET
                       : dmono && j->decompand[MIPU_DECOMPAND_N - 1] > j->decompand[0] ? MIPU_SRC_REGS
                                                                                       : MIPU_SRC_IMPLAUSIBLE;
}

static inline float decompand_at(const uint16_t *t, float v)
{
    float p = v / 16.0f;
    if (p <= 0) {
        return 0;
    }
    if (p >= MIPU_DECOMPAND_N) {
        return t[MIPU_DECOMPAND_N - 1];
    }
    int e = (int)p;
    float lo = e ? t[e - 1] : 0;
    return lo + (t[e] - lo) * (p - e);
}

static void tone_knots(uint32_t *x)
{
    static const uint8_t seg_n[] = { 8, 4, 4, 12, 8, 24, 16, 16, 112 };
    int i = 0;
    x[0] = 0;
    for (int s = 0; s < 9; s++) {
        for (int k = 0; k < seg_n[s]; k++, i++) {
            x[i + 1] = x[i] + (1u << s);
        }
    }
}

static inline float y2x_gain_at(const uint16_t *t, float y)
{
    float p = y <= 0 ? 0 : y >= 1 ? 256 : y * 256;
    int e = (int)p;
    if (e > 255) {
        e = 255;
    }
    return (t[e] + (t[e + 1] - t[e]) * (p - e)) / 4096.0f;
}

static inline float y2x_gain_blend(const uint16_t (*t)[MIPU_Y2X_GAIN_N], const uint16_t *w, float y)
{
    return (w[0] * y2x_gain_at(t[0], y) + w[1] * y2x_gain_at(t[1], y)) / 256.0f;
}

static inline uint32_t unpack(const uint8_t *row, uint32_t x, int bits)
{
    uint64_t bit = (uint64_t)x * bits;
    const uint8_t *p = row + bit / 8;
    uint32_t v = p[0] | p[1] << 8 | p[2] << 16;
    return (v >> (bit % 8)) & ((1u << bits) - 1);
}

static float dpcm6_recon(int mag)
{
    static float t[33];
    if (!t[1]) {
        float e = 0;
        for (int c = 1; c <= 24; c++) {
            float w = c <= 2 ? 8 : c <= 5 ? 16 : c <= 13 ? 32 : 64;
            t[c] = e + w / 2;
            e += w;
        }
        for (int c = 25; c <= 32; c++) {
            t[c] = t[24];
        }
    }
    return t[mag];
}

void mipu_dpcm6_decode(const uint8_t *buf, uint32_t rows, float *out)
{
    const uint32_t W = MIPU_DPCM6_CODES;
    uint8_t *codes = malloc(W);
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *b = buf + (size_t)r * MIPU_DPCM6_ROW;
        for (uint32_t q = 0; q < W / 4; q++) {
            uint32_t v = b[3 * q] | b[3 * q + 1] << 8 | (uint32_t)b[3 * q + 2] << 16;
            for (int k = 0; k < 4; k++) {
                codes[4 * q + k] = (v >> (6 * k)) & 63;
            }
        }
        float *o = out + (size_t)r * W;
        const int g_odd = (r & 1) == 0;
        for (uint32_t x = 0; x < W; x++) {
            if ((int)(x & 1) == g_odd) {
                continue;
            }
            int c = codes[x];
            if (r < 2) {
                o[x] = c * 64.0f + 32.0f;
            } else {
                int sv = c >= 32 ? c - 64 : c;
                float d = sv < 0 ? -dpcm6_recon(-sv) : sv ? dpcm6_recon(sv) : 0;
                o[x] = out[(size_t)(r - 2) * W + x] + d;
            }
        }
        for (uint32_t x = 0; x < W; x++) {
            if ((int)(x & 1) != g_odd) {
                continue;
            }
            int c = codes[x];
            if (r == 0) {
                o[x] = c * 64.0f + 32.0f;
            } else {
                const float *u = out + (size_t)(r - 1) * W;
                const uint32_t xl = x ? x - 1 : x + 1, xr = x + 1 < W ? x + 1 : x - 1;
                const float ga = (r & 1) ? MIPU_DPCM6_GA_ODD : MIPU_DPCM6_GA_EVEN;
                const float gb = (r & 1) ? MIPU_DPCM6_GB_ODD : MIPU_DPCM6_GB_EVEN;
                int sv = c >= 32 ? c - 64 : c;
                float d = sv < 0 ? -dpcm6_recon(-sv) : sv ? dpcm6_recon(sv) : 0;
                o[x] = ga * (u[xl] + u[xr]) / 2 + gb * (o[xl] + o[xr]) / 2 + d;
            }
        }
    }
    free(codes);
}

static int dpcm6_quantise(float r, float *q)
{
    const float mag = fabsf(r);
    int k = 0;
    float best = mag;
    for (int c = 1; c <= 24; c++) {
        const float d = fabsf(mag - dpcm6_recon(c));
        if (d < best) {
            best = d;
            k = c;
        }
    }
    *q = k ? (r < 0 ? -dpcm6_recon(k) : dpcm6_recon(k)) : 0;
    return r < 0 ? (64 - k) & 63 : k;
}

void mipu_dpcm6_encode(const float *img, uint32_t rows, uint32_t w, uint8_t *out)
{
    const uint32_t W = MIPU_DPCM6_CODES;
    float *rec = malloc((size_t)rows * W * sizeof(float));
    uint8_t *codes = malloc(W);
    for (uint32_t r = 0; r < rows; r++) {
        const float *src = img + (size_t)r * w;
        float *o = rec + (size_t)r * W;
        const int g_odd = (r & 1) == 0;
#define SRC(x) ((x) < w ? src[x] : src[w - 2 + (((x) - w) & 1)])
        for (uint32_t x = 0; x < W; x++) {
            if ((int)(x & 1) == g_odd) {
                continue;
            }
            if (r < 2) {
                const float v = floorf(SRC(x) / 64.0f);
                codes[x] = v < 0 ? 0 : v > 63 ? 63 : (uint8_t)v;
                o[x] = codes[x] * 64.0f + 32.0f;
            } else {
                float q;
                const float up2 = rec[(size_t)(r - 2) * W + x];
                codes[x] = dpcm6_quantise(SRC(x) - up2, &q);
                o[x] = up2 + q;
            }
        }
        for (uint32_t x = 0; x < W; x++) {
            if ((int)(x & 1) != g_odd) {
                continue;
            }
            if (r == 0) {
                const float v = floorf(SRC(x) / 64.0f);
                codes[x] = v < 0 ? 0 : v > 63 ? 63 : (uint8_t)v;
                o[x] = codes[x] * 64.0f + 32.0f;
            } else {
                const float *u = rec + (size_t)(r - 1) * W;
                const uint32_t xl = x ? x - 1 : x + 1, xr = x + 1 < W ? x + 1 : x - 1;
                const float ga = (r & 1) ? MIPU_DPCM6_GA_ODD : MIPU_DPCM6_GA_EVEN;
                const float gb = (r & 1) ? MIPU_DPCM6_GB_ODD : MIPU_DPCM6_GB_EVEN;
                const float pred = ga * (u[xl] + u[xr]) / 2 + gb * (o[xl] + o[xr]) / 2;
                float q;
                codes[x] = dpcm6_quantise(SRC(x) - pred, &q);
                o[x] = pred + q;
            }
        }
#undef SRC
        uint8_t *b = out + (size_t)r * MIPU_DPCM6_ROW;
        for (uint32_t i = 0; i < W / 4; i++) {
            const uint32_t v =
                codes[4 * i] | codes[4 * i + 1] << 6 | codes[4 * i + 2] << 12 | (uint32_t)codes[4 * i + 3] << 18;
            b[3 * i] = v & 0xff;
            b[3 * i + 1] = (v >> 8) & 0xff;
            b[3 * i + 2] = v >> 16;
        }
    }
    free(codes);
    free(rec);
}

static inline uint8_t clamp8(float v)
{
    return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f);
}

int mipu_model_develop(const MipuJob *j, MipuMem mem)
{
    const int bits = j->in_bits;
    const int dpcm = bits == MIPU_IN_DPCM6;
    const float widen = bits == 14 ? 1.0f : 4.0f;
    const uint32_t qw = j->w / 2, qh = j->h / 2;
    const uint64_t need = dpcm ? 0 : ((uint64_t)(j->x0 + j->w) * bits + 7) / 8;

    if (j->out_stream) {
        return 1;
    }
    if (!j->in_addr || !j->y_addr || !j->c_addr || j->w < 2 || j->h < 2 || j->w > MIPU_MAX_DIM ||
        j->h > MIPU_MAX_DIM || j->out_pitch < j->w || j->in_pitch < need || (j->x0 | j->y0) & 1 ||
        (dpcm && j->x0 + j->w > MIPU_DPCM6_CODES)) {
        return -1;
    }
    if (j->tone_src == MIPU_SRC_UNSET || j->matrix_src == MIPU_SRC_UNSET || j->rggb_src == MIPU_SRC_UNSET ||
        (dpcm && j->decompand_src == MIPU_SRC_UNSET)) {
        return 2;
    }

    float *frame = NULL;
    if (dpcm) {
        const uint32_t rows = j->y0 + j->h;
        uint8_t *buf = malloc((size_t)rows * MIPU_DPCM6_ROW);
        mem.read(mem.opaque, j->in_addr, buf, (size_t)rows * MIPU_DPCM6_ROW);
        frame = malloc((size_t)rows * MIPU_DPCM6_CODES * sizeof(float));
        mipu_dpcm6_decode(buf, rows, frame);
        free(buf);
        for (size_t i = 0; i < (size_t)rows * MIPU_DPCM6_CODES; i++) {
            frame[i] = decompand_at(j->decompand, frame[i]);
        }
    }
    const uint32_t rw = j->x0 + j->w;
    float *f0 = malloc(rw * sizeof(float)), *f1 = malloc(rw * sizeof(float));
    uint8_t *pk = dpcm ? NULL : calloc(1, j->in_pitch + 8);
#define LOAD_ROW(dst, row)                                                                                        \
    do {                                                                                                          \
        if (dpcm) {                                                                                               \
            memcpy(dst, frame + (size_t)(row) * MIPU_DPCM6_CODES, rw * sizeof(float));                            \
        } else {                                                                                                  \
            mem.read(mem.opaque, j->in_addr + (uint64_t)(row) * j->in_pitch, pk, j->in_pitch);                    \
            for (uint32_t xx = 0; xx < rw; xx++) {                                                                \
                (dst)[xx] = unpack(pk, xx, bits) * widen;                                                         \
            }                                                                                                     \
        }                                                                                                         \
    } while (0)
    uint8_t *yrow = malloc(j->w), *crow = malloc(j->w);

    float wg[4], wo[4], wo2[4];
    for (int c = 0; c < 4; c++) {
        wg[c] = j->rggb_gain[c] / 256.0f;
        wo[c] = j->rggb_in_offset[c];
        wo2[c] = j->rggb_out_offset[c];
    }
#define SITE(v, c) fmaxf(((v) + wo[c]) * wg[c] + wo2[c], 0.0f)
#define QUAD_LIN(out, x)                                                                                          \
    do {                                                                                                          \
        (out)[0] = SITE(f0[x], 0);                                                                                \
        (out)[1] = (SITE(f0[(x) + 1], 1) + SITE(f1[x], 2)) * 0.5f;                                                \
        (out)[2] = SITE(f1[(x) + 1], 3);                                                                          \
    } while (0)

    const float tone_offset = j->tone_offset;
    float *tone_lin = malloc(3 * (MIPU_TONE_MAX + 1) * sizeof(float));
    uint32_t knot[MIPU_TONE_N];
    tone_knots(knot);
    for (int c = 0; c < 3; c++) {
        float *l = tone_lin + c * (MIPU_TONE_MAX + 1);
        for (int e = 0; e < MIPU_TONE_N - 1; e++) {
            const uint32_t d = knot[e + 1] - knot[e];
            for (uint32_t v = knot[e]; v < knot[e + 1]; v++) {
                uint32_t q = (j->tone[c][e] * (knot[e + 1] - v) + j->tone[c][e + 1] * (v - knot[e]) + d / 2) / d;
                l[v] = q / 32768.0f;
            }
        }
        l[MIPU_TONE_MAX] = j->tone[c][MIPU_TONE_N - 1] / 32768.0f;
    }
    float mat[9];
    for (int i = 0; i < 9; i++) {
        mat[i] = j->matrix[i] / 1024.0f;
    }

    for (uint32_t qy = 0; qy < qh; qy++) {
        LOAD_ROW(f0, j->y0 + 2 * qy);
        LOAD_ROW(f1, j->y0 + 2 * qy + 1);
        for (uint32_t qx = 0; qx < qw; qx++) {
            uint32_t x = j->x0 + 2 * qx;
            float lin[3];
            QUAD_LIN(lin, x);
            float g[3];
            for (int c = 0; c < 3; c++) {
                float v = mat[3 * c] * lin[0] + mat[3 * c + 1] * lin[1] + mat[3 * c + 2] * lin[2] - tone_offset;
                int i = v <= 0 ? 0 : v >= MIPU_TONE_MAX ? MIPU_TONE_MAX : (int)v;
                g[c] = tone_lin[c * (MIPU_TONE_MAX + 1) + i];
            }
            float y = j->ycc[0] * g[0] + j->ycc[1] * g[1] + j->ycc[2] * g[2];
            float cb = j->ycc[3] * g[0] + j->ycc[4] * g[1] + j->ycc[5] * g[2];
            float cr = j->ycc[6] * g[0] + j->ycc[7] * g[1] + j->ycc[8] * g[2];
            y *= y2x_gain_blend(j->y2y_gain, j->y2x_gain_weight, y);
            const float cs = y2x_gain_blend(j->y2c_gain, j->y2x_gain_weight, y);
            cb *= cs;
            cr *= cs;
            yrow[2 * qx] = yrow[2 * qx + 1] = clamp8(255.0f * y);
            crow[2 * qx] = clamp8(128 + 255.0f * cb);
            crow[2 * qx + 1] = clamp8(128 + 255.0f * cr);
        }
        mem.write(mem.opaque, j->y_addr + (uint64_t)(2 * qy) * j->out_pitch, yrow, 2 * qw);
        mem.write(mem.opaque, j->y_addr + (uint64_t)(2 * qy + 1) * j->out_pitch, yrow, 2 * qw);
        mem.write(mem.opaque, j->c_addr + (uint64_t)qy * j->out_pitch, crow, 2 * qw);
    }
#undef LOAD_ROW
#undef QUAD_LIN
#undef SITE
    free(tone_lin);
    free(frame);
    free(pk);
    free(f0);
    free(f1);
    free(yrow);
    free(crow);
    return 0;
}

long mipu_model_raw_out(const MipuJob *j, MipuMem mem)
{
    if (!j->rawout_addr) {
        return 0;
    }
    if (j->in_bits == MIPU_IN_DPCM6) {
        return -1;
    }
    const uint32_t row = (uint32_t)(((uint64_t)j->frame_w * j->in_bits + 7) / 8);
    if (!j->frame_w || !j->frame_h || j->frame_w > MIPU_MAX_DIM || j->frame_h > MIPU_MAX_DIM || !j->in_addr ||
        row > j->in_pitch || row > j->rawout_pitch) {
        return -1;
    }
    uint8_t *buf = malloc(row);
    if (!buf) {
        return -1;
    }
    for (uint32_t y = 0; y < j->frame_h; y++) {
        mem.read(mem.opaque, j->in_addr + (uint64_t)y * j->in_pitch, buf, row);
        mem.write(mem.opaque, j->rawout_addr + (uint64_t)y * j->rawout_pitch, buf, row);
    }
    free(buf);
    return (long)row * j->frame_h;
}
