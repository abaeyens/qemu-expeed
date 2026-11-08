/*
 * GEVG (DMP SMAPH-F) command list interpreter
 *
 * No QEMU in here
 *
 * References:
 * - ShivaVG, the OpenVG renderer whose output this model was compared with.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gevg_model.h"

#define GEVG_TEX_YUV420SP 0x0d

#define GMIN(a, b) ((a) < (b) ? (a) : (b))
#define GMAX(a, b) ((a) > (b) ? (a) : (b))

void gevg_model_init(GevgModel *m, GevgMem mem)
{
    memset(m, 0, sizeof(*m));
    m->mem = mem;
}

void gevg_model_free(GevgModel *m)
{
    for (int i = 0; i < GEVG_MODEL_MASKS; i++) {
        free(m->masks[i].cov);
        m->masks[i].cov = NULL;
        m->masks[i].cap = 0;
    }
    for (int i = 0; i < GEVG_MODEL_CACHE; i++) {
        free(m->cache[i].rec);
        m->cache[i].rec = NULL;
    }
}

uint32_t gevg_model_read(GevgModel *m, uint32_t off)
{
    if (off == GEVG_OFF_STATUS) {
        return 0;
    }
    return (off / 4 < GEVG_MODEL_REGS) ? m->reg[off / 4] : 0;
}

static int gevg_mask_reset(GevgMask *k, uint32_t w, uint32_t h)
{
    size_t need = (size_t)w * h;
    if (k->cov && need <= k->cap) {
        for (uint32_t y = k->y0; k->x1 > k->x0 && y < k->y1; y++) {
            memset(k->cov + (size_t)y * k->w + k->x0, 0, k->x1 - k->x0);
        }
    } else {
        free(k->cov);
        k->cov = calloc(need, 1);
        k->cap = k->cov ? need : 0;
    }
    k->w = w;
    k->h = h;
    k->x0 = k->y0 = k->x1 = k->y1 = 0;
    return k->cov != NULL;
}

static GevgMask *gevg_mask_get(GevgModel *m, uint32_t addr, uint32_t w, uint32_t h, int create)
{
    GevgMask *victim = &m->masks[0];
    for (int i = 0; i < GEVG_MODEL_MASKS; i++) {
        GevgMask *k = &m->masks[i];
        if (k->addr == addr && k->cov) {
            if (create && (k->w != w || k->h != h) && !gevg_mask_reset(k, w, h)) {
                return NULL;
            }
            k->stamp = ++m->stamp;
            return k;
        }
        if (!k->cov) {
            victim = k;
        } else if (k->stamp < victim->stamp && victim->cov) {
            victim = k;
        }
    }
    if (!create) {
        return NULL;
    }
    if (!gevg_mask_reset(victim, w, h)) {
        return NULL;
    }
    victim->addr = addr;
    victim->stamp = ++m->stamp;
    return victim;
}

typedef struct {
    float x0, y0, x1, y1;
} GevgEdge;
typedef struct {
    float x;
    int dir;
} GevgCrossing;

#define GEVG_EDGE_START   1
#define GEVG_EDGE_CLOSING 2

typedef struct {
    GevgEdge e[GEVG_MODEL_MAX_EDGES];
    uint8_t flags[GEVG_MODEL_MAX_EDGES];
    int n;
    float cx, cy, sx, sy;
    int open, first, stroke;
} GevgPoly;

static void poly_line(GevgPoly *p, float x, float y)
{
    if (x == p->cx && y == p->cy) return;
    if (p->n < GEVG_MODEL_MAX_EDGES) {
        p->flags[p->n] = p->first ? GEVG_EDGE_START : 0;
        p->e[p->n++] = (GevgEdge){ p->cx, p->cy, x, y };
    }
    p->first = 0;
    p->cx = x;
    p->cy = y;
}

static void poly_close(GevgPoly *p, int explicit)
{
    if (p->open && (explicit || !p->stroke) && (p->cx != p->sx || p->cy != p->sy)) {
        poly_line(p, p->sx, p->sy);
        if (explicit && p->n) p->flags[p->n - 1] |= GEVG_EDGE_CLOSING;
    }
    p->open = 0;
    p->cx = p->sx;
    p->cy = p->sy;
    p->first = 1;
}

static int gevg_build_poly(GevgPoly *p, const uint8_t *seg, uint32_t nseg, const float *coord, uint32_t ncoord,
                           int stroke)
{
    uint32_t c = 0, s;
    p->n = 0;
    p->open = 0;
    p->first = 1;
    p->stroke = stroke;
    p->cx = p->cy = p->sx = p->sy = 0;
    for (s = 0; s < nseg; s++) {
        uint32_t op = seg[s] & 0x1e, rel = seg[s] & 1;
        float ox = rel ? p->cx : 0, oy = rel ? p->cy : 0;
        switch (op) {
        case 0:                                /* VG_CLOSE_PATH */
            poly_close(p, 1);
            break;
        case 2:                                /* VG_MOVE_TO */
            if (c + 2 > ncoord) return s;
            poly_close(p, 0);
            p->cx = p->sx = coord[c] + ox;
            p->cy = p->sy = coord[c + 1] + oy;
            p->open = 1;
            p->first = 1;
            c += 2;
            break;
        case 4:                                 /* VG_LINE_TO */
            if (c + 2 > ncoord) return s;
            poly_line(p, coord[c] + ox, coord[c + 1] + oy);
            c += 2;
            break;
        case 6:                                 /* VG_HLINE_TO */
            if (c + 1 > ncoord) return s;
            poly_line(p, coord[c] + ox, p->cy);
            c += 1;
            break;
        case 8:                                 /* VG_VLINE_TO */
            if (c + 1 > ncoord) return s;
            poly_line(p, p->cx, coord[c] + oy);
            c += 1;
            break;
        case 10: {                              /* VG_QUAD_TO: flattened in GEVG_MODEL_CURVE_STEPS */
            if (c + 4 > ncoord) return s;
            float x0 = p->cx, y0 = p->cy, x1 = coord[c] + ox, y1 = coord[c + 1] + oy;
            float x2 = coord[c + 2] + ox, y2 = coord[c + 3] + oy;
            for (int k = 1; k <= GEVG_MODEL_CURVE_STEPS; k++) {
                float t = (float)k / GEVG_MODEL_CURVE_STEPS, u = 1 - t;
                poly_line(p, u * u * x0 + 2 * u * t * x1 + t * t * x2, u * u * y0 + 2 * u * t * y1 + t * t * y2);
            }
            c += 4;
            break;
        }
        case 12: {                              /* VG_CUBIC_TO */
            if (c + 6 > ncoord) return s;
            float x0 = p->cx, y0 = p->cy, x1 = coord[c] + ox, y1 = coord[c + 1] + oy;
            float x2 = coord[c + 2] + ox, y2 = coord[c + 3] + oy;
            float x3 = coord[c + 4] + ox, y3 = coord[c + 5] + oy;
            for (int k = 1; k <= GEVG_MODEL_CURVE_STEPS; k++) {
                float t = (float)k / GEVG_MODEL_CURVE_STEPS, u = 1 - t;
                poly_line(p, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                          u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
            }
            c += 6;
            break;
        }
        default:
            poly_close(p, 0);
            return s;
        }
        if (op != 0 && op != 2) p->open = 1;
    }
    poly_close(p, 0);
    return s;
}

