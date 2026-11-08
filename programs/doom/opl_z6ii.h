/*
 * The port's OPL renderer (opl_z6ii.c), for its mixer
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OPL_Z6II_H
#define OPL_Z6II_H

#include <stdint.h>

#include "../z6ii/z6ii_audio.h"

#define OPL_Z6II_RATE Z6II_AUDIO_RATE

void opl_z6ii_render(int32_t *out, unsigned n);

#endif
