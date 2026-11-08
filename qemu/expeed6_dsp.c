/*
 * The Expeed 6 IPCU peer that stands in for the in-SoC core B (IPCU unit 1)
 *
 * Doesn't actually emulate core B's job, just does some signalling
 * so the rest of the Expeed thinks core B is running.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"

#define IPCU_SIZE 0x1000

#define COREB_IPCU_BASE 0x24061000
#define COREB_IPCU_SPI0 195
#define IPCU_MB(n)      (0x100 + 0x80 * (n))
#define IPCU_REPLY_MB   7
#define IPCU_SOURCE     0x00
#define IPCU_SEND       0x08
#define IPCU_DEST_SET   0x10
#define IPCU_DEST_CLR   0x14
#define IPCU_DEST_STAT  0x18
#define IPCU_ACK_SET    0x30
#define IPCU_ACK_CLR    0x34
#define IPCU_ACK_STAT   0x38
#define IPCU_DATA       0x40
#define IPCU_MBADR      0x80
#define IPCU_MBSTAT     0x900

#define TYPE_EXPEED6_DSP_PEER "expeed6-dsp-peer"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6IpcuState, EXPEED6_DSP_PEER)

struct Expeed6IpcuState {
    SysBusDevice parent_obj;
    MemoryRegion ipcu_mmio;
    uint32_t ipcu[IPCU_SIZE / 4];
    qemu_irq irq[2];
    QEMUTimer *job;
    int32_t job_mb;
};

static uint32_t *ipcu_mb(Expeed6IpcuState *s, int n, hwaddr reg)
{
    return &s->ipcu[(IPCU_MB(n) + reg) / 4];
}

static void ipcu_mbstat(Expeed6IpcuState *s)
{
    uint32_t st = 0;
    for (int n = 0; n < 8; n++) {
        if (*ipcu_mb(s, n, IPCU_SOURCE)) {
            st |= 1u << n;
        }
    }
    s->ipcu[IPCU_MBSTAT / 4] = st;
}

static void ipcu_answer(void *opaque)
{
    Expeed6IpcuState *s = opaque;
    int n = s->job_mb;
    uint32_t user = *ipcu_mb(s, n, IPCU_DATA);

    *ipcu_mb(s, n, IPCU_ACK_STAT) |= 1u << 1;
    s->ipcu[(IPCU_MBADR + 0) / 4] = IPCU_MB(n);
    qemu_set_irq(s->irq[0], 1);

    int r = IPCU_REPLY_MB;
    *ipcu_mb(s, r, IPCU_SOURCE) = 1u << 1;
    for (int i = 0; i < 9; i++) {
        *ipcu_mb(s, r, IPCU_DATA + 4 * i) = i == 0 ? user : 0;
    }
    *ipcu_mb(s, r, IPCU_DEST_STAT) |= 1u << 1;
    s->ipcu[(IPCU_MBADR + 4) / 4] = IPCU_MB(r);
    ipcu_mbstat(s);
    qemu_set_irq(s->irq[1], 1);
}

static uint64_t ipcu_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6IpcuState *s = opaque;
    return off / 4 < IPCU_SIZE / 4 ? s->ipcu[off / 4] : 0;
}

static void ipcu_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6IpcuState *s = opaque;
    if (off < IPCU_MB(0) || off >= IPCU_MB(8)) {
        if (off / 4 < IPCU_SIZE / 4) {
            s->ipcu[off / 4] = val;
        }
        return;
    }
    int n = (off - IPCU_MB(0)) / 0x80;
    hwaddr reg = (off - IPCU_MB(0)) % 0x80;
    uint32_t *r = ipcu_mb(s, n, reg);
    switch (reg) {
    case IPCU_DEST_SET:
        *ipcu_mb(s, n, IPCU_DEST_STAT) |= val;
        break;
    case IPCU_DEST_CLR:
        *ipcu_mb(s, n, IPCU_DEST_STAT) &= ~(uint32_t)val;
        if (n == IPCU_REPLY_MB && !*ipcu_mb(s, n, IPCU_DEST_STAT)) {
            *ipcu_mb(s, n, IPCU_SOURCE) = 0;
            ipcu_mbstat(s);
            qemu_set_irq(s->irq[1], 0);
        }
        break;
    case IPCU_ACK_SET:
        *ipcu_mb(s, n, IPCU_ACK_STAT) |= val;
        break;
    case IPCU_ACK_CLR:
        *ipcu_mb(s, n, IPCU_ACK_STAT) &= ~(uint32_t)val;
        if (!*ipcu_mb(s, n, IPCU_ACK_STAT)) {
            qemu_set_irq(s->irq[0], 0);
        }
        break;
    case IPCU_SEND:
        *r = val;
        if (val & 1) {
            s->job_mb = n;
            timer_mod(s->job, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 50 * SCALE_US);
        }
        break;
    default:
        *r = val;
        if (reg == IPCU_SOURCE) {
            ipcu_mbstat(s);
        }
        break;
    }
}

static const MemoryRegionOps ipcu_ops = {
    .read = ipcu_read,
    .write = ipcu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void dsp_peer_realize(DeviceState *dev, Error **errp)
{
    Expeed6IpcuState *s = EXPEED6_DSP_PEER(dev);

    s->job = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipcu_answer, s);
    for (int i = 0; i < 2; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
    memory_region_init_io(&s->ipcu_mmio, OBJECT(dev), &ipcu_ops, s, "expeed6.coreb-ipcu", IPCU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->ipcu_mmio);
}

static void dsp_peer_reset_enter(Object *obj, ResetType type)
{
    Expeed6IpcuState *s = EXPEED6_DSP_PEER(obj);

    memset(s->ipcu, 0, sizeof(s->ipcu));
    s->job_mb = 0;
    timer_del(s->job);
}

static void dsp_peer_reset_hold(Object *obj, ResetType type)
{
    Expeed6IpcuState *s = EXPEED6_DSP_PEER(obj);

    for (int i = 0; i < 2; i++) {
        qemu_set_irq(s->irq[i], 0);
    }
}

static const VMStateDescription vmstate_dsp_peer = {
    .name = TYPE_EXPEED6_DSP_PEER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_UINT32_ARRAY(ipcu, Expeed6IpcuState, IPCU_SIZE / 4),
                                      VMSTATE_TIMER_PTR(job, Expeed6IpcuState),
                                      VMSTATE_INT32(job_mb, Expeed6IpcuState), VMSTATE_END_OF_LIST() }
};

static void dsp_peer_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = dsp_peer_realize;
    dc->vmsd = &vmstate_dsp_peer;
    rc->phases.enter = dsp_peer_reset_enter;
    rc->phases.hold = dsp_peer_reset_hold;
}

static const TypeInfo dsp_peer_info = {
    .name = TYPE_EXPEED6_DSP_PEER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6IpcuState),
    .class_init = dsp_peer_class_init,
};

static void dsp_peer_register_types(void)
{
    type_register_static(&dsp_peer_info);
}

type_init(dsp_peer_register_types)

    void expeed6_install_dsp_peers(MemoryRegion *root, DeviceState *gic)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_DSP_PEER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, COREB_IPCU_BASE);
    for (int i = 0; i < 2; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, qdev_get_gpio_in(gic, COREB_IPCU_SPI0 + i));
    }
}
