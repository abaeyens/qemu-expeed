/*
 * MIPU (0x20380000/0x203c0000)
 *
 * a crude raw-to-YUV developer
 *
 * - no QEMU in here
 * - this model exists to have _a_ picture in the pipeline,
 *   no intention to perfectly match a real-life's Expeed MIPU rendering
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MIPU_MODEL_H
#define MIPU_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "mipu_regs.h"

#define MIPU_TONE_N      MIPU_TONE_LUT_R_N
#define MIPU_TONE_MAX    32768
#define MIPU_Y2X_GAIN_N  MIPU_Y2Y_GAIN_TABLE_A_N
#define MIPU_DECOMPAND_N MIPU_LV_DECOMPAND_N
#define MIPU_MODEL_REGS  0x6000
#define MIPU_IN_DPCM6    6
#define MIPU_DPCM6_ROW   1140
#define MIPU_DPCM6_CODES 1520
/* green's DPCM prediction coefficients, per row parity (even rows = Gr):
 * A * (up-left + up-right)/2 + B * (left R/B + right R/B)/2.
 * TODO these are fitted values, find where the camera programs the real ones */
#define MIPU_DPCM6_GA_EVEN 0.6932f
#define MIPU_DPCM6_GB_EVEN 0.3212f
#define MIPU_DPCM6_GA_ODD  0.6117f
#define MIPU_DPCM6_GB_ODD  0.4493f
#define MIPU_STREAM_LO     0x80000000u
#define MIPU_STREAM_HI     0x80100000u

/**
 * struct MipuMem: Emulator memory access helper
 * @opaque: context pointer passed back to the callbacks
 * @read: a function that reads @len bytes at guest address @addr into @buf
 * @write: a function that writes @len bytes at guest address @addr into @buf
 *
 * Allows to cleanly read and write emulator guest memory without depending on QEMU
 * TODO: Move this to a separate header because also used outside MIPU
 */
typedef struct MipuMem {
    void *opaque;
    void (*read)(void *opaque, uint64_t addr, void *buf, size_t len);
    void (*write)(void *opaque, uint64_t addr, const void *buf, size_t len);
} MipuMem;

typedef struct {
    uint64_t in_addr;
    uint32_t in_pitch;
    int in_bits;
    int out_stream;
    uint32_t x0, y0, w, h;
    uint64_t y_addr, c_addr;
    uint32_t out_pitch;
    int matrix_src, tone_src;
    int16_t matrix[9];
    uint16_t tone[3][MIPU_TONE_N];
    uint16_t y2y_gain[2][MIPU_Y2X_GAIN_N];
    uint16_t y2c_gain[2][MIPU_Y2X_GAIN_N];
    uint16_t y2x_gain_weight[2];
    float ycc[9];
    int rggb_src;
    uint16_t rggb_gain[4];
    int16_t rggb_in_offset[4];
    uint16_t rggb_out_offset[4];
    uint16_t tone_offset;
    int decompand_src;
    uint16_t decompand[MIPU_DECOMPAND_N];
    uint32_t frame_w, frame_h;
    uint64_t rawout_addr;
    uint32_t rawout_pitch;
} MipuJob;

enum { MIPU_SRC_REGS = 0, MIPU_SRC_UNSET = 1, MIPU_SRC_IMPLAUSIBLE = 2 };

void mipu_job_from_regs(MipuJob *j, const uint32_t *regs);

int mipu_model_develop(const MipuJob *j, MipuMem mem);

long mipu_model_raw_out(const MipuJob *j, MipuMem mem);

void mipu_dpcm6_decode(const uint8_t *buf, uint32_t rows, float *out);

void mipu_dpcm6_encode(const float *img, uint32_t rows, uint32_t w, uint8_t *out);

#endif
