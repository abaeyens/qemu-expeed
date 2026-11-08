/*
 * Platform layer for doomgeneric on the Nikon Z 6II
 *
 * DOOM runs on the master Expeed as a task of its own, from the photo menu,
 * with the camera's own code still running underneath.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../z6ii/z6ii.h"
#include "../z6ii/z6ii_text.h"
#include "doom_z6ii.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "d_player.h"
#include "doomstat.h"
#include "i_video.h"
#include "m_controls.h"

uint32_t doom_wad_size;

#define GUARD_WORDS 1024
__attribute__((section(".guard.data_lo"), used)) static uint32_t guard_data_lo[GUARD_WORDS] = { 1 };
__attribute__((section(".guard.data_hi"), used)) static uint32_t guard_data_hi[GUARD_WORDS] = { 1 };
__attribute__((section(".guard.bss_lo"), used)) static uint32_t guard_bss_lo[GUARD_WORDS];
__attribute__((section(".guard.bss_hi"), used)) static uint32_t guard_bss_hi[GUARD_WORDS];
extern const uint32_t __image_start[], __text_end[];
static uint32_t text_sum;

static uint32_t sum_text(void)
{
    uint32_t s = 0;
    for (const volatile uint32_t *p = __image_start; p < __text_end; p++) {
        s = (s << 1 | s >> 31) ^ *p;
    }
    return s;
}

static void guards_fill(void)
{
    for (uint32_t i = 0; i < GUARD_WORDS; i++) {
        guard_data_lo[i] = 0xd0a7a000u ^ i;
        guard_data_hi[i] = 0xd0a7b000u ^ i;
        guard_bss_lo[i] = 0xd0a7c000u ^ i;
        guard_bss_hi[i] = 0xd0a7d000u ^ i;
    }
}

static uint32_t guard_bad(const volatile uint32_t *g, uint32_t base)
{
    for (uint32_t i = 0; i < GUARD_WORDS; i++) {
        if (g[i] != (base ^ i)) {
            return (uint32_t)(uintptr_t)&g[i];
        }
    }
    return 0;
}

static void alive(void);

static void check(const char *where)
{
    alive();
    const uint32_t t = sum_text();
    const uint32_t dl = guard_bad(guard_data_lo, 0xd0a7a000u), dh = guard_bad(guard_data_hi, 0xd0a7b000u);
    const uint32_t bl = guard_bad(guard_bss_lo, 0xd0a7c000u), bh = guard_bad(guard_bss_hi, 0xd0a7d000u);
    if (t == text_sum && !dl && !dh && !bl && !bh) {
        return;
    }
    printf("INTEGRITY FAILED %s: text %08lx (was %08lx), guards %08lx %08lx %08lx %08lx\n", where,
           (unsigned long)t, (unsigned long)text_sum, (unsigned long)dl, (unsigned long)dh, (unsigned long)bl,
           (unsigned long)bh);
    doom_quit(2);
}

static Z6iiQuiet quiet;
static Z6iiPlaneSaved gui_plane;
static Z6iiPlane plane;
static int quieted;

static void wait_disp_or_switch(void);
static void show_log(void);

static void alive(void)
{
    ((volatile uint32_t *)DOOM_RUNNING)[1] = z6ii_counter_lo();
}

void doom_quit(int status)
{
    printf("doom_quit(%d)\n", status);
    if (quieted && status != 0 && plane.w != 0) {
        show_log();
        wait_disp_or_switch();
    }
    sound_z6ii_stop();
    if (quieted) {
        z6ii_plane_restore(&gui_plane);
        z6ii_unquiet(&quiet);
    }
    ((volatile uint32_t *)DOOM_RUNNING)[0] = 0;
    tk_ext_tsk();
}

uint32_t DG_GetTicksMs(void)
{
    static uint32_t last, ms_base, rest;
    const uint32_t now = z6ii_counter_lo();
    uint32_t delta = now - last;
    last = now;
    delta += rest;
    ms_base += delta / (Z6II_COUNTER_HZ / 1000u);
    rest = delta % (Z6II_COUNTER_HZ / 1000u);
    return ms_base;
}

void DG_SleepMs(uint32_t ms)
{
    tk_dly_tsk((int32_t)(ms ? ms : 1));
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

#define SCALE 3
#define OUT_W (320 * SCALE)
#define OUT_H (200 * SCALE)

static uint8_t pal_y[256], pal_cb[256], pal_cr[256];
static uint32_t pal_pair[256];

static void palette_update(void)
{
    for (int i = 0; i < 256; i++) {
        const uint32_t ycc = z6ii_ycc(colors[i].r, colors[i].g, colors[i].b);
        pal_y[i] = (uint8_t)(ycc >> 16);
        pal_cb[i] = (uint8_t)(ycc >> 8);
        pal_cr[i] = (uint8_t)ycc;
        pal_pair[i] =
            pal_y[i] | ((uint32_t)pal_y[i] << 8) | ((uint32_t)pal_cb[i] << 16) | ((uint32_t)pal_cr[i] << 24);
    }
}

extern int sfxVolume;  /* doomgeneric s_sound.c */

