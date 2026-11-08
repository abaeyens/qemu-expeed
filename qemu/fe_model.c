/*
 * The front end's raw outputs
 *
 * No QEMU in here
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "fe_model.h"
#include "mipu_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t clampu(int64_t v, uint32_t hi)
{
    return v < 0 ? 0 : v > hi ? hi : (uint32_t)v;
}

static void fe_digital_gain(float *fr, uint32_t w, uint32_t rows, uint32_t y0, const float *dgain)
{
    float g[4];
    int any = 0;
    for (int c = 0; c < 4; c++) {
        g[c] = dgain[c] > 0 ? dgain[c] : 1.0f;
        any |= g[c] != 1.0f;
    }
    if (!any) {
        return;
    }
    for (uint32_t yo = 0; yo < rows; yo++) {
        const uint32_t y = y0 + yo;
        for (uint32_t x = 0; x < w; x++) {
            float *p = fr + (size_t)yo * w + x;
            const float v = SENSOR_BLACK14 + (*p - SENSOR_BLACK14) * g[(y & 1) * 2 + (x & 1)];
            *p = v < 0 ? 0 : v > SENSOR_WHITE14 ? SENSOR_WHITE14 : v;
        }
    }
}

void fe_resize(const float *in, uint32_t iw, uint32_t ih, float *out, uint32_t ow, uint32_t oh)
{
    if (iw == ow && ih == oh) {
        memcpy(out, in, (size_t)iw * ih * sizeof(float));
        return;
    }
    const uint32_t pw = iw / 2, ph = ih / 2, qw = ow / 2, qh = oh / 2;
    for (uint32_t c = 0; c < 4; c++) {
        const uint32_t cy = c >> 1, cx = c & 1;
        for (uint32_t v = 0; v < qh; v++) {
            float sy = (v + 0.5f) * ph / qh - 0.5f;
            sy = sy < 0 ? 0 : sy > ph - 1 ? ph - 1 : sy;
            const uint32_t y0 = (uint32_t)sy, y1 = y0 + 1 < ph ? y0 + 1 : y0;
            const float fy = sy - y0;
            for (uint32_t u = 0; u < qw; u++) {
                float sx = (u + 0.5f) * pw / qw - 0.5f;
                sx = sx < 0 ? 0 : sx > pw - 1 ? pw - 1 : sx;
                const uint32_t x0 = (uint32_t)sx, x1 = x0 + 1 < pw ? x0 + 1 : x0;
                const float fx = sx - x0;
#define P(yy, xx) in[(size_t)(2 * (yy) + cy) * iw + 2 * (xx) + cx]
                const float top = P(y0, x0) + (P(y0, x1) - P(y0, x0)) * fx;
                const float bot = P(y1, x0) + (P(y1, x1) - P(y1, x0)) * fx;
#undef P
                out[(size_t)(2 * v + cy) * ow + 2 * u + cx] = top + (bot - top) * fy;
            }
        }
    }
}

static int fe_bits(int depth)
{
    return depth == 1 ? 14 : depth == 2 ? 12 : 0;
}

size_t fe_out_bytes(const FeOutJob *j)
{
    const int bits = fe_bits(j->depth);
    if (!bits || (j->dpcm && bits != 12) || !j->in_w || !j->in_h || !j->out_w || !j->out_h ||
        (j->out_w | j->out_h | j->win_y0 | j->win_h) & 1 || (uint64_t)j->win_y0 + j->win_h > j->in_h ||
        j->in_w > SENSOR_MAX_DIM || j->in_h > SENSOR_MAX_DIM || j->out_w > SENSOR_MAX_DIM ||
        j->out_h > SENSOR_MAX_DIM) {
        return 0;
    }
    if (j->dpcm) {
        return j->out_w <= MIPU_DPCM6_CODES && j->compand ? (size_t)j->out_h * MIPU_DPCM6_ROW : 0;
    }
    return j->pitch >= ((uint64_t)j->out_w * bits + 7) / 8 ? (size_t)j->out_h * j->pitch : 0;
}

static float compand_at(const uint16_t *t, float v)
{
    float p = v / 64.0f;
    if (p <= 0) {
        return 0;
    }
    if (p >= 256) {
        return 4.0f * t[255];
    }
    const int e = (int)p;
    const float lo = e ? 4.0f * t[e - 1] : 0, hi = 4.0f * t[e];
    return lo + (hi - lo) * (p - e);
}

int fe_render(const SensorScene *sc, const FeOutJob *j, uint8_t *dst)
{
    const size_t bytes = fe_out_bytes(j);
    if (!bytes) {
        return -1;
    }
    const uint32_t wh = j->win_h ? j->win_h : j->in_h;
    float *in = malloc((size_t)j->in_w * wh * sizeof(float));
    float *fr = malloc((size_t)j->out_w * j->out_h * sizeof(float));
    sensor_readout(sc, j->in_w, j->in_h, j->win_y0, wh, j->exposure, in);
    fe_digital_gain(in, j->in_w, wh, j->win_y0, j->dgain);
    fe_resize(in, j->in_w, wh, fr, j->out_w, j->out_h);
    free(in);
    if (j->dpcm) {
        for (size_t i = 0; i < (size_t)j->out_w * j->out_h; i++) {
            fr[i] = compand_at(j->compand, fr[i]);
        }
        mipu_dpcm6_encode(fr, j->out_h, j->out_w, dst);
    } else if (fe_bits(j->depth) == 14) {
        memset(dst, 0, bytes);
        for (uint32_t y = 0; y < j->out_h; y++) {
            uint8_t *row = dst + (size_t)y * j->pitch;
            for (uint32_t x = 0; x < j->out_w; x++) { /* little-endian bit-packed, as the NEF's rows */
                const uint32_t v = clampu((int64_t)lrintf(fr[(size_t)y * j->out_w + x]), 16383);
                const uint64_t bit = (uint64_t)x * 14;
                uint8_t *p = row + bit / 8;
                const uint32_t w = v << (bit % 8);
                p[0] |= w & 0xff;
                p[1] |= (w >> 8) & 0xff;
                p[2] |= (w >> 16) & 0xff;
            }
        }
    } else {
        memset(dst, 0, bytes);
        for (uint32_t y = 0; y < j->out_h; y++) {
            uint8_t *row = dst + (size_t)y * j->pitch;
            for (uint32_t x = 0; x < j->out_w; x += 2) { /* two pixels in three bytes, LSB first */
                const float *p = fr + (size_t)y * j->out_w + x;
                const uint32_t a = clampu((int64_t)lrintf(p[0] / 4.0f), 4095);
                const uint32_t b = clampu((int64_t)lrintf(p[1] / 4.0f), 4095);
                row[3 * (x / 2)] = a & 0xff;
                row[3 * (x / 2) + 1] = (a >> 8) | (b & 0xf) << 4;
                row[3 * (x / 2) + 2] = b >> 4;
            }
        }
    }
    free(fr);
    return 0;
}
