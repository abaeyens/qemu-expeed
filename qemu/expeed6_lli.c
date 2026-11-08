/*
 * The Expeed 6 DualEngine/LLI link block at 0x20130000
 *
 * The master-slave handshake, the IPCU mailbox
 * and the bus windows the IDMAC's view of the peer goes through.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"

#define LLI_WIN_REGS 8

typedef struct {
    int reg;
    uint32_t bus, size;
    uint32_t dflt;
} LliWinSlot;

static const LliWinSlot lli_slots_master[] = {
    { 0, 0x00000000, 0x04000000, 0 },          { 1, 0x04000000, 0x04000000, 0 },
    { 2, 0x08000000, 0x08000000, 0 },          { 3, 0x10000000, 0x08000000, 0 },
    { 4, 0x20000000, 0x10000000, 0xc0000000 }, { 5, 0x30000000, 0x10000000, 0xd0000000 },
};
static const LliWinSlot lli_slots_slave[] = {
    { 0, 0x00000000, 0x02000000, 0 },          { 1, 0x02000000, 0x02000000, 0 },
    { 2, 0x08000000, 0x08000000, 0 },          { 3, 0x10000000, 0x08000000, 0 },
    { 4, 0x18000000, 0x08000000, 0 },          { 5, 0x20000000, 0x10000000, 0xc0000000 },
    { 6, 0x30000000, 0x10000000, 0xd0000000 },
};

static AddressSpace *lli_dma_view(MemoryRegion *own, MemoryRegion *peer, const char *name, const LliWinSlot *slots,
                                  int nslots, MemoryRegion **win_mr, uint32_t *win_dflt)
{
    MemoryRegion *view = g_new(MemoryRegion, 1);
    MemoryRegion *self = g_new(MemoryRegion, 1);
    memory_region_init(view, NULL, name, UINT64_MAX);
    memory_region_init_alias(self, NULL, "expeed6.lli.dma.own", own, 0, memory_region_size(own));
    memory_region_add_subregion_overlap(view, 0, self, 0);
    for (int i = 0; i < nslots; i++) {
        MemoryRegion *w = g_new(MemoryRegion, 1);
        memory_region_init_alias(w, NULL, "expeed6.lli.dma.window", peer, slots[i].dflt, slots[i].size);
        memory_region_add_subregion_overlap(view, slots[i].bus, w, 1);
        memory_region_set_enabled(w, slots[i].dflt != 0);
        win_mr[slots[i].reg] = w;
        win_dflt[slots[i].reg] = slots[i].dflt;
    }
    AddressSpace *as = g_new0(AddressSpace, 1);
    address_space_init(as, view, name);
    return as;
}

#define LLI_BASE      0x20130000
#define LLI_SIZE      0x10000
#define LLI_FOLD      0x7fff
#define LLI_SPI       95
#define LLI_UP        0x4908
#define LLI_STATE     0x490c
#define LLI_STATUS    0x5020
#define LLI_ACK       0x5028
#define LLI_WIN       0x50b0
#define LLI_SELF_BITS 0x22

#define LLI_MB_RX0    0x6000
#define LLI_MB_RX1    0x6004
#define LLI_MB_TX0    0x6008
#define LLI_MB_TX1    0x600c
#define LLI_MB_IRQ    0x8000
#define LLI_DOORBELL  0x413c
#define LLI_CH_STAT   0x104
#define LLI_CH_STRIDE 0x400
#define LLI_CH_READY  3
#define LLI_PIN_PORT  0x18
#define LLI_PIN_BIT   3

#define TYPE_EXPEED6_LLI "expeed6-lli"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6LliState, EXPEED6_LLI)

struct Expeed6LliState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t status;
    uint32_t up;
    uint32_t state;
    qemu_irq irq;
    int32_t level;
    Expeed6GpioState *gpio;
    struct Expeed6LliState *peer;
    bool data;
    uint32_t mb_pending;
    uint32_t pin_port;
    uint32_t pin_bit;

    QEMUTimer *req_retry;
    uint32_t win[LLI_WIN_REGS];
    uint32_t win_set;
    MemoryRegion *win_mr[LLI_WIN_REGS];
    uint32_t win_dflt[LLI_WIN_REGS];
};

#define LLI_HANDSHAKE_BITS 0x22

Expeed6LliState *s_master_lli, *s_slave_lli;
typedef struct Z6iiExmcuState Z6iiExmcuState;
Z6iiExmcuState *s_exmcu;
static void lli_update(Expeed6LliState *s)
{
    int l = s->status ? 1 : 0;
    if (l != s->level) {
        s->level = l;
        qemu_set_irq(s->irq, l);
    }
}

static void lli_notify(Expeed6LliState *t, uint32_t bits, bool doorbell);

static void lli_req_retry(void *opaque)
{
    Expeed6LliState *s = opaque;
    if (s->peer && s->state == 6 && s->peer->state != 4) {
        lli_notify(s->peer, 1, false);
        timer_mod(s->req_retry, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    }
}

static bool lli_is_chan_status(hwaddr folded)
{
    return folded >= LLI_CH_STAT && folded < LLI_CH_STAT + 4 * LLI_CH_STRIDE &&
           (folded - LLI_CH_STAT) % LLI_CH_STRIDE == 0;
}


static void lli_notify(Expeed6LliState *t, uint32_t bits, bool doorbell)
{
    t->status |= bits;
    t->up |= 1;
    if (doorbell && t->gpio) {
        gpio_set_input(t->gpio, t->pin_port, t->pin_bit, false);
    }
    lli_update(t);
}

static void lli_window_set(Expeed6LliState *s, int n, uint32_t val)
{
    MemoryRegion *w = s->win_mr[n];
    s->win[n] = val;
    s->win_set |= 1u << n;
    if (w) {
        memory_region_transaction_begin();
        memory_region_set_alias_offset(w, (uint64_t)val << 12);
        memory_region_set_enabled(w, true);
        memory_region_transaction_commit();
    }
}

static uint64_t lli_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6LliState *s = opaque;
    hwaddr f = off & LLI_FOLD;
    uint64_t v;
    if (lli_is_chan_status(f)) {

        v = LLI_CH_READY;
    } else {
        switch (f) {
        case LLI_MB_RX0:
        case LLI_MB_RX1:
            if (s->data) {
                v = f == LLI_MB_RX1 ? s->mb_pending : 0;
                break;
            }
            return allones_read((void *)(uintptr_t)LLI_BASE, off, size);
        case LLI_STATUS:
            v = s->status;
            break;
        case LLI_UP:
            v = s->up;
            break;
        case LLI_STATE:
            v = s->state;
            break;
        default:
            return allones_read((void *)(uintptr_t)LLI_BASE, off, size);
        }
    }
    return v;
}

static void lli_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6LliState *s = opaque;
    hwaddr f = off & LLI_FOLD;
    if (s->data && s->peer && (f == LLI_MB_TX0 || f == LLI_MB_TX1)) {

        if (f == LLI_MB_TX0 && (val & 1)) {
            s->peer->mb_pending = 1;
            s->peer->status |= LLI_MB_IRQ;
            lli_update(s->peer);
        } else if (f == LLI_MB_TX1 && (val & 1)) {
            s->mb_pending = 0;
        }
        return;
    }
    if (f >= LLI_WIN && f < LLI_WIN + 4 * LLI_WIN_REGS && !(f & 3)) {
        lli_window_set(s, (f - LLI_WIN) / 4, val);
    }
    switch (f) {
    case LLI_ACK:
        s->status &= ~(uint32_t)val;
        break;
    case LLI_UP:
        s->up = val;
        break;
    case LLI_STATE:
        s->state = val;
        break;
    default:
        allones_write((void *)(uintptr_t)LLI_BASE, off, val, size);
        break;
    }
    if (f != LLI_ACK) {
        if (s->peer) {
            uint32_t bits = LLI_HANDSHAKE_BITS;
            if (s->data && f == LLI_STATE && val == 6) {
                bits |= 1;
                timer_mod(s->req_retry, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
            }
            lli_notify(s->peer, bits, f == LLI_DOORBELL);
        }
        lli_notify(s, LLI_SELF_BITS, f == LLI_DOORBELL);
    }
    lli_update(s);
}

static const MemoryRegionOps lli_ops = {
    .read = lli_read,
    .write = lli_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void lli_realize(DeviceState *dev, Error **errp)
{
    Expeed6LliState *s = EXPEED6_LLI(dev);

    s->pin_port = LLI_PIN_PORT;
    s->pin_bit = LLI_PIN_BIT;
    s->req_retry = timer_new_ms(QEMU_CLOCK_VIRTUAL, lli_req_retry, s);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    memory_region_init_io(&s->mmio, OBJECT(dev), &lli_ops, s, "expeed6.lli", LLI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void lli_windows_apply(Expeed6LliState *s)
{
    memory_region_transaction_begin();
    for (int n = 0; n < LLI_WIN_REGS; n++) {
        MemoryRegion *w = s->win_mr[n];
        if (!w) {
            continue;
        }
        bool set = s->win_set & (1u << n);
        uint32_t peer = set ? s->win[n] << 12 : s->win_dflt[n];
        memory_region_set_alias_offset(w, peer);
        memory_region_set_enabled(w, set || peer != 0);
    }
    memory_region_transaction_commit();
}

static void lli_reset_enter(Object *obj, ResetType type)
{
    Expeed6LliState *s = EXPEED6_LLI(obj);

    s->status = s->up = s->state = 0;
    s->level = 0;
    s->mb_pending = 0;
    memset(s->win, 0, sizeof(s->win));
    s->win_set = 0;
    timer_del(s->req_retry);
    lli_windows_apply(s);
}

static void lli_reset_hold(Object *obj, ResetType type)
{
    qemu_set_irq(EXPEED6_LLI(obj)->irq, 0);
}

static int lli_post_load(void *opaque, int version_id)
{
    lli_windows_apply(opaque);
    return 0;
}

static const VMStateDescription vmstate_lli = {
    .name = TYPE_EXPEED6_LLI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = lli_post_load,
    .fields =
        (const VMStateField[]){ VMSTATE_UINT32(status, Expeed6LliState), VMSTATE_UINT32(up, Expeed6LliState),
                                VMSTATE_UINT32(state, Expeed6LliState), VMSTATE_INT32(level, Expeed6LliState),
                                VMSTATE_UINT32(mb_pending, Expeed6LliState),
                                VMSTATE_TIMER_PTR(req_retry, Expeed6LliState),
                                VMSTATE_UINT32_ARRAY(win, Expeed6LliState, LLI_WIN_REGS),
                                VMSTATE_UINT32(win_set, Expeed6LliState), VMSTATE_END_OF_LIST() }
};


static void lli_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = lli_realize;
    dc->vmsd = &vmstate_lli;
    rc->phases.enter = lli_reset_enter;
    rc->phases.hold = lli_reset_hold;
}

static const TypeInfo lli_info = {
    .name = TYPE_EXPEED6_LLI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6LliState),
    .class_init = lli_class_init,
};

static void lli_register_types(void)
{
    type_register_static(&lli_info);
}

type_init(lli_register_types)

    Expeed6LliState *expeed6_install_lli(MemoryRegion *root, DeviceState *gic, bool master)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_LLI);
    Expeed6LliState *lli = EXPEED6_LLI(dev);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    if (master) {
        s_master_lli = lli;
    } else {
        s_slave_lli = lli;
    }
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, LLI_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, LLI_SPI));
    return lli;
}

void expeed6_lli_connect_gpio(Expeed6LliState *lli, Expeed6GpioState *gpio)
{
    lli->gpio = gpio;
}

void expeed6_lli_link_clusters(MemoryRegion *sysmem)
{
    if (!s_slave_lli) {
        return;
    }
    s_master_lli->peer = s_slave_lli;
    s_slave_lli->peer = s_master_lli;

    s_master_lli->data = s_slave_lli->data = true;
    s_slave_lli->pin_port = 0x20;
    s_slave_lli->pin_bit = 10;
    if (s_slave_root) {
        MemoryRegion *win = g_new(MemoryRegion, 1);
        memory_region_init_alias(win, NULL, "expeed6.lli.window", s_slave_root, 0xc0000000ULL, 0x10000000ULL);
        memory_region_add_subregion_overlap(sysmem, 0x40000000ULL, win, 2);
        MemoryRegion *winsla = g_new(MemoryRegion, 1);
        memory_region_init_alias(winsla, NULL, "expeed6.lli.window.sla", sysmem, 0xc0000000ULL, 0x10000000ULL);
        memory_region_add_subregion_overlap(s_slave_root, 0x40000000ULL, winsla, 2);
        if (s_master_idmac && s_slave_idmac) {
            expeed6_idmac_set_as(s_master_idmac, lli_dma_view(sysmem, s_slave_root, "expeed6.lli.dma.mas",
                                                              lli_slots_master, ARRAY_SIZE(lli_slots_master),
                                                              s_master_lli->win_mr, s_master_lli->win_dflt));
            expeed6_idmac_set_as(s_slave_idmac, lli_dma_view(s_slave_root, sysmem, "expeed6.lli.dma.sla",
                                                             lli_slots_slave, ARRAY_SIZE(lli_slots_slave),
                                                             s_slave_lli->win_mr, s_slave_lli->win_dflt));
        }
    }
}
