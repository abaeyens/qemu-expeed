/* The IIP
 *
 * Its unit graph, routed as its registers and its unit command lists say,
 * with the resize, the grid warp and the internal buffers as the only processing.
 * No QEMU in here
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef IIP_MODEL_H
#define IIP_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "iip_regs.h"     /* the IIP's register fields */
#include "mipu_model.h"   /* MipuMem: the same memory interface */

typedef struct {
    uint64_t y, c;            /* plane addresses (c = 0: single plane) */
    uint32_t ypitch, cpitch;
    uint32_t w, h;
} IipImage;

/* A resize unit's scale descriptor, named by the pointer in its register 0x54 (the save develop's builder
 * puts it 0x20 bytes before the unit's command list): word 0/1 x/y phase and word 2/5 x/y step (16.16, phase signed),
 * word 6 the resize output height << 16 | width.
 * The model maps destination pixels through it, with an integer pre-factor before the resize inferred from the sizes;
 * destination pixels whose centre falls outside the source are black -- which is how the save's 160x120 thumbnail gets
 * the camera's letterbox bars. */
typedef struct {
    int32_t phase[2]; /* x, y, 16.16 */
    uint32_t step[2]; /* x, y, 16.16 */
    uint32_t ow, oh; /* the resize output */
} IipScale;

/* Read the scale descriptor at desc; -1 if implausible. */
int iip_scale_from_desc(IipScale *sc, uint64_t desc, MipuMem mem);

/* Box-filter src to dst's size. Returns 0, or a negative value when either is implausible. */
int iip_model_resample(const IipImage *src, const IipImage *dst, MipuMem mem);

/* Resample src into dst through a scale descriptor (see IipScale).
 * Returns 0, or -1 (dst untouched) when its factors are implausible (over 4096). */
int iip_model_resample_scaled(const IipImage *src, const IipImage *dst, const IipScale *sc, MipuMem mem);

/* The tile grid a grid unit (0x2d-0x30) names: its register 0x50 holds rows << 16 | columns, 0x54 the grid's address (0
 * at 1x: no warp); on the camera the dimensions read 0x000b0010 at every zoom step, and the save
 * develop's 2:1 stage is a 32 x 48 grid too.
 * The grid, as the firmware lays it out: one 0x28-byte entry per output tile, row-major, words 0-7
 * the source position of the tile's four corners (top-left, top-right, bottom-left, bottom-right; x, y in 1/256 source
 * pixel), word 8 the output tile size as log2 (bits 0-3 width, 8-11 height: 0x606, 64 x 64), word 9 not understood
 * (0x22100).
 * At zoom step 1 the tiles step 128 source pixels for 64 output pixels from x = 2000 = (6048 - 2048) / 2, i.e. the
 * centred 2048-column crop of the 6048-wide band at 2:1.
 * That the IIP maps output pixels through it this way is inferred from the numbers (likely); the camera's grid
 * content is not read. */
#define IIP_MODEL_GRID_ENTRY 0x28

typedef struct {
    uint64_t addr;
    uint32_t cols, rows;
} IipGrid;

/* The output size a grid covers (the first row's tile widths, the first column's heights); -1 if a tile is implausible. */
int iip_grid_extent(const IipGrid *g, uint32_t *w, uint32_t *h, MipuMem mem);

/* Map every destination pixel through the grid into src (bilinear for Y, nearest CbCr pair for NV12 chroma); pixels
 * whose source falls outside src are black. Returns the number of tiles drawn, or -1 if a tile is implausible. */
int iip_model_warp(const IipImage *src, const IipImage *dst, const IipGrid *g, MipuMem mem);

/* ---- The unit graph: the IIP driven by its registers ----------------------------------------------------------
 * The IIP is a graph of units joined through a crossbar of global port ids.
 * Unit ids 0x00-0x0f are the DMA windows, 0x10-0x17 internal-buffer readers (reader 0x10+j reads what writer 0x39+j
 * wrote), 0x18-0x21 pass-through selectors with no registers, 0x22-0x4b processing units, each with a 0x20-byte block
 * (iip_unit_block) whose +0x10 holds a pointer to a register command list in DRAM: packets {u16 register offset, u8
 * words including the header, u8 flags (0x11 = another follows)} and their data words, word k of a packet being the
 * unit's register offset + 4k.
 * In those registers a producer's output-port fields hold the consumer's input port id and the consumer's input field
 * the producer's output port id, both global crossbar port ids (iip_port_owner); a unit reading memory names its window (or reader)
 * in a nibble, an output unit (0x31-0x38) its destination window in register 0x08 bits 0-3.
 * Which units form a job: on a one-shot job the units the firmware started (1 to block +0x00), on a live-view channel
 * the unit masks in its page (+0x10/+0x14, see iip_unit_in_masks).
 * The unit ids, register blocks and port ids are the hardware's numbering.
 * Of the units' processing only resize, grid warp and the internal buffers are modelled, crudely; an output whose path
 * runs through any other unit is not written. */
