/*
 * The Expeed 6 SIO block at 0x2c0a0000
 *
 * Eleven serial channels: the Ex MCU link on channels 0/1 with its peer stand-in,
 * the sensor on 8, the codec on 6.
 *
 * References:
 * - Ricoh GR III Linux 1.21.2: drivers/spi/spi-sni.c
 * - Ricoh GR IV Linux 5.15.182: drivers/spi/spi-sni.c
 * - Nikon Z 8 Linux 4.14.224: drivers/spi/spi-nikon.c
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "expeed6_sio.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "z6ii.h"

#define EXMCU_UART 0x2C0A0000ULL

static bool sio_is_cfg_reg(hwaddr r)
{
    switch (r) {
    case 0x08:
    case 0x0c:
    case 0x10:
    case 0x18:
    case 0x1c:
    case 0x34:
    case 0x38:
    case 0x3c:
    case 0x4c:
        return true;
    default:
        return false;
    }
}


#define EXMCU_RDY_PORT 0x14
#define EXMCU_RDY_BIT  10
#define EXMCU_RDY_SRC  3
#define EXMCU_ACK_SRC  2
#define SIO_FIFO_DEPTH 256

static bool exmcu_reply_pending(const Z6iiExmcuState *s)
{
    return s->rq_head != s->rq_tail;
}

/* The Ex MCU's answer to a lens request (opcode 0x020d, request type in payload byte 0): command 13, class 2, a
 * 0x0c-byte payload with [0] = 0x80 | type, [4] the request's [4] echoed and [5] 0 (done) or 0x80.
 * Types 2 and 4 switch a lens-power output and type 1 with both outputs off is a status query; none of them talks to
 * the lens, so the answer is the same with or without one attached. Types 3 and 5 get no answer.
 * Without these answers the master's lens state machine times out and starts over for as long as the machine runs,
 * and its lens connection never settles, which holds a release block and the shutdown. */
static bool exmcu_lens_reply(const uint16_t *tx, uint8_t *b)
{
    const uint8_t type = tx[4] & 0xff, arg = tx[6] & 0xff, x = tx[10] & 0xff;
    uint8_t status;
    if ((tx[1] & 0xffff) != 0x10) {
        return false;
    }
    if (type == 2 || type == 4) {
        status = x <= 1 ? 0 : 0x80;
    } else if (type == 1 && x == 0) {
        status = 0;
    } else {
        return false;
    }
    memset(b, 0, 256);
    b[0] = 0x0d;
    b[1] = 2;
    b[2] = 0x0c;
    b[8] = 0x80 | type;
    b[8 + 4] = arg;
    b[8 + 5] = status;
    b[0xfc] = 1;
    b[0xfd] = 1;
    uint8_t sum = 0;
    for (int i = 0; i < 0xff; i++) {
        sum += b[i];
    }
    b[0xff] = 0xff - sum; /* the block's bytes sum to 0xff */
    return true;
}

void exmcu_reply_pin_update(Z6iiExmcuState *s)
{
    if (s_master_gpio) {
        gpio_set_input(s_master_gpio, EXMCU_RDY_PORT, EXMCU_RDY_BIT, exmcu_reply_pending(s));
    }
}

static void sio_update_irq(Z6iiExmcuState *s, unsigned ch)
{
    int level = (s->ist[ch] & s->ien[ch]) ? 1 : 0;
    if (level != s->lvl[ch]) {
        s->lvl[ch] = level;
        qemu_set_irq(s->irq[ch][0], level);
        qemu_set_irq(s->irq[ch][1], level);
    }
}

static void sio_shift_word(Z6iiExmcuState *s, unsigned ch)
{
    unsigned wmk = s->cfg[ch][0x4c / 4] & 0xff;
    s->ist[ch] |= EXMCU_IST_TX;
    if (!(s->cfg[ch][0x10 / 4] & 8)) {
        s->rxq[ch]++;
        if (s->rxq[ch] >= (wmk ? wmk : 1)) {
            s->ist[ch] |= EXMCU_IST_RX;
        }
    }
    sio_update_irq(s, ch);
}

