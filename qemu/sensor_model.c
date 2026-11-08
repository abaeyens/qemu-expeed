/*
 * The image sensor
 *
 * No QEMU in here
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "sensor_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sensor_scene_load(SensorScene *sc, const char *path)
{
    FILE *f = fopen(path, "rb");
    char magic[8];
    uint32_t wh[2];
    memset(sc, 0, sizeof(*sc));
    if (!f) {
        return -1;
    }
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, SENSOR_SCENE_MAGIC, 8) || fread(wh, 4, 2, f) != 2 || !wh[0] ||
        !wh[1] || wh[0] > SENSOR_MAX_DIM || wh[1] > SENSOR_MAX_DIM || (wh[0] | wh[1]) & 1) {
        fclose(f);
        return -1;
    }
    const size_t n = (size_t)wh[0] * wh[1];
    uint16_t *px = malloc(n * sizeof(uint16_t));
    if (!px || fread(px, sizeof(uint16_t), n, f) != n) {
        free(px);
        fclose(f);
        return -1;
    }
    fclose(f);
    sc->w = wh[0];
    sc->h = wh[1];
    sc->px = px;
    return 0;
}

static inline uint32_t clampu(int64_t v, uint32_t hi)
{
    return v < 0 ? 0 : v > hi ? hi : (uint32_t)v;
}

void sensor_readout(const SensorScene *sc, uint32_t in_w, uint32_t in_h, uint32_t y0w, uint32_t rows,
                    float exposure, float *out)
{
    const uint32_t fx = sc->w / in_w ? sc->w / in_w : 1, fy = sc->h / in_h ? sc->h / in_h : 1;
    const int64_t x0 = ((int64_t)sc->w - (int64_t)in_w * fx) / 2 & ~1LL;
    const int64_t y0 = ((int64_t)sc->h - (int64_t)in_h * fy) / 2 & ~1LL;
    const float black = SENSOR_BLACK14, n = (float)(fx * fy);
    for (uint32_t yo = 0; yo < rows; yo++) {
        const uint32_t y = y0w + yo;
        const int64_t by = y0 + (int64_t)(y >> 1) * 2 * fy + (y & 1);
        for (uint32_t x = 0; x < in_w; x++) {
            const int64_t bx = x0 + (int64_t)(x >> 1) * 2 * fx + (x & 1);
            float sum = 0;
            for (uint32_t k = 0; k < fy; k++) { /* the f x f pixels of this colour in the 2f x 2f block */
                const uint16_t *row = sc->px + (size_t)clampu(by + 2 * k, sc->h - 1) * sc->w;
                for (uint32_t l = 0; l < fx; l++) {
                    sum += row[clampu(bx + 2 * l, sc->w - 1)];
                }
            }
            float v = black + (sum / n - black) * exposure;
            out[(size_t)yo * in_w + x] = v < 0 ? 0 : v > SENSOR_WHITE14 ? SENSOR_WHITE14 : v;
        }
    }
}

int sensor_exposure(const uint8_t *reg, uint32_t v, uint32_t h, uint32_t ofs_ns, SensorExposure *out)
{
    static const uint16_t gain_regs[4] = { 0x3d, 0x3f, 0x47, 0x49 };
    const unsigned shr = (reg[5] | reg[6] << 8) & 0x3fff;
    double g = 0;
    int n = 0;
    for (int k = 0; k < 4; k++) {
        const unsigned code = (reg[gain_regs[k]] | reg[gain_regs[k] + 1] << 8) & 0xfff;
        if (code < 4095) {
            g += 4095.0 / (4095 - code);
            n++;
        }
    }
    if (!shr || !n || !v || !h || v < 2 * shr) {
        return -1;
    }
    out->shr = shr;
    out->t_s = (double)(v - 2 * shr) * h / 72e6 + ofs_ns * 1e-9;
    out->gain = g / n;
    out->hcg = reg[0x05e1] == 3;
    out->sens = out->t_s * out->gain * (out->hcg ? SENSOR_HCG : 1.0) / SENSOR_G_ISO100;
    return 0;
}
