/*
 * big text on the overlay plane
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef Z6II_TEXT_H
#define Z6II_TEXT_H

#include "z6ii.h"

#define Z6II_W4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static inline uint8_t z6ii_glyph_col(char c, int col)
{
    uint64_t g;
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A'); /* draw lowercase as uppercase */
    }
    switch (c) {
    case '0':
        g = 0x3e5149453eull;
        break;
    case '1':
        g = 0x00427f4000ull;
        break;
    case '2':
        g = 0x4261514946ull;
        break;
    case '3':
        g = 0x2141454b31ull;
        break;
    case '4':
        g = 0x1814127f10ull;
        break;
    case '5':
        g = 0x2745454539ull;
        break;
    case '6':
        g = 0x3c4a494930ull;
        break;
    case '7':
        g = 0x0171090503ull;
        break;
    case '8':
        g = 0x3649494936ull;
        break;
    case '9':
        g = 0x064949291eull;
        break;
    case 'A':
        g = 0x7e1111117eull;
        break;
    case 'B':
        g = 0x7f49494936ull;
        break;
    case 'C':
        g = 0x3e41414122ull;
        break;
    case 'D':
        g = 0x7f4141221cull;
        break;
    case 'E':
        g = 0x7f49494941ull;
        break;
    case 'F':
        g = 0x7f09090901ull;
        break;
    case 'G':
        g = 0x3e4149497aull;
        break;
    case 'H':
        g = 0x7f0808087full;
        break;
    case 'I':
        g = 0x00417f4100ull;
        break;
    case 'J':
        g = 0x2040413f01ull;
        break;
    case 'K':
        g = 0x7f08142241ull;
        break;
    case 'L':
        g = 0x7f40404040ull;
        break;
    case 'M':
        g = 0x7f020c027full;
        break;
    case 'N':
        g = 0x7f0408107full;
        break;
    case 'O':
        g = 0x3e4141413eull;
        break;
    case 'P':
        g = 0x7f09090906ull;
        break;
    case 'Q':
        g = 0x3e4151215eull;
        break;
    case 'R':
        g = 0x7f09192946ull;
        break;
    case 'S':
        g = 0x4649494931ull;
        break;
    case 'T':
        g = 0x01017f0101ull;
        break;
    case 'U':
        g = 0x3f4040403full;
        break;
    case 'V':
        g = 0x1f2040201full;
        break;
    case 'W':
        g = 0x3f4038403full;
        break;
    case 'X':
        g = 0x6314081463ull;
        break;
    case 'Y':
        g = 0x0708700807ull;
        break;
    case 'Z':
        g = 0x6151494543ull;
        break;
    case '!':
        g = 0x00005f0000ull;
        break;
    case '"':
        g = 0x0007000700ull;
        break;
    case '%':
        g = 0x2313086462ull;
        break;
    case '\'':
        g = 0x0000070000ull;
        break;
    case '(':
        g = 0x001c224100ull;
        break;
    case ')':
        g = 0x0041221c00ull;
        break;
    case '+':
        g = 0x08083e0808ull;
        break;
    case ',':
        g = 0x0050300000ull;
        break;
    case '-':
        g = 0x0808080808ull;
        break;
    case '.':
        g = 0x0060600000ull;
        break;
    case '/':
        g = 0x2010080402ull;
        break;
    case ':':
        g = 0x0036360000ull;
        break;
    case '=':
        g = 0x1414141414ull;
        break;
    case '<':
        g = 0x0814224100ull;
        break;
    case '>':
        g = 0x0041221408ull;
        break;
    case '?':
        g = 0x0201510906ull;
        break;
    case '_':
        g = 0x4040404040ull;
        break;
    case '[':
        g = 0x007f414100ull;
        break;
    case ']':
        g = 0x0041417f00ull;
        break;
    case '#':
        g = 0x147f147f14ull;
        break;
    case '*':
        g = 0x082a1c2a08ull;
        break;
    default:
        return 0;
    }
    const uint32_t hi = (uint32_t)(g >> 32), lo = (uint32_t)g;
    return (uint8_t)((col == 0 ? hi : lo >> (8 * (4 - col))) & 0x7f);
}

static inline void z6ii_draw_char(const Z6iiPlane *p, uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t ycc)
{
    for (int col = 0; col < 5; col++) {
        const uint8_t bits = z6ii_glyph_col(c, col);
        for (int row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                z6ii_fill(p, x + (uint32_t)col * scale, y + (uint32_t)row * scale, scale, scale, ycc);
            }
        }
    }
}

static inline uint32_t z6ii_draw_w4(const Z6iiPlane *p, uint32_t x, uint32_t y, uint32_t w4, uint32_t scale,
                                    uint32_t ycc)
{
    for (int i = 3; i >= 0; i--) {
        const char c = (char)(w4 >> (8 * i));
        if (!c) {
            break;
        }
        z6ii_draw_char(p, x, y, c, scale, ycc);
        x += 6 * scale;
    }
    return x;
}

static inline uint32_t z6ii_draw_hex(const Z6iiPlane *p, uint32_t x, uint32_t y, uint32_t v, uint32_t scale,
                                     uint32_t ycc)
{
    for (int i = 7; i >= 0; i--) {
        const uint32_t d = (v >> (4 * i)) & 0xf;
        z6ii_draw_char(p, x, y, (char)(d < 10 ? '0' + d : 'A' + d - 10), scale, ycc);
        x += 6 * scale;
    }
    return x;
}

static inline uint32_t z6ii_draw_dec(const Z6iiPlane *p, uint32_t x, uint32_t y, uint32_t v, uint32_t scale,
                                     uint32_t ycc)
{
    char d[10];
    int n = 0;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n--) {
        z6ii_draw_char(p, x, y, d[n], scale, ycc);
        x += 6 * scale;
    }
    return x;
}

#endif