static void gevg_rasterise(GevgMask *k, const GevgPoly *p, const float *m3, float scale)
{
    GevgEdge e[GEVG_MODEL_MAX_EDGES];
    float ymin = 1e30f, ymax = -1e30f;
    for (int i = 0; i < p->n; i++) {
        const GevgEdge *s = &p->e[i];
        e[i].x0 = (m3[0] * s->x0 + m3[3] * s->y0 + m3[6]) * scale;
        e[i].y0 = (m3[1] * s->x0 + m3[4] * s->y0 + m3[7]) * scale;
        e[i].x1 = (m3[0] * s->x1 + m3[3] * s->y1 + m3[6]) * scale;
        e[i].y1 = (m3[1] * s->x1 + m3[4] * s->y1 + m3[7]) * scale;
        ymin = GMIN(ymin, GMIN(e[i].y0, e[i].y1));
        ymax = GMAX(ymax, GMAX(e[i].y0, e[i].y1));
    }
    int32_t ya = GMAX((int32_t)ymin, 0), yb = GMIN((int32_t)ymax + 1, (int32_t)k->h);
    for (int32_t y = ya; y < yb; y++) {
        float sy = y + 0.5f;
        GevgCrossing xs[GEVG_MODEL_MAX_EDGES];
        int nx = 0;
        for (int i = 0; i < p->n; i++) {
            float y0 = e[i].y0, y1 = e[i].y1;
            if (y0 == y1) continue;
            int dir = y1 > y0 ? 1 : -1;
            float top = GMIN(y0, y1), bot = GMAX(y0, y1);
            if (sy < top || sy >= bot) continue;
            xs[nx].x = e[i].x0 + (sy - y0) * (e[i].x1 - e[i].x0) / (y1 - y0);
            xs[nx].dir = dir;
            nx++;
        }
        for (int i = 1; i < nx; i++) {
            int j = i;
            while (j > 0 && xs[j - 1].x > xs[j].x) {
                GevgCrossing t = xs[j];
                xs[j] = xs[j - 1];
                xs[j - 1] = t;
                j--;
            }
        }
        int wind = 0;
        uint8_t *row = k->cov + (size_t)y * k->w;
        for (int i = 0; i + 1 < nx; i++) {
            wind += xs[i].dir;
            if (wind == 0) continue;
            int32_t xa = (int32_t)(xs[i].x + 0.5f), xb = (int32_t)(xs[i + 1].x + 0.5f);
            xa = GMAX(xa, 0);
            xb = GMIN(xb, (int32_t)k->w);
            if (xb > xa) {
                memset(row + xa, 0xff, xb - xa);
                if (k->x1 <= k->x0) {
                    k->x0 = xa;
                    k->x1 = xb;
                    k->y0 = y;
                    k->y1 = y + 1;
                } else {
                    k->x0 = GMIN(k->x0, (uint32_t)xa);
                    k->x1 = GMAX(k->x1, (uint32_t)xb);
                    k->y0 = GMIN(k->y0, (uint32_t)y);
                    k->y1 = GMAX(k->y1, (uint32_t)y + 1);
                }
            }
        }
    }
}

static float gevg_f32(uint32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float gevg_f24(uint32_t v)
{
    if (!(v & 0xffffff)) return 0.0f;
    float mant = 1.0f + (v & 0xffff) / 65536.0f;
    int e = (int)((v >> 16) & 0x7f) - 63;
    float r = mant * (e >= 0 ? (float)(1u << e) : 1.0f / (float)(1u << -e));
    return (v & 0x800000) ? -r : r;
}

static void gevg_circle(GevgMask *k, GevgPoly *q, const float *m3, float scale, float cx, float cy, float r)
{
    const int n = 32;
    q->n = 0;
    for (int i = 0; i < n; i++) {
        float a0 = 2 * 3.14159265f * i / n, a1 = 2 * 3.14159265f * (i + 1) / n;
        q->e[q->n++] = (GevgEdge){ cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1) };
    }
    gevg_rasterise(k, q, m3, scale);
}

