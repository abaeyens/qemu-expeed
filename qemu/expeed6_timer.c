/*
 * The Expeed 6 SoC TIMER block at 0x2c010000, TIMER_0..11, GIC 172..183
 *
 * | TIMER | SPI | GIC |
 * | ----- | --- | --- |
 * |     0 | 140 | 172 |
 * |     1 | 141 | 173 |
 * |     2 | 142 | 174 |
 * |     3 | 143 | 175 |
 * |     4 | 144 | 176 |
 * |     5 | 145 | 177 |
 * |     6 | 146 | 178 |
 * |     7 | 147 | 179 | 
 * |     8 | 148 | 180 |
 * |     9 | 149 | 181 |
 * |    10 | 150 | 182 |
 * |    11 | 151 | 183 |
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"

#define TIMER_BASE     0x2c010000
#define TIMER_CHANNELS 12
#define TIMER_SPI0     140
#define TIMER_MHZ      50
#define TIMER_CEIL_MS  1000

#define TYPE_EXPEED6_SOCTIMER "expeed6-soctimer"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6TimerState, EXPEED6_SOCTIMER)

typedef struct {
    Expeed6TimerState *s;
    int n;
} Expeed6TimerChan;

struct Expeed6TimerState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    Expeed6TimerChan chan[TIMER_CHANNELS];
    uint32_t reg[0x2000 / 4];
    uint32_t limit[TIMER_CHANNELS];
    bool has_limit[TIMER_CHANNELS];
    QEMUTimer *t[TIMER_CHANNELS];
    qemu_irq irq[TIMER_CHANNELS];
    uint32_t pending_limit;
    uint64_t fired[TIMER_CHANNELS];
    bool clamped[TIMER_CHANNELS];
};

#define TIMER_CTRL(n) (((n) + 1) * 0x100 / 4)
#define TIMER_CNT(n)  ((0x1000 + ((n) + 1) * 0x100) / 4)

static void soctimer_update_irq(Expeed6TimerState *s, int n)
{
    qemu_set_irq(s->irq[n], s->reg[TIMER_CTRL(n) + 0x10 / 4] != 0);
}

static void soctimer_start(Expeed6TimerState *s, int n)
{
    uint32_t period = s->reg[TIMER_CNT(n) + 0x08 / 4];
    uint64_t ticks = s->has_limit[n] ? (uint64_t)(uint32_t)(period - s->limit[n]) : (uint64_t)period + 1;
    uint32_t prescale = (s->reg[TIMER_CNT(n) + 0x04 / 4] & 0xff) + 1;
    int64_t ns = (int64_t)(ticks * prescale * 1000 / TIMER_MHZ);
    int64_t lim = MAX(ns, 100000);
    lim = MIN(lim, (int64_t)TIMER_CEIL_MS * 1000000);
    if (lim != ns && !s->clamped[n]) {
        s->clamped[n] = true;
        fprintf(stderr,
                "expeed6.soctimer: channel %d period %" PRIu64 " ticks (%" PRId64 " us, mode byte %#x) "
                "clamped to %" PRId64 " us\n",
                n, ticks, ns / 1000, s->reg[TIMER_CNT(n) + 0x04 / 4] & 0xff, lim / 1000);
    }
    timer_mod(s->t[n], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + lim);
}

static void soctimer_expire(void *opaque)
{
    Expeed6TimerChan *c = opaque;
    Expeed6TimerState *s = c->s;
    if (!(s->reg[0x1000 / 4] & (1u << c->n))) {
        return;
    }
    s->reg[TIMER_CTRL(c->n) + 0x10 / 4] |= 1;
    s->fired[c->n]++;
    soctimer_update_irq(s, c->n);
    soctimer_start(s, c->n);
}

static uint64_t soctimer_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6TimerState *s = opaque;
    uint64_t v = s->reg[(off & 0x1fff) / 4];
    return v;
}

static void soctimer_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6TimerState *s = opaque;
    uint32_t idx = (off & 0x1fff) / 4, old = s->reg[idx];

    for (int n = 0; n < TIMER_CHANNELS; n++) {
        if (idx == TIMER_CTRL(n) + 0x10 / 4) {
            s->reg[idx] &= ~(uint32_t)val;
            soctimer_update_irq(s, n);
            return;
        }
    }
    s->reg[idx] = (uint32_t)val;
    if (idx == 0x1004 / 4) {
        s->pending_limit = (uint32_t)val;
    } else if (idx == 0x1008 / 4) {
        for (int n = 0; n < TIMER_CHANNELS; n++) {
            if (val & (1u << n)) {
                s->limit[n] = s->pending_limit;
                s->has_limit[n] = true;
            }
        }
    } else if (idx == 0x1000 / 4) {
        for (int n = 0; n < TIMER_CHANNELS; n++) {
            uint32_t b = 1u << n;
            if ((val & b) && !(old & b)) {
                soctimer_start(s, n);
            } else if (!(val & b) && (old & b)) {
                timer_del(s->t[n]);
            }
        }
    }
}

static const MemoryRegionOps soctimer_ops = {
    .read = soctimer_read,
    .write = soctimer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void soctimer_realize(DeviceState *dev, Error **errp)
{
    Expeed6TimerState *tm = EXPEED6_SOCTIMER(dev);

    for (int n = 0; n < TIMER_CHANNELS; n++) {
        tm->chan[n] = (Expeed6TimerChan){ .s = tm, .n = n };
        tm->t[n] = timer_new_ns(QEMU_CLOCK_VIRTUAL, soctimer_expire, &tm->chan[n]);
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &tm->irq[n]);
    }
    memory_region_init_io(&tm->mmio, OBJECT(dev), &soctimer_ops, tm, "expeed6.soctimer", 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &tm->mmio);
}

static void soctimer_reset_enter(Object *obj, ResetType type)
{
    Expeed6TimerState *tm = EXPEED6_SOCTIMER(obj);

    memset(tm->reg, 0, sizeof(tm->reg));
    memset(tm->limit, 0, sizeof(tm->limit));
    memset(tm->has_limit, 0, sizeof(tm->has_limit));
    memset(tm->fired, 0, sizeof(tm->fired));
    memset(tm->clamped, 0, sizeof(tm->clamped));
    tm->pending_limit = 0;
    for (int n = 0; n < TIMER_CHANNELS; n++) {
        timer_del(tm->t[n]);
    }
}

static void soctimer_reset_hold(Object *obj, ResetType type)
{
    Expeed6TimerState *tm = EXPEED6_SOCTIMER(obj);

    for (int n = 0; n < TIMER_CHANNELS; n++) {
        soctimer_update_irq(tm, n);
    }
}

static const VMStateDescription vmstate_soctimer = {
    .name = TYPE_EXPEED6_SOCTIMER,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]){ VMSTATE_UINT32_ARRAY(reg, Expeed6TimerState, 0x2000 / 4),
                                      VMSTATE_UINT32_ARRAY(limit, Expeed6TimerState, TIMER_CHANNELS),
                                      VMSTATE_BOOL_ARRAY(has_limit, Expeed6TimerState, TIMER_CHANNELS),
                                      VMSTATE_TIMER_PTR_ARRAY(t, Expeed6TimerState, TIMER_CHANNELS),
                                      VMSTATE_UINT32(pending_limit, Expeed6TimerState),
                                      VMSTATE_UINT64_ARRAY(fired, Expeed6TimerState, TIMER_CHANNELS),
                                      VMSTATE_BOOL_ARRAY(clamped, Expeed6TimerState, TIMER_CHANNELS),
                                      VMSTATE_END_OF_LIST() }
};

static void soctimer_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = soctimer_realize;
    dc->vmsd = &vmstate_soctimer;
    rc->phases.enter = soctimer_reset_enter;
    rc->phases.hold = soctimer_reset_hold;
}

static const TypeInfo soctimer_info = {
    .name = TYPE_EXPEED6_SOCTIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6TimerState),
    .class_init = soctimer_class_init,
};

static void soctimer_register_types(void)
{
    type_register_static(&soctimer_info);
}

type_init(soctimer_register_types)

void expeed6_install_soctimer(MemoryRegion *root, DeviceState *gic)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_SOCTIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, TIMER_BASE);
    for (int n = 0; n < TIMER_CHANNELS; n++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), n, qdev_get_gpio_in(gic, TIMER_SPI0 + n));
    }
}
