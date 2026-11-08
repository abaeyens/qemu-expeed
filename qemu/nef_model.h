/*
 * The NEF compressor (0x20204000, GIC 121)
 *
 * No QEMU in here
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEF_MODEL_H
#define NEF_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "mipu_model.h"   /* use MipuMem */

#define NEF_MODEL_SYMS 15

typedef struct {
    uint64_t src, dst;
    uint32_t spitch;
    uint32_t w, h;
    int bits;
    uint32_t limit;
    uint16_t pred0;
    uint16_t code[NEF_MODEL_SYMS];
    uint8_t len[NEF_MODEL_SYMS];
    int lossy;
    uint16_t lut[256];
} NefJob;

long nef_model_encode_lossless(const NefJob *j, MipuMem mem);

#endif