static void gevg_quad(GevgMask *k, GevgPoly *q, const float *m3, float scale, float ax, float ay, float bx,
                      float by, float cx, float cy, float dx, float dy)
{
    q->n = 4;
    q->e[0] = (GevgEdge){ ax, ay, bx, by };
    q->e[1] = (GevgEdge){ bx, by, cx, cy };
    q->e[2] = (GevgEdge){ cx, cy, dx, dy };
    q->e[3] = (GevgEdge){ dx, dy, ax, ay };
    gevg_rasterise(k, q, m3, scale);
}

static void gevg_stroke(GevgMask *k, const GevgPoly *p, const float *m3, float scale, float hw, float cut, int cap,
                        int join)
{
    GevgPoly *q = malloc(sizeof *q);
    if (!q || hw <= 0) {
        free(q);
        return;
    }
    int sub_start = 0;
    for (int i = 0; i < p->n; i++) {
        const GevgEdge *e = &p->e[i];
        if (p->flags[i] & GEVG_EDGE_START) sub_start = i;
        int closed = 0;
        for (int j = sub_start; j < p->n && (j == sub_start || !(p->flags[j] & GEVG_EDGE_START)); j++) {
            if (p->flags[j] & GEVG_EDGE_CLOSING) closed = 1;
        }
        float dx = e->x1 - e->x0, dy = e->y1 - e->y0, len = sqrtf(dx * dx + dy * dy);
        if (len <= 0) continue;
        float ux = dx / len, uy = dy / len, nx = -uy * hw, ny = ux * hw;
        gevg_quad(k, q, m3, scale, e->x0 + nx, e->y0 + ny, e->x1 + nx, e->y1 + ny, e->x1 - nx, e->y1 - ny,
                  e->x0 - nx, e->y0 - ny);
        const GevgEdge *f = NULL;
        if (p->flags[i] & GEVG_EDGE_CLOSING) {
            f = &p->e[sub_start];
        } else if (i + 1 < p->n && !(p->flags[i + 1] & GEVG_EDGE_START)) {
            f = &p->e[i + 1];
        }
        if (!f && !closed) {
            if (cap == 1) gevg_circle(k, q, m3, scale, e->x1, e->y1, hw);
            if (cap == 2)
                gevg_quad(k, q, m3, scale, e->x1 + nx, e->y1 + ny, e->x1 + nx + ux * hw, e->y1 + ny + uy * hw,
                          e->x1 - nx + ux * hw, e->y1 - ny + uy * hw, e->x1 - nx, e->y1 - ny);
        }
        if ((p->flags[i] & GEVG_EDGE_START) && !closed) {
            if (cap == 1) gevg_circle(k, q, m3, scale, e->x0, e->y0, hw);
            if (cap == 2)
                gevg_quad(k, q, m3, scale, e->x0 - nx, e->y0 - ny, e->x0 - nx - ux * hw, e->y0 - ny - uy * hw,
                          e->x0 + nx - ux * hw, e->y0 + ny - uy * hw, e->x0 + nx, e->y0 + ny);
        }
        if (!f) continue;
        float fx = f->x1 - f->x0, fy = f->y1 - f->y0, flen = sqrtf(fx * fx + fy * fy);
        if (flen <= 0) continue;
        float vx = fx / flen, vy = fy / flen;
        float n1x = -uy, n1y = ux, n2x = -vy, n2y = vx, dot = n1x * n2x + n1y * n2y;
        if (dot > 0.9999f) continue;
        float px = e->x1, py = e->y1;
        if (join == 1) {
            gevg_circle(k, q, m3, scale, px, py, hw);
            continue;
        }
        for (int side = -1; side <= 1; side += 2) {
            float ax = px + side * hw * n1x, ay = py + side * hw * n1y;
            float bx = px + side * hw * n2x, by = py + side * hw * n2y;
            float mlen = hw * sqrtf(2.0f / (1.0f + dot));
            if (join == 0 && dot > -0.9999f && mlen * mlen <= hw * hw + cut * cut) {
                float tx = px + side * hw * (n1x + n2x) / (1.0f + dot);
                float ty = py + side * hw * (n1y + n2y) / (1.0f + dot);
                gevg_quad(k, q, m3, scale, px, py, ax, ay, tx, ty, bx, by);
            } else {
                q->n = 3;
                q->e[0] = (GevgEdge){ px, py, ax, ay };
                q->e[1] = (GevgEdge){ ax, ay, bx, by };
                q->e[2] = (GevgEdge){ bx, by, px, py };
                gevg_rasterise(k, q, m3, scale);
            }
        }
    }
    free(q);
}

