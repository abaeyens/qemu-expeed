/*
 * The image sensor
 *
 * Is probably (a variant of) the Sony IMX410.
 * No QEMU in here
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SENSOR_MODEL_H
#define SENSOR_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENSOR_BLACK14     1008.0f
#define SENSOR_WHITE14     16383.0f
#define SENSOR_G_ISO100    1.121
#define SENSOR_HCG         5.09
#define SENSOR_REGS        0x10000
#define SENSOR_SCENE_MAGIC "NKSCENE1"
#define SENSOR_MAX_DIM     16384

typedef struct {
    uint32_t w, h;
    uint16_t *px;
} SensorScene;

typedef struct {
    unsigned shr;
    double t_s;
    double gain;
    bool hcg;
    double sens;
} SensorExposure;

int sensor_scene_load(SensorScene *sc, const char *path);

void sensor_readout(const SensorScene *sc, uint32_t in_w, uint32_t in_h, uint32_t y0, uint32_t rows,
                    float exposure, float *out);

int sensor_exposure(const uint8_t *reg, uint32_t v, uint32_t h, uint32_t ofs_ns, SensorExposure *out);

#endif
