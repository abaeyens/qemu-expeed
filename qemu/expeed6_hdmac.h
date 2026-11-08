/*
 * The Expeed 6 HDMAC, two 8-channel units at 0x2c020000/0x2c030000
 *
 * the HDMAC's channel state, which the audio interface's DMA pacing shares
 * (expeed6_hdmac.c, expeed6_audio.c)
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef EXPEED6_HDMAC_H
#define EXPEED6_HDMAC_H

#include "expeed6.h"

#define HDMAC_BASE0    0x2c020000ULL
#define HDMAC_BASE1    0x2c030000ULL
#define HDMAC_SPI0     118
#define HDMAC_SPI1     110
#define HDMAC_A_EB     (1u << 31)
#define HDMAC_A_ST     (1u << 29)
#define HDMAC_B_FS     (1u << 25)
#define HDMAC_B_FD     (1u << 24)
#define HDMAC_B_EI     (1u << 20)
#define HDMAC_B_CI     (1u << 19)
#define HDMAC_SS_SHIFT 16
#define HDMAC_SS_MASK  (7u << HDMAC_SS_SHIFT)
#define HDMAC_SS_OK    5u
#define HDMAC_MAXLEN   0x10000

#define TYPE_EXPEED6_HDMAC "expeed6-hdmac"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6HdmacState, EXPEED6_HDMAC)

typedef struct Expeed6HdmacUnit {
    struct Expeed6HdmacState *s;
    unsigned unit;
    MemoryRegion mmio;
    uint32_t reg[0x90 / 4];
    qemu_irq irq[8];
} Expeed6HdmacUnit;

struct Expeed6HdmacState {
    SysBusDevice parent_obj;
    Expeed6HdmacUnit u[2];
    AddressSpace *as;
};

extern Expeed6HdmacState *s_hdmac;
uint32_t *hdmac_ch(Expeed6HdmacUnit *u, unsigned n, unsigned r);
void hdmac_finish(Expeed6HdmacUnit *u, unsigned n);
uint32_t hdmac_len(Expeed6HdmacUnit *u, unsigned n, unsigned *width);

bool audio_dma_kick(Expeed6HdmacUnit *u, unsigned n);
void audio_dma_cancel(Expeed6HdmacUnit *u, unsigned n);

#endif