#define IIP_UNIT_IDS      0x4c
#define IIP_UNIT_OUT0     0x31 /* output units 0x31-0x38: status +0x1c bit id - 0x31 */
#define IIP_UNIT_OUTS     8
#define IIP_UNIT_LIST_OFF (IIP_UNIT_LIST_REG - IIP_UNIT_START_REG) /* in a unit's block: its command list */
#define IIP_UREGS         0x240 /* a unit's internal registers the model keeps (class 7's list reaches 0x22c) */
#define IIP_REGS_SIZE     0x4100 /* the register file the model reads: through the readers' (READER_W) */

/* A unit's internal registers, the ones the model reads from its command list */
#define IIP_U_OUT_WIN      0x08 /* an output unit's destination window, bits 0-3 */
#define IIP_U_SRC_KIND     0x14 /* a resize or grid unit's source kind: */
#define IIP_U_SRC_INTERNAL 0x10000000 /* ... bit 28 set, a reader or an input unit; clear, a DMA window */
#define IIP_U_SRC          0x18 /* ... and its index, bits 4-7 */
#define IIP_U_GRID_DIMS    0x50 /* a grid unit's grid: rows << 16 | columns */
#define IIP_U_GRID_ADDR    0x54 /* ... and its address (0: no warp) */
#define IIP_U_SCALE_DESC   0x54 /* a resize unit's scale descriptor */
/* the input port id of an output unit, a buffer writer and some others: register 0x00 byte 2 */
#define IIP_U_IN_PORT(u) (((u)->r[0] >> 16) & 0xff)

/* The block offset of unit id's registers from the IIP's base, 0 if it has none; and the unit whose block starts at
 * off, or -1. */
uint32_t iip_unit_block(unsigned id);
int iip_unit_at_block(uint32_t off);
/* The unit's class (0 window, 1 reader, ... 0x12), -1 for no unit. */
int iip_unit_class(unsigned id);
/* The unit owning a global port id (its port in *port), or -1. */
int iip_port_owner(unsigned pid, unsigned *port);
/* Whether unit id is in a channel's job masks A/B (its page's +0x10/+0x14). */
int iip_unit_in_masks(unsigned id, uint32_t a, uint32_t b);

/* A unit's internal registers, read from its command list; -1 if the list is not plausible. */
typedef struct {
    uint32_t r[IIP_UREGS / 4];
} IipUnitRegs;
int iip_unit_regs_load(IipUnitRegs *u, uint64_t list, MipuMem mem);

/* One job's evaluation: the register file (word-indexed from the block base), the job's units, and the ring row a
 * live-view channel is on (window n's plane 0 at slots[n], plane 1 at slots[n] + its +0x28; NULL: a one-shot job, whose
 * windows hold their own addresses).
 * A window whose slot is in the device page (0x80000000) is the MIPU's stream, given by iip_eval_stream.
 * Intermediate images (a resize's or a grid's output, the internal buffers) are kept on the host. */
typedef struct IipEval IipEval;
IipEval *iip_eval_new(const uint32_t *regs, const uint8_t *units, const uint32_t *slots, MipuMem mem);
void iip_eval_stream(IipEval *ev, const uint8_t *y, const uint8_t *c, uint32_t w, uint32_t h);
void iip_eval_free(IipEval *ev);

/* What feeds output unit out: its destination window and the path back to memory, as text ("w1>29>35>w2"). */
typedef struct {
    int win; /* the destination window, -1 if the output unit is not usable */
    IipImage dst; /* ... and its geometry, the slot's address in a ring */
    int src_win; /* the window the path starts from, -1 if none was found */
    int stream; /* 1 when that window is the MIPU's stream */
    uint64_t src_addr; /* ... and its plane 0 there */
    char path[96];
} IipOutPath;
/* Trace an output unit's path without moving pixels; -1 if it has none. */
int iip_out_trace(IipEval *ev, unsigned out, IipOutPath *p);
/* Render it into its destination window; returns 0, or -1 when there is no usable path. */
int iip_out_run(IipEval *ev, unsigned out, IipOutPath *p);
/* Why the last iip_out_trace/iip_out_run returned -1: the first refusal along its path. */
const char *iip_eval_why(const IipEval *ev);

#endif
