/*
 * The IIP
 *
 * No QEMU in here
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "iip_model.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IIP_MAX_DIM 16384

static void resample_plane(uint64_t sa, uint32_t spitch, uint32_t sw, uint32_t sh, uint64_t da, uint32_t dpitch,
                           uint32_t dw, uint32_t dh, int bpu, MipuMem mem)
{
    uint8_t *srow = malloc((size_t)sw * bpu);
    uint32_t *acc = calloc((size_t)dw * bpu, sizeof(uint32_t));
    uint8_t *drow = malloc((size_t)dw * bpu);
    uint32_t *cnt = calloc(dw, sizeof(uint32_t));
    uint32_t *xmap = malloc((size_t)sw * sizeof(uint32_t));
    uint32_t *nearest = malloc((size_t)dw * sizeof(uint32_t));
    for (uint32_t x = 0; x < sw; x++) {
        xmap[x] = (uint64_t)x * dw / sw;
    }
    for (uint32_t dx = 0; dx < dw; dx++) {
        nearest[dx] = (uint64_t)dx * sw / dw;
    }

    for (uint32_t dy = 0; dy < dh; dy++) {
        uint32_t y0 = (uint64_t)dy * sh / dh;
        uint32_t y1 = (uint64_t)(dy + 1) * sh / dh;
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        memset(acc, 0, (size_t)dw * bpu * sizeof(uint32_t));
        memset(cnt, 0, dw * sizeof(uint32_t));
        for (uint32_t y = y0; y < y1 && y < sh; y++) {
            mem.read(mem.opaque, sa + (uint64_t)y * spitch, srow, (size_t)sw * bpu);
            if (bpu == 1) {
                for (uint32_t x = 0; x < sw; x++) {
                    acc[xmap[x]] += srow[x];
                    cnt[xmap[x]]++;
                }
            } else {
                for (uint32_t x = 0; x < sw; x++) {
                    uint32_t dx = xmap[x];
                    for (int b = 0; b < bpu; b++) {
                        acc[dx * bpu + b] += srow[x * bpu + b];
                    }
                    cnt[dx]++;
                }
            }
        }
        for (uint32_t dx = 0; dx < dw; dx++) {
            uint32_t n = cnt[dx];
            if (!n) {
                uint32_t x = nearest[dx];
                for (int b = 0; b < bpu; b++) {
                    drow[dx * bpu + b] = srow[x * bpu + b];
                }
                continue;
            }
            for (int b = 0; b < bpu; b++) {
                drow[dx * bpu + b] = (acc[dx * bpu + b] + n / 2) / n;
            }
        }
        mem.write(mem.opaque, da + (uint64_t)dy * dpitch, drow, (size_t)dw * bpu);
    }
    free(srow);
    free(acc);
    free(drow);
    free(cnt);
    free(xmap);
    free(nearest);
}

int iip_model_resample(const IipImage *src, const IipImage *dst, MipuMem mem)
{
    resample_plane(src->y, src->ypitch, src->w, src->h, dst->y, dst->ypitch, dst->w, dst->h, 1, mem);
    if (dst->c) {
        if (src->c) {
            resample_plane(src->c, src->cpitch, src->w / 2, src->h / 2, dst->c, dst->cpitch, dst->w / 2,
                           dst->h / 2, 2, mem);
        } else {
            uint8_t *row = malloc(dst->w);
            memset(row, 128, dst->w);
            for (uint32_t y = 0; y < dst->h / 2; y++) {
                mem.write(mem.opaque, dst->c + (uint64_t)y * dst->cpitch, row, dst->w);
            }
            free(row);
        }
    }
    return 0;
}

int iip_scale_from_desc(IipScale *sc, uint64_t desc, MipuMem mem)
{
    uint32_t w[8];
    if (desc < 0x80000000u) {
        return -1;
    }
    mem.read(mem.opaque, desc, w, sizeof(w));
    sc->phase[0] = (int32_t)w[0];
    sc->phase[1] = (int32_t)w[1];
    sc->step[0] = w[2];
    sc->step[1] = w[5];
    sc->ow = w[6] & 0xffff;
    sc->oh = w[6] >> 16;
    if (!sc->step[0] || !sc->step[1] || sc->step[0] > (64u << 16) || sc->step[1] > (64u << 16) || !sc->ow ||
        !sc->oh || sc->ow > IIP_MAX_DIM || sc->oh > IIP_MAX_DIM) {
        return -1;
    }
    return 0;
}

static int scaled_span(int64_t phase, int64_t step, int64_t pre, int64_t post, int64_t d, int64_t n, int64_t div,
                       int64_t sdim, int64_t *lo, int64_t *hi)
{
    int64_t centre = phase + d * post * step + (n * post - 1) * step / 2;
    int64_t half = n * post * step / 2;
    int64_t den = 65536 * div;
    if (centre < 0 || centre * pre >= sdim * den) {
        return 0;
    }
    int64_t a = (centre - half) * pre, b = (centre + half) * pre;
    *lo = a <= 0 ? 0 : a / den;
    *hi = b <= 0 ? 0 : (b + den - 1) / den;
    if (*hi > sdim) {
        *hi = sdim;
    }
    if (*hi <= *lo) {
        *hi = *lo + 1 <= sdim ? *lo + 1 : sdim;
        *lo = *hi - 1;
    }
    return 1;
}

static void scaled_plane(uint64_t sa, uint32_t spitch, uint32_t sw, uint32_t sh, uint64_t da, uint32_t dpitch,
                         uint32_t dw, uint32_t dh, int bpu, int div, const IipScale *sc, int64_t pre, int64_t px,
                         int64_t py, MipuMem mem)
{
    uint8_t *srow = malloc((size_t)sw * bpu);
    uint8_t *drow = malloc((size_t)dw * bpu);
    uint64_t *acc = calloc((size_t)dw * bpu, sizeof(uint64_t));
    int64_t *xlo = malloc(dw * sizeof(int64_t)), *xhi = malloc(dw * sizeof(int64_t));
    uint8_t *xin = malloc(dw);
    const uint8_t black = bpu == 2 ? 128 : 0;

    for (uint32_t dx = 0; dx < dw; dx++) {
        xin[dx] =
            scaled_span(sc->phase[0], sc->step[0], pre, px, (int64_t)dx * div, div, div, sw, &xlo[dx], &xhi[dx]);
    }
    for (uint32_t dy = 0; dy < dh; dy++) {
        int64_t ylo, yhi;
        int yin = scaled_span(sc->phase[1], sc->step[1], pre, py, (int64_t)dy * div, div, div, sh, &ylo, &yhi);
        memset(acc, 0, (size_t)dw * bpu * sizeof(uint64_t));
        for (int64_t y = ylo; yin && y < yhi; y++) {
            mem.read(mem.opaque, sa + (uint64_t)y * spitch, srow, (size_t)sw * bpu);
            for (uint32_t dx = 0; dx < dw; dx++) {
                for (int64_t x = xlo[dx]; xin[dx] && x < xhi[dx]; x++) {
                    for (int b = 0; b < bpu; b++) {
                        acc[dx * bpu + b] += srow[x * bpu + b];
                    }
                }
            }
        }
        for (uint32_t dx = 0; dx < dw; dx++) {
            uint64_t n = yin && xin[dx] ? (uint64_t)(yhi - ylo) * (xhi[dx] - xlo[dx]) : 0;
            for (int b = 0; b < bpu; b++) {
                drow[dx * bpu + b] = n ? (acc[dx * bpu + b] + n / 2) / n : black;
            }
        }
        mem.write(mem.opaque, da + (uint64_t)dy * dpitch, drow, (size_t)dw * bpu);
    }
    free(srow);
    free(drow);
    free(acc);
    free(xlo);
    free(xhi);
    free(xin);
}

static int64_t ratio(uint64_t a, uint64_t b)
{
    int64_t r = b ? (int64_t)((a + b / 2) / b) : 1;
    return r < 1 ? 1 : r;
}

int iip_model_resample_scaled(const IipImage *src, const IipImage *dst, const IipScale *sc, MipuMem mem)
{
    int64_t pre = ratio((uint64_t)src->w << 16, (uint64_t)sc->ow * sc->step[0]);
    int64_t px = ratio(sc->ow, dst->w), py = ratio(sc->oh, dst->h);
    if (pre > 4096 || px > 4096 || py > 4096) {
        fprintf(stderr,
                "iip model: a %ux%u -> %ux%u resize refused: factors %" PRId64 ", %" PRId64 ", %" PRId64
                " (step %#x, %#x)\n",
                src->w, src->h, dst->w, dst->h, pre, px, py, sc->step[0], sc->step[1]);
        return -1;
    }

    scaled_plane(src->y, src->ypitch, src->w, src->h, dst->y, dst->ypitch, dst->w, dst->h, 1, 1, sc, pre, px, py,
                 mem);
    if (dst->c && src->c) {
        scaled_plane(src->c, src->cpitch, src->w / 2, src->h / 2, dst->c, dst->cpitch, dst->w / 2, dst->h / 2, 2,
                     2, sc, pre, px, py, mem);
    }
    return 0;
}

typedef struct {
    uint8_t *p;
    int64_t x0, y0, w, h;
    int bpu;
} Box;

#define BOX_MAX_PIXELS (1u << 22)

static int box_read(Box *b, uint64_t sa, uint32_t spitch, uint32_t sw, uint32_t sh, int64_t x0, int64_t y0,
                    int64_t x1, int64_t y1, int bpu, MipuMem mem)
{
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > sw ? sw : x1;
    y1 = y1 > sh ? sh : y1;
    b->x0 = x0;
    b->y0 = y0;
    b->w = x1 > x0 ? x1 - x0 : 0;
    b->h = y1 > y0 ? y1 - y0 : 0;
    b->bpu = bpu;
    uint8_t *p = b->w * b->h > BOX_MAX_PIXELS ? NULL : realloc(b->p, b->w * b->h * bpu + 1);
    if (!p) {
        b->w = b->h = 0;
        return -1;
    }
    b->p = p;
    for (int64_t y = 0; y < b->h; y++) {
        mem.read(mem.opaque, sa + (uint64_t)(y0 + y) * spitch + x0 * bpu, b->p + y * b->w * bpu, b->w * bpu);
    }
    return 0;
}

static int box_at(const Box *b, int64_t x, int64_t y, int k)
{
    x -= b->x0;
    y -= b->y0;
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) {
        return -1;
    }
    return b->p[(y * b->w + x) * b->bpu + k];
}

static void corner_lerp(const int32_t *c, double u, double v, double *x, double *y)
{
    double tx = c[0] + (c[2] - c[0]) * u, ty = c[1] + (c[3] - c[1]) * u;
    double bx = c[4] + (c[6] - c[4]) * u, by = c[5] + (c[7] - c[5]) * u;
    *x = tx + (bx - tx) * v;
    *y = ty + (by - ty) * v;
}

int iip_grid_extent(const IipGrid *g, uint32_t *w, uint32_t *h, MipuMem mem)
{
    *w = *h = 0;
    for (uint32_t i = 0; i < g->cols || i < g->rows; i++) {
        uint32_t e8;
        if (i < g->cols) {
            mem.read(mem.opaque, g->addr + (uint64_t)i * IIP_MODEL_GRID_ENTRY + 8 * 4, &e8, sizeof(e8));
            if ((e8 & 0xf) < 2 || (e8 & 0xf) > 8) {
                return -1;
            }
            *w += 1u << (e8 & 0xf);
        }
        if (i < g->rows) {
            mem.read(mem.opaque, g->addr + (uint64_t)i * g->cols * IIP_MODEL_GRID_ENTRY + 8 * 4, &e8, sizeof(e8));
            if (((e8 >> 8) & 0xf) < 2 || ((e8 >> 8) & 0xf) > 8) {
                return -1;
            }
            *h += 1u << ((e8 >> 8) & 0xf);
        }
    }
    return 0;
}

int iip_model_warp(const IipImage *src, const IipImage *dst, const IipGrid *g, MipuMem mem)
{
    Box by = { 0 }, bc = { 0 };
    uint8_t *row = NULL;
    int tiles = 0;
    uint32_t oy = 0, xnext = 0, th0 = 0;
    for (uint32_t r = 0; r < g->rows; r++, oy += th0) {
        for (uint32_t col = 0; col < g->cols; col++) {
            uint32_t e[10];
            mem.read(mem.opaque, g->addr + (uint64_t)(r * g->cols + col) * IIP_MODEL_GRID_ENTRY, e, sizeof(e));
            unsigned lw = e[8] & 0xf, lh = (e[8] >> 8) & 0xf;
            if (lw < 2 || lw > 8 || lh < 2 || lh > 8) {
                free(by.p);
                free(bc.p);
                free(row);
                return -1;
            }
            uint32_t tw = 1u << lw, th = 1u << lh;
            if (col == 0) {
                xnext = 0;
                th0 = th;
            }
            uint32_t ox = xnext;
            xnext += tw;
            if (ox >= dst->w || oy >= dst->h) {
                continue;
            }
            uint32_t ow = dst->w - ox < tw ? dst->w - ox : tw, oh = dst->h - oy < th ? dst->h - oy : th;
            const int32_t *c = (const int32_t *)e;
            int32_t xmin = c[0], xmax = c[0], ymin = c[1], ymax = c[1];
            for (int i = 1; i < 4; i++) {
                xmin = c[2 * i] < xmin ? c[2 * i] : xmin;
                xmax = c[2 * i] > xmax ? c[2 * i] : xmax;
                ymin = c[2 * i + 1] < ymin ? c[2 * i + 1] : ymin;
                ymax = c[2 * i + 1] > ymax ? c[2 * i + 1] : ymax;
            }
            uint8_t *nrow = realloc(row, (size_t)tw * 2);
            if (!nrow || box_read(&by, src->y, src->ypitch, src->w, src->h, (xmin >> 8) - 1, (ymin >> 8) - 1,
                                  (xmax >> 8) + 2, (ymax >> 8) + 2, 1, mem)) {
                fprintf(stderr, "iip model: grid tile (%u, %u) refused: its source footprint is over %u pixels\n",
                        col, r, BOX_MAX_PIXELS);
                free(by.p);
                free(bc.p);
                free(nrow ? nrow : row);
                return -1;
            }
            row = nrow;
            for (uint32_t v = 0; v < oh; v++) {
                for (uint32_t u = 0; u < ow; u++) {
                    double x, y;
                    corner_lerp(c, (u + 0.5) / tw, (v + 0.5) / th, &x, &y);
                    x = x / 256 - 0.5;
                    y = y / 256 - 0.5;
                    int64_t xi = (int64_t)floor(x), yi = (int64_t)floor(y);
                    double fx = x - xi, fy = y - yi;
                    int p00 = box_at(&by, xi, yi, 0), p10 = box_at(&by, xi + 1, yi, 0);
                    int p01 = box_at(&by, xi, yi + 1, 0), p11 = box_at(&by, xi + 1, yi + 1, 0);
                    if (p00 < 0) {
                        row[u] = 0;
                        continue;
                    }
                    p10 = p10 < 0 ? p00 : p10;
                    p01 = p01 < 0 ? p00 : p01;
                    p11 = p11 < 0 ? p10 : p11;
                    double t = p00 + (p10 - p00) * fx, b = p01 + (p11 - p01) * fx;
                    row[u] = (uint8_t)(t + (b - t) * fy + 0.5);
                }
                mem.write(mem.opaque, dst->y + (uint64_t)(oy + v) * dst->ypitch + ox, row, ow);
            }
            if (dst->c && src->c) {
                box_read(&bc, src->c, src->cpitch, src->w / 2, src->h / 2, (xmin >> 9) - 1, (ymin >> 9) - 1,
                         (xmax >> 9) + 2, (ymax >> 9) + 2, 2, mem);
                for (uint32_t v = 0; v < oh / 2; v++) {
                    for (uint32_t u = 0; u < ow / 2; u++) {
                        double x, y;
                        corner_lerp(c, (2 * u + 1.0) / tw, (2 * v + 1.0) / th, &x, &y);
                        int64_t xi = (int64_t)floor(x / 512), yi = (int64_t)floor(y / 512);
                        int cb = box_at(&bc, xi, yi, 0), cr = box_at(&bc, xi, yi, 1);
                        row[2 * u] = cb < 0 ? 128 : cb;
                        row[2 * u + 1] = cr < 0 ? 128 : cr;
                    }
                    mem.write(mem.opaque, dst->c + (uint64_t)(oy / 2 + v) * dst->cpitch + ox, row, ow / 2 * 2);
                }
            }
            tiles++;
        }
    }
    free(by.p);
    free(bc.p);
    free(row);
    return tiles;
}

#define IIP_UNIT_START_REG 0x800
static const uint8_t block_owner[] = {
    0x25, 0x26, 0x27, 0x00, 0x28, 0x29, 0x2a, 0x2b, 0x2d, 0x22, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x48, 0x49,
    0x41, 0x44, 0x42, 0x43, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x4b, 0x37, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2c, 0x00, 0x2f, 0x30, 0x45, 0x46, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x47, 0x23, 0x24,
};
static const uint16_t port_owner[0x71] = {
    0x94,  0x98,  0x9c,  0x00,  0xa0,  0xa1,  0xa4,  0xa8,  0xac,  0xb4,  0xc4,  0x00,  0xc8,  0x00, 0xcc,
    0x00,  0xd0,  0x00,  0xd4,  0x00,  0xd8,  0x00,  0xdc,  0x00,  0xb8,  0x00,  0xe0,  0x00,  0x00, 0x00,
    0x00,  0x00,  0x120, 0x124, 0x104, 0x105, 0x106, 0x107, 0x110, 0x111, 0x108, 0x109, 0x10a, 0x00, 0x00,
    0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x128, 0x12c, 0x00,  0x00,  0x00,  0x00,  0xb0, 0x10c,
    0x10d, 0x10e, 0x00,  0x00,  0xa5,  0xa6,  0xa7,  0xa9,  0xaa,  0xab,  0xad,  0xae,  0xaf,  0xb1, 0xb2,
    0xb3,  0xb5,  0xb6,  0xb7,  0xb9,  0xba,  0xbb,  0xbc,  0xbd,  0xbe,  0xbf,  0xc0,  0xc1,  0xc2, 0xc3,
    0x114, 0x115, 0x116, 0x118, 0x119, 0x11a, 0xe4,  0xe5,  0xe8,  0xe9,  0xec,  0xed,  0xf0,  0xf1, 0xf4,
    0xf5,  0xf8,  0xf9,  0xfc,  0xfd,  0x100, 0x101, 0x11c,
};

int iip_unit_at_block(uint32_t off)
{
    if (off < IIP_UNIT_START_REG || (off & 0x1f) || (off - IIP_UNIT_START_REG) / 0x20 >= sizeof(block_owner)) {
        return -1;
    }
    return block_owner[(off - IIP_UNIT_START_REG) / 0x20] ? block_owner[(off - IIP_UNIT_START_REG) / 0x20] : -1;
}

uint32_t iip_unit_block(unsigned id)
{
    for (unsigned k = 0; id && k < sizeof(block_owner); k++) {
        if (block_owner[k] == id) {
            return IIP_UNIT_START_REG + 0x20 * k;
        }
    }
    return 0;
}

int iip_unit_class(unsigned id)
{
    static const struct {
        uint8_t lo, hi, cls;
    } r[] = {
        { 0x00, 0x0f, 0x0 }, { 0x10, 0x17, 0x1 }, { 0x18, 0x1b, 0x2 },  { 0x1c, 0x21, 0x3 },  { 0x22, 0x22, 0x12 },
        { 0x23, 0x23, 0x5 }, { 0x24, 0x24, 0x6 }, { 0x25, 0x27, 0x4 },  { 0x28, 0x28, 0x7 },  { 0x29, 0x2c, 0x8 },
        { 0x2d, 0x30, 0x9 }, { 0x31, 0x38, 0xa }, { 0x39, 0x40, 0xb },  { 0x41, 0x41, 0xc },  { 0x42, 0x43, 0xd },
        { 0x44, 0x44, 0xe }, { 0x45, 0x46, 0xf }, { 0x47, 0x47, 0x10 }, { 0x48, 0x4b, 0x11 },
    };
    for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
        if (id >= r[i].lo && id <= r[i].hi) {
            return r[i].cls;
        }
    }
    return -1;
}

int iip_port_owner(unsigned pid, unsigned *port)
{
    if (pid >= sizeof(port_owner) / sizeof(port_owner[0]) || !port_owner[pid]) {
        return -1;
    }
    *port = port_owner[pid] & 3;
    return port_owner[pid] >> 2;
}

int iip_unit_in_masks(unsigned id, uint32_t a, uint32_t b)
{
    const uint32_t blk = iip_unit_block(id);
    if (!blk) {
        return 0;
    }
    const unsigned k = (blk - IIP_UNIT_START_REG) / 0x20;
    return k < 32 ? (a >> k) & 1 : (b >> (k - 32)) & 1;
}

int iip_unit_regs_load(IipUnitRegs *u, uint64_t list, MipuMem mem)
{
    memset(u, 0, sizeof(*u));
    if (list < 0x80000000u) {
        return -1;
    }
    for (int k = 0; k < 32; k++) {
        uint32_t h;
        mem.read(mem.opaque, list, &h, sizeof(h));
        unsigned off = h & 0xffff, n = (h >> 16) & 0xff, more = (h >> 24) & 0x10;
        if (!n || (off & 3) || off + 4 * (n - 1) > IIP_UREGS) {
            return -1;
        }
        mem.read(mem.opaque, list + 4, &u->r[off / 4], 4 * (n - 1));
        list += 4 * n;
        if (!more) {
            return 0;
        }
    }
    return -1;
}

#define EV_HOST     0x4000000000ull
#define EV_SLOT     0x100000000ull
#define EV_BUFS     32
#define STREAM_PAGE 0x80000000u

struct IipEval {
    const uint32_t *regs;
    const uint8_t *units;
    const uint32_t *slots;
    MipuMem guest;
    struct {
        uint8_t *p;
        size_t len;
        int own;
    } buf[EV_BUFS];
    int nbuf;
    IipImage stream;
    int done[IIP_UNIT_IDS];
    IipImage img[IIP_UNIT_IDS];
    unsigned written;
    const char *why;
    char why_buf[160];
};

static int ev_fail(IipEval *ev, const char *why)
{
    if (!ev->why) {
        ev->why = why;
    }
    return -1;
}

static void ev_read(void *opaque, uint64_t addr, void *b, size_t len)
{
    IipEval *ev = opaque;
    if (addr >= EV_HOST) {
        unsigned k = (addr - EV_HOST) / EV_SLOT;
        uint64_t off = (addr - EV_HOST) % EV_SLOT;
        if (k < (unsigned)ev->nbuf && off + len <= ev->buf[k].len) {
            memcpy(b, ev->buf[k].p + off, len);
        } else {
            memset(b, 0, len);
        }
        return;
    }
    ev->guest.read(ev->guest.opaque, addr, b, len);
}

static void ev_write(void *opaque, uint64_t addr, const void *b, size_t len)
{
    IipEval *ev = opaque;
    if (addr >= EV_HOST) {
        unsigned k = (addr - EV_HOST) / EV_SLOT;
        uint64_t off = (addr - EV_HOST) % EV_SLOT;
        if (k < (unsigned)ev->nbuf && ev->buf[k].own && off + len <= ev->buf[k].len) {
            memcpy(ev->buf[k].p + off, b, len);
        }
        return;
    }
    ev->guest.write(ev->guest.opaque, addr, b, len);
}

static MipuMem ev_mem(IipEval *ev)
{
    return (MipuMem){ ev, ev_read, ev_write };
}

#define EV_MAX_BYTES (64u << 20)

static uint64_t ev_buf(IipEval *ev, uint8_t *p, size_t len)
{
    if (len > EV_MAX_BYTES) {
        fprintf(stderr, "iip model: a %zu-byte image refused (over %u)\n", len, EV_MAX_BYTES);
        return 0;
    }
    uint8_t *q = p ? p : calloc(1, len);
    if (ev->nbuf == EV_BUFS || !q) {
        if (!p) {
            free(q);
        }
        ev_fail(ev, ev->nbuf == EV_BUFS ? "out of host image slots" : "no memory for a host image");
        return 0;
    }
    int k = ev->nbuf++;
    ev->buf[k].own = !p;
    ev->buf[k].p = q;
    ev->buf[k].len = len;
    return EV_HOST + (uint64_t)k * EV_SLOT;
}

static int ev_image(IipEval *ev, IipImage *im, uint32_t w, uint32_t h, int chroma)
{
    memset(im, 0, sizeof(*im));
    im->w = w;
    im->h = h;
    im->ypitch = w;
    im->y = ev_buf(ev, NULL, (size_t)w * h);
    if (chroma) {
        im->cpitch = w;
        im->c = ev_buf(ev, NULL, (size_t)w * (h / 2));
        if (im->c) {
            for (int k = 0; k < ev->nbuf; k++) {
                if (EV_HOST + (uint64_t)k * EV_SLOT == im->c) {
                    memset(ev->buf[k].p, 128, ev->buf[k].len);
                }
            }
        }
    }
    return im->y && (!chroma || im->c) ? 0 : -1;
}

IipEval *iip_eval_new(const uint32_t *regs, const uint8_t *units, const uint32_t *slots, MipuMem mem)
{
    IipEval *ev = calloc(1, sizeof(*ev));
    ev->regs = regs;
    ev->units = units;
    ev->slots = slots;
    ev->guest = mem;
    return ev;
}

void iip_eval_stream(IipEval *ev, const uint8_t *y, const uint8_t *c, uint32_t w, uint32_t h)
{
    IipImage *s = &ev->stream;
    memset(s, 0, sizeof(*s));
    s->y = ev_buf(ev, (uint8_t *)y, (size_t)w * h);
    s->c = c ? ev_buf(ev, (uint8_t *)c, (size_t)w * (h / 2)) : 0;
    s->ypitch = s->cpitch = w;
    s->w = s->y ? w : 0;
    s->h = h;
}

void iip_eval_free(IipEval *ev)
{
    for (int k = 0; k < ev->nbuf; k++) {
        if (ev->buf[k].own) {
            free(ev->buf[k].p);
        }
    }
    free(ev);
}

static void path_add(IipOutPath *p, const char *fmt, unsigned v)
{
    size_t n = strlen(p->path);
    if (n + 8 < sizeof(p->path)) {
        snprintf(p->path + n, sizeof(p->path) - n, fmt, v);
    }
}

static int ev_window(IipEval *ev, unsigned n, IipImage *im, int *stream)
{
    const uint32_t *r = ev->regs;
    *stream = 0;
    memset(im, 0, sizeof(*im));
    if (n >= IIP_WIN_EN_INST || !IIP_GETI(r, WIN_EN, n)) {
        return ev_fail(ev, "a window that is not enabled");
    }
    uint32_t b = ev->slots ? ev->slots[n] : IIP_GETI(r, WIN_ADDR0, n);
    if (ev->slots && (b & 0xfff00000u) == STREAM_PAGE) {
        if (!ev->stream.w) {
            return ev_fail(ev, "the stream window with no stream");
        }
        *im = ev->stream;
        *stream = 1;
        return 0;
    }
    im->ypitch = IIP_GETI(r, WIN_PITCH0, n);
    im->cpitch = IIP_GETI(r, WIN_PITCH1, n);
    im->w = IIP_GETI(r, WIN_W, n);
    im->h = IIP_GETI(r, WIN_H, n);
    im->y = b;
    uint32_t a1 = IIP_GETI(r, WIN_ADDR1, n);
    im->c = !im->cpitch ? 0 : ev->slots ? (uint64_t)b + a1 : a1;
    if (b < 0x80000000u || (b & 0x7f) || im->w < 2 || im->h < 2 || im->ypitch < im->w ||
        (im->c && (im->cpitch < im->w || im->c < 0x80000000u))) {
        if (!ev->why) {
            snprintf(ev->why_buf, sizeof(ev->why_buf),
                     "window %u outside DRAM or implausible: plane 0 %#" PRIx64 " pitch %u, plane 1 %#" PRIx64
                     " pitch %u, %ux%u",
                     n, im->y, im->ypitch, im->c, im->cpitch, im->w, im->h);
        }
        return ev_fail(ev, ev->why_buf);
    }
    return 0;
}

static int render(IipEval *ev, unsigned id, IipOutPath *p, int dry, int depth, uint32_t ww, uint32_t wh);

static uint64_t iip_unit_list(const uint32_t *regs, uint32_t blk)
{
    return regs[(blk + IIP_UNIT_LIST_OFF) / 4];
}

static int render_window(IipEval *ev, unsigned n, IipOutPath *p, IipImage *im)
{
    int stream;
    path_add(p, "w%u", n);
    if (ev_window(ev, n, im, &stream)) {
        return -1;
    }
    p->src_win = n;
    p->stream = stream;
    p->src_addr = im->y;
    return 0;
}

static int producer_of(IipEval *ev, unsigned pid)
{
    unsigned port;
    int id = iip_port_owner(pid, &port);
    return id >= 0 && ev->units[id] ? id : -1;
}

static IipImage fit_view(IipImage in, uint32_t w, uint32_t h)
{
    uint32_t k = in.w / w < in.h / h ? in.w / w : in.h / h;
    if (k >= 1 && 2 * (in.w - k * w) < w && 2 * (in.h - k * h) < h) {
        in.w = k * w;
        in.h = k * h;
    }
    return in;
}

static int source_of(const IipUnitRegs *u)
{
    unsigned n = (u->r[IIP_U_SRC / 4] >> 4) & 0xf;
    return !(u->r[IIP_U_SRC_KIND / 4] & IIP_U_SRC_INTERNAL) || n >= 8 ? (int)n : (int)(0x10 + n);
}

static int render_unit(IipEval *ev, unsigned id, IipOutPath *p, int dry, int depth, IipImage *out, uint32_t ww,
                       uint32_t wh)
{
    MipuMem mem = ev_mem(ev);
    IipUnitRegs u;
    int cls = iip_unit_class(id);
    if (cls == 0) {
        return render_window(ev, id, p, out);
    }
    path_add(p, "%02x<", id);
    if (cls == 1) {
        unsigned j = id - 0x10;
        uint32_t rw = IIP_GETI(ev->regs, READER_W, j), rh = IIP_GETI(ev->regs, READER_H, j);
        if (!ev->units[0x39 + j] || render(ev, 0x39 + j, p, dry, depth + 1, rw, rh)) {
            return -1;
        }
        *out = ev->img[0x39 + j];
        if (rw < 2 || rh < 2 || (rw == out->w && rh == out->h)) {
            return 0;
        }
        if (dry) {
            out->w = rw;
            out->h = rh;
            return 0;
        }
        IipImage in = fit_view(*out, rw, rh);
        if (ev_image(ev, out, rw, rh, in.c != 0)) {
            return -1;
        }
        return iip_model_resample(&in, out, mem);
    }
    uint32_t blk = iip_unit_block(id);
    if (!blk || iip_unit_regs_load(&u, iip_unit_list(ev->regs, blk), ev->guest)) {
        return ev_fail(ev, "a unit whose command list does not load");
    }
    int src;
    switch (cls) {
    case 5:
    case 6:
        src = 8 + (u.r[0] & 7);
        break;
    case 8:
    case 9:
        src = source_of(&u);
        path_add(p, "(%x)", u.r[IIP_U_SRC_KIND / 4]);
        break;
    case 0xb:
        src = producer_of(ev, IIP_U_IN_PORT(&u));
        break;
    case 0x10:
    case 0x11:
        src = producer_of(ev, IIP_U_IN_PORT(&u));
        break;
    default:
        path_add(p, "%02x?", id);
        return ev_fail(ev, "a unit the model does not have");
    }
    if (src < 0) {
        return ev_fail(ev, "a unit input no unit of this job feeds");
    }
    if (render(ev, src, p, dry, depth + 1, cls == 8 ? 0 : ww, cls == 8 ? 0 : wh)) {
        return -1;
    }
    IipImage in = ev->img[src];
    *out = in;
    if (cls == 8) {
        IipScale sc;
        if (iip_scale_from_desc(&sc, u.r[IIP_U_SCALE_DESC / 4], ev->guest)) {
            return 0;
        }
        uint32_t ow = ww ? ww : sc.ow, oh = wh ? wh : sc.oh;
        if (dry) {
            out->w = ow;
            out->h = oh;
            return 0;
        }
        if (ev_image(ev, out, ow, oh, in.c != 0)) {
            return -1;
        }
        if (iip_model_resample_scaled(&in, out, &sc, mem)) {
            return ev_fail(ev, "a resize refused (its factors)");
        }
    } else if (cls == 9 && u.r[IIP_U_GRID_ADDR / 4]) {
        IipGrid g = { u.r[IIP_U_GRID_ADDR / 4], u.r[IIP_U_GRID_DIMS / 4] & 0xffff,
                      u.r[IIP_U_GRID_DIMS / 4] >> 16 };
        uint32_t gw, gh;
        if (g.addr < 0x80000000u || !g.cols || !g.rows || g.cols > 256 || g.rows > 256 ||
            iip_grid_extent(&g, &gw, &gh, ev->guest)) {
            return 0;
        }
        if (dry) {
            out->w = gw;
            out->h = gh;
            return 0;
        }
        if (ev_image(ev, out, gw, gh, in.c != 0) || iip_model_warp(&in, out, &g, mem) < 0) {
            *out = in;
        }
    }
    return 0;
}

static int render(IipEval *ev, unsigned id, IipOutPath *p, int dry, int depth, uint32_t ww, uint32_t wh)
{
    if (id >= IIP_UNIT_IDS || depth > 24) {
        return ev_fail(ev, "a path deeper than 24 units");
    }
    if (ev->done[id]) {
        path_add(p, "%02x*", id);
        return ev->done[id] > 0 ? 0 : ev_fail(ev, "a unit that failed earlier in this job");
    }
    int r = render_unit(ev, id, p, dry, depth, &ev->img[id], ww, wh);
    if (!dry) {
        ev->done[id] = r ? -1 : 1;
    }
    return r;
}

static int out_unit(IipEval *ev, unsigned out, IipOutPath *p, int dry)
{
    IipUnitRegs u;
    int stream;
    memset(p, 0, sizeof(*p));
    p->win = p->src_win = -1;
    ev->why = NULL;
    uint32_t blk = iip_unit_block(out);
    if (iip_unit_class(out) != 0xa || iip_unit_regs_load(&u, iip_unit_list(ev->regs, blk), ev->guest)) {
        return ev_fail(ev, "an output unit whose command list does not load");
    }
    unsigned win = u.r[IIP_U_OUT_WIN / 4] & 0xf;
    if (ev_window(ev, win, &p->dst, &stream)) {
        return -1;
    }
    if (stream) {
        return ev_fail(ev, "an output into the stream window");
    }
    p->win = win;
    path_add(p, "w%u<", win);
    path_add(p, "%02x<", out);
    int src = producer_of(ev, IIP_U_IN_PORT(&u));
    if (src < 0) {
        return ev_fail(ev, "an output unit no unit of this job feeds");
    }
    if (dry) {
        IipImage im;
        return render_unit(ev, src, p, 1, 0, &im, p->dst.w, p->dst.h) ? -1 : src;
    }
    return render(ev, src, p, 0, 0, p->dst.w, p->dst.h) ? -1 : src;
}

int iip_out_trace(IipEval *ev, unsigned out, IipOutPath *p)
{
    return out_unit(ev, out, p, 1) < 0 ? -1 : 0;
}

const char *iip_eval_why(const IipEval *ev)
{
    return ev->why ? ev->why : "no reason recorded";
}

int iip_out_run(IipEval *ev, unsigned out, IipOutPath *p)
{
    int src = out_unit(ev, out, p, 0);
    if (src < 0) {
        return -1;
    }
    if (ev->written & (1u << p->win)) {
        return 0;
    }
    ev->written |= 1u << p->win;
    IipImage in = fit_view(ev->img[src], p->dst.w, p->dst.h);
    return iip_model_resample(&in, &p->dst, ev_mem(ev));
}