static uint64_t exmcu_read(void *opaque, hwaddr off, unsigned size)
{
    Z6iiExmcuState *s = opaque;
    uint64_t v = 0;
    unsigned ch = (off >> 8) % SIO_CHANNELS;
    hwaddr r = off & 0xff;

    if (r == 0x04) {
        if (s->rxq[ch] == 0) {
            goto out;
        }
        s->rxq[ch]--;
        if (s_audio && ch == AUDIO_CODEC_CH) {
            v = audio_codec_rx(s_audio);
            goto out;
        }
    }
    switch (r) {
    case 0x14:
        v = (1u << 2) | (s->rxq[ch] ? 0 : 1u << 5);
        goto out;
    case 0x20:
        v = s->ien[ch];
        goto out;
    case 0x24:
        v = s->ist[ch];
        goto out;
    case 0x48:
        v = SIO_FIFO_DEPTH;
        goto out;
    default:
        break;
    }
    switch (off) {
    case 0x04:
        s->rxpos++;
        if (s->rxpos >= EXMCU_BLOCK) {
            bool request = s->txn >= 1 && (s->tx[0] & 0xff) != 0;
            if (request && (s->tx[0] & 0xffff) == 0x020d) {
                uint8_t b[256];
                if (exmcu_lens_reply(s->tx, b)) {
                    if (s->rq_tail - s->rq_head >= ARRAY_SIZE(s->rq)) {
                        s->rq_head++;
                    }
                    memcpy(s->rq[s->rq_tail % ARRAY_SIZE(s->rq)], b, 256);
                    s->rq_tail++;
                }
            } else if (request && s->rdir) {

                char *path = g_strdup_printf("%s/%04x.bin", s->rdir, s->tx[0] & 0xffff);
                gchar *data = NULL;
                gsize len = 0;
                if (g_file_get_contents(path, &data, &len, NULL) && len >= 256) {
                    uint16_t op = s->tx[0] & 0xffff;
                    unsigned nblk = len / 256, i, idx = 0;

                    for (i = 0; i < ARRAY_SIZE(s->rseq); i++) {
                        if (s->rseq[i].n && s->rseq[i].op == op) {
                            break;
                        }
                        if (!s->rseq[i].n) {
                            s->rseq[i].op = op;
                            break;
                        }
                    }
                    if (i < ARRAY_SIZE(s->rseq)) {
                        idx = s->rseq[i].n;
                        s->rseq[i].n++;
                    }
                    if (idx >= nblk) {
                        idx = nblk - 1;
                    }
                    if (s->rq_tail - s->rq_head >= ARRAY_SIZE(s->rq)) {
                        s->rq_head++;
                    }
                    memcpy(s->rq[s->rq_tail % ARRAY_SIZE(s->rq)], data + idx * 256, 256);
                    expeed6_keys_overlay_reply(s->rq[s->rq_tail % ARRAY_SIZE(s->rq)]);
                    s->rq_tail++;
                    g_free(data);
                } else {
                    g_free(data);
                }
                g_free(path);
            }
            s->exchange++;
            if (s->extint && request) {
                exmcu_reply_pin_update(s);
                extint_raise(s->extint, EXMCU_ACK_SRC);
                if (exmcu_reply_pending(s)) {
                    extint_raise(s->extint, EXMCU_RDY_SRC);
                }
            }
        }
        break;
    case 0x104:
        if (s->mode == 4 && s->rx1pos < EXMCU_BLOCK) {
            const uint8_t *blk = NULL;
            if (s->rx1pos == 0 && s->rq_head != s->rq_tail) {
                memcpy(s->rlast, s->rq[s->rq_head % ARRAY_SIZE(s->rq)], 256);
                s->rq_head++;
                s->rlast_valid = true;
            }
            if (s->rlast_valid) {
                blk = s->rlast;
            }
            if (blk) {
                const uint8_t *b = blk + s->rx1pos * 2;
                v = b[0] | (b[1] << 8);
            }
        }
        if (s->rx1pos < EXMCU_BLOCK) {
            s->rcur[s->rx1pos * 2] = v;
            s->rcur[s->rx1pos * 2 + 1] = v >> 8;
        }
        s->rx1pos++;
        if (s->rx1pos >= EXMCU_BLOCK) {
            unsigned sum = 0;
            for (unsigned i = 0; i < 256; i++) {
                sum += s->rcur[i];
            }
            if (s->rcur[0] == 2 && s->rcur[8] == 1 && (sum & 0xff) == 0xff) {
                memcpy(s->status_last, s->rcur + 8, EXMCU_STATUS_PLEN);
                s->status_valid = true;
            }
            s->exch1++;
            s->rx1pos = 0;
            s->tx1n = 0;
            exmcu_reply_pin_update(s);
        }
        break;
    default:
        if (sio_is_cfg_reg(off & 0xff)) {
            v = s->cfg[(off >> 8) % SIO_CHANNELS][(off & 0xff) / 4];
        }
        break;
    }
out:
    return v;
}