void DG_Init(void)
{
    /* let sound effects volume default to 4 instead of 8 (range 0..15)*/
    sfxVolume = 4;
}

static void heartbeat(uint32_t slot, uint32_t value)
{
    if (plane.w == 0) {
        return;
    }
    const uint32_t scale = 2, w = 10 * 6 * scale;
    const uint32_t x0 = ((plane.w - OUT_W) / 2 + slot * (w + 24)) & ~1u, y = (plane.h + OUT_H) / 2 + 12;
    z6ii_fill(&plane, x0, y, w, 7 * scale, z6ii_ycc(0, 0, 0));
    const char label = slot == 2 ? 'V' : slot ? 'T' : 'F';
    const uint32_t x = z6ii_draw_w4(&plane, x0, y, Z6II_W4(label, ' ', 0, 0), scale, z6ii_ycc(255, 255, 0));
    if (slot == 2) {
        z6ii_draw_dec(&plane, x, y, value, scale, z6ii_ycc(255, 255, 0));
    } else {
        z6ii_draw_hex(&plane, x, y, value, scale, z6ii_ycc(255, 255, 0));
    }
}

void DG_DrawFrame(void)
{
    if (plane.w == 0) {
        return;
    }
    if (palette_changed) {
        palette_update();
        palette_changed = false;
    }
    const uint32_t x0 = ((plane.w - OUT_W) / 2) & ~1u, y0 = (plane.h - OUT_H) / 2;
    const uint8_t *src = (const uint8_t *)DG_ScreenBuffer;
    for (uint32_t y = 0; y < 200; y++, src += 320) {
        uint32_t *row = (uint32_t *)(uintptr_t)(plane.cbase + (y0 + y * SCALE) * plane.cpitch + x0 * 2);
        for (uint32_t x = 0; x < 320; x += 2) {
            const uint8_t a = src[x], b = src[x + 1];
            *row++ = pal_pair[a];
            *row++ = pal_y[a] | ((uint32_t)pal_y[b] << 8) | ((uint32_t)((pal_cb[a] + pal_cb[b]) >> 1) << 16) |
                     ((uint32_t)((pal_cr[a] + pal_cr[b]) >> 1) << 24);
            *row++ = pal_pair[b];
        }
        const uint32_t first = plane.cbase + (y0 + y * SCALE) * plane.cpitch + x0 * 2;
        for (uint32_t k = 1; k < SCALE; k++) {
            memcpy((void *)(uintptr_t)(first + k * plane.cpitch), (const void *)(uintptr_t)first, OUT_W * 2);
        }
    }
    z6ii_dcache_clean(plane.cbase + y0 * plane.cpitch, OUT_H * plane.cpitch);

    static uint32_t frames;
    alive();
    heartbeat(0, ++frames);
}

static char doom_error[160];

void __real_I_Error(char *error, ...);

void __wrap_I_Error(char *error, ...)
{
    va_list ap;
    va_start(ap, error);
    vsnprintf(doom_error, sizeof(doom_error), error, ap);
    va_end(ap);
    printf("ERROR: %s\n", doom_error);
    __real_I_Error("%s", doom_error);
    doom_quit(-1);
}

static uint32_t draw_text(uint32_t x0, uint32_t y, const char *t, uint32_t n, uint32_t scale, uint32_t ycc)
{
    const uint32_t cw = 6 * scale, lh = 8 * scale, cols = (plane.w - x0) / cw;
    uint32_t col = 0;
    for (uint32_t i = 0; i < n && t[i]; i++) {
        if (t[i] == '\n' || col >= cols) {
            y += lh;
            col = 0;
            if (t[i] == '\n') {
                continue;
            }
        }
        if (t[i] != ' ') {
            z6ii_draw_char(&plane, x0 + col * cw, y, t[i], scale, ycc);
        }
        col++;
    }
    return y + lh;
}

