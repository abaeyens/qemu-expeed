/*
 * MIPU register fields
 *
 * The register field definitions the emulator models use.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MIPU_REGS_H
#define MIPU_REGS_H

#include <stdint.h>

static inline uint32_t mipu_regs_get(const uint32_t *regs, unsigned reg, unsigned shift, unsigned bits)
{
    return (uint32_t)((regs[reg / 4] >> shift) & ((1ull << bits) - 1));
}

static inline int32_t mipu_regs_gets(const uint32_t *regs, unsigned reg, unsigned shift, unsigned bits)
{
    uint32_t v = mipu_regs_get(regs, reg, shift, bits);
    return (v >> (bits - 1)) & 1 ? (int32_t)(v - (1ull << bits)) : (int32_t)v;
}

/* entry i of a u16 table starting at register reg, two per register, low half first */
static inline uint16_t mipu_regs_table(const uint32_t *regs, unsigned reg, unsigned i)
{
    return (uint16_t)(regs[reg / 4 + i / 2] >> ((i & 1) * 16));
}

#define MIPU_GET(regs, F)  mipu_regs_get(regs, MIPU_##F##_REG, MIPU_##F##_SHIFT, MIPU_##F##_BITS)
#define MIPU_GETS(regs, F) mipu_regs_gets(regs, MIPU_##F##_REG, MIPU_##F##_SHIFT, MIPU_##F##_BITS)
#define MIPU_GETI(regs, F, i)                                                                                     \
    mipu_regs_get(regs, MIPU_##F##_REG + (i) * MIPU_##F##_STRIDE, MIPU_##F##_SHIFT, MIPU_##F##_BITS)
#define MIPU_TABLE(regs, F, i) mipu_regs_table(regs, MIPU_##F##_REG, i)

#define MIPU_FRAME_W_REG   0x48
#define MIPU_FRAME_W_SHIFT 16
#define MIPU_FRAME_W_BITS  16

#define MIPU_FRAME_H_REG   0x48
#define MIPU_FRAME_H_SHIFT 0
#define MIPU_FRAME_H_BITS  16


#define MIPU_IN_MODE_REG   0xa0
#define MIPU_IN_MODE_SHIFT 0
#define MIPU_IN_MODE_BITS  2

#define MIPU_RAW_FMT_REG   0x300
#define MIPU_RAW_FMT_SHIFT 0
#define MIPU_RAW_FMT_BITS  8

#define MIPU_RAW_PITCH_REG   0x30c
#define MIPU_RAW_PITCH_SHIFT 0
#define MIPU_RAW_PITCH_BITS  16

#define MIPU_RAW_ADDR0_REG   0x310
#define MIPU_RAW_ADDR0_SHIFT 0
#define MIPU_RAW_ADDR0_BITS  32

#define MIPU_RAW_ADDR1_REG   0x314
#define MIPU_RAW_ADDR1_SHIFT 0
#define MIPU_RAW_ADDR1_BITS  32

#define MIPU_RAWOUT_CTL_REG   0x3e8
#define MIPU_RAWOUT_CTL_SHIFT 0
#define MIPU_RAWOUT_CTL_BITS  1

#define MIPU_RAWOUT_ADDR_REG   0x3ec
#define MIPU_RAWOUT_ADDR_SHIFT 0
#define MIPU_RAWOUT_ADDR_BITS  32

#define MIPU_RAWOUT_PITCH_REG   0x3f0
#define MIPU_RAWOUT_PITCH_SHIFT 0
#define MIPU_RAWOUT_PITCH_BITS  16

#define MIPU_CROP_Y0_REG   0x400
#define MIPU_CROP_Y0_SHIFT 16
#define MIPU_CROP_Y0_BITS  16

#define MIPU_CROP_H_REG   0x400
#define MIPU_CROP_H_SHIFT 0
#define MIPU_CROP_H_BITS  16

#define MIPU_CROP_W_REG   0x40c
#define MIPU_CROP_W_SHIFT 16
#define MIPU_CROP_W_BITS  16

#define MIPU_CROP_X0_REG   0x40c
#define MIPU_CROP_X0_SHIFT 0
#define MIPU_CROP_X0_BITS  16

#define MIPU_OUT_Y_ADDR_REG   0x488
#define MIPU_OUT_Y_ADDR_SHIFT 0
#define MIPU_OUT_Y_ADDR_BITS  32

#define MIPU_OUT_Y_ADDR1_REG   0x48c
#define MIPU_OUT_Y_ADDR1_SHIFT 0
#define MIPU_OUT_Y_ADDR1_BITS  32

#define MIPU_OUT_Y_ADDR2_REG   0x490
#define MIPU_OUT_Y_ADDR2_SHIFT 0
#define MIPU_OUT_Y_ADDR2_BITS  32

#define MIPU_OUT_Y_ADDR3_REG   0x494
#define MIPU_OUT_Y_ADDR3_SHIFT 0
#define MIPU_OUT_Y_ADDR3_BITS  32

#define MIPU_OUT_C_ADDR_REG   0x498
#define MIPU_OUT_C_ADDR_SHIFT 0
#define MIPU_OUT_C_ADDR_BITS  32

#define MIPU_OUT_C_ADDR1_REG   0x49c
#define MIPU_OUT_C_ADDR1_SHIFT 0
#define MIPU_OUT_C_ADDR1_BITS  32

#define MIPU_OUT_C_ADDR2_REG   0x4a0
#define MIPU_OUT_C_ADDR2_SHIFT 0
#define MIPU_OUT_C_ADDR2_BITS  32

#define MIPU_OUT_C_ADDR3_REG   0x4a4
#define MIPU_OUT_C_ADDR3_SHIFT 0
#define MIPU_OUT_C_ADDR3_BITS  32

#define MIPU_OUT_PITCH_REG   0x4c8
#define MIPU_OUT_PITCH_SHIFT 0
#define MIPU_OUT_PITCH_BITS  16

#define MIPU_SLOTA_W7_REG   0x67c
#define MIPU_SLOTA_W7_SHIFT 0
#define MIPU_SLOTA_W7_BITS  16

#define MIPU_RGGB_GAIN_R_REG   0x708
#define MIPU_RGGB_GAIN_R_SHIFT 16
#define MIPU_RGGB_GAIN_R_BITS  16

#define MIPU_RGGB_GAIN_GR_REG   0x708
#define MIPU_RGGB_GAIN_GR_SHIFT 0
#define MIPU_RGGB_GAIN_GR_BITS  16

#define MIPU_RGGB_GAIN_GB_REG   0x70c
#define MIPU_RGGB_GAIN_GB_SHIFT 16
#define MIPU_RGGB_GAIN_GB_BITS  16

#define MIPU_RGGB_GAIN_B_REG   0x70c
#define MIPU_RGGB_GAIN_B_SHIFT 0
#define MIPU_RGGB_GAIN_B_BITS  16

#define MIPU_RGGB_IN_OFFSET_R_REG   0x710
#define MIPU_RGGB_IN_OFFSET_R_SHIFT 16
#define MIPU_RGGB_IN_OFFSET_R_BITS  13

#define MIPU_RGGB_IN_OFFSET_GR_REG   0x710
#define MIPU_RGGB_IN_OFFSET_GR_SHIFT 0
#define MIPU_RGGB_IN_OFFSET_GR_BITS  13

#define MIPU_RGGB_IN_OFFSET_GB_REG   0x714
#define MIPU_RGGB_IN_OFFSET_GB_SHIFT 16
#define MIPU_RGGB_IN_OFFSET_GB_BITS  13

#define MIPU_RGGB_IN_OFFSET_B_REG   0x714
#define MIPU_RGGB_IN_OFFSET_B_SHIFT 0
#define MIPU_RGGB_IN_OFFSET_B_BITS  13

#define MIPU_RGGB_OUT_OFFSET_R_REG   0x718
#define MIPU_RGGB_OUT_OFFSET_R_SHIFT 16
#define MIPU_RGGB_OUT_OFFSET_R_BITS  16

#define MIPU_RGGB_OUT_OFFSET_GR_REG   0x718
#define MIPU_RGGB_OUT_OFFSET_GR_SHIFT 0
#define MIPU_RGGB_OUT_OFFSET_GR_BITS  16

#define MIPU_RGGB_OUT_OFFSET_GB_REG   0x71c
#define MIPU_RGGB_OUT_OFFSET_GB_SHIFT 16
#define MIPU_RGGB_OUT_OFFSET_GB_BITS  16

#define MIPU_RGGB_OUT_OFFSET_B_REG   0x71c
#define MIPU_RGGB_OUT_OFFSET_B_SHIFT 0
#define MIPU_RGGB_OUT_OFFSET_B_BITS  16

#define MIPU_RGGB_CLIP_REG   0x720
#define MIPU_RGGB_CLIP_SHIFT 0
#define MIPU_RGGB_CLIP_BITS  16

#define MIPU_TONE_OFFSET_REG   0x724
#define MIPU_TONE_OFFSET_SHIFT 0
#define MIPU_TONE_OFFSET_BITS  16

#define MIPU_CMAT_0_REG   0xb00
#define MIPU_CMAT_0_SHIFT 0
#define MIPU_CMAT_0_BITS  13

#define MIPU_CMAT_1_REG   0xb04
#define MIPU_CMAT_1_SHIFT 16
#define MIPU_CMAT_1_BITS  13

#define MIPU_CMAT_2_REG   0xb04
#define MIPU_CMAT_2_SHIFT 0
#define MIPU_CMAT_2_BITS  13

#define MIPU_CMAT_3_REG   0xb08
#define MIPU_CMAT_3_SHIFT 16
#define MIPU_CMAT_3_BITS  13

#define MIPU_CMAT_4_REG   0xb08
#define MIPU_CMAT_4_SHIFT 0
#define MIPU_CMAT_4_BITS  13

#define MIPU_CMAT_5_REG   0xb0c
#define MIPU_CMAT_5_SHIFT 16
#define MIPU_CMAT_5_BITS  13

#define MIPU_CMAT_6_REG   0xb0c
#define MIPU_CMAT_6_SHIFT 0
#define MIPU_CMAT_6_BITS  13

#define MIPU_CMAT_7_REG   0xb10
#define MIPU_CMAT_7_SHIFT 16
#define MIPU_CMAT_7_BITS  13

#define MIPU_CMAT_8_REG   0xb10
#define MIPU_CMAT_8_SHIFT 0
#define MIPU_CMAT_8_BITS  13

#define MIPU_RGB2YCC_MAT_0_REG   0xb64
#define MIPU_RGB2YCC_MAT_0_SHIFT 0
#define MIPU_RGB2YCC_MAT_0_BITS  12

#define MIPU_RGB2YCC_MAT_1_REG   0xb68
#define MIPU_RGB2YCC_MAT_1_SHIFT 16
#define MIPU_RGB2YCC_MAT_1_BITS  12

#define MIPU_RGB2YCC_MAT_2_REG   0xb68
#define MIPU_RGB2YCC_MAT_2_SHIFT 0
#define MIPU_RGB2YCC_MAT_2_BITS  12

#define MIPU_RGB2YCC_MAT_3_REG   0xb6c
#define MIPU_RGB2YCC_MAT_3_SHIFT 16
#define MIPU_RGB2YCC_MAT_3_BITS  12

#define MIPU_RGB2YCC_MAT_4_REG   0xb6c
#define MIPU_RGB2YCC_MAT_4_SHIFT 0
#define MIPU_RGB2YCC_MAT_4_BITS  12

#define MIPU_RGB2YCC_MAT_5_REG   0xb70
#define MIPU_RGB2YCC_MAT_5_SHIFT 16
#define MIPU_RGB2YCC_MAT_5_BITS  12

#define MIPU_RGB2YCC_MAT_6_REG   0xb70
#define MIPU_RGB2YCC_MAT_6_SHIFT 0
#define MIPU_RGB2YCC_MAT_6_BITS  12

#define MIPU_RGB2YCC_MAT_7_REG   0xb74
#define MIPU_RGB2YCC_MAT_7_SHIFT 16
#define MIPU_RGB2YCC_MAT_7_BITS  12

#define MIPU_RGB2YCC_MAT_8_REG   0xb74
#define MIPU_RGB2YCC_MAT_8_SHIFT 0
#define MIPU_RGB2YCC_MAT_8_BITS  12

#define MIPU_Y2X_GAIN_TABLE_WEIGHT_B_REG   0xbac
#define MIPU_Y2X_GAIN_TABLE_WEIGHT_B_SHIFT 16
#define MIPU_Y2X_GAIN_TABLE_WEIGHT_B_BITS  16

#define MIPU_Y2X_GAIN_TABLE_WEIGHT_A_REG   0xbac
#define MIPU_Y2X_GAIN_TABLE_WEIGHT_A_SHIFT 0
#define MIPU_Y2X_GAIN_TABLE_WEIGHT_A_BITS  16

#define MIPU_LV_DECOMPAND_REG 0x2100
#define MIPU_LV_DECOMPAND_N   256

#define MIPU_TONE_LUT_R_REG 0x3900
#define MIPU_TONE_LUT_R_N   205

#define MIPU_TONE_LUT_G_REG 0x3c40
#define MIPU_TONE_LUT_G_N   205

#define MIPU_TONE_LUT_B_REG 0x3f80
#define MIPU_TONE_LUT_B_N   205

#define MIPU_Y2Y_GAIN_TABLE_A_REG 0x5134
#define MIPU_Y2Y_GAIN_TABLE_A_N   257

#define MIPU_Y2Y_GAIN_TABLE_B_REG 0x533c
#define MIPU_Y2Y_GAIN_TABLE_B_N   257

#define MIPU_Y2C_GAIN_TABLE_A_REG 0x5544
#define MIPU_Y2C_GAIN_TABLE_A_N   257

#define MIPU_Y2C_GAIN_TABLE_B_REG 0x574c
#define MIPU_Y2C_GAIN_TABLE_B_N   257

#endif /* MIPU_REGS_H */
