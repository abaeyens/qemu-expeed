/*
 * The Z6II master's services for our own bare-metal code
 * (Expeed 6, Cortex-A7, firmware v1.70)
 *
 * header-only so a program stays one position-independent blob
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef Z6II_H
#define Z6II_H

#include <stdint.h>

#include "z6ii_task_groups.h"

typedef int32_t ER;
typedef int32_t ID;

typedef struct {
    void *exinf;
    uint32_t tskatr;
    uint32_t task;
    int32_t itskpri;
    int32_t stksz;
    uint32_t reserved[6];
    uint32_t assprc;
    char oname[8];
} T_CTSK;

#define TA_HLNG 1u

#define Z6II_STUB(addr) ((uintptr_t)(addr))

static inline ID tk_cre_tsk(const T_CTSK *pk)
{
    return ((ID (*)(const T_CTSK *))Z6II_STUB(0x101044ec))(pk);
}

static inline ER tk_sta_tsk(ID tskid, int32_t stacd)
{
    return ((ER (*)(ID, int32_t))Z6II_STUB(0x10104954))(tskid, stacd);
}

static inline ER tk_dly_tsk(int32_t ms)
{
    return ((ER (*)(int32_t))Z6II_STUB(0x101045a4))(ms);
}

__attribute__((noreturn)) static inline void tk_ext_tsk(void)
{
    ((void (*)(void))Z6II_STUB(0x101045f4))();
    __builtin_unreachable();
}

static inline ID tk_get_tid(void)
{
    return ((ID (*)(void))Z6II_STUB(0x101046ac))();
}

typedef struct {
    void *exinf;
    uint32_t mtxatr;
    int32_t ceilpri;
    uint32_t reserved;
} T_CMTX;

#define TA_INHERIT 2u
#define TMO_FEVR   (-1)

static inline ID tk_cre_mtx(const T_CMTX *pk)
{
    return ((ID (*)(const T_CMTX *))Z6II_STUB(0x101044a4))(pk);
}

static inline ER tk_loc_mtx(ID mtxid, int32_t tmout)
{
    return ((ER (*)(ID, int32_t))Z6II_STUB(0x101046f8))(mtxid, tmout);
}

static inline ER tk_unl_mtx(ID mtxid)
{
    return ((ER (*)(ID))Z6II_STUB(0x101049f0))(mtxid);
}

static inline ID z6ii_mutex_create(void)
{
    T_CMTX c = { 0, TA_INHERIT, 0, 0 };
    return tk_cre_mtx(&c);
}

__attribute__((naked, target("arm"), noinline, unused)) static ER tk_sus_tsk(ID tskid)
{
    (void)tskid;
    __asm__ volatile("stmdb sp!, {r4}\n"
                     "add r4, sp, #4\n"
                     "stmdb sp!, {lr}\n"
                     "movw ip, #0x0100\n"
                     "movt ip, #0x8018\n"
                     "svc #6\n"
                     "ldm sp!, {lr}\n"
                     "ldm sp!, {r4}\n"
                     "bx lr\n");
}

__attribute__((naked, target("arm"), noinline, unused)) static ER tk_rsm_tsk(ID tskid)
{
    (void)tskid;
    __asm__ volatile("stmdb sp!, {r4}\n"
                     "add r4, sp, #4\n"
                     "stmdb sp!, {lr}\n"
                     "movw ip, #0x0100\n"
                     "movt ip, #0x8019\n"
                     "svc #6\n"
                     "ldm sp!, {lr}\n"
                     "ldm sp!, {r4}\n"
                     "bx lr\n");
}

#define Z6II_TASK_PROGRAM(body, prio, stack) Z6II_TASK_PROGRAM_GUARDED(body, prio, stack, (void)0)

#define Z6II_ONCE_STALE (10u * 48000000u)

__attribute__((noinline, unused)) static int z6ii_once_claim(uint32_t marker, uint32_t magic)
{
    volatile uint32_t *m = (volatile uint32_t *)(uintptr_t)marker;
    const uint32_t now = *(const volatile uint32_t *)(uintptr_t)0x04266008u; /* the 48 MHz counter */
    if (m[0] == magic && now - m[1] < Z6II_ONCE_STALE) {
        return 0;
    }
    m[0] = magic;
    m[1] = now;
    return 1;
}

static inline void z6ii_once_alive(uint32_t marker)
{
    ((volatile uint32_t *)(uintptr_t)marker)[1] = *(const volatile uint32_t *)(uintptr_t)0x04266008u;
}

static inline void z6ii_once_release(uint32_t marker)
{
    ((volatile uint32_t *)(uintptr_t)marker)[0] = 0;
}

#define Z6II_TASK_PROGRAM_ONCE(body, prio, stack, marker, magic)                                                  \
    Z6II_TASK_PROGRAM_GUARDED(body, prio, stack, if (!z6ii_once_claim((marker), (magic))) return -1)

