/*
 * JPEG encoder and decoder (0x24072000, GIC 105)
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "jpeg_model.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

/* zigzag index -> row-major index */
static const uint8_t zigzag_to_natural[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jmp;
    char msg[JMSG_LENGTH_MAX];
} JpegErr;

static void jpeg_err_exit(j_common_ptr cinfo)
{
    JpegErr *e = (JpegErr *)cinfo->err;
    e->pub.format_message(cinfo, e->msg);
    longjmp(e->jmp, 1);
}

uint8_t *jpeg_model_encode(const JpegJob *j, MipuMem mem, size_t *len, const char **err)
{
    static char msg[JMSG_LENGTH_MAX + 32];
    const unsigned f = 1u << j->shift;
    const uint32_t ow = j->w >> j->shift, oh = j->h >> j->shift;
    struct jpeg_compress_struct ci;
    JpegErr je;
    unsigned char *volatile out = NULL;
    unsigned long out_len = 0;
    uint8_t *volatile yrows = NULL, *volatile crows = NULL, *volatile line = NULL;

    *len = 0;
    if (j->shift > 3 || ow < 1 || oh < 1 || j->w > JPEG_MODEL_MAX_DIM || j->h > JPEG_MODEL_MAX_DIM ||
        j->ypitch < j->w || j->cpitch < (j->w & ~1u)) {
        snprintf(msg, sizeof(msg), "bad geometry %ux%u >> %u, pitches %u/%u", j->w, j->h, j->shift, j->ypitch,
                 j->cpitch);
        *err = msg;
        return NULL;
    }
    yrows = malloc((size_t)j->ypitch * f);
    crows = malloc((size_t)j->cpitch * f);
    line = malloc((size_t)ow * 3);
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jpeg_err_exit;
    if (setjmp(je.jmp)) {
        snprintf(msg, sizeof(msg), "libjpeg: %s", je.msg);
        *err = msg;
        jpeg_destroy_compress(&ci);
        free(out);
        free(yrows);
        free(crows);
        free(line);
        return NULL;
    }
    jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci, (unsigned char **)&out, &out_len);
    ci.image_width = ow;
    ci.image_height = oh;
    ci.input_components = 3;
    ci.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&ci);
    jpeg_set_colorspace(&ci, JCS_YCbCr);
    ci.write_JFIF_header = FALSE;  /* the camera's streams carry no APP0 */
    ci.comp_info[0].h_samp_factor = 2;  /* 4:2:2 like the camera's JPEGs */
    ci.comp_info[0].v_samp_factor = 1;
    for (int t = 0; t < 2; t++) {
        unsigned int nat[64];
        for (int k = 0; k < 64; k++) {
            nat[zigzag_to_natural[k]] = j->qtab[t][k] ? j->qtab[t][k] : 1;
        }
        jpeg_add_quant_table(&ci, t, nat, 100, TRUE);
    }
    jpeg_start_compress(&ci, TRUE);
    for (uint32_t oy = 0; oy < oh; oy++) {
        /* the f source rows of this output row, and their chroma (NV12: one CbCr row per two) */
        for (unsigned k = 0; k < f; k++) {
            uint32_t sy = oy * f + k;
            mem.read(mem.opaque, j->y + (uint64_t)sy * j->ypitch, yrows + (size_t)k * j->ypitch, j->w);
            mem.read(mem.opaque, j->c + (uint64_t)(sy / 2) * j->cpitch, crows + (size_t)k * j->cpitch, j->w & ~1u);
        }
        for (uint32_t ox = 0; ox < ow; ox++) {
            unsigned ys = 0, cbs = 0, crs = 0;
            for (unsigned k = 0; k < f; k++) {
                const uint8_t *yr = yrows + (size_t)k * j->ypitch;
                const uint8_t *cr = crows + (size_t)k * j->cpitch;
                for (unsigned m = 0; m < f; m++) {
                    uint32_t sx = ox * f + m;
                    ys += yr[sx];
                    cbs += cr[sx & ~1u];
                    crs += cr[(sx & ~1u) + 1];
                }
            }
            line[ox * 3] = ys / (f * f);
            line[ox * 3 + 1] = cbs / (f * f);
            line[ox * 3 + 2] = crs / (f * f);
        }
        JSAMPROW row = (JSAMPROW)line;
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    free(yrows);
    free(crows);
    free(line);
    *len = out_len;
    return out;
}

