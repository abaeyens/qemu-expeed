/*
 * The DOOM port's memory map and the calls between its two files.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DOOM_Z6II_H
#define DOOM_Z6II_H

#include <stdint.h>

#define DOOM_PLANE_C       0x90240000u  /* the overlay plane colour buffer */
#define DOOM_PLANE_A       0x903a0000u  /* ... and its alpha */
#define DOOM_LOG           0x90450000u  /* the printf ring: a write index and then the text */
#define DOOM_LOG_SIZE      0x10000u
#define DOOM_WAD           0x90460000u  /* the WAD, as read from the card */
#define DOOM_WAD_MAX       0x480000u
#define DOOM_RUNNING       0x9044ff00u  /* the counter when DOOM last said it was alive */
#define DOOM_RUNNING_MAGIC 0x4d4f4f44u
#define DOOM_STATE         0x9044ff40u
#define DOOM_STATE_WORDS   26

extern uint32_t doom_wad_size; /* 0 until the WAD has been read */

void z6ii_malloc_lock_init(void);

void sound_z6ii_stop(void);

int sound_z6ii_volume(int dir);

__attribute__((noreturn)) void doom_quit(int status);

#endif