#define Z6II_TASK_PROGRAM_GUARDED(body, prio, stack, guard)                                                       \
    __asm__(".section .text.start, \"ax\"\n.global _start\n.thumb_func\n_start: b.w z6ii_entry\n.text\n");        \
    __attribute__((section(".text.entry"), noinline)) static void body(int32_t stacd, void *exinf);               \
    __attribute__((section(".text.entry"), used)) ID z6ii_entry(void)                                             \
    {                                                                                                             \
        guard;                                                                                                    \
        uintptr_t entry;                                                                                          \
        __asm__("adr %0, %c1" : "=r"(entry) : "i"(body));                                                         \
        T_CTSK c;                                                                                                 \
        for (volatile uint32_t *w = (volatile uint32_t *)&c; w < (volatile uint32_t *)(&c + 1); w++) {            \
            *w = 0; /* not "= {0}": freestanding, there is no memset */                                           \
        }                                                                                                         \
        c.tskatr = TA_HLNG;                                                                                       \
        c.task = entry | 1u;                                                                                      \
        c.itskpri = (prio);                                                                                       \
        c.stksz = (stack);                                                                                        \
        const ID id = tk_cre_tsk(&c);                                                                             \
        if (id > 0) {                                                                                             \
            const ER er = tk_sta_tsk(id, 0);                                                                      \
            if (er < 0) {                                                                                         \
                return er;                                                                                        \
            }                                                                                                     \
        }                                                                                                         \
        return id;                                                                                                \
    }                                                                                                             \
    __attribute__((section(".text.entry"), noinline)) static void body(int32_t stacd, void *exinf)

#define Z6II_KNL_TCB_TABLE 0xc18ad81cu
#define Z6II_KNL_MAX_TSKID 0xc18ad820u
#define Z6II_TCB_STRIDE    0xd8u

#define Z6II_QUIET_MAX 256u

typedef struct {
    uint32_t n;
    uint32_t refused;
    uint16_t id[Z6II_QUIET_MAX];
} Z6iiQuiet;

static inline void z6ii_quiet(Z6iiQuiet *q)
{
    q->n = q->refused = 0;
    const uint32_t tcb1 = *(const volatile uint32_t *)(uintptr_t)Z6II_KNL_TCB_TABLE;
    const uint32_t max = *(const volatile uint32_t *)(uintptr_t)Z6II_KNL_MAX_TSKID;
    for (uint32_t n = 1; n <= max; n++) {
        const volatile uint32_t *tcb = (const volatile uint32_t *)(uintptr_t)(tcb1 + (n - 1) * Z6II_TCB_STRIDE);
        if (tcb[0x08 / 4] != (0x01000000u | n)) {
            continue;
        }
        const uint32_t state = tcb[0x28 / 4] >> 24;
        const uint32_t exinf = tcb[0x0c / 4];
        if (state == 0 || state == 8 || !(z6ii_task_in_ui(exinf) || z6ii_task_in_imaging(exinf))) {
            continue;
        }
        if (q->n < Z6II_QUIET_MAX && tk_sus_tsk((ID)(0x01000000u | n)) == 0) {
            q->id[q->n++] = (uint16_t)n;
        } else {
            q->refused++;
        }
    }
}

#define Z6II_KEEP_AUDIO_STREAM 1u

static inline void z6ii_quiet_except(Z6iiQuiet *q, uint32_t keep)
{
    q->n = q->refused = 0;
    const uint32_t tcb1 = *(const volatile uint32_t *)(uintptr_t)Z6II_KNL_TCB_TABLE;
    const uint32_t max = *(const volatile uint32_t *)(uintptr_t)Z6II_KNL_MAX_TSKID;
    for (uint32_t n = 1; n <= max; n++) {
        const volatile uint32_t *tcb = (const volatile uint32_t *)(uintptr_t)(tcb1 + (n - 1) * Z6II_TCB_STRIDE);
        if (tcb[0x08 / 4] != (0x01000000u | n)) {
            continue;
        }
        const uint32_t state = tcb[0x28 / 4] >> 24;
        const uint32_t exinf = tcb[0x0c / 4];
        if (state == 0 || state == 8 || !(z6ii_task_in_ui(exinf) || z6ii_task_in_imaging(exinf)) ||
            ((keep & Z6II_KEEP_AUDIO_STREAM) && z6ii_task_in_audio_stream(exinf))) {
            continue;
        }
        if (q->n < Z6II_QUIET_MAX && tk_sus_tsk((ID)(0x01000000u | n)) == 0) {
            q->id[q->n++] = (uint16_t)n;
        } else {
            q->refused++;
        }
    }
}

static inline void z6ii_unquiet(Z6iiQuiet *q)
{
    for (uint32_t i = 0; i < q->n; i++) {
        tk_rsm_tsk((ID)(0x01000000u | q->id[i]));
    }
    q->n = 0;
}

static inline void z6ii_dcache_clean(uint32_t base, uint32_t len)
{
    for (uint32_t a = base & ~63u; a < base + len; a += 64) {
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(a) : "memory");
    }
    __asm__ volatile("dsb" : : : "memory");
}