static void show_log(void)
{
    const uint32_t white = z6ii_ycc(255, 255, 255);
    z6ii_fill(&plane, 0, 0, plane.w, plane.h, z6ii_ycc(160, 0, 0));
    uint32_t y = 8;
    if (doom_error[0]) {
        y = draw_text(8, y, doom_error, sizeof(doom_error), 4, white) + 8;
    }
    const uint32_t scale = 2, lh = 8 * scale, cols = (plane.w - 8) / (6 * scale);
    const uint32_t rows = (plane.h - y) / lh;
    const uint32_t n = *(volatile uint32_t *)DOOM_LOG, size = DOOM_LOG_SIZE - 4;
    const volatile char *ring = (const volatile char *)(DOOM_LOG + 4);
    const uint32_t oldest = n > size ? n - size : 0;
    uint32_t start = n, used = 0;
    while (start > oldest) {
        uint32_t ls = start - 1;
        if (ls > oldest && ring[ls % size] == '\n') {
            ls--;
        }
        while (ls > oldest && ring[(ls - 1) % size] != '\n') {
            ls--;
        }
        const uint32_t len = start - ls, need = len / cols + 1;
        if (used + need > rows) {
            break;
        }
        used += need;
        start = ls;
    }
    static char text[DOOM_LOG_SIZE];
    uint32_t k = 0;
    for (uint32_t i = start; i < n; i++) {
        text[k++] = ring[i % size];
    }
    draw_text(8, y, text, k, scale, white);
}

typedef struct {
    uint8_t word;
    uint32_t mask;
    unsigned char key;
} KeyMap;

static const KeyMap keymap[] = {
    { 2, 0x20 | 0x8 | 0x2, KEY_UPARROW },       /* sub-selector up, and its up diagonals */
    { 2, 0x10 | 0x4 | 0x1, KEY_DOWNARROW },     /* down */
    { 2, 0x40 | 0x2 | 0x1, KEY_LEFTARROW },     /* left */
    { 2, 0x80 | 0x8 | 0x4, KEY_RIGHTARROW },    /* right */
    { 2, 0x8000, KEY_USE },         /* sub-selector pressed: use, and in menus select */
    { 2, 0x8000, KEY_ENTER },
    { 2, 0x8000, 'y' },
    { 0, 0x40, KEY_STRAFE_L },      /* multi-selector left */
    { 0, 0x80, KEY_STRAFE_R },      /* right */
    { 0, 0x80000000u, KEY_FIRE },   /* shutter half press */
    { 0, 0x200000, KEY_RSHIFT },    /* AF-ON: run */
    { 1, 0x1, KEY_ENTER },          /* OK: Enter, use and "y" */
    { 1, 0x1, KEY_USE },
    { 1, 0x1, 'y' },
    { 1, 0x8000, KEY_ESCAPE },      /* MENU */
    { 1, 0x2, KEY_TAB },            /* PLAY: the automap */
    { 1, 0x8, KEY_EQUALS },         /* zoom in */
    { 1, 0x4, KEY_MINUS },          /* zoom out */
};
#define NKEYS (sizeof(keymap) / sizeof(keymap[0]))

static uint32_t held;
static int32_t prev_dial;
static unsigned char dial_up;
static uint32_t prev0 = 0xffffffffu, prev2;

static void wait_disp_or_switch(void)
{
    uint32_t p0 = z6ii_status(0), p2 = z6ii_status(2);
    for (;;) {
        const uint32_t w0 = z6ii_status(0), w2 = z6ii_status(2);
        if (((p0 & Z6II_W0_MAIN_SWITCH) && !(w0 & Z6II_W0_MAIN_SWITCH)) ||
            (!(p2 & Z6II_W2_DISP) && (w2 & Z6II_W2_DISP))) {
            return;
        }
        p0 = w0;
        p2 = w2;
        alive();
        tk_dly_tsk(20);
    }
}

#define Z6II_W0_FN1 0x4000u
#define Z6II_W2_FN2 0x4000u

static void check_volume(uint32_t w0, uint32_t w2)
{
    static uint32_t prev_fn = 0xffffffffu;
    const uint32_t fn = (w0 & Z6II_W0_FN1 ? 1u : 0u) | (w2 & Z6II_W2_FN2 ? 2u : 0u);
    const uint32_t pressed = prev_fn == 0xffffffffu ? 0 : fn & ~prev_fn;
    prev_fn = fn;
    if (pressed) {
        const int level = sound_z6ii_volume(pressed & 2u ? 1 : -1);
        printf("volume %d\n", level);
        if (level >= 0) {
            heartbeat(2, (uint32_t)level);
        }
    }
}

