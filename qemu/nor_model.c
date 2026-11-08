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
#include "nor_model.h"

#include <string.h>

#define UNLOCK1  0x555
#define UNLOCK2  0x2aa
#define CFI_ADDR 0x55

enum {
    C_IDLE,
    C_UNLOCK1,
    C_UNLOCK2,
    C_ERASE1,
    C_ERASE2,
    C_ERASE3,
    C_PROGRAM,
    C_WB_COUNT,
    C_WB_DATA,
    C_WB_CONFIRM,
};

void nor_init(NorFlash *f, uint8_t *mem, uint64_t size, uint32_t sector_size, unsigned buf_words)
{
    memset(f, 0, sizeof(*f));
    f->mem = mem;
    f->size = size;
    f->sector_size = sector_size;
    f->buf_words = buf_words > NOR_BUF_WORDS_MAX ? NOR_BUF_WORDS_MAX : buf_words;
    f->id[0] = 0x0089;
    f->id[1] = 0x227e;
    f->id[2] = 0x2228;
    f->id[3] = 0x2201;
    f->cfi_device_log2 = 27;
    f->mode = NOR_READ;
}

static unsigned log2u(uint64_t v)
{
    unsigned n = 0;
    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

static uint16_t cfi_word(const NorFlash *f, unsigned w)
{
    uint64_t blocks = ((uint64_t)1 << f->cfi_device_log2) / f->sector_size;
    switch (w) {
    case 0x10:
        return 'Q';
    case 0x11:
        return 'R';
    case 0x12:
        return 'Y';
    case 0x13:
        return 0x02;
    case 0x15:
        return 0x40;
    case 0x1b:
        return 0x17;
    case 0x1c:
        return 0x19;
    case 0x1f:
        return 0x08;
    case 0x20:
        return 0x0a;
    case 0x21:
        return 0x09;
    case 0x22:
        return 0x13;
    case 0x23:
        return 0x02;
    case 0x24:
        return 0x02;
    case 0x25:
        return 0x03;
    case 0x26:
        return 0x02;
    case 0x27:
        return f->cfi_device_log2;
    case 0x28:
        return 0x01;
    case 0x2a:
        return log2u((uint64_t)f->buf_words * 2);
    case 0x2c:
        return 1;
    case 0x2d:
        return (blocks - 1) & 0xff;
    case 0x2e:
        return ((blocks - 1) >> 8) & 0xff;
    case 0x2f:
        return (f->sector_size >> 8) & 0xff;
    case 0x30:
        return (f->sector_size >> 16) & 0xff;
    case 0x40:
        return 'P';
    case 0x41:
        return 'R';
    case 0x42:
        return 'I';
    case 0x43:
        return '1';
    case 0x44:
        return '3';
    default:
        return 0;
    }
}

uint16_t nor_read(const NorFlash *f, uint64_t off)
{
    unsigned w = (unsigned)(off >> 1);
    switch (f->mode) {
    case NOR_AUTOSELECT:
        switch (w & 0xff) {
        case 0x00:
            return f->id[0];
        case 0x01:
            return f->id[1];
        case 0x02:
            return 0;
        case 0x0e:
            return f->id[2];
        case 0x0f:
            return f->id[3];
        default:
            return 0;
        }
    case NOR_CFI:
        return cfi_word(f, w & 0xff);
    case NOR_READ:
    default:
        if (off + 2 > f->size) {
            return 0xffff;
        }
        return (uint16_t)(f->mem[off] | f->mem[off + 1] << 8);
    }
}

static void mark(NorFlash *f, uint64_t lo, uint64_t hi)
{
    if (f->dirty_lo == f->dirty_hi) {
        f->dirty_lo = lo;
        f->dirty_hi = hi;
        return;
    }
    if (lo < f->dirty_lo) {
        f->dirty_lo = lo;
    }
    if (hi > f->dirty_hi) {
        f->dirty_hi = hi;
    }
}

static void program_word(NorFlash *f, uint64_t off, uint16_t data)
{
    off &= ~(uint64_t)1;
    if (off + 2 > f->size) {
        return;
    }
    f->mem[off] &= data & 0xff;
    f->mem[off + 1] &= data >> 8;
    mark(f, off, off + 2);
}

static uint64_t sector_base(const NorFlash *f, uint64_t off)
{
    return off - off % f->sector_size;
}

static NorWrite abort_seq(NorFlash *f)
{
    f->cycle = C_IDLE;
    return NOR_W_COMMAND;
}

NorWrite nor_write(NorFlash *f, uint64_t off, uint16_t data)
{
    unsigned w = (unsigned)(off >> 1) & 0x7ff;
    uint8_t cmd = data & 0xff;

    f->dirty_lo = f->dirty_hi = 0;
    switch (f->cycle) {
    case C_IDLE:
        if (cmd == 0xf0) {
            f->mode = NOR_READ;
            return NOR_W_COMMAND;
        }
        if (cmd == 0x98 && w == CFI_ADDR) {
            f->mode = NOR_CFI;
            return NOR_W_COMMAND;
        }
        if (cmd == 0xaa && w == UNLOCK1) {
            f->cycle = C_UNLOCK1;
            return NOR_W_COMMAND;
        }
        if (f->mode != NOR_READ) {
            return NOR_W_COMMAND;
        }
        return NOR_W_PLAIN;
    case C_UNLOCK1:
        if (cmd == 0x55 && w == UNLOCK2) {
            f->cycle = C_UNLOCK2;
            return NOR_W_COMMAND;
        }
        return abort_seq(f);
    case C_UNLOCK2:
        if (cmd == 0x25) {
            f->wb_base = sector_base(f, off);
            f->cycle = C_WB_COUNT;
            return NOR_W_COMMAND;
        }
        if (w != UNLOCK1) {
            return abort_seq(f);
        }
        switch (cmd) {
        case 0x80:
            f->cycle = C_ERASE1;
            return NOR_W_COMMAND;
        case 0xa0:
            f->cycle = C_PROGRAM;
            return NOR_W_COMMAND;
        case 0x90:
            f->mode = NOR_AUTOSELECT;
            f->cycle = C_IDLE;
            return NOR_W_COMMAND;
        case 0xf0:
            f->mode = NOR_READ;
            f->cycle = C_IDLE;
            return NOR_W_COMMAND;
        default:
            return abort_seq(f);
        }
    case C_ERASE1:
        if (cmd == 0xaa && w == UNLOCK1) {
            f->cycle = C_ERASE2;
            return NOR_W_COMMAND;
        }
        return abort_seq(f);
    case C_ERASE2:
        if (cmd == 0x55 && w == UNLOCK2) {
            f->cycle = C_ERASE3;
            return NOR_W_COMMAND;
        }
        return abort_seq(f);
    case C_ERASE3:
        f->cycle = C_IDLE;
        if (cmd == 0x30) {
            uint64_t base = sector_base(f, off);
            uint64_t end = base + f->sector_size;
            if (end > f->size) {
                end = f->size;
            }
            if (base < end) {
                memset(f->mem + base, 0xff, end - base);
                mark(f, base, end);
            }
            return f->dirty_hi > f->dirty_lo ? NOR_W_CHANGED : NOR_W_COMMAND;
        }
        if (cmd == 0x10 && w == UNLOCK1) {
            memset(f->mem, 0xff, f->size);
            mark(f, 0, f->size);
            return NOR_W_CHANGED;
        }
        return abort_seq(f);
    case C_PROGRAM:
        f->cycle = C_IDLE;
        program_word(f, off, data);
        return f->dirty_hi > f->dirty_lo ? NOR_W_CHANGED : NOR_W_COMMAND;
    case C_WB_COUNT:
        if (sector_base(f, off) != f->wb_base || (unsigned)data + 1 > f->buf_words) {
            return abort_seq(f);
        }
        f->wb_count = (unsigned)data + 1;
        f->wb_n = 0;
        f->cycle = C_WB_DATA;
        return NOR_W_COMMAND;
    case C_WB_DATA:
        if (sector_base(f, off) != f->wb_base) {
            return abort_seq(f);
        }
        f->wb_addr[f->wb_n] = off;
        f->wb_data[f->wb_n] = data;
        if (++f->wb_n == f->wb_count) {
            f->cycle = C_WB_CONFIRM;
        }
        return NOR_W_COMMAND;
    case C_WB_CONFIRM:
        f->cycle = C_IDLE;
        if (cmd != 0x29 || sector_base(f, off) != f->wb_base) {
            return abort_seq(f);
        }
        for (unsigned i = 0; i < f->wb_n; i++) {
            program_word(f, f->wb_addr[i], f->wb_data[i]);
        }
        return f->dirty_hi > f->dirty_lo ? NOR_W_CHANGED : NOR_W_COMMAND;
    }
    return abort_seq(f);
}
