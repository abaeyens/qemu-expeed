/*
 * Parallel NOR flash, AMD/JEDEC command set, x16
 *
 * No QEMU in here
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOR_MODEL_H
#define NOR_MODEL_H

#include <stdint.h>

#define NOR_BUF_WORDS_MAX 512

typedef enum {
    NOR_READ,
    NOR_AUTOSELECT,
    NOR_CFI,
} NorMode;

typedef enum {
    NOR_W_PLAIN,
    NOR_W_COMMAND,
    NOR_W_CHANGED,
} NorWrite;

typedef struct {
    uint8_t *mem;
    uint64_t size;
    uint32_t sector_size;
    unsigned buf_words;
    uint16_t id[4];
    uint8_t cfi_device_log2;

    NorMode mode;
    int cycle;
    uint64_t wb_base;
    unsigned wb_count, wb_n;
    uint64_t wb_addr[NOR_BUF_WORDS_MAX];
    uint16_t wb_data[NOR_BUF_WORDS_MAX];

    uint64_t dirty_lo, dirty_hi;
} NorFlash;

void nor_init(NorFlash *f, uint8_t *mem, uint64_t size, uint32_t sector_size, unsigned buf_words);

NorWrite nor_write(NorFlash *f, uint64_t off, uint16_t data);

uint16_t nor_read(const NorFlash *f, uint64_t off);

#endif