int jpeg_model_decode(const JpegDecJob *j, MipuMem mem, uint32_t *w, uint32_t *h, uint32_t *used, const char **err)
{
    static char msg[JMSG_LENGTH_MAX + 32];
    struct jpeg_decompress_struct di;
    JpegErr je;
    uint8_t *volatile in = NULL, *volatile yrows = NULL, *volatile crows = NULL, *volatile line = NULL;
    uint16_t *volatile acc = NULL;
    uint32_t len = 0;

    *w = *h = *used = 0;
    if (j->shift > 3 || !j->max_len || j->max_len > 64u << 20) {
        snprintf(msg, sizeof(msg), "bad decode job: shift %u, %u bytes", j->shift, j->max_len);
        *err = msg;
        return -1;
    }
    in = malloc(j->max_len);
    if (!in) {
        *err = "out of memory";
        return -1;
    }
    mem.read(mem.opaque, j->src, in, j->max_len);
    if (j->max_len < 4 || in[0] != 0xff || in[1] != 0xd8) {
        snprintf(msg, sizeof(msg), "no SOI at %#llx", (unsigned long long)j->src);
        free(in);
        *err = msg;
        return -1;
    }
    for (len = 2; len + 1 < j->max_len && !(in[len] == 0xff && in[len + 1] == 0xd9); len++) {
    }
    len = len + 2 <= j->max_len ? len + 2 : j->max_len;  /* through EOI, or all of it */

    di.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jpeg_err_exit;
    if (setjmp(je.jmp)) {
        snprintf(msg, sizeof(msg), "libjpeg: %s", je.msg);
        jpeg_destroy_decompress(&di);
        free(in);
        free(yrows);
        free(crows);
        free(line);
        free(acc);
        *err = msg;
        return -1;
    }
    jpeg_create_decompress(&di);
    jpeg_mem_src(&di, in, len);
    jpeg_read_header(&di, TRUE);
    di.out_color_space = JCS_YCbCr;
    di.scale_num = 1;
    di.scale_denom = 1u << j->shift;
    jpeg_start_decompress(&di);
    const uint32_t ow = di.output_width, oh = di.output_height;
    const uint32_t align = j->pitch_align ? j->pitch_align : 1;
    const uint32_t pitch = j->pitch ? j->pitch : (ow + align - 1) / align * align;
    if (ow < 2 || oh < 2 || ow > JPEG_MODEL_MAX_DIM || oh > JPEG_MODEL_MAX_DIM || pitch < ow) {
        snprintf(msg, sizeof(msg), "bad decoded size %ux%u, pitch %u", ow, oh, pitch);
        longjmp(je.jmp, 1);
    }
    yrows = calloc((size_t)pitch, 2);
    crows = calloc((size_t)pitch, 2);
    line = malloc((size_t)ow * 3);
    acc = calloc((size_t)ow + 1, sizeof(uint16_t));
    if (!yrows || !crows || !line || !acc) {
        snprintf(msg, sizeof(msg), "out of memory");
        longjmp(je.jmp, 1);
    }
    /* two decoded rows at a time: both Y rows, and one NV12 chroma row averaged over the 2x2 block */
    for (uint32_t r = 0; r < oh; r += 2) {
        unsigned n = r + 1 < oh ? 2 : 1;
        memset(crows, 0, pitch);
        memset(acc, 0, ((size_t)ow + 1) * sizeof(uint16_t));
        for (unsigned k = 0; k < n; k++) {
            JSAMPROW row = line;
            jpeg_read_scanlines(&di, &row, 1);
            for (uint32_t x = 0; x < ow; x++) {
                yrows[(size_t)k * pitch + x] = line[x * 3];
                acc[(x & ~1u)] += line[x * 3 + 1];
                acc[(x & ~1u) + 1] += line[x * 3 + 2];
            }
        }
        for (uint32_t x = 0; x + 1 < ow; x += 2) {
            const unsigned div = 2 * n;
            crows[x] = (uint8_t)((acc[x] + div / 2) / div);
            crows[x + 1] = (uint8_t)((acc[x + 1] + div / 2) / div);
        }
        mem.write(mem.opaque, j->y + (uint64_t)r * pitch, yrows, (size_t)pitch * n);
        mem.write(mem.opaque, (j->c ? j->c : j->y + (uint64_t)pitch * oh) + (uint64_t)(r / 2) * pitch, crows,
                  pitch);
    }
    jpeg_finish_decompress(&di);
    jpeg_destroy_decompress(&di);
    free(in);
    free(yrows);
    free(crows);
    free(line);
    free(acc);
    *w = ow;
    *h = oh;
    *used = len;
    return 0;
}