static void gevg_gp_draw(GevgModel *m, const uint8_t *seg, uint32_t nseg, const float *coord, uint32_t ncoord)
{
    uint32_t addr = m->vreg[0x080], fmt = m->vreg[0x082], size = m->vreg[0x083];
    uint32_t w = size & 0xffff, h = size >> 16;
    float scale = (fmt == 0x200) ? 4.0f : 1.0f;
    if (!addr || !w || !h || w > 8192 || h > 8192) {
        return;
    }
    GevgMask *k = gevg_mask_get(m, addr, w, h, 1);
    if (!k) return;
    GevgPoly *p = malloc(sizeof *p);
    if (!p) return;
    int stroke = !(m->vreg[0x065] & 0x100);
    gevg_build_poly(p, seg, nseg, coord, ncoord, stroke);
    for (uint32_t y = k->y0; k->x1 > k->x0 && y < k->y1; y++) {
        memset(k->cov + (size_t)y * w + k->x0, 0, k->x1 - k->x0);
    }
    k->x0 = k->y0 = k->x1 = k->y1 = 0;
    if (stroke) {
        float hw = gevg_f32(m->vreg[0x05c]), cut = gevg_f32(m->vreg[0x05d]);
        gevg_stroke(k, p, m->matrix, scale, hw, cut, (m->vreg[0x065] >> 16) & 0xf, (m->vreg[0x065] >> 20) & 0xf);
    } else {
        gevg_rasterise(k, p, m->matrix, scale);
    }
    free(p);
}

static void gevg_gp_draw_external(GevgModel *m)
{
    uint32_t seg_addr = m->vreg[0x002], nseg = m->vreg[0x003] & 0xffff;
    uint32_t co_addr = m->vreg[0x004], nco = m->vreg[0x005] & 0xffff;
    uint8_t seg[256];
    uint32_t raw[GEVG_MODEL_MAX_COORDS];
    float coord[GEVG_MODEL_MAX_COORDS];
    if (!seg_addr || !co_addr || !nseg || !nco) return;
    nseg = GMIN(nseg, 256u);
    nco = GMIN(nco, (uint32_t)GEVG_MODEL_MAX_COORDS);
    m->mem.read(m->mem.opaque, seg_addr, seg, nseg);
    m->mem.read(m->mem.opaque, co_addr, raw, nco * 4);
    for (uint32_t i = 0; i < nco; i++) {
        if ((m->vreg[0x005] >> 28) == 0xd) {
            memcpy(&coord[i], &raw[i], 4);
        } else {
            coord[i] = (float)(int32_t)raw[i];
        }
    }
    gevg_gp_draw(m, seg, nseg, coord, nco);
    m->vreg[0x002] = m->vreg[0x004] = 0;
}

#define GEVG_DST_XRGB   0x000
#define GEVG_DST_YUVA   0x107
#define GEVG_DST_YUV420 0x105

static void gevg_hash(uint64_t h[2], const void *buf, size_t len)
{
    const uint8_t *p = buf;
    h[0] ^= len;
    h[1] += len;
    for (; len >= 8; len -= 8, p += 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        h[0] = (h[0] ^ v) * 0x9e3779b97f4a7c15ull;
        h[0] ^= h[0] >> 29;
        h[1] = (h[1] + v) * 0xc2b2ae3d27d4eb4full;
        h[1] ^= h[1] >> 31;
    }
    for (; len; len--, p++) {
        h[0] = (h[0] ^ *p) * 0x9e3779b97f4a7c15ull;
        h[1] = (h[1] + *p) * 0xc2b2ae3d27d4eb4full;
    }
}

static void gevg_dst_mem_write(GevgModel *m, uint32_t addr, const void *buf, uint32_t len)
{
    m->mem.write(m->mem.opaque, addr, buf, len);
    GevgCacheEntry *e = m->recording;
    if (!e) return;
    if (e->len + 8 + len > e->cap) {
        size_t cap = GMAX(e->cap * 2, e->len + 8 + len);
        uint8_t *r = realloc(e->rec, cap);
        if (!r) {
            m->recording = NULL;
            e->len = 0;
            return;
        }
        e->rec = r;
        e->cap = cap;
    }
    uint32_t rel = addr - m->recording_base;
    memcpy(e->rec + e->len, &rel, 4);
    memcpy(e->rec + e->len + 4, &len, 4);
    memcpy(e->rec + e->len + 8, buf, len);
    e->len += 8 + len;
}

static inline uint8_t gevg_clip8(float v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5f);
}

static inline uint32_t gevg_yuv_to_argb(uint8_t a, float y, float cb, float cr)
{
    return ((uint32_t)a << 24) | ((uint32_t)gevg_clip8(y + 1.402f * cr) << 16) |
           ((uint32_t)gevg_clip8(y - 0.344136f * cb - 0.714136f * cr) << 8) | gevg_clip8(y + 1.772f * cb);
}

