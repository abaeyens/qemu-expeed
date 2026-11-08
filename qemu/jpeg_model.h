/*
 * JPEG encoder and decoder (0x24072000, GIC 105)
 *
 * - no QEMU in here
 * - uses libjpeg-turbo and its bitstream, so no exact match with the camera
 * - downscale with box filter
 * - always 4:2:2, no JFIF APP0 (matching the Expeed encoder)
 * - writes the whole stream (like the Expeed encoder)
 *
 * TODO: Check the camera indeed reads and writes the images as NV12
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef JPEG_MODEL_H
#define JPEG_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "mipu_model.h"     /* Use MipuMem memory interface */

#define JPEG_MODEL_MAX_DIM 16384

/**
 * struct JpegJob: Job for the JPEG encoder
 * @y: NV12 input luma plane address
 * @c: NV12 input chroma plane address
 * @ypitch: source luma pitch
 * @cpitch: source chroma pitch
 * @w: source image width
 * @h: source image height
 * @shift: input downscaler: ``output size = source size >> shift``, range 0..3
 * @qtab: luma and chroma quantisation tables in zigzag order as in DQT
 */
typedef struct JpegJob {
    uint64_t y, c;
    uint32_t ypitch, cpitch;
    uint32_t w, h;
    unsigned shift;
    uint8_t qtab[2][64];
} JpegJob;

/**
 * jpeg_model_encode: Encode the job @j
 * @j: the job
 * @mem: memory accessor
 * @len: set to the length of the returned stream
 * @err: set to an error message (static storage) on failure
 *
 * Returns: malloc'd buffer holding a whole baseline JPEG stream (SOI..EOI),
 *     or %NULL on failure
 */
uint8_t *jpeg_model_encode(const JpegJob *j, MipuMem mem, size_t *len, const char **err);

/**
 * struct JpegDecJob: Job for the JPEG decoder
 * @src: input stream address
 * @max_len: max read length
 * @y: NV12 luma output plane address
 * @c: chroma address, if 0 then goes at ``y + pitch * height``
 * @pitch: output pitch
 * @pitch_align: if @pitch == 0, pitch is output width rounded up to multiple of @pitch_align
 * @shift: output downscaler ``output size = ceil(stream size / 2^shift)``, range 0..3
 */
typedef struct JpegDecJob {
    uint64_t src;
    uint32_t max_len;
    uint64_t y;
    uint64_t c;
    uint32_t pitch;
    uint32_t pitch_align;
    unsigned shift;
} JpegDecJob;

/**
 * jpeg_model_decode: Decode the job @j
 * @j: the job
 * @mem: memory accessor
 * @w: set to the decoded width
 * @h: set to the decoded height
 * @used: set to the stream bytes consumed
 * @err: set to an error message on failure
 *
 * Returns: 0, or -1 on failure with nothing written
 */
int jpeg_model_decode(const JpegDecJob *j, MipuMem mem, uint32_t *w, uint32_t *h, uint32_t *used,
                      const char **err);

#endif