static void check_handback(uint32_t w0, uint32_t w2)
{
    if (prev0 != 0xffffffffu && (((prev0 & Z6II_W0_MAIN_SWITCH) && !(w0 & Z6II_W0_MAIN_SWITCH)) ||
                                 (!(prev2 & Z6II_W2_DISP) && (w2 & Z6II_W2_DISP)))) {
        doom_quit(0);
    }
    prev0 = w0;
    prev2 = w2;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    volatile uint32_t *state = (volatile uint32_t *)DOOM_STATE;
    state[0] = (uint32_t)players[consoleplayer].readyweapon;
    state[1] = (uint32_t)players[consoleplayer].pendingweapon;
    if (dial_up) {
        *pressed = 0;
        *key = dial_up;
        dial_up = 0;
        return 1;
    }
    static uint32_t last_hb;
    const uint32_t ms = DG_GetTicksMs();
    if (ms - last_hb >= 100) {
        last_hb = ms;
        alive();
        heartbeat(1, ms);
        static uint32_t last_check;
        if (ms - last_check >= 1000) {
            last_check = ms;
            check("while running");
        }
    }
    const uint32_t w[3] = { z6ii_status(0), z6ii_status(1), z6ii_status(2) };
    check_handback(w[0], w[2]);
    check_volume(w[0], w[2]);
    const int32_t dial = (int32_t)z6ii_status(4);
    if (dial != prev_dial) {
        prev_dial = dial;
        if (dial != 0) {
            *pressed = 1;
            *key = dial > 0 ? ']' : '[';
            dial_up = *key;
            return 1;
        }
    }
    for (uint32_t i = 0; i < NKEYS; i++) {
        const int down = (w[keymap[i].word] & keymap[i].mask) != 0;
        if (down != (int)((held >> i) & 1u)) {
            held ^= 1u << i;
            *pressed = down;
            *key = keymap[i].key;
            return 1;
        }
    }
    return 0;
}

static void load_wad(void)
{
    char path[] = "A:\\DOOM1.WAD";
    const uint32_t t0 = z6ii_counter_lo();
    z6ii_dcache_clean(DOOM_WAD, DOOM_WAD_MAX);
    z6ii_dcache_invalidate(DOOM_WAD, DOOM_WAD_MAX);
    const int32_t h = z6ii_file_open(path);
    uint32_t total = 0;
    int32_t st = 0;
    if (h >= 0) {
        for (;;) {
            uint32_t got = 0;
            st = z6ii_file_read(h, (void *)(uintptr_t)(DOOM_WAD + total), 0x10000, &got);
            total += got;
            if (st != 0 || got != 0x10000 || total + 0x10000 > DOOM_WAD_MAX) {
                break;
            }
        }
        z6ii_file_close(h);
    }
    z6ii_dcache_invalidate(DOOM_WAD, total);
    doom_wad_size = h >= 0 && st == 0 ? total : 0;
    printf("load_wad: handle %ld, %lu bytes, status %ld, %lu ms\n", (long)h, (unsigned long)total, (long)st,
           (unsigned long)((z6ii_counter_lo() - t0) / (Z6II_COUNTER_HZ / 1000u)));
}

static void doom_task(int32_t stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    tk_dly_tsk(1000);
    printf("DOOM on the Z6II\n");
    printf("image %p..%p, text sum %08lx, guards %p %p %p %p\n", (void *)__image_start, (void *)__text_end,
           (unsigned long)text_sum, (void *)guard_data_lo, (void *)guard_data_hi, (void *)guard_bss_lo,
           (void *)guard_bss_hi);
    check("start");
    load_wad();
    check("after the WAD read");

    z6ii_quiet_except(&quiet, Z6II_KEEP_AUDIO_STREAM);
    quieted = 1;
    gui_plane = z6ii_plane_save();
    plane = z6ii_plane_geometry(DOOM_PLANE_C, DOOM_PLANE_A);
    printf("quieted %lu tasks (%lu refused), plane %lux%lu\n", (unsigned long)quiet.n,
           (unsigned long)quiet.refused, (unsigned long)plane.w, (unsigned long)plane.h);
    check("after quieting");
    if (plane.w != 0) {
        z6ii_fill(&plane, 0, 0, plane.w, plane.h, z6ii_ycc(0, 0, 0));
        z6ii_plane_show(&plane);
    }
    check("after taking the plane");
    if (!doom_wad_size) {
        printf("no WAD: quitting\n");
        doom_quit(1);
    }

    z6ii_malloc_lock_init();
    static char *argv[] = { "doom", "-iwad", "DOOM1.WAD", NULL };
    doomgeneric_Create(3, argv);
    key_nextweapon = ']';
    key_prevweapon = '[';
    check("after doomgeneric_Create");
    for (;;) {
        doomgeneric_Tick();
    }
}

int doom_entry(void)
{
    *(volatile uint32_t *)DOOM_LOG = 0;
    for (int i = 0; i < DOOM_STATE_WORDS; i++) {
        ((volatile uint32_t *)DOOM_STATE)[i] = 0;
    }
    guards_fill();
    text_sum = sum_text();
    T_CTSK c;
    memset(&c, 0, sizeof(c));
    c.tskatr = TA_HLNG;
    c.task = (uint32_t)(uintptr_t)doom_task;
    c.itskpri = 100;
    c.stksz = 0x10000;
    const ID id = tk_cre_tsk(&c);
    if (id > 0) {
        const ER er = tk_sta_tsk(id, 0);
        if (er < 0) {
            return er;
        }
    }
    return id;
}
