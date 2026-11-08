/*
 * The byte swap macros Chocolate Doom's i_swap.h and midifile.c take from SDL
 * (external/chocolate-doom, built into the port for its OPL music player)
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef Z6II_SDL_ENDIAN_H
#define Z6II_SDL_ENDIAN_H

#include <stdint.h>

#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#define SDL_BYTEORDER  SDL_LIL_ENDIAN

#define SDL_SwapLE16(x) ((uint16_t)(x))
#define SDL_SwapLE32(x) ((uint32_t)(x))
#define SDL_SwapBE16(x) __builtin_bswap16((uint16_t)(x))
#define SDL_SwapBE32(x) __builtin_bswap32((uint32_t)(x))

#endif