static inline uint32_t gevg_swap_rb(uint32_t p)
{
    return (p & 0xff00ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
}

static inline float gevg_luma(uint32_t p)
{
    return 0.299f * ((p >> 16) & 0xff) + 0.587f * ((p >> 8) & 0xff) + 0.114f * (p & 0xff);
}

static void gevg_dst_read(GevgModel *m, uint32_t fmt, uint32_t dst, uint32_t dw, uint32_t y, uint32_t n,
                          uint32_t *row)
{
    uint32_t second = m->vreg[0x1b5];
    if (fmt == GEVG_DST_YUVA) {
        uint8_t c[2 * 8192], a[8192];
        m->mem.read(m->mem.opaque, dst + (size_t)y * dw * 2, c, n * 2);
        m->mem.read(m->mem.opaque, second + (size_t)y * dw, a, n);
        for (uint32_t x = 0; x < n; x++) {
            const uint8_t *q = c + (x & ~1u) * 2;
            row[x] = gevg_yuv_to_argb(a[x], q[x & 1], q[2] - 128.0f, q[3] - 128.0f);
        }
    } else if (fmt == GEVG_DST_YUV420) {
        uint8_t yy[8192], cc[8192];
        m->mem.read(m->mem.opaque, dst + (size_t)y * dw, yy, n);
        m->mem.read(m->mem.opaque, second + (size_t)(y / 2) * dw, cc, n);
        for (uint32_t x = 0; x < n; x++) {
            row[x] = gevg_yuv_to_argb(0xff, yy[x], cc[x & ~1u] - 128.0f, cc[(x & ~1u) + 1] - 128.0f);
        }
    } else {
        m->mem.read(m->mem.opaque, dst + (size_t)y * dw * 4, row, n * 4);
        for (uint32_t x = 0; x < n; x++) row[x] = gevg_swap_rb(row[x]);
    }
}

static void gevg_dst_hash_row(GevgModel *m, uint32_t fmt, uint32_t dst, uint32_t dw, uint32_t y, uint32_t n,
                              uint64_t key[2])
{
    uint32_t second = m->vreg[0x1b5];
    uint8_t b[4 * 8192];
    if (fmt == GEVG_DST_YUVA) {
        m->mem.read(m->mem.opaque, dst + (size_t)y * dw * 2, b, n * 2);
        gevg_hash(key, b, n * 2);
        m->mem.read(m->mem.opaque, second + (size_t)y * dw, b, n);
        gevg_hash(key, b, n);
    } else if (fmt == GEVG_DST_YUV420) {
        m->mem.read(m->mem.opaque, dst + (size_t)y * dw, b, n);
        gevg_hash(key, b, n);
        m->mem.read(m->mem.opaque, second + (size_t)(y / 2) * dw, b, n);
        gevg_hash(key, b, n);
    } else {
        m->mem.read(m->mem.opaque, dst + (size_t)y * dw * 4, b, n * 4);
        gevg_hash(key, b, n * 4);
    }
}

static void gevg_dst_write(GevgModel *m, uint32_t fmt, uint32_t dst, uint32_t dw, uint32_t y, uint32_t n,
                           const uint32_t *row)
{
    uint32_t second = m->vreg[0x1b5];
    if (fmt == GEVG_DST_YUVA || fmt == GEVG_DST_YUV420) {
        uint8_t c[2 * 8192], yy[8192], cc[8192], a[8192];
        for (uint32_t x = 0; x + 1 < n + 1; x += 2) {
            uint32_t p0 = row[x], p1 = (x + 1 < n) ? row[x + 1] : row[x];
            float y0 = gevg_luma(p0), y1 = gevg_luma(p1);
            float cb = ((float)(p0 & 0xff) - y0 + (float)(p1 & 0xff) - y1) / (2 * 1.772f) + 128.0f;
            float cr = ((float)((p0 >> 16) & 0xff) - y0 + (float)((p1 >> 16) & 0xff) - y1) / (2 * 1.402f) + 128.0f;
            c[2 * x] = gevg_clip8(y0);
            c[2 * x + 1] = gevg_clip8(y1);
            c[2 * x + 2] = gevg_clip8(cb);
            c[2 * x + 3] = gevg_clip8(cr);
            yy[x] = c[2 * x];
            yy[x + 1] = c[2 * x + 1];
            cc[x] = c[2 * x + 2];
            cc[x + 1] = c[2 * x + 3];
            a[x] = p0 >> 24;
            a[x + 1] = p1 >> 24;
        }
        if (fmt == GEVG_DST_YUVA) {
            gevg_dst_mem_write(m, dst + (size_t)y * dw * 2, c, n * 2);
            gevg_dst_mem_write(m, second + (size_t)y * dw, a, n);
        } else {
            gevg_dst_mem_write(m, dst + (size_t)y * dw, yy, n);
            if (!(y & 1)) {
                gevg_dst_mem_write(m, second + (size_t)(y / 2) * dw, cc, n);
            }
        }
    } else {
        uint32_t *out = malloc((size_t)n * 4);
        if (!out) return;
        for (uint32_t x = 0; x < n; x++) out[x] = gevg_swap_rb(row[x]);
        gevg_dst_mem_write(m, dst + (size_t)y * dw * 4, out, n * 4);
        free(out);
    }
}

static void gevg_ppf_composite(GevgModel *m)
{
    uint32_t src = m->vreg[0x0a0], sfmt = m->vreg[0x0a2];
    uint32_t dst = m->vreg[0x1a4], dsize = m->vreg[0x1a3];
    uint32_t dw = dsize & 0xffff, dh = dsize >> 16;
    uint32_t c = m->vreg[0x185];
    uint32_t cr = c & 0xff, cg = (c >> 8) & 0xff, cb = (c >> 16) & 0xff, ca = c >> 24;
    const uint32_t pr = cr, pg = cg, pb = cb, pa = ca;
    uint32_t clipx = (m->vreg[0x0c0] >> 16) + 1, clipy = (m->vreg[0x0c1] >> 16) + 1;
    uint32_t clipx0 = m->vreg[0x0c0] & 0xffff, clipy0 = m->vreg[0x0c1] & 0xffff;
    uint32_t sub = (sfmt == 0x200) ? 4 : 1;
    uint32_t tmode = m->vreg[0x156] & 0xffff, tfmt = tmode & 0xff;
    int image = tmode != 0 && (m->vreg[0x184] & 2) != 0;
    uint32_t tbpp = (tfmt == 0x03 || tfmt == 0x04) ? 2 : (tfmt == 0x07 || tfmt == 0x18) ? 1 : 4;
    int blend = (m->vreg[0x1a5] & 1) != 0;
    int gradient = !image && (m->vreg[0x115] & 0x10000) != 0;
    float gm[9];
    for (int i = 0; i < 9; i++) gm[i] = gevg_f24(m->vreg[0x103 + i]);
    uint32_t taddr = m->vreg[0x155], th = m->vreg[0x158] & 0xffff, tw = m->vreg[0x158] >> 16;
    uint8_t *tex = NULL;
    float tm[9];
    for (int i = 0; i < 9; i++) tm[i] = gevg_f24(m->vreg[0x10c + i]);
    GevgMask *k = gevg_mask_get(m, src, 0, 0, 0);
    if (!k || !dst || !dw || !dh || dw > 8192 || dh > 8192) return;
    if (image) {
        if (!taddr || !tw || !th || tw > 8192 || th > 8192) return;
        tex = malloc((size_t)tw * th * tbpp);
        if (!tex) return;
        if (tfmt == GEVG_TEX_YUV420SP) {
            uint8_t *yuv = malloc((size_t)tw * th * 3 / 2 + 2);
            if (!yuv) {
                free(tex);
                return;
            }
            m->mem.read(m->mem.opaque, taddr, yuv, (size_t)tw * th * 3 / 2);
            for (uint32_t ty = 0; ty < th; ty++) {
                const uint8_t *cp = yuv + (size_t)tw * th + (size_t)(ty / 2) * (tw & ~1u);
                for (uint32_t tx = 0; tx < tw; tx++) {
                    float yy = yuv[(size_t)ty * tw + tx];
                    float u = cp[tx & ~1u] - 128.0f, v = cp[(tx & ~1u) + 1] - 128.0f;
                    uint8_t *o = tex + ((size_t)ty * tw + tx) * 4;
                    float r = yy + 1.402f * v, g = yy - 0.344136f * u - 0.714136f * v, b = yy + 1.772f * u;
                    o[0] = (uint8_t)GMIN(GMAX(r + 0.5f, 0.0f), 255.0f);
                    o[1] = (uint8_t)GMIN(GMAX(g + 0.5f, 0.0f), 255.0f);
                    o[2] = (uint8_t)GMIN(GMAX(b + 0.5f, 0.0f), 255.0f);
                    o[3] = 255;
                }
            }
            free(yuv);
        } else {
            m->mem.read(m->mem.opaque, taddr, tex, tw * th * tbpp);
        }
    }
    uint32_t xmax = GMIN(dw, clipx), ymax = GMIN(dh, clipy);
    xmax = GMIN(xmax, k->w / sub);
    ymax = GMIN(ymax, k->h / sub);
    uint32_t dfmt = m->vreg[0x1a2];
    if (dfmt != GEVG_DST_XRGB && dfmt != GEVG_DST_YUVA && dfmt != GEVG_DST_YUV420) {
        free(tex);
        return;
    }
    if (dfmt != GEVG_DST_XRGB) {
        uint32_t second = m->vreg[0x1b5], bpp = (dfmt == GEVG_DST_YUVA) ? 2 : 1;
        if (second > dst && dw) {
            uint32_t prows = (second - dst) / (dw * bpp);
            if (prows && dh > prows && clipy0 < dh - prows) {
                clipy0 = dh - prows;
            }
        }
    }
    uint32_t nrow = (dfmt == GEVG_DST_XRGB) ? xmax : GMIN((xmax + 1) & ~1u, dw & ~1u);
    uint32_t *row = malloc((dw + 2) * 4);
    if (!row) {
        free(tex);
        return;
    }
    uint32_t vx0 = GMAX(clipx0, k->x0 / sub), vx1 = GMIN(xmax, (k->x1 + sub - 1) / sub);
    uint32_t vy0 = GMAX(clipy0, k->y0 / sub), vy1 = GMIN(ymax, (k->y1 + sub - 1) / sub);
    m->recording = NULL;
    if (vx0 < vx1 && vy0 < vy1 && (uint64_t)(vy1 - vy0) * (vx1 - vx0) >= GEVG_MODEL_CACHE_MIN_PX) {
        uint64_t key[2] = { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull };
        static const uint16_t used[] = { 0x0a2, 0x0a3, 0x0c0, 0x0c1, 0x103, 0x104, 0x105, 0x106,
                                         0x107, 0x108, 0x109, 0x10a, 0x10b, 0x10c, 0x10d, 0x10e,
                                         0x10f, 0x110, 0x111, 0x112, 0x113, 0x114, 0x115, 0x140,
                                         0x156, 0x158, 0x184, 0x185, 0x1a2, 0x1a3, 0x1a5 };
        uint32_t vals[sizeof used / sizeof used[0]];
        for (size_t i = 0; i < sizeof used / sizeof used[0]; i++) vals[i] = m->vreg[used[i]];
        const uint32_t geo[] = { dw,  dh,   dfmt, nrow,
                                 vx0, vx1,  vy0,  vy1,
                                 sub, k->w, k->h, (uint32_t)image,
                                 tw,  th,   tbpp, dfmt != GEVG_DST_XRGB ? m->vreg[0x1b5] - dst : 0 };
        gevg_hash(key, geo, sizeof geo);
        gevg_hash(key, vals, sizeof vals);
        gevg_hash(key, m->lut[0], sizeof m->lut[0]);
        gevg_hash(key, k->cov + (size_t)vy0 * sub * k->w, (size_t)(vy1 - vy0) * sub * k->w);
        if (tex) gevg_hash(key, tex, (size_t)tw * th * tbpp);
        int replace = !blend && vx0 == 0 && vx1 >= nrow;
        for (uint32_t y = vy0 * sub; replace && y < vy1 * sub; y++) {
            const uint8_t *r = k->cov + (size_t)y * k->w;
            for (uint32_t x = 0; x < nrow * sub; x++) {
                if (r[x] != 0xff) {
                    replace = 0;
                    break;
                }
            }
        }
        gevg_hash(key, &replace, sizeof replace);
        for (uint32_t y = vy0; !replace && y < vy1; y++) {
            gevg_dst_hash_row(m, dfmt, dst, dw, dh - 1 - y, nrow, key);
        }
        GevgCacheEntry *slot = &m->cache[0];
        for (int i = 0; i < GEVG_MODEL_CACHE; i++) {
            GevgCacheEntry *e = &m->cache[i];
            if (e->rec && e->len && e->key[0] == key[0] && e->key[1] == key[1]) {
                for (size_t o = 0; o < e->len;) {
                    uint32_t a, l;
                    memcpy(&a, e->rec + o, 4);
                    memcpy(&l, e->rec + o + 4, 4);
                    m->mem.write(m->mem.opaque, dst + a, e->rec + o + 8, l);
                    o += 8 + l;
                }
                e->stamp = ++m->stamp;
                free(row);
                free(tex);
                return;
            }
            if (!e->len || (slot->len && e->stamp < slot->stamp)) slot = e;
        }
        slot->key[0] = key[0];
        slot->key[1] = key[1];
        slot->len = 0;
        m->recording = slot;
        m->recording_base = dst;
    }
    for (uint32_t y = vy0; y < vy1 && vx0 < vx1; y++) {
        int touched = 0;
        uint32_t my = dh - 1 - y;
        gevg_dst_read(m, dfmt, dst, dw, my, nrow, row);
        for (uint32_t x = vx0; x < vx1; x++) {
            uint32_t sum = 0;
            for (uint32_t j = 0; j < sub; j++) {
                const uint8_t *r = k->cov + (size_t)(y * sub + j) * k->w + x * sub;
                for (uint32_t i = 0; i < sub; i++) sum += r[i];
            }
            if (!sum) continue;
            if (gradient) {
                float px = x + 0.5f, py = y + 0.5f;
                float u = gm[0] * px + gm[1] * py + gm[2];
                u -= floorf(u);
                float fi = u * 128.0f;
                int32_t i0 = (int32_t)fi;
                float fr = fi - i0;
                i0 = GMIN(GMAX(i0, 0), 127);
                int32_t i1 = GMIN(i0 + 1, 127);
                uint32_t c0 = m->lut[0][i0], c1 = m->lut[0][i1];
                cr = (uint32_t)((c0 & 0xff) * (1 - fr) + (c1 & 0xff) * fr + 0.5f);
                cg = (uint32_t)(((c0 >> 8) & 0xff) * (1 - fr) + ((c1 >> 8) & 0xff) * fr + 0.5f);
                cb = (uint32_t)(((c0 >> 16) & 0xff) * (1 - fr) + ((c1 >> 16) & 0xff) * fr + 0.5f);
                ca = (uint32_t)((c0 >> 24) * (1 - fr) + (c1 >> 24) * fr + 0.5f);
            } else if (tex) {
                float px = x + 0.5f, py = y + 0.5f;
                float u = (tm[0] * px + tm[1] * py + tm[2]) * tw - 0.5f;
                float v = (tm[3] * px + tm[4] * py + tm[5]) * th - 0.5f;
                int32_t tx = (int32_t)floorf(u), ty = (int32_t)floorf(v);
                float fx = u - tx, fy = v - ty;
                uint32_t acc[4] = { 0, 0, 0, 0 };
                for (int j = 0; j < 2; j++) {
                    for (int i = 0; i < 2; i++) {
                        int32_t sx = GMIN(GMAX(tx + i, 0), (int32_t)tw - 1);
                        int32_t sy = GMIN(GMAX(ty + j, 0), (int32_t)th - 1);
                        float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                        const uint8_t *tp = tex + ((size_t)sy * tw + sx) * tbpp;
                        uint32_t tr, tg, tb, ta;
                        if (tbpp == 4) {
                            tr = tp[0];
                            tg = tp[1];
                            tb = tp[2];
                            ta = tp[3];
                        } else if (tfmt == 0x03) {
                            uint32_t t = tp[0] | (tp[1] << 8);
                            tb = ((t >> 11) & 31) * 255 / 31;
                            tg = ((t >> 5) & 63) * 255 / 63;
                            tr = (t & 31) * 255 / 31;
                            ta = 255;
                        } else if (tfmt == 0x04) {
                            uint32_t t = tp[0] | (tp[1] << 8);
                            ta = (t >> 12) * 17;
                            tb = ((t >> 8) & 15) * 17;
                            tg = ((t >> 4) & 15) * 17;
                            tr = (t & 15) * 17;
                        } else if (tfmt == 0x07) {
                            tr = tg = tb = tp[0];
                            ta = 255;
                        } else {
                            tr = pr;
                            tg = pg;
                            tb = pb;
                            ta = tp[0] * pa / 255;
                        }
                        acc[0] += (uint32_t)(tr * wgt * 256 + 0.5f);
                        acc[1] += (uint32_t)(tg * wgt * 256 + 0.5f);
                        acc[2] += (uint32_t)(tb * wgt * 256 + 0.5f);
                        acc[3] += (uint32_t)(ta * wgt * 256 + 0.5f);
                    }
                }
                cr = GMIN((acc[0] + 128) >> 8, 255u);
                cg = GMIN((acc[1] + 128) >> 8, 255u);
                cb = GMIN((acc[2] + 128) >> 8, 255u);
                ca = GMIN((acc[3] + 128) >> 8, 255u);
            }
            uint32_t a = blend ? ca * sum / (255 * sub * sub) : sum / (sub * sub);
            uint32_t d = row[x], dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
            uint32_t da = d >> 24;
            dr = (cr * a + dr * (255 - a) + 127) / 255;
            dg = (cg * a + dg * (255 - a) + 127) / 255;
            db = (cb * a + db * (255 - a) + 127) / 255;
            if (blend)
                da = a + da * (255 - a) / 255;
            else
                da = (ca * a + da * (255 - a) + 127) / 255;
            row[x] = (da << 24) | (dr << 16) | (dg << 8) | db;
            touched = 1;
        }
        if (touched) {
            gevg_dst_write(m, dfmt, dst, dw, my, nrow, row);
        }
    }
    if (m->recording) {
        m->recording->stamp = ++m->stamp;
        m->recording = NULL;
    }
    free(row);
    free(tex);
}

static int gevg_step_list(GevgModel *m, const uint32_t *w, uint32_t nwords, uint32_t *pi, int is_b,
                          uint32_t stop_reg)
{
    uint32_t i = *pi;
    int hit = 0;
    while (i < nwords && !hit) {
        uint32_t hdr = w[i];
        if (hdr == 0x0041f046 && i + 9 < nwords) {
            memcpy(m->matrix, &w[i + 1], 9 * 4);
            i += 10;
        } else if ((hdr & 4) && (hdr & 0xf000) && !(hdr >> 28)) {
            uint32_t reg = hdr >> 16, cnt = ((hdr & 0xfff) >> 3) + 1, nib = (hdr >> 12) & 0xf;
            uint32_t mask = 0;
            for (int b = 0; b < 4; b++)
                if (nib & (1 << b)) mask |= 0xffu << (8 * b);
            for (uint32_t k = 0; k < cnt && k < 16 && i + 1 + k < nwords; k++) {
                if (reg + k < GEVG_MODEL_VREGS) {
                    m->vreg[reg + k] = (m->vreg[reg + k] & ~mask) | (w[i + 1 + k] & mask);
                }
            }
            if (is_b && reg == 0x16c) {
                uint32_t bank = (m->vreg[0x16b] >> 16) & 1;
                for (uint32_t k = 0; k < cnt && k < 256 && i + 1 + k < nwords; k++) {
                    m->lut[bank][k] = w[i + 1 + k];
                }
            }
            if (!is_b && reg == 0x085) {
                gevg_gp_draw_external(m);
            } else if (is_b && reg == 0x0a6) {
                gevg_ppf_composite(m);
            }
            hit = (reg == stop_reg);
            i += 1 + cnt;
        } else if (hdr == 0x0000000a && i + 8 < nwords && w[i + 1] == 0x07090702) {
            uint8_t seg[8];
            float coord[5];
            memcpy(seg, &w[i + 1], 8);
            for (int k = 0; k < 5; k++) {
                coord[k] = (float)(int32_t)w[i + 4 + k];
            }
            gevg_gp_draw(m, seg, 5, coord, 5);
            i += 9;
        } else {
            i = nwords;
        }
    }
    *pi = i;
    return hit;
}

void gevg_model_run_job(GevgModel *m)
{
    float id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    memcpy(m->matrix, id, sizeof id);
    uint32_t a_addr = m->reg[GEVG_OFF_LIST_A_ADDR / 4], a_len = m->reg[GEVG_OFF_LIST_A_LEN / 4];
    uint32_t b_addr = m->reg[GEVG_OFF_LIST_B_ADDR / 4], b_len = m->reg[GEVG_OFF_LIST_B_LEN / 4];
    uint32_t na = 0, nb = 0, ia = 0, ib = 0;
    if (a_addr && a_len) {
        na = GMIN(a_len * 2, (uint32_t)GEVG_MODEL_MAX_LIST);
        m->mem.read(m->mem.opaque, a_addr, m->list_a, na * 4);
    }
    if (b_addr && b_len) {
        nb = GMIN(b_len * 2, (uint32_t)GEVG_MODEL_MAX_LIST);
        m->mem.read(m->mem.opaque, b_addr, m->list_b, nb * 4);
    }
    if (a_len * 2 > (uint32_t)GEVG_MODEL_MAX_LIST || b_len * 2 > (uint32_t)GEVG_MODEL_MAX_LIST) {
        fprintf(stderr, "gevg: command list truncated to %u words (A %u, B %u): draws are missing\n",
                (unsigned)GEVG_MODEL_MAX_LIST, a_len * 2, b_len * 2);
    }
    while (ia < na || ib < nb) {
        gevg_step_list(m, m->list_a, na, &ia, 0, 0x085);
        gevg_step_list(m, m->list_b, nb, &ib, 1, 0x0a6);
    }
}

int gevg_model_write(GevgModel *m, uint32_t off, uint32_t val)
{
    if (off / 4 < GEVG_MODEL_REGS) {
        m->reg[off / 4] = val;
    }
    if (((off == GEVG_OFF_LIST_A_GO || off == GEVG_OFF_LIST_B_GO) && val == 1) ||
        (off == GEVG_OFF_GO_C && val != 0)) {
        if (off == GEVG_OFF_LIST_B_GO) {
            gevg_model_run_job(m);
        }
        return 1;
    }
    return 0;
}
