/* The front end's raw outputs
 *
 * No QEMU in here
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FE_MODEL_H
#define FE_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "sensor_model.h"

typedef struct {
    uint32_t in_w, in_h;
    uint32_t win_y0, win_h;
    uint32_t out_w, out_h;
    int depth;
    int dpcm;
    uint32_t pitch;
    float exposure;
    float dgain[4];
    const uint16_t *compand;
} FeOutJob;

size_t fe_out_bytes(const FeOutJob *j);

int fe_render(const SensorScene *sc, const FeOutJob *j, uint8_t *dst);

void fe_resize(const float *in, uint32_t iw, uint32_t ih, float *out, uint32_t ow, uint32_t oh);

#endif
