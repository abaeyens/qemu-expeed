/*
 * The Expeed 6 SIO block's Ex MCU link state
 *
 * the board's Ex MCU (z6ii_exmcu.c: its pushes, the keys, the third cluster)
 * shares with the link's model (expeed6_sio.c).
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef EXPEED6_SIO_H
#define EXPEED6_SIO_H

#include "expeed6.h"

#define EXMCU_TXBUF       4096
#define EXMCU_IST_TX      (1u << 1)
#define EXMCU_IST_RX      (1u << 2)
#define EXMCU_BLOCK       128
#define EXMCU_STATUS_PLEN 0x7c

#define SIO_CHANNELS   16
#define EXMCU_PUSH_MAX 1024

typedef struct {
    Z6iiExmcuState *s;
    char *path;
    QEMUTimer *t;
    int64_t first_ns, due_ns, period_ns;
} ExmcuPush;

#define TYPE_EXPEED6_SIO "expeed6-sio"
OBJECT_DECLARE_SIMPLE_TYPE(Z6iiExmcuState, EXPEED6_SIO)

struct Z6iiExmcuState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    char *reply_dir_prop, *push_spec;
    ExmcuPush push[EXMCU_PUSH_MAX];
    uint32_t n_push;
    char *push_paths;
    uint32_t push_paths_len;
    uint32_t cfg[SIO_CHANNELS][0x40];
    uint32_t ien[SIO_CHANNELS];
    uint32_t ist[SIO_CHANNELS];
    uint32_t rxq[SIO_CHANNELS];
    int32_t lvl[SIO_CHANNELS];
    qemu_irq irq[SIO_CHANNELS][2];
    uint16_t tx[EXMCU_TXBUF];
    uint32_t txn;
    uint32_t rxpos;
    int32_t mode;
    Expeed6ExtIntState *extint;
    char *rdir;
    uint8_t rq[8][256];
    uint32_t rq_head, rq_tail;
    uint8_t rlast[256];
    bool rlast_valid;
    uint8_t rcur[256];
    uint8_t status_last[EXMCU_STATUS_PLEN];
    bool status_valid;

    struct Z6iiExmcuSeq {
        uint16_t op;
        uint32_t n;
    } rseq[16];
    uint32_t exchange;
    uint32_t tx1n, rx1pos, exch1;
    AddressSpace *as;
};

void exmcu_reply_pin_update(Z6iiExmcuState *s);

#endif
