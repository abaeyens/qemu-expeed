/*
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef Z6II_TASK_GROUPS_H
#define Z6II_TASK_GROUPS_H

#include <stdint.h>

static inline int z6ii_task_in_ui(uint32_t exinf)
{
    switch (exinf) {
    case 0xc181d8d0u:
    case 0xc181d9e4u:
    case 0xc1824eb4u:
    case 0xc18280a8u:
    case 0xc182814cu:
    case 0xc1ce3cf4u:
    case 0xc1d01610u:
    case 0xc1d0163cu:
    case 0xc1d02074u:
    case 0xc1d0da20u:
    case 0xc1d19e10u:
    case 0xc1d413a8u:
    case 0xc1eccae0u:
    case 0xc1fa627cu:
    case 0xc25c4834u:
    case 0xc296737cu:
    case 0xc29ddf98u:
        return 1;
    default:
        return 0;
    }
}

static inline int z6ii_task_in_imaging(uint32_t exinf)
{
    switch (exinf) {
    case 0xc1807c60u:
    case 0xc1807ce4u:
    case 0xc1807d68u:
    case 0xc1807decu:
    case 0xc1807e70u:
    case 0xc1864918u:
    case 0xc186494cu:
    case 0xc1864980u:
    case 0xc18649b4u:
    case 0xc18649e8u:
    case 0xc1864a1cu:
    case 0xc1864a50u:
    case 0xc1864a84u:
    case 0xc1864ab8u:
    case 0xc1864b38u:
    case 0xc1864b6cu:
    case 0xc1864ba0u:
    case 0xc1864bd4u:
    case 0xc1864c08u:
    case 0xc1864c3cu:
    case 0xc1864decu:
    case 0xc1864e20u:
    case 0xc1864e54u:
    case 0xc1864e88u:
    case 0xc1864ebcu:
    case 0xc1864ef0u:
    case 0xc1864f24u:
    case 0xc1864f58u:
    case 0xc1864f8cu:
    case 0xc1864fc0u:
    case 0xc1864ff4u:
    case 0xc1865028u:
    case 0xc186505cu:
    case 0xc1865090u:
    case 0xc18650c4u:
    case 0xc18650f8u:
    case 0xc186512cu:
    case 0xc1ce2934u:
    case 0xc1cf173cu:
    case 0xc1cf28bcu:
    case 0xc1cf7440u:
    case 0xc1cf7558u:
    case 0xc1cf75b0u:
    case 0xc1cf778cu:
    case 0xc1d1b094u:
    case 0xc1d1b1bcu:
    case 0xc1d41b08u:
    case 0xc1d50288u:
    case 0xc1d503e0u:
    case 0xc1d51710u:
    case 0xc1d523c0u:
    case 0xc1d52600u:
    case 0xc1d52690u:
    case 0xc1d527e4u:
    case 0xc1d52938u:
    case 0xc1d52a8cu:
    case 0xc1d52be0u:
    case 0xc1d52d34u:
    case 0xc1d52e88u:
    case 0xc1d52fdcu:
    case 0xc1d53130u:
    case 0xc1d53284u:
    case 0xc1d60440u:
    case 0xc1d60474u:
    case 0xc1d604a8u:
    case 0xc1d604dcu:
    case 0xc1d60510u:
    case 0xc1d60544u:
    case 0xc1d60578u:
    case 0xc1d605acu:
    case 0xc1d605e0u:
    case 0xc1d60614u:
    case 0xc1d60648u:
    case 0xc1d6067cu:
    case 0xc1d606b0u:
    case 0xc1d606e4u:
    case 0xc1d60718u:
    case 0xc1d6074cu:
    case 0xc1d60780u:
    case 0xc1d607b4u:
    case 0xc1f2d828u:
    case 0xc1f4c458u:
    case 0xc1f58dd8u:
    case 0xc1f59048u:
    case 0xc1f7207cu:
    case 0xc21d6300u:
    case 0xc21de964u:
    case 0xc237a754u:
    case 0xc239b338u:
    case 0xc23d8d80u:
    case 0xc23d9728u:
    case 0xc23d9ee8u:
    case 0xc25dacb8u:
    case 0xc269eaa0u:
    case 0xc269f9c8u:
    case 0xc296e230u:
    case 0xc2b2352cu:
        return 1;
    default:
        return 0;
    }
}

static inline int z6ii_task_in_audio_stream(uint32_t exinf)
{
    switch (exinf) {
    case 0xc1cf7440u:
        return 1;
    default:
        return 0;
    }
}

#endif