static void exmcu_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Z6iiExmcuState *s = opaque;
    unsigned ch = (off >> 8) % SIO_CHANNELS;
    hwaddr r = off & 0xff;

    if (sio_is_cfg_reg(r)) {
        uint32_t was = s->cfg[ch][r / 4];
        s->cfg[ch][r / 4] = val;

        if (r == 0x3c && (val & 1) && !(was & 1) && (s->cfg[ch][0x0c / 4] & 0x400)) {
            hdmac_sio_exchange(ch);
        }

        if (r == 0x3c && !(val & 1) && (was & 1)) {
            s->rxq[ch] = 0;
        }
    } else if (r == 0x20) {
        s->ien[ch] = val;
        sio_update_irq(s, ch);
    } else if (r == 0x28) {
        s->ist[ch] &= ~(uint32_t)val;
        sio_update_irq(s, ch);
    }
    if (s_sensor.on && ch == SENSOR_SIO_CH) {
        if (r == 0x10 || r == 0x20) {
            sensor_spi_start();
        } else if (r == 0x00) {
            sensor_spi_word((uint16_t)val);
        }
    }
    if (r == 0x00 && s_audio && ch == AUDIO_CODEC_CH) {
        audio_codec_tx(s_audio, (uint16_t)val, s->cfg[ch][0x10 / 4] & 8);
    }
    switch (off) {
    case 0x00: /* TX data */
        if (s->rxpos >= EXMCU_BLOCK) {
            s->txn = 0;
            s->rxpos = 0;
        }
        if (s->txn < EXMCU_TXBUF) {
            s->tx[s->txn++] = (uint16_t)val;
        }
        break;
    case 0x100:
        s->tx1n++;
        break;
    case 0x164:
        if (val & 1) {
            s->rx1pos = 0;
            s->tx1n = 0;
        }
        break;
    default:
        break;
    }
    if (r == 0x00) {
        sio_shift_word(s, ch);
    }
}

static const MemoryRegionOps exmcu_ops = {
    .read = exmcu_read,
    .write = exmcu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void sio_realize(DeviceState *dev, Error **errp)
{
    Z6iiExmcuState *exs = EXPEED6_SIO(dev);

    exs->mode = 0;
    exs->rdir = exs->reply_dir_prop;
    if (exs->rdir) {
        exs->mode = 4;
    }
    exmcu_push_init(exs);
    if (exs->push_spec) {
        exmcu_push_schedule(exs, exs->push_spec);
    }
    for (unsigned ch = 0; ch < SIO_CHANNELS; ch++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &exs->irq[ch][0]);
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &exs->irq[ch][1]);
    }
    memory_region_init_io(&exs->mmio, OBJECT(dev), &exmcu_ops, exs, "expeed6.exmcu", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &exs->mmio);
}

