/*
 * The NEF compressor (0x20204000, GIC 121)
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>

#include "nef_model.h"

typedef struct {
    uint8_t *buf;
    size_t cap, n;
    uint64_t acc;
    int nacc;
} BitOut;

static void put_bits(BitOut *b, uint32_t v, int n)
{
    if (n == 0) {
        return;
    }
    b->acc = (b->acc << n) | (v & ((1u << n) - 1));
    b->nacc += n;
    while (b->nacc >= 8) {
        b->nacc -= 8;
        if (b->n < b->cap) {
            b->buf[b->n] = (uint8_t)(b->acc >> b->nacc);
        }
        b->n++;
    }
}

static void flush_bits(BitOut *b)
{
    if (b->nacc) {
        put_bits(b, 0, 8 - b->nacc);
    }
}

static unsigned pxn(const uint8_t *row, unsigned x, int bits)
{
    unsigned bit = x * bits;
    unsigned v = row[bit / 8] | row[bit / 8 + 1] << 8 | row[bit / 8 + 2] << 16;
    return (v >> (bit % 8)) & ((1u << bits) - 1);
}

static int bit_length(unsigned v)
{
    int n = 0;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

long nef_model_encode_lossless(const NefJob *j, MipuMem mem)
{
    int bits = j->bits ? j->bits : 14;
    if (!j->w || !j->h || j->w % 2 || j->w > 16384 || j->h > 16384 || (bits != 12 && bits != 14) ||
        j->spitch < (uint64_t)j->w * bits / 8) {
        return -1;
    }
    for (int s = 0; s <= bits && s < NEF_MODEL_SYMS; s++) {
        if (j->len[s] == 0 || j->len[s] > 14) {
            return -1;
        }
    }
    size_t cap = j->limit ? j->limit : (size_t)j->w * j->h * 2;
    BitOut b = { .buf = malloc(cap), .cap = cap };
    uint8_t *row = malloc(j->spitch + 4);
    if (!b.buf || !row) {
        free(b.buf);
        free(row);
        return -1;
    }
    int vpred[2][2] = { { j->pred0, j->pred0 }, { j->pred0, j->pred0 } };
    for (uint32_t y = 0; y < j->h; y++) {
        int hpred[2] = { 0, 0 };
        memset(row + j->spitch, 0, 4);
        mem.read(mem.opaque, j->src + (uint64_t)y * j->spitch, row, j->spitch);
        for (uint32_t x = 0; x < j->w; x++) {
            int v = pxn(row, x, bits);
            if (j->lossy) {
                int sh = bits - 8, k = v >> sh, f = v & ((1 << sh) - 1);
                int y0 = k ? j->lut[k - 1] : 0, y1 = j->lut[k < 256 ? k : 255];
                v = y0 + (((y1 - y0) * f + (1 << (sh - 1))) >> sh);
            }
            int pred = x < 2 ? vpred[y & 1][x] : hpred[x & 1];
            if (x < 2) {
                vpred[y & 1][x] = v;
            }
            hpred[x & 1] = v;
            int diff = v - pred;
            int len = bit_length(diff < 0 ? -diff : diff);
            put_bits(&b, j->code[len] >> (14 - j->len[len]), j->len[len]);
            put_bits(&b, (uint32_t)(diff < 0 ? diff - 1 : diff), len);
        }
    }
    flush_bits(&b);
    size_t n = b.n < cap ? b.n : cap;
    mem.write(mem.opaque, j->dst, b.buf, n);
    free(b.buf);
    free(row);
    return (long)n;
}
