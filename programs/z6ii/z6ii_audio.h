/*
 * sound out of the Z 6II speaker
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef Z6II_AUDIO_H
#define Z6II_AUDIO_H

#include "z6ii.h"

#define Z6II_AUDIO_RATE    24000u
#define Z6II_AUDIO_STREAM0 0xc1cf7468u

#define Z6II_AUDIO_DAC_LEVEL 0x72u

static inline int32_t z6ii_codec_write(uint32_t reg, uint32_t value)
{
    return ((int32_t (*)(uint32_t, uint32_t))0x10370cbf)(reg, value);
}

static inline int32_t z6ii_codec_script(uint32_t id, uint32_t mode)
{
    return ((int32_t (*)(uint32_t, uint32_t))0x10370fb7)(id, mode);
}

static inline int32_t z6ii_audio_codec_up(uint32_t dac_level)
{
    ((void (*)(int32_t))0x10201369)(0);
    int32_t st = z6ii_codec_script(0, 0);
    const int32_t st8 = z6ii_codec_script(8, 3), st7 = z6ii_codec_script(7, 3);
    if (st == 0) {
        st = st8 ? st8 : st7;
    }
    z6ii_codec_write(0x0a, dac_level & 0xffu);
    z6ii_codec_write(0x0b, 0x100u | (dac_level & 0xffu));
    return st;
}

typedef struct {
    volatile uint32_t *rec;
    uint32_t base;
    uint32_t frames;
    uint32_t slots, lead;
    uint32_t submitted;
    uint32_t last_idx;
    uint32_t written;
    uint32_t underruns;
} Z6iiAudioRing;

static inline void z6ii_audio_track(Z6iiAudioRing *r)
{
    const uint32_t idx = r->rec[9];
    if (idx < r->slots && idx != r->last_idx) {
        r->submitted += (idx + r->slots - r->last_idx) % r->slots;
        r->last_idx = idx;
    }
}

static inline int16_t *z6ii_audio_slot(Z6iiAudioRing *r)
{
    z6ii_audio_track(r);
    if (r->written < r->submitted) {
        r->underruns += r->submitted - r->written;
        r->written = r->submitted;
    }
    if (r->written > r->submitted + r->lead || r->written + 3 > r->submitted + r->slots) {
        return 0;
    }
    return (int16_t *)(uintptr_t)(r->base + (r->written % r->slots) * r->frames * 4u);
}

static inline void z6ii_audio_commit(Z6iiAudioRing *r)
{
    z6ii_dcache_clean(r->base + (r->written % r->slots) * r->frames * 4u, r->frames * 4u);
    r->written++;
}

static inline void z6ii_audio_start(Z6iiAudioRing *r, uint32_t base, uint32_t frames, uint32_t slots,
                                    uint32_t lead)
{
    r->rec = (volatile uint32_t *)(uintptr_t)Z6II_AUDIO_STREAM0;
    r->base = base;
    r->frames = frames;
    r->slots = slots;
    r->lead = lead;
    r->submitted = r->last_idx = r->written = r->underruns = 0;
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)base;
    for (uint32_t i = 0; i < slots * frames; i++) {
        w[i] = 0;
    }
    z6ii_dcache_clean(base, slots * frames * 4u);
    r->written = lead + 1;
    r->rec[0] = base;
    r->rec[1] = frames * 4u;
    r->rec[2] = slots;
    r->rec[3] = 0;
    r->rec[4] = 1;
    r->rec[5] = 0;
    r->rec[0xc] = 0;
    ((int32_t (*)(int32_t, int32_t, int32_t, int32_t, int32_t))0x102e1645)(1, 0, 0, 0, 0);
    r->last_idx = r->rec[9] < slots ? r->rec[9] : 0;
    r->submitted = r->last_idx;
}

static inline void z6ii_audio_stop(Z6iiAudioRing *r)
{
    r->rec[0xc] = 1;
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)r->base;
    for (uint32_t i = 0; i < r->slots * r->frames; i++) {
        w[i] = 0;
    }
    z6ii_dcache_clean(r->base, r->slots * r->frames * 4u);
    tk_dly_tsk((int32_t)(3u * r->frames * 1000u / Z6II_AUDIO_RATE + 1u));
}

#endif
