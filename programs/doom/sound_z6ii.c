/*
 * DOOM sound effects on the camera speaker.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "../z6ii/z6ii.h"
#include "../z6ii/z6ii_audio.h"
#include "doom_z6ii.h"
#include "doomtype.h"
#include "opl_z6ii.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#define FRAMES  512u
#define SLOTS   8u
#define LEAD    3u
#define NVOICES 16u
#define NCMDS   64u

int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

enum { CMD_START = 1, CMD_STOP, CMD_PARAMS };

typedef struct {
    uint32_t type, ch, seq;
    const uint8_t *data;
    uint32_t len, step;
    int32_t gl, gr;
} Cmd;

typedef struct {
    const uint8_t *data;
    uint32_t len, pos, step, seq;
    int32_t gl, gr;
    int on;
} Voice;

static Cmd cmds[NCMDS];
static volatile uint32_t cmd_head, cmd_tail;
static uint32_t started_seq[NVOICES], next_seq;
static volatile uint32_t done_seq[NVOICES];
static Voice voices[NVOICES];

static uint32_t ring_buf[SLOTS * FRAMES] __attribute__((aligned(64)));
static Z6iiAudioRing snd_ring;
static volatile int mixer_state;
static boolean sound_ready;

static void post(const Cmd *c)
{
    const uint32_t head = cmd_head;
    if (head - cmd_tail >= NCMDS) {
        return;
    }
    cmds[head % NCMDS] = *c;
    __sync_synchronize();
    cmd_head = head + 1;
}

static void apply(const Cmd *c)
{
    Voice *v = &voices[c->ch];
    switch (c->type) {
    case CMD_START:
        v->data = c->data;
        v->len = c->len;
        v->pos = 0;
        v->step = c->step;
        v->gl = c->gl;
        v->gr = c->gr;
        v->seq = c->seq;
        v->on = 1;
        break;
    case CMD_STOP:
        if (v->seq == c->seq) {
            v->on = 0;
            done_seq[c->ch] = c->seq;
        }
        break;
    case CMD_PARAMS:
        if (v->seq == c->seq) {
            v->gl = c->gl;
            v->gr = c->gr;
        }
        break;
    }
}

static void mix(int16_t *out)
{
    while (cmd_tail != cmd_head) {
        __sync_synchronize();
        apply(&cmds[cmd_tail % NCMDS]);
        cmd_tail = cmd_tail + 1;
    }
    static int32_t music[FRAMES];
    const uint32_t t0 = z6ii_counter_lo();
    opl_z6ii_render(music, FRAMES);
    const uint32_t dt = z6ii_counter_lo() - t0;
    volatile uint32_t *state = (volatile uint32_t *)DOOM_STATE;
    state[22] += dt;
    if (dt > state[23]) {
        state[23] = dt;
    }
    state[24]++;
    for (uint32_t i = 0; i < FRAMES; i++) {
        int32_t l = music[i] * 4, r = music[i] * 4;
        for (uint32_t k = 0; k < NVOICES; k++) {
            Voice *v = &voices[k];
            if (!v->on) {
                continue;
            }
            const int32_t s = (int32_t)v->data[v->pos >> 16] - 128;
            l += s * v->gl;
            r += s * v->gr;
            v->pos += v->step;
            if ((v->pos >> 16) >= v->len) {
                v->on = 0;
                done_seq[k] = v->seq;
            }
        }
        out[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

static void mixer_task(int32_t stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    volatile uint32_t *state = (volatile uint32_t *)DOOM_STATE;
    while (mixer_state == 1) {
        int16_t *s;
        while ((s = z6ii_audio_slot(&snd_ring)) != 0) {
            mix(s);
            z6ii_audio_commit(&snd_ring);
        }
        state[2] = snd_ring.submitted;
        state[3] = snd_ring.underruns;
        tk_dly_tsk(5);
    }
    z6ii_audio_stop(&snd_ring);
    mixer_state = 3;
    tk_ext_tsk();
}

#define VOL_MIN  0x52u
#define VOL_MAX  0xa2u
#define VOL_STEP 8u

static uint32_t dac_level = Z6II_AUDIO_DAC_LEVEL;

int sound_z6ii_volume(int dir)
{
    if (!sound_ready) {
        return -1;
    }
    if (dir > 0 && dac_level + VOL_STEP <= VOL_MAX) {
        dac_level += VOL_STEP;
    } else if (dir < 0 && dac_level >= VOL_MIN + VOL_STEP) {
        dac_level -= VOL_STEP;
    }
    z6ii_codec_write(0x0a, dac_level);
    z6ii_codec_write(0x0b, 0x100u | dac_level);
    ((volatile uint32_t *)DOOM_STATE)[25] = dac_level;
    return (int)((dac_level - VOL_MIN) / VOL_STEP);
}

void sound_z6ii_stop(void)
{
    if (mixer_state != 1) {
        return;
    }
    mixer_state = 2;
    for (int i = 0; i < 200 && mixer_state != 3; i++) {
        tk_dly_tsk(10);
    }
}

static snddevice_t sound_devices[] = { SNDDEVICE_SB,          SNDDEVICE_PAS,         SNDDEVICE_GUS,
                                       SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };
static boolean use_prefix;

static boolean Init(boolean use_sfx_prefix)
{
    use_prefix = use_sfx_prefix;
    volatile uint32_t *state = (volatile uint32_t *)DOOM_STATE;
    const int32_t st = z6ii_audio_codec_up(Z6II_AUDIO_DAC_LEVEL);
    state[5] = (uint32_t)st;
    printf("sound: codec %ld\n", (long)st);
    state[25] = dac_level;
    z6ii_audio_start(&snd_ring, (uint32_t)(uintptr_t)ring_buf, FRAMES, SLOTS, LEAD);
    mixer_state = 1;
    T_CTSK c;
    memset(&c, 0, sizeof(c));
    c.tskatr = TA_HLNG;
    c.task = (uint32_t)(uintptr_t)mixer_task;
    c.itskpri = 90;
    c.stksz = 0x2000;
    const ID id = tk_cre_tsk(&c);
    if (id <= 0 || tk_sta_tsk(id, 0) < 0) {
        printf("sound: no mixer task (%ld)\n", (long)id);
        mixer_state = 0;
        z6ii_audio_stop(&snd_ring);
        return false;
    }
    sound_ready = true;
    DG_music_module = music_opl_module;
    return true;
}

static void Shutdown(void)
{
    sound_z6ii_stop();
}

static int GetSfxLumpNum(sfxinfo_t *sfx)
{
    if (sfx->link != NULL) {
        sfx = sfx->link;
    }
    char name[9];
    if (use_prefix) {
        name[0] = 'd';
        name[1] = 's';
        strncpy(name + 2, sfx->name, 6);
        name[8] = 0;
    } else {
        strncpy(name, sfx->name, 8);
        name[8] = 0;
    }
    return W_CheckNumForName(name);
}

static void Update(void)
{
}

static void gains(int vol, int sep, int32_t *gl, int32_t *gr)
{
    *gl = ((254 - sep) * vol) / 127;
    *gr = (sep * vol) / 127;
}

static void UpdateSoundParams(int channel, int vol, int sep)
{
    if (!sound_ready || channel < 0 || channel >= (int)NVOICES) {
        return;
    }
    Cmd c = { .type = CMD_PARAMS, .ch = (uint32_t)channel, .seq = started_seq[channel] };
    gains(vol, sep, &c.gl, &c.gr);
    post(&c);
}

static int StartSound(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    if (!sound_ready || channel < 0 || channel >= (int)NVOICES || sfx->lumpnum < 0) {
        return -1;
    }
    const uint8_t *lump = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
    const uint32_t lumplen = (uint32_t)W_LumpLength(sfx->lumpnum);
    if (lumplen < 8 || lump[0] != 3 || lump[1] != 0) {
        return -1;
    }
    const uint32_t rate = lump[2] | (uint32_t)lump[3] << 8;
    const uint32_t length = lump[4] | (uint32_t)lump[5] << 8 | (uint32_t)lump[6] << 16 | (uint32_t)lump[7] << 24;
    if (length > lumplen - 8 || length <= 48 || rate == 0) {
        return -1;
    }
    Cmd c = { .type = CMD_START, .ch = (uint32_t)channel, .seq = ++next_seq };
    c.data = lump + 8 + 16;
    c.len = length - 32;
    c.step = (rate << 16) / Z6II_AUDIO_RATE;
    gains(vol, sep, &c.gl, &c.gr);
    started_seq[channel] = c.seq;
    post(&c);
    volatile uint32_t *state = (volatile uint32_t *)DOOM_STATE;
    const uint32_t n = state[4]++;
    state[6 + 2 * (n % 8)] = (uint32_t)sfx->lumpnum;
    state[7 + 2 * (n % 8)] = snd_ring.submitted;
    return channel;
}

static void StopSound(int channel)
{
    if (!sound_ready || channel < 0 || channel >= (int)NVOICES) {
        return;
    }
    const Cmd c = { .type = CMD_STOP, .ch = (uint32_t)channel, .seq = started_seq[channel] };
    post(&c);
}

static boolean SoundIsPlaying(int channel)
{
    if (!sound_ready || channel < 0 || channel >= (int)NVOICES) {
        return false;
    }
    return done_seq[channel] != started_seq[channel];
}

static void CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds;
    (void)num_sounds;
}

sound_module_t DG_sound_module = {
    sound_devices,
    sizeof(sound_devices) / sizeof(sound_devices[0]),
    Init,
    Shutdown,
    GetSfxLumpNum,
    Update,
    UpdateSoundParams,
    StartSound,
    StopSound,
    SoundIsPlaying,
    CacheSounds,
};

static boolean MusInit(void)
{
    return false;
}
static void MusNone(void)
{
}
static void MusVolume(int volume)
{
    (void)volume;
}
static void *MusRegister(void *data, int len)
{
    (void)data;
    (void)len;
    return NULL;
}
static void MusUnregister(void *handle)
{
    (void)handle;
}
static void MusPlay(void *handle, boolean looping)
{
    (void)handle;
    (void)looping;
}
static boolean MusPlaying(void)
{
    return false;
}

music_module_t DG_music_module = {
    NULL,          0,       MusInit, MusNone,    MusVolume, MusNone, MusNone, MusRegister,
    MusUnregister, MusPlay, MusNone, MusPlaying, NULL,
};