static void sio_reset_enter(Object *obj, ResetType type)
{
    Z6iiExmcuState *s = EXPEED6_SIO(obj);

    memset(s->cfg, 0, sizeof(s->cfg));
    memset(s->ien, 0, sizeof(s->ien));
    memset(s->ist, 0, sizeof(s->ist));
    memset(s->rxq, 0, sizeof(s->rxq));
    memset(s->lvl, 0, sizeof(s->lvl));
    s->txn = s->rxpos = 0;
    s->rq_head = s->rq_tail = 0;
    s->rlast_valid = false;
    s->status_valid = false;
    memset(s->rseq, 0, sizeof(s->rseq));
    s->exchange = 0;
    s->tx1n = s->rx1pos = s->exch1 = 0;
    exmcu_push_arm(s);
}

static void sio_reset_hold(Object *obj, ResetType type)
{
    Z6iiExmcuState *s = EXPEED6_SIO(obj);

    for (unsigned ch = 0; ch < SIO_CHANNELS; ch++) {
        qemu_set_irq(s->irq[ch][0], 0);
        qemu_set_irq(s->irq[ch][1], 0);
    }
    exmcu_reply_pin_update(s);
}

static const VMStateDescription vmstate_sio_seq = {
    .name = TYPE_EXPEED6_SIO "/seq",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_UINT16(op, struct Z6iiExmcuSeq),
                                      VMSTATE_UINT32(n, struct Z6iiExmcuSeq), VMSTATE_END_OF_LIST() }
};

static const VMStateDescription vmstate_sio_push = {
    .name = TYPE_EXPEED6_SIO "/push",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]){ VMSTATE_INT64(first_ns, ExmcuPush), VMSTATE_INT64(period_ns, ExmcuPush),
                                      VMSTATE_INT64(due_ns, ExmcuPush), VMSTATE_TIMER_PTR(t, ExmcuPush),
                                      VMSTATE_END_OF_LIST() }
};

static int sio_pre_save(void *opaque)
{
    Z6iiExmcuState *s = opaque;
    GString *p = g_string_new(NULL);
    for (unsigned i = 0; i < s->n_push; i++) {
        g_string_append_printf(p, "%s\n", s->push[i].path);
    }
    g_free(s->push_paths);
    s->push_paths_len = p->len;
    s->push_paths = g_string_free(p, false);
    return 0;
}

static int sio_post_load(void *opaque, int version_id)
{
    Z6iiExmcuState *s = opaque;
    g_autofree char *dup = g_strndup(s->n_push ? s->push_paths : "", s->n_push ? s->push_paths_len : 0);
    g_auto(GStrv) paths = g_strsplit(dup, "\n", -1);
    for (unsigned i = 0; i < EXMCU_PUSH_MAX; i++) {
        g_free(s->push[i].path);
        s->push[i].path = i < s->n_push ? g_strdup(paths[i]) : NULL;
    }
    for (unsigned i = 0; i < s->n_push; i++) {
        struct stat st;
        if (stat(s->push[i].path, &st) || st.st_size < 256) {
            error_report("expeed6-sio: the Ex MCU frame %s the snapshot's push schedule names is missing or short",
                         s->push[i].path);
            return -EINVAL;
        }
    }
    return 0;
}

