/*
 * The Expeed 6 audio interface at 0x2c080000 and the WM8962 codec stand-in on SIO channel 6
 *
 * References:
 * - GoPro GP2 Linux 4.19.124: sound/soc/codecs/wm8962.c
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "expeed6_hdmac.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"

#define AUDIO_IF_BASE    0x2c080000ULL
#define AUDIO_IF_SIZE    0x400
#define AUDIO_CODEC_REGS 0x6000
#define AUDIO_RX_MAX     16
#define AUDIO_RX_RATE    48000
#define AUDIO_TX_RATE    24000

typedef struct Expeed6AudioDma {
    bool active;
    bool rx;
    uint32_t dst;
    uint8_t *data;
    uint32_t len;
    QEMUTimer *t;
    struct Expeed6Audio *a;
    Expeed6HdmacUnit *u;
    unsigned n;
} Expeed6AudioDma;

#define TYPE_EXPEED6_AUDIO "expeed6-audio"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6Audio, EXPEED6_AUDIO)

struct Expeed6Audio {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t reg[AUDIO_IF_SIZE / 4];
    uint16_t codec[AUDIO_CODEC_REGS];
    uint32_t cstate;
    uint16_t creg;
    uint16_t crx[AUDIO_RX_MAX];
    uint32_t crx_n;
    Expeed6AudioDma dma[2][8];
    char *wav_path;
    FILE *wav;
    uint32_t wav_bytes;
    int64_t busy_until;
    int64_t rx_busy_until;
};

Expeed6Audio *s_audio;

static void audio_wav_header(Expeed6Audio *a)
{
    uint8_t h[44] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16,  0,   0, 0, 1, 0,
        2,   0,   0,   0,   0, 0, 0, 0, 0,   0,   4,   0,   16,  0,   'd', 'a', 't', 'a', 0, 0, 0, 0
    };
    const uint32_t rate = AUDIO_TX_RATE;
    uint32_t v = 36 + a->wav_bytes, br = rate * 4;

    memcpy(h + 4, &v, 4);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &br, 4);
    memcpy(h + 40, &a->wav_bytes, 4);
    fseek(a->wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), a->wav);
    fseek(a->wav, 0, SEEK_END);
    fflush(a->wav);
}

static void audio_capture(Expeed6Audio *a, const void *buf, uint32_t len)
{
    if (!a->wav) {
        return;
    }
    fwrite(buf, 1, len, a->wav);
    a->wav_bytes += len;
    audio_wav_header(a);
}

static bool audio_is_fifo(uint32_t addr)
{
    uint32_t o = (addr - AUDIO_IF_BASE) & 0xff;

    return addr >= AUDIO_IF_BASE && addr < AUDIO_IF_BASE + AUDIO_IF_SIZE &&
           (o == 0x04 || o == 0x30 || o == 0x34 || o == 0x3c) && (addr - AUDIO_IF_BASE) >= 0x100;
}

static bool audio_is_rx(uint32_t addr)
{
    return addr >= AUDIO_IF_BASE + 0x100 && addr < AUDIO_IF_BASE + AUDIO_IF_SIZE &&
           ((addr - AUDIO_IF_BASE) & 0xff) == 0;
}

static void audio_dma_done(void *opaque)
{
    Expeed6AudioDma *d = opaque;
    Expeed6Audio *a = d->a;

    if (!d->active) {
        return;
    }
    if (d->rx) {
        /* no microphone in emulator so capture is silent */
        address_space_write(s_hdmac->as, d->dst, MEMTXATTRS_UNSPECIFIED, d->data, d->len);
    } else {
        audio_capture(a, d->data, d->len);
    }
    g_free(d->data);
    d->data = NULL;
    d->len = 0;
    d->active = false;
    hdmac_finish(d->u, d->n);
}

