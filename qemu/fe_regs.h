/* FE register fields
 *
 * The register field definitions the emulator's models use.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FE_REGS_H
#define FE_REGS_H

#include <stdint.h>

static inline uint32_t fe_regs_get(const uint32_t *regs, unsigned reg, unsigned shift, unsigned bits)
{
    return (uint32_t)((regs[reg / 4] >> shift) & ((1ull << bits) - 1));
}

static inline int32_t fe_regs_gets(const uint32_t *regs, unsigned reg, unsigned shift, unsigned bits)
{
    uint32_t v = fe_regs_get(regs, reg, shift, bits);
    return (v >> (bits - 1)) & 1 ? (int32_t)(v - (1ull << bits)) : (int32_t)v;
}

static inline uint16_t fe_regs_table(const uint32_t *regs, unsigned reg, unsigned i)
{
    return (uint16_t)(regs[reg / 4 + i / 2] >> ((i & 1) * 16));
}

#define FE_GET(regs, F)      fe_regs_get(regs, FE_##F##_REG, FE_##F##_SHIFT, FE_##F##_BITS)
#define FE_GETS(regs, F)     fe_regs_gets(regs, FE_##F##_REG, FE_##F##_SHIFT, FE_##F##_BITS)
#define FE_GETI(regs, F, i)  fe_regs_get(regs, FE_##F##_REG + (i) * FE_##F##_STRIDE, FE_##F##_SHIFT, FE_##F##_BITS)
#define FE_TABLE(regs, F, i) fe_regs_table(regs, FE_##F##_REG, i)

#define FE_DGAIN0_REG   0x7d8
#define FE_DGAIN0_SHIFT 0
#define FE_DGAIN0_BITS  32

#define FE_DGAIN1_REG   0x7dc
#define FE_DGAIN1_SHIFT 0
#define FE_DGAIN1_BITS  32

#define FE_DGAIN2_REG   0x7e0
#define FE_DGAIN2_SHIFT 0
#define FE_DGAIN2_BITS  32

#define FE_DGAIN3_REG   0x7e4
#define FE_DGAIN3_SHIFT 0
#define FE_DGAIN3_BITS  32

#define FE_RAWA_DPCM_REG   0x1500
#define FE_RAWA_DPCM_SHIFT 2
#define FE_RAWA_DPCM_BITS  1

#define FE_RAWA_DEPTH_REG   0x1500
#define FE_RAWA_DEPTH_SHIFT 0
#define FE_RAWA_DEPTH_BITS  2

#define FE_RAWA_W0_REG   0x1504
#define FE_RAWA_W0_SHIFT 16
#define FE_RAWA_W0_BITS  16

#define FE_RAWA_YEND_REG   0x1508
#define FE_RAWA_YEND_SHIFT 16
#define FE_RAWA_YEND_BITS  16

#define FE_RAWA_Y0_REG   0x1508
#define FE_RAWA_Y0_SHIFT 0
#define FE_RAWA_Y0_BITS  16

#define FE_RAWA_SPLIT_REG   0x150c
#define FE_RAWA_SPLIT_SHIFT 0
#define FE_RAWA_SPLIT_BITS  1

#define FE_RAWA_W1_REG   0x1510
#define FE_RAWA_W1_SHIFT 0
#define FE_RAWA_W1_BITS  16

#define FE_RAWB_DPCM_REG   0x1514
#define FE_RAWB_DPCM_SHIFT 2
#define FE_RAWB_DPCM_BITS  1

#define FE_RAWB_DEPTH_REG   0x1514
#define FE_RAWB_DEPTH_SHIFT 0
#define FE_RAWB_DEPTH_BITS  2

#define FE_RAWB_W_REG   0x1518
#define FE_RAWB_W_SHIFT 16
#define FE_RAWB_W_BITS  16

#define FE_RAWB_H_REG   0x151c
#define FE_RAWB_H_SHIFT 16
#define FE_RAWB_H_BITS  16

#define FE_RAW_RUN_REG   0x152c
#define FE_RAW_RUN_SHIFT 0
#define FE_RAW_RUN_BITS  1

#define FE_COMPAND_REG 0x1600
#define FE_COMPAND_N   256

#define FE_OUT0_PITCH_REG   0x1808
#define FE_OUT0_PITCH_SHIFT 0
#define FE_OUT0_PITCH_BITS  16

#define FE_OUT0_ADDR0_REG   0x1850
#define FE_OUT0_ADDR0_SHIFT 0
#define FE_OUT0_ADDR0_BITS  32

#define FE_OUT0_ADDR1_REG   0x1854
#define FE_OUT0_ADDR1_SHIFT 0
#define FE_OUT0_ADDR1_BITS  32

#define FE_OUT0_ADDR2_REG   0x1858
#define FE_OUT0_ADDR2_SHIFT 0
#define FE_OUT0_ADDR2_BITS  32

#define FE_OUT0_ADDR3_REG   0x185c
#define FE_OUT0_ADDR3_SHIFT 0
#define FE_OUT0_ADDR3_BITS  32

#define FE_OUT1_PITCH_REG   0x1908
#define FE_OUT1_PITCH_SHIFT 0
#define FE_OUT1_PITCH_BITS  16

#define FE_OUT1_ADDR0_REG   0x1950
#define FE_OUT1_ADDR0_SHIFT 0
#define FE_OUT1_ADDR0_BITS  32

#define FE_OUT2_PITCH_REG   0x1a08
#define FE_OUT2_PITCH_SHIFT 0
#define FE_OUT2_PITCH_BITS  16

#define FE_OUT2_ADDR0_REG   0x1a50
#define FE_OUT2_ADDR0_SHIFT 0
#define FE_OUT2_ADDR0_BITS  32

#define FE_OUT2_ADDR1_REG   0x1a54
#define FE_OUT2_ADDR1_SHIFT 0
#define FE_OUT2_ADDR1_BITS  32

#define FE_OUT2_ADDR2_REG   0x1a58
#define FE_OUT2_ADDR2_SHIFT 0
#define FE_OUT2_ADDR2_BITS  32

#define FE_OUT2_ADDR3_REG   0x1a5c
#define FE_OUT2_ADDR3_SHIFT 0
#define FE_OUT2_ADDR3_BITS  32

#define FE_IN_H_REG   0x1d00
#define FE_IN_H_SHIFT 16
#define FE_IN_H_BITS  16

#define FE_IN_W_REG   0x1d04
#define FE_IN_W_SHIFT 16
#define FE_IN_W_BITS  16

#endif /* FE_REGS_H */
