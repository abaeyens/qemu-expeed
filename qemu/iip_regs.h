/*
 * IIP register fields
 *
 * The register field definitions the emulator models use.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef IIP_REGS_H
#define IIP_REGS_H

#include <stdint.h>

static inline uint32_t iip_regs_get(const uint32_t *regs, unsigned reg, unsigned shift, unsigned bits)
{
    return (uint32_t)((regs[reg / 4] >> shift) & ((1ull << bits) - 1));
}

static inline int32_t iip_regs_gets(const uint32_t *regs, unsigned reg, unsigned shift, unsigned bits)
{
    uint32_t v = iip_regs_get(regs, reg, shift, bits);
    return (v >> (bits - 1)) & 1 ? (int32_t)(v - (1ull << bits)) : (int32_t)v;
}

static inline uint16_t iip_regs_table(const uint32_t *regs, unsigned reg, unsigned i)
{
    return (uint16_t)(regs[reg / 4 + i / 2] >> ((i & 1) * 16));
}

#define IIP_GET(regs, F)  iip_regs_get(regs, IIP_##F##_REG, IIP_##F##_SHIFT, IIP_##F##_BITS)
#define IIP_GETS(regs, F) iip_regs_gets(regs, IIP_##F##_REG, IIP_##F##_SHIFT, IIP_##F##_BITS)
#define IIP_GETI(regs, F, i)                                                                                      \
    iip_regs_get(regs, IIP_##F##_REG + (i) * IIP_##F##_STRIDE, IIP_##F##_SHIFT, IIP_##F##_BITS)
#define IIP_TABLE(regs, F, i) iip_regs_table(regs, IIP_##F##_REG, i)

#define IIP_IRQ_EN_UNITS_REG   0x10
#define IIP_IRQ_EN_UNITS_SHIFT 0
#define IIP_IRQ_EN_UNITS_BITS  8

#define IIP_STATUS_FLAG_REG   0x18
#define IIP_STATUS_FLAG_SHIFT 0
#define IIP_STATUS_FLAG_BITS  32

#define IIP_STATUS_UNITS_REG   0x1c
#define IIP_STATUS_UNITS_SHIFT 0
#define IIP_STATUS_UNITS_BITS  32

#define IIP_STATUS_LV_REG   0x20
#define IIP_STATUS_LV_SHIFT 0
#define IIP_STATUS_LV_BITS  32

#define IIP_WIN_COMMIT_REG   0x2c
#define IIP_WIN_COMMIT_SHIFT 0
#define IIP_WIN_COMMIT_BITS  4
#define IIP_CH1_FRAMES_REG   0x44
#define IIP_CH1_FRAMES_SHIFT 0
#define IIP_CH1_FRAMES_BITS  32

#define IIP_WIN_EN_REG    0x400
#define IIP_WIN_EN_SHIFT  0
#define IIP_WIN_EN_BITS   1
#define IIP_WIN_EN_STRIDE 0x40
#define IIP_WIN_EN_INST   16

#define IIP_WIN_PITCH0_REG    0x410
#define IIP_WIN_PITCH0_SHIFT  0
#define IIP_WIN_PITCH0_BITS   17
#define IIP_WIN_PITCH0_STRIDE 0x40

#define IIP_WIN_PITCH0_INST   16
#define IIP_WIN_PITCH1_REG    0x414
#define IIP_WIN_PITCH1_SHIFT  0
#define IIP_WIN_PITCH1_BITS   17
#define IIP_WIN_PITCH1_STRIDE 0x40
#define IIP_WIN_PITCH1_INST   16

#define IIP_WIN_H_REG    0x420
#define IIP_WIN_H_SHIFT  16
#define IIP_WIN_H_BITS   14
#define IIP_WIN_H_STRIDE 0x40
#define IIP_WIN_H_INST   16

#define IIP_WIN_W_REG    0x420
#define IIP_WIN_W_SHIFT  0
#define IIP_WIN_W_BITS   15
#define IIP_WIN_W_STRIDE 0x40
#define IIP_WIN_W_INST   16

#define IIP_WIN_ADDR0_REG    0x424
#define IIP_WIN_ADDR0_SHIFT  0
#define IIP_WIN_ADDR0_BITS   32
#define IIP_WIN_ADDR0_STRIDE 0x40
#define IIP_WIN_ADDR0_INST   16

#define IIP_WIN_ADDR1_REG    0x428
#define IIP_WIN_ADDR1_SHIFT  0
#define IIP_WIN_ADDR1_BITS   32
#define IIP_WIN_ADDR1_STRIDE 0x40
#define IIP_WIN_ADDR1_INST   16

#define IIP_UNIT_START_REG    0x800
#define IIP_UNIT_START_SHIFT  0
#define IIP_UNIT_START_BITS   1
#define IIP_UNIT_START_STRIDE 0x20
#define IIP_UNIT_START_INST   53

#define IIP_UNIT_LIST_REG    0x810
#define IIP_UNIT_LIST_SHIFT  0
#define IIP_UNIT_LIST_BITS   32
#define IIP_UNIT_LIST_STRIDE 0x20
#define IIP_UNIT_LIST_INST   53

#define IIP_UNIT_LOAD_REG    0x814
#define IIP_UNIT_LOAD_SHIFT  0
#define IIP_UNIT_LOAD_BITS   1
#define IIP_UNIT_LOAD_STRIDE 0x20
#define IIP_UNIT_LOAD_INST   53

#define IIP_CH_RUNNING_REG    0x2000
#define IIP_CH_RUNNING_SHIFT  1
#define IIP_CH_RUNNING_BITS   1
#define IIP_CH_RUNNING_STRIDE 0x100
#define IIP_CH_RUNNING_INST   2

#define IIP_CH_START_REG    0x2000
#define IIP_CH_START_SHIFT  0
#define IIP_CH_START_BITS   1
#define IIP_CH_START_STRIDE 0x100
#define IIP_CH_START_INST   2

#define IIP_CH_MASK_A_REG    0x2010
#define IIP_CH_MASK_A_SHIFT  0
#define IIP_CH_MASK_A_BITS   32
#define IIP_CH_MASK_A_STRIDE 0x100
#define IIP_CH_MASK_A_INST   2

#define IIP_CH_MASK_B_REG    0x2014
#define IIP_CH_MASK_B_SHIFT  0
#define IIP_CH_MASK_B_BITS   32
#define IIP_CH_MASK_B_STRIDE 0x100
#define IIP_CH_MASK_B_INST   2

#define IIP_CH_ROW_REG    0x2040
#define IIP_CH_ROW_SHIFT  0
#define IIP_CH_ROW_BITS   32
#define IIP_CH_ROW_STRIDE 0x100
#define IIP_CH_ROW_INST   2

#define IIP_RING_REG    0x2200
#define IIP_RING_SHIFT  0
#define IIP_RING_BITS   32
#define IIP_RING_STRIDE 0x100
#define IIP_RING_INST   2

#define IIP_IN_MODE_REG    0x2608
#define IIP_IN_MODE_SHIFT  0
#define IIP_IN_MODE_BITS   8
#define IIP_IN_MODE_STRIDE 0x40
#define IIP_IN_MODE_INST   2

#define IIP_READER_H_REG    0x4004
#define IIP_READER_H_SHIFT  16
#define IIP_READER_H_BITS   14
#define IIP_READER_H_STRIDE 0x10
#define IIP_READER_H_INST   8

#define IIP_READER_W_REG    0x4004
#define IIP_READER_W_SHIFT  0
#define IIP_READER_W_BITS   15
#define IIP_READER_W_STRIDE 0x10
#define IIP_READER_W_INST   8

#endif /* IIP_REGS_H */