bool audio_dma_kick(Expeed6HdmacUnit *u, unsigned n)
{
    Expeed6Audio *a = s_audio;
    uint32_t b, dst, src, len;
    unsigned w;

    if (!a) {
        return false;
    }
    dst = *hdmac_ch(u, n, 0xc);
    src = *hdmac_ch(u, n, 8);
    b = *hdmac_ch(u, n, 4);
    bool rx = audio_is_rx(src);
    if (!rx && (!audio_is_fifo(dst) || (b & HDMAC_B_FS))) {
        if (audio_is_fifo(src) || audio_is_fifo(dst) || audio_is_rx(dst)) {
            qemu_log_mask(LOG_UNIMP,
                          "expeed6.audio: HDMAC unit %u ch%u on an audio FIFO not handled "
                          "(src %#x dst %#x DMACB %#x)\n",
                          u->unit, n, src, dst, b);
        }
        return false;
    }
    len = hdmac_len(u, n, &w);
    uint32_t bound = rx ? 0x400000 : HDMAC_MAXLEN;
    if (!len || len > bound) {
        fprintf(stderr, "expeed6.audio: HDMAC unit %u ch%u %s of %u bytes refused (bound %u)\n", u->unit, n,
                rx ? "capture" : "transfer", len, bound);
        return false;
    }
    Expeed6AudioDma *d = &a->dma[u->unit][n];
    if (d->active) {
        timer_del(d->t);
        g_free(d->data);
    }
    d->active = true;
    d->rx = rx;
    d->dst = dst;
    d->len = len;
    if (rx) {
        d->data = g_malloc0(len);
    } else {
        d->data = g_malloc(len);
        address_space_read(s_hdmac->as, src, MEMTXATTRS_UNSPECIFIED, d->data, len);
    }
    int64_t *busy = rx ? &a->rx_busy_until : &a->busy_until;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t start = *busy > now ? *busy : now;
    const uint32_t rate = rx ? AUDIO_RX_RATE : AUDIO_TX_RATE;
    int64_t end = start + (int64_t)len * NANOSECONDS_PER_SECOND / (rate * 4);

    *busy = end;
    timer_mod(d->t, end);
    return true;
}

void audio_dma_cancel(Expeed6HdmacUnit *u, unsigned n)
{
    Expeed6Audio *a = s_audio;

    if (a && a->dma[u->unit][n].active) {
        Expeed6AudioDma *d = &a->dma[u->unit][n];

        timer_del(d->t);
        g_free(d->data);
        d->data = NULL;
        d->len = 0;
        d->active = false;
    }
}

static uint64_t audio_if_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6Audio *a = opaque;
    uint64_t v = off < AUDIO_IF_SIZE ? a->reg[off / 4] : 0;

    return v;
}

static void audio_if_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6Audio *a = opaque;

    if (off >= AUDIO_IF_SIZE) {
        return;
    }
    if (audio_is_fifo(AUDIO_IF_BASE + off)) {
        uint32_t w32 = (uint32_t)val;

        audio_capture(a, &w32, 4);
    }
    a->reg[off / 4] = (uint32_t)val;
}

