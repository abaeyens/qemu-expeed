/* 
 * GEVG (DMP SMAPH-F) command list interpreter
 *
 * Turns the firmware's two command lists into pixels.
 * Keep free from QEMU.
 *
 * References:
 * - ShivaVG, the OpenVG renderer whose output this model was compared with.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory. *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEVG_MODEL_H
#define GEVG_MODEL_H

#include <stdint.h>
#include <stdio.h>

#define GEVG_MODEL_REGS       (0x2000 / 4)
#define GEVG_MODEL_VREGS      0x1000
#define GEVG_MODEL_MASKS      8
#define GEVG_MODEL_MAX_EDGES  1024
#define GEVG_MODEL_MAX_COORDS 2048
#define GEVG_MODEL_MAX_LIST   16384

#define GEVG_MODEL_CACHE        8
#define GEVG_MODEL_CACHE_MIN_PX 65536
#define GEVG_MODEL_CURVE_STEPS  16

#define GEVG_OFF_STATUS      0x58
#define GEVG_OFF_LIST_A_ADDR 0x10d4
#define GEVG_OFF_LIST_A_LEN  0x10dc
#define GEVG_OFF_LIST_A_GO   0x10d0
#define GEVG_OFF_LIST_B_ADDR 0x10e4
#define GEVG_OFF_LIST_B_LEN  0x10ec
#define GEVG_OFF_LIST_B_GO   0x10e0
#define GEVG_OFF_GO_C        0x1c
#define GEVG_OFF_KICK        0x1214

typedef struct {
    void *opaque;
    void (*read)(void *opaque, uint32_t addr, void *buf, uint32_t len);
    void (*write)(void *opaque, uint32_t addr, const void *buf, uint32_t len);
} GevgMem;

typedef struct {
    uint32_t addr, w, h;
    uint8_t *cov;
    size_t cap;
    uint32_t stamp;

    uint32_t x0, y0, x1, y1;
} GevgMask;

typedef struct {
    uint64_t key[2];
    uint8_t *rec;
    size_t len, cap;
    uint32_t stamp;
} GevgCacheEntry;

typedef struct {
    uint32_t reg[GEVG_MODEL_REGS];
    uint32_t vreg[GEVG_MODEL_VREGS];
    GevgMem mem;
    float matrix[9];
    GevgMask masks[GEVG_MODEL_MASKS];
    uint32_t stamp;
    uint32_t list_a[GEVG_MODEL_MAX_LIST], list_b[GEVG_MODEL_MAX_LIST];
    uint32_t lut[2][256];
    GevgCacheEntry cache[GEVG_MODEL_CACHE];
    GevgCacheEntry *recording;
    uint32_t recording_base;
} GevgModel;

void gevg_model_init(GevgModel *m, GevgMem mem);
void gevg_model_free(GevgModel *m);
uint32_t gevg_model_read(GevgModel *m, uint32_t off);
int gevg_model_write(GevgModel *m, uint32_t off, uint32_t val);
void gevg_model_run_job(GevgModel *m);

#endif