static inline void z6ii_dcache_invalidate(uint32_t base, uint32_t len)
{
    for (uint32_t a = base & ~63u; a < base + len; a += 64) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(a) : "memory");
    }
    __asm__ volatile("dsb" : : : "memory");
}

static inline int32_t z6ii_file_open(const char *path)
{
    return ((int32_t (*)(const char *, int32_t, int32_t))0x105c1717)(path, 0, 0x20);
}

static inline int32_t z6ii_file_read(int32_t h, void *buf, uint32_t len, uint32_t *got)
{
    uint32_t req[4] = { 1, (uint32_t)(uintptr_t)buf, len, 0 };
    *got = 0;
    return ((int32_t (*)(int32_t, uint32_t *, uint32_t, uint32_t *))0x105c1795)(h, req, len, got);
}

static inline int32_t z6ii_file_close(int32_t h)
{
    return ((int32_t (*)(int32_t))0x105c1749)(h);
}

#define Z6II_DISPENG   0x20140000u
#define Z6II_U6_SIZE   0x6204u
#define Z6II_U6_CBASE  0x6440u
#define Z6II_U6_CPITCH 0x6500u
#define Z6II_U6_ABASE  0x64c0u
#define Z6II_U6_APITCH 0x6540u

static inline volatile uint32_t *z6ii_dispeng(uint32_t off)
{
    return (volatile uint32_t *)(uintptr_t)(Z6II_DISPENG + off);
}

typedef struct {
    uint32_t w, h, cbase, cpitch, abase, apitch;
} Z6iiPlane;

static inline Z6iiPlane z6ii_plane_geometry(uint32_t cbase, uint32_t abase)
{
    Z6iiPlane p;
    const uint32_t size = *z6ii_dispeng(Z6II_U6_SIZE);
    p.w = size & 0xffff;
    p.h = size >> 16;
    p.cpitch = *z6ii_dispeng(Z6II_U6_CPITCH) & 0xffff;
    p.apitch = *z6ii_dispeng(Z6II_U6_APITCH) & 0xffff;
    p.cbase = cbase;
    p.abase = abase;
    if (p.w == 0 || p.h == 0 || p.w > 4096 || p.h > 4096) {
        p.w = 0;
    }
    return p;
}

typedef struct {
    uint32_t cbase, abase;
} Z6iiPlaneSaved;

static inline Z6iiPlaneSaved z6ii_plane_save(void)
{
    Z6iiPlaneSaved v = { *z6ii_dispeng(Z6II_U6_CBASE), *z6ii_dispeng(Z6II_U6_ABASE) };
    return v;
}

static inline void z6ii_plane_restore(const Z6iiPlaneSaved *v)
{
    *z6ii_dispeng(Z6II_U6_CBASE) = v->cbase;
    *z6ii_dispeng(Z6II_U6_ABASE) = v->abase;
}

static inline void z6ii_plane_show(const Z6iiPlane *p)
{
    *z6ii_dispeng(Z6II_U6_CBASE) = p->cbase;
    *z6ii_dispeng(Z6II_U6_ABASE) = p->abase;
}

static inline uint32_t z6ii_ycc(int r, int g, int b)
{
    const int y = (77 * r + 150 * g + 29 * b) >> 8;
    const int cb = (((b - y) * 144) >> 8) + 128;
    const int cr = (((r - y) * 183) >> 8) + 128;
    return ((uint32_t)y << 16) | ((uint32_t)cb << 8) | (uint32_t)cr;
}

static inline void z6ii_fill(const Z6iiPlane *p, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t ycc)
{
    for (uint32_t y = y0; y < y0 + h && y < p->h; y++) {
        volatile uint8_t *c = (volatile uint8_t *)(uintptr_t)(p->cbase + y * p->cpitch);
        volatile uint8_t *a = (volatile uint8_t *)(uintptr_t)(p->abase + y * p->apitch);
        for (uint32_t x = x0; x < x0 + w && x < p->w; x += 2) {
            c[x * 2 + 0] = (uint8_t)(ycc >> 16);
            c[x * 2 + 1] = (uint8_t)(ycc >> 16);
            c[x * 2 + 2] = (uint8_t)(ycc >> 8);
            c[x * 2 + 3] = (uint8_t)ycc;
            a[x] = 0xff;
            a[x + 1] = 0xff;
        }
        z6ii_dcache_clean(p->cbase + y * p->cpitch + x0 * 2, w * 2);
        z6ii_dcache_clean(p->abase + y * p->apitch + x0, w);
    }
}

#define Z6II_STATUS 0xc25d6fc0u

static inline uint32_t z6ii_status(uint32_t word)
{
    return ((const volatile uint32_t *)(uintptr_t)Z6II_STATUS)[word];
}

#define Z6II_W0_MAIN_SWITCH 0x200u
#define Z6II_W2_DISP        0x100000u

#define Z6II_COUNTER_HZ 48000000u

static inline uint32_t z6ii_counter_lo(void)
{
    return *(const volatile uint32_t *)(uintptr_t)0x04266008u;
}

#endif
