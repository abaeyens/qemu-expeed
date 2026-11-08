/*
 * The OPL library API (external/chocolate-doom/opl/opl.h) on the Z 6II:
 * DOSBox DBOPL emulator, rendered by the port mixer task.
 *
 * Copyright (C) 2005-2014 Simon Howard (this file is based on opl.c and opl_sdl.c)
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
#include "dbopl.h"
#include "opl.h"
#include "opl_queue.h"
#include "opl_z6ii.h"

unsigned int opl_sample_rate = 22050;

static Chip chip;
static opl_callback_queue_t *queue;
static uint64_t current_time;
static uint64_t pause_offset;
static int paused;
static int register_num;
static int ready;
static ID queue_mtx, lock_mtx;
static ID render_tid;

static void lock(ID m)
{
    if (m > 0) {
        tk_loc_mtx(m, TMO_FEVR);
    }
}

static void unlock(ID m)
{
    if (m > 0) {
        tk_unl_mtx(m);
    }
}

opl_init_result_t OPL_Init(unsigned int port_base)
{
    (void)port_base;
    if (!ready) {
        queue_mtx = z6ii_mutex_create();
        lock_mtx = z6ii_mutex_create();
        DBOPL_InitTables();
        Chip__Chip(&chip);
        Chip__Setup(&chip, OPL_Z6II_RATE);
        queue = OPL_Queue_Create();
        ready = 1;
        printf("OPL_Init: DBOPL at %u Hz, mutexes %ld %ld\n", OPL_Z6II_RATE, (long)queue_mtx, (long)lock_mtx);
    }
    return OPL_INIT_OPL2;
}

void OPL_Shutdown(void)
{
    OPL_ClearCallbacks();
}

void OPL_SetSampleRate(unsigned int rate)
{
    opl_sample_rate = rate;
}

static void write_reg(int reg, int value)
{
    const int foreign = tk_get_tid() != render_tid;
    if (foreign) {
        lock(lock_mtx);
    }
    Chip__WriteReg(&chip, (Bit32u)reg, (Bit8u)value);
    if (foreign) {
        unlock(lock_mtx);
    }
}

void OPL_WritePort(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT) {
        register_num = (int)value;
    } else if (port == OPL_REGISTER_PORT_OPL3) {
        register_num = (int)value | 0x100;
    } else if (port == OPL_DATA_PORT) {
        write_reg(register_num, (int)value);
    }
}

unsigned int OPL_ReadPort(opl_port_t port)
{
    (void)port;
    return 0;
}

unsigned int OPL_ReadStatus(void)
{
    return 0;
}

void OPL_WriteRegister(int reg, int value)
{
    write_reg(reg, value);
}

opl_init_result_t OPL_Detect(void)
{
    return OPL_INIT_OPL2;
}

void OPL_InitRegisters(int opl3)
{
    int r;
    for (r = OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r) {
        OPL_WriteRegister(r, 0x3f);
    }
    for (r = OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r) {
        OPL_WriteRegister(r, 0x00);
    }
    for (r = 1; r < OPL_REGS_LEVEL; ++r) {
        OPL_WriteRegister(r, 0x00);
    }
    OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x60);
    OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x80);
    OPL_WriteRegister(OPL_REG_WAVEFORM_ENABLE, 0x20);
    if (opl3) {
        OPL_WriteRegister(OPL_REG_NEW, 0x01);
        for (r = OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r) {
            OPL_WriteRegister(r | 0x100, 0x3f);
        }
        for (r = OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r) {
            OPL_WriteRegister(r | 0x100, 0x00);
        }
        for (r = 1; r < OPL_REGS_LEVEL; ++r) {
            OPL_WriteRegister(r | 0x100, 0x00);
        }
    }
    OPL_WriteRegister(OPL_REG_FM_MODE, 0x40);
    if (opl3) {
        OPL_WriteRegister(OPL_REG_NEW, 0x01);
    }
}

void OPL_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    lock(queue_mtx);
    OPL_Queue_Push(queue, callback, data, current_time - pause_offset + us);
    unlock(queue_mtx);
}

void OPL_AdjustCallbacks(float factor)
{
    lock(queue_mtx);
    OPL_Queue_AdjustCallbacks(queue, current_time, factor);
    unlock(queue_mtx);
}

void OPL_ClearCallbacks(void)
{
    if (!ready) {
        return;
    }
    lock(queue_mtx);
    OPL_Queue_Clear(queue);
    unlock(queue_mtx);
}

void OPL_Lock(void)
{
    lock(lock_mtx);
}

void OPL_Unlock(void)
{
    unlock(lock_mtx);
}

void OPL_Delay(uint64_t us)
{
    tk_dly_tsk((int32_t)(us / 1000u) + 1);
}

void OPL_SetPaused(int p)
{
    paused = p;
}

static void advance(unsigned n)
{
    opl_callback_t callback;
    void *data;
    lock(queue_mtx);
    const uint64_t us = (uint64_t)n * OPL_SECOND / OPL_Z6II_RATE;
    current_time += us;
    if (paused) {
        pause_offset += us;
    }
    while (!OPL_Queue_IsEmpty(queue) && current_time >= OPL_Queue_Peek(queue) + pause_offset) {
        if (!OPL_Queue_Pop(queue, &callback, &data)) {
            break;
        }
        unlock(queue_mtx);
        lock(lock_mtx);
        callback(data);
        unlock(lock_mtx);
        lock(queue_mtx);
    }
    unlock(queue_mtx);
}

void opl_z6ii_render(int32_t *out, unsigned n)
{
    if (!ready) {
        memset(out, 0, n * sizeof(*out));
        return;
    }
    render_tid = tk_get_tid();
    unsigned filled = 0;
    while (filled < n) {
        uint64_t k = n - filled;
        lock(queue_mtx);
        if (!paused && !OPL_Queue_IsEmpty(queue)) {
            const uint64_t next = OPL_Queue_Peek(queue) + pause_offset;
            const uint64_t until = next > current_time ? next - current_time : 0;
            const uint64_t want = (until * OPL_Z6II_RATE + OPL_SECOND - 1) / OPL_SECOND;
            if (want < k) {
                k = want;
            }
        }
        unlock(queue_mtx);
        if (k) {
            lock(lock_mtx);
            Chip__GenerateBlock2(&chip, (Bitu)k, out + filled);
            unlock(lock_mtx);
        }
        filled += (unsigned)k;
        advance((unsigned)k);
    }
}