static const MemoryRegionOps audio_if_ops = {
    .read = audio_if_read,
    .write = audio_if_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint16_t audio_codec_word(Expeed6Audio *a, uint16_t w)
{
    uint16_t rx = 0;

    switch (a->cstate) {
    case 1:
        if (a->creg < AUDIO_CODEC_REGS && a->creg != 0x0f) {
            a->codec[a->creg] = w;
        }
        a->cstate = 0;
        break;
    case 2:
        rx = a->creg == 0x0f ? 0x6243 : (a->creg < AUDIO_CODEC_REGS ? a->codec[a->creg] : 0);
        a->cstate = 0;
        break;
    default:
        a->creg = w & 0x7fff;
        a->cstate = (w & 0x8000) ? 2 : 1;
        break;
    }
    return rx;
}

void audio_codec_tx(Expeed6Audio *a, uint16_t w, bool rx_inhibit)
{
    uint16_t rx = audio_codec_word(a, w);

    if (!rx_inhibit && a->crx_n < AUDIO_RX_MAX) {
        a->crx[a->crx_n++] = rx;
    }
}

uint16_t audio_codec_rx(Expeed6Audio *a)
{
    uint16_t v = 0;

    if (a->crx_n) {
        v = a->crx[0];
        memmove(a->crx, a->crx + 1, (--a->crx_n) * sizeof(a->crx[0]));
    }
    return v;
}

static void audio_realize(DeviceState *dev, Error **errp)
{
    Expeed6Audio *a = EXPEED6_AUDIO(dev);

    if (!s_hdmac) {
        error_setg(errp, "expeed6-audio: needs the HDMAC model");
        return;
    }
    for (unsigned k = 0; k < 2; k++) {
        for (unsigned n = 0; n < 8; n++) {
            Expeed6AudioDma *d = &a->dma[k][n];

            d->a = a;
            d->u = &s_hdmac->u[k];
            d->n = n;
            d->t = timer_new_ns(QEMU_CLOCK_VIRTUAL, audio_dma_done, d);
        }
    }
    if (a->wav_path) {
        a->wav = fopen(a->wav_path, "w+b");
        if (!a->wav) {
            error_setg(errp, "EXPEED_AUDIO_WAV: cannot open %s", a->wav_path);
            return;
        }
        audio_wav_header(a);
    }
    memory_region_init_io(&a->mmio, OBJECT(dev), &audio_if_ops, a, "expeed6.audio_if", AUDIO_IF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &a->mmio);
}

static void audio_reset_enter(Object *obj, ResetType type)
{
    Expeed6Audio *a = EXPEED6_AUDIO(obj);

    memset(a->reg, 0, sizeof(a->reg));
    memset(a->codec, 0, sizeof(a->codec));
    a->cstate = 0;
    a->creg = 0;
    a->crx_n = 0;
    a->busy_until = a->rx_busy_until = 0;
    for (unsigned k = 0; k < 2; k++) {
        for (unsigned n = 0; n < 8; n++) {
            Expeed6AudioDma *d = &a->dma[k][n];

            timer_del(d->t);
            g_free(d->data);
            d->data = NULL;
            d->len = 0;
            d->active = false;
        }
    }
}

static const VMStateDescription vmstate_audio_dma = {
    .name = TYPE_EXPEED6_AUDIO "/dma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_BOOL(active, Expeed6AudioDma), VMSTATE_BOOL(rx, Expeed6AudioDma),
                                      VMSTATE_UINT32(dst, Expeed6AudioDma), VMSTATE_UINT32(len, Expeed6AudioDma),
                                      VMSTATE_VBUFFER_ALLOC_UINT32(data, Expeed6AudioDma, 0, NULL, len),
                                      VMSTATE_TIMER_PTR(t, Expeed6AudioDma), VMSTATE_END_OF_LIST() }
};

static const VMStateDescription vmstate_audio = {
    .name = TYPE_EXPEED6_AUDIO,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields =
        (const VMStateField[]){
            VMSTATE_UINT32_ARRAY(reg, Expeed6Audio, AUDIO_IF_SIZE / 4),
            VMSTATE_UINT16_ARRAY(codec, Expeed6Audio, AUDIO_CODEC_REGS), VMSTATE_UINT32(cstate, Expeed6Audio),
            VMSTATE_UINT16(creg, Expeed6Audio), VMSTATE_UINT16_ARRAY(crx, Expeed6Audio, AUDIO_RX_MAX),
            VMSTATE_UINT32(crx_n, Expeed6Audio),
            VMSTATE_STRUCT_2DARRAY(dma, Expeed6Audio, 2, 8, 1, vmstate_audio_dma, Expeed6AudioDma),
            VMSTATE_INT64(busy_until, Expeed6Audio), VMSTATE_INT64(rx_busy_until, Expeed6Audio),
            VMSTATE_END_OF_LIST() }
};

static Property audio_properties[] = {
    DEFINE_PROP_STRING("wav", Expeed6Audio, wav_path),
    DEFINE_PROP_END_OF_LIST(),
};

static void audio_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = audio_realize;
    dc->vmsd = &vmstate_audio;
    device_class_set_props(dc, audio_properties);
    rc->phases.enter = audio_reset_enter;
}

static const TypeInfo audio_info = {
    .name = TYPE_EXPEED6_AUDIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6Audio),
    .class_init = audio_class_init,
};

static void audio_register_types(void)
{
    type_register_static(&audio_info);
}

type_init(audio_register_types)

    void expeed6_install_audio(MemoryRegion *root)
{
    const char *wav = getenv("EXPEED_AUDIO_WAV");
    DeviceState *dev = qdev_new(TYPE_EXPEED6_AUDIO);

    if (wav) {
        qdev_prop_set_string(dev, "wav", wav);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, AUDIO_IF_BASE);
    s_audio = EXPEED6_AUDIO(dev);
}