static const VMStateDescription vmstate_sio = {
    .name = TYPE_EXPEED6_SIO,
    .version_id = 3,
    .minimum_version_id = 3,
    .pre_save = sio_pre_save,
    .post_load = sio_post_load,
    .fields =
        (const VMStateField[]){
            VMSTATE_UINT32_2DARRAY(cfg, Z6iiExmcuState, SIO_CHANNELS, 0x40),
            VMSTATE_UINT32_ARRAY(ien, Z6iiExmcuState, SIO_CHANNELS),
            VMSTATE_UINT32_ARRAY(ist, Z6iiExmcuState, SIO_CHANNELS),
            VMSTATE_UINT32_ARRAY(rxq, Z6iiExmcuState, SIO_CHANNELS),
            VMSTATE_INT32_ARRAY(lvl, Z6iiExmcuState, SIO_CHANNELS),
            VMSTATE_UINT16_ARRAY(tx, Z6iiExmcuState, EXMCU_TXBUF),
            VMSTATE_UINT32(txn, Z6iiExmcuState),
            VMSTATE_UINT32(rxpos, Z6iiExmcuState),
            VMSTATE_INT32(mode, Z6iiExmcuState),
            VMSTATE_UINT8_2DARRAY(rq, Z6iiExmcuState, 8, 256),
            VMSTATE_UINT32(rq_head, Z6iiExmcuState),
            VMSTATE_UINT32(rq_tail, Z6iiExmcuState),
            VMSTATE_UINT8_ARRAY(rlast, Z6iiExmcuState, 256),
            VMSTATE_BOOL(rlast_valid, Z6iiExmcuState),
            VMSTATE_UINT8_ARRAY(rcur, Z6iiExmcuState, 256),
            VMSTATE_UINT8_ARRAY(status_last, Z6iiExmcuState, EXMCU_STATUS_PLEN),
            VMSTATE_BOOL(status_valid, Z6iiExmcuState),
            VMSTATE_STRUCT_ARRAY(rseq, Z6iiExmcuState, 16, 1, vmstate_sio_seq, struct Z6iiExmcuSeq),
            VMSTATE_UINT32(exchange, Z6iiExmcuState),
            VMSTATE_UINT32(tx1n, Z6iiExmcuState),
            VMSTATE_UINT32(rx1pos, Z6iiExmcuState),
            VMSTATE_UINT32(exch1, Z6iiExmcuState),
            VMSTATE_UINT32(n_push, Z6iiExmcuState),
            VMSTATE_UINT32(push_paths_len, Z6iiExmcuState),
            VMSTATE_VBUFFER_ALLOC_UINT32(push_paths, Z6iiExmcuState, 0, NULL, push_paths_len),
            VMSTATE_STRUCT_ARRAY(push, Z6iiExmcuState, EXMCU_PUSH_MAX, 2, vmstate_sio_push, ExmcuPush),
            VMSTATE_END_OF_LIST() }
};

static Property sio_properties[] = {

    DEFINE_PROP_STRING("reply-dir", Z6iiExmcuState, reply_dir_prop),
    DEFINE_PROP_STRING("push", Z6iiExmcuState, push_spec),
    DEFINE_PROP_END_OF_LIST(),
};

static void sio_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = sio_realize;
    dc->vmsd = &vmstate_sio;
    device_class_set_props(dc, sio_properties);
    rc->phases.enter = sio_reset_enter;
    rc->phases.hold = sio_reset_hold;
}

static const TypeInfo sio_info = {
    .name = TYPE_EXPEED6_SIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Z6iiExmcuState),
    .class_init = sio_class_init,
};

static void sio_register_types(void)
{
    type_register_static(&sio_info);
}

type_init(sio_register_types)

void expeed6_install_sio(MemoryRegion *root, AddressSpace *as, DeviceState *gic, Expeed6ExtIntState *extint)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_SIO);
    Z6iiExmcuState *exs = EXPEED6_SIO(dev);
    const char *e;

    exs->extint = extint;
    exs->as = as;

    if ((e = getenv("Z6II_EXMCU_REPLY_DIR"))) {
        qdev_prop_set_string(dev, "reply-dir", e);
    }
    if ((e = getenv("Z6II_EXMCU_PUSH"))) {
        qdev_prop_set_string(dev, "push", e);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, EXMCU_UART);
    for (unsigned ch = 0; ch <= 10; ch++) { /* SIO_0..10: SPI 160 + 2n RX, 161 + 2n TX */
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 2 * ch, qdev_get_gpio_in(gic, 160 + 2 * ch));
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 2 * ch + 1, qdev_get_gpio_in(gic, 161 + 2 * ch));
    }
    s_exmcu = exs;
}
