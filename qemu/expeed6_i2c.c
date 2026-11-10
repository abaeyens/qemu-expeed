/*
 * The Expeed 6 I2C controllers I2C_0..3 at 0x2c018000 (Socionext's F_I2C)
 *
 * Includes the orientation sensor on channel 2
 *
 * References:
 * - GoPro GP2 Linux 4.19.124: drivers/i2c/busses/i2c-milbeaut.c
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/mma845x.h"
#include "migration/vmstate.h"

#define I2C_BASE       0x2c018000ULL
#define I2C_CHANNELS   4
#define I2C_SPI0       182
#define I2C_FIFO       16
#define I2C_BYTE_NS    22500
#define I2C_CR_RW      (1u << 0)
#define I2C_CR_NEA     (1u << 2)
#define I2C_CR_ACKEN   (1u << 3)
#define I2C_CR_HOLD    (1u << 4)
#define I2C_CR_CLRFIFO (1u << 6)
#define I2C_SR_RXNE    (1u << 5)
#define I2C_SR_BUSY    (1u << 8)
#define I2C_ISR_COMP   (1u << 0)
#define I2C_ISR_DATA   (1u << 1)
#define I2C_ISR_NACK   (1u << 2)
#define I2C_ISR_RXOVF  (1u << 5)
#define I2C_ISR_TXOVF  (1u << 6)
#define I2C_ISR_RXUNF  (1u << 7)
#define I2C_ISR_ALL    0x2ffu

typedef enum { I2C_IDLE, I2C_ADDR, I2C_XFER, I2C_STALL } Expeed6I2cPhase;

typedef struct Expeed6I2cChan {
    unsigned n;
    I2CBus *bus;
    MemoryRegion mmio;
    qemu_irq irq;
    QEMUTimer *t;
    uint32_t cr, ar, isr, imr, tsr, smpr, tor;
    uint8_t tx[I2C_FIFO], rx[I2C_FIFO];
    uint32_t tx_n, rx_n, rx_left;
    int32_t phase;
    bool rw, on_bus;
    uint8_t addr;
} Expeed6I2cChan;

#define TYPE_EXPEED6_I2C "expeed6-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6I2cState, EXPEED6_I2C)

struct Expeed6I2cState {
    SysBusDevice parent_obj;
    Expeed6I2cChan ch[I2C_CHANNELS];
};

static void i2c_update_irq(Expeed6I2cChan *c)
{
    qemu_set_irq(c->irq, (c->isr & ~c->imr & I2C_ISR_ALL) != 0);
}

static void i2c_schedule(Expeed6I2cChan *c)
{
    timer_mod(c->t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + I2C_BYTE_NS);
}

static void i2c_stop(Expeed6I2cChan *c, uint32_t isr)
{
    if (c->on_bus) {
        if (c->rw) {
            i2c_nack(c->bus);
        }
        i2c_end_transfer(c->bus);
    }
    c->on_bus = false;
    c->phase = I2C_IDLE;
    c->isr |= isr;
    i2c_update_irq(c);
}

static void i2c_chunk_done(Expeed6I2cChan *c)
{
    if (c->rw ? !(c->cr & I2C_CR_ACKEN) : !(c->cr & I2C_CR_HOLD)) {
        i2c_stop(c, I2C_ISR_COMP);
        return;
    }
    c->phase = I2C_STALL;
    c->isr |= c->rw ? I2C_ISR_DATA : I2C_ISR_COMP | I2C_ISR_DATA;
    i2c_update_irq(c);
}

static void i2c_step(void *opaque)
{
    Expeed6I2cChan *c = opaque;

    if (c->phase == I2C_ADDR) {
        if (i2c_start_transfer(c->bus, c->addr, c->rw)) {
            i2c_end_transfer(c->bus);
            c->on_bus = false;
            i2c_stop(c, I2C_ISR_NACK);
            return;
        }
        c->on_bus = true;
        c->phase = I2C_XFER;
        if ((c->rw && !c->rx_left) || (!c->rw && !c->tx_n)) {
            i2c_chunk_done(c);
        } else {
            i2c_schedule(c);
        }
        return;
    }
    if (c->phase != I2C_XFER) {
        return;
    }
    if (c->rw) {
        uint8_t b = i2c_recv(c->bus);

        if (c->rx_n < I2C_FIFO) {
            c->rx[c->rx_n++] = b;
        } else {
            c->isr |= I2C_ISR_RXOVF;
        }
        c->rx_left--;
    } else {
        uint8_t b = c->tx[0];

        memmove(c->tx, c->tx + 1, --c->tx_n);
        if (i2c_send(c->bus, b)) {
            i2c_stop(c, I2C_ISR_NACK);
            return;
        }
    }
    if ((c->rw && !c->rx_left) || (!c->rw && !c->tx_n)) {
        i2c_chunk_done(c);
    } else {
        i2c_schedule(c);
    }
}

static uint64_t i2c_read_reg(Expeed6I2cChan *c, hwaddr off);

static uint64_t i2c_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6I2cChan *c = opaque;
    uint64_t v = i2c_read_reg(c, off);

    return v;
}

static uint64_t i2c_read_reg(Expeed6I2cChan *c, hwaddr off)
{
    switch (off) {
    case 0x00:
        return c->cr;
    case 0x04:
        return (c->phase != I2C_IDLE ? I2C_SR_BUSY : 0) | (c->rx_n ? I2C_SR_RXNE : 0);
    case 0x08:
        return c->ar;
    case 0x0c:
        if (!c->rx_n) {
            c->isr |= I2C_ISR_RXUNF;
            i2c_update_irq(c);
            return 0;
        } else {
            uint8_t b = c->rx[0];

            memmove(c->rx, c->rx + 1, --c->rx_n);
            return b;
        }
    case 0x10:
        return c->isr;
    case 0x14:
        return c->rw && c->phase != I2C_IDLE ? c->rx_left : c->tx_n;
    case 0x18:
        return c->smpr;
    case 0x1c:
        return c->tor;
    case 0x20:
        return c->imr;
    default:
        return 0;
    }
}

static void i2c_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6I2cChan *c = opaque;

    switch (off) {
    case 0x00:
        if (val & I2C_CR_CLRFIFO) {
            c->tx_n = c->rx_n = 0;

            if (c->phase == I2C_ADDR || c->phase == I2C_XFER ||
                (c->phase == I2C_STALL && (c->rw || !(val & I2C_CR_HOLD)))) {
                timer_del(c->t);
                i2c_stop(c, 0);
            }
        }
        c->cr = val & ~I2C_CR_CLRFIFO;
        if (c->phase == I2C_STALL && c->rw && !(c->cr & I2C_CR_ACKEN)) {
            i2c_stop(c, I2C_ISR_COMP);
        } else if (c->phase == I2C_STALL && !c->rw && !(c->cr & I2C_CR_HOLD) && !c->tx_n) {
            i2c_stop(c, 0);
        }
        break;
    case 0x08:
        c->ar = val;
        if (c->phase == I2C_ADDR || c->phase == I2C_XFER) {
            qemu_log_mask(LOG_GUEST_ERROR, "expeed6.i2c%u: START while a transfer runs\n", c->n);
            break;
        }
        if (!(c->cr & I2C_CR_NEA)) {
            qemu_log_mask(LOG_UNIMP, "expeed6.i2c%u: 10-bit addressing\n", c->n);
            c->isr |= I2C_ISR_NACK;
            i2c_update_irq(c);
            break;
        }
        if (c->on_bus && ((val & 0x7f) != c->addr)) {
            i2c_end_transfer(c->bus);
            c->on_bus = false;
        }
        c->addr = val & 0x7f;
        c->rw = c->cr & I2C_CR_RW;
        c->rx_left = c->rw ? MIN(c->tsr & 0x1f, I2C_FIFO) : 0;
        c->phase = I2C_ADDR;
        i2c_schedule(c);
        break;
    case 0x0c:
        if (c->tx_n < I2C_FIFO) {
            c->tx[c->tx_n++] = val;
        } else {
            c->isr |= I2C_ISR_TXOVF;
            i2c_update_irq(c);
        }
        if (c->phase == I2C_STALL && !c->rw) {
            c->phase = I2C_XFER;
            i2c_schedule(c);
        }
        break;
    case 0x10:
        c->isr &= ~val;
        i2c_update_irq(c);
        break;
    case 0x14:
        c->tsr = val;
        if (c->phase == I2C_STALL && c->rw) {
            c->rx_left = MIN(val & 0x1f, I2C_FIFO);
            c->phase = I2C_XFER;
            i2c_schedule(c);
        }
        break;
    case 0x18:
        c->smpr = val;
        break;
    case 0x1c:
        c->tor = val;
        break;
    case 0x24:
        c->imr &= ~val;
        i2c_update_irq(c);
        break;
    case 0x28:
        c->imr |= val & I2C_ISR_ALL;
        i2c_update_irq(c);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "expeed6.i2c%u: write +0x%02x = 0x%" PRIx64 "\n", c->n, (unsigned)off, val);
        break;
    }
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_read,
    .write = i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void expeed6_i2c_realize(DeviceState *dev, Error **errp)
{
    Expeed6I2cState *s = EXPEED6_I2C(dev);

    for (unsigned n = 0; n < I2C_CHANNELS; n++) {
        Expeed6I2cChan *c = &s->ch[n];
        g_autofree char *name = g_strdup_printf("expeed6.i2c%u", n);

        c->n = n;
        c->bus = i2c_init_bus(dev, name);
        c->t = timer_new_ns(QEMU_CLOCK_VIRTUAL, i2c_step, c);
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &c->irq);
        memory_region_init_io(&c->mmio, OBJECT(dev), &i2c_ops, c, name, 0x1000);
        sysbus_init_mmio(SYS_BUS_DEVICE(dev), &c->mmio);
    }
}

static void expeed6_i2c_reset_enter(Object *obj, ResetType type)
{
    Expeed6I2cState *s = EXPEED6_I2C(obj);

    for (unsigned n = 0; n < I2C_CHANNELS; n++) {
        Expeed6I2cChan *c = &s->ch[n];

        timer_del(c->t);
        c->cr = c->ar = c->isr = c->tsr = c->smpr = c->tor = 0;
        c->imr = I2C_ISR_ALL;
        c->tx_n = c->rx_n = c->rx_left = 0;
        c->phase = I2C_IDLE;
        c->rw = c->on_bus = false;
        c->addr = 0;
    }
}

static void expeed6_i2c_reset_hold(Object *obj, ResetType type)
{
    Expeed6I2cState *s = EXPEED6_I2C(obj);

    for (unsigned n = 0; n < I2C_CHANNELS; n++) {
        i2c_update_irq(&s->ch[n]);
    }
}

static const VMStateDescription vmstate_expeed6_i2c_chan = {
    .name = TYPE_EXPEED6_I2C "/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_UINT32(cr, Expeed6I2cChan), VMSTATE_UINT32(ar, Expeed6I2cChan),
                                      VMSTATE_UINT32(isr, Expeed6I2cChan), VMSTATE_UINT32(imr, Expeed6I2cChan),
                                      VMSTATE_UINT32(tsr, Expeed6I2cChan), VMSTATE_UINT32(smpr, Expeed6I2cChan),
                                      VMSTATE_UINT32(tor, Expeed6I2cChan),
                                      VMSTATE_UINT8_ARRAY(tx, Expeed6I2cChan, I2C_FIFO),
                                      VMSTATE_UINT8_ARRAY(rx, Expeed6I2cChan, I2C_FIFO),
                                      VMSTATE_UINT32(tx_n, Expeed6I2cChan), VMSTATE_UINT32(rx_n, Expeed6I2cChan),
                                      VMSTATE_UINT32(rx_left, Expeed6I2cChan),
                                      VMSTATE_INT32(phase, Expeed6I2cChan), VMSTATE_BOOL(rw, Expeed6I2cChan),
                                      VMSTATE_BOOL(on_bus, Expeed6I2cChan), VMSTATE_UINT8(addr, Expeed6I2cChan),
                                      VMSTATE_TIMER_PTR(t, Expeed6I2cChan), VMSTATE_END_OF_LIST() }
};

static const VMStateDescription vmstate_expeed6_i2c = {
    .name = TYPE_EXPEED6_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_STRUCT_ARRAY(ch, Expeed6I2cState, I2C_CHANNELS, 1,
                                                           vmstate_expeed6_i2c_chan, Expeed6I2cChan),
                                      VMSTATE_END_OF_LIST() }
};

static void expeed6_i2c_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = expeed6_i2c_realize;
    dc->vmsd = &vmstate_expeed6_i2c;
    rc->phases.enter = expeed6_i2c_reset_enter;
    rc->phases.hold = expeed6_i2c_reset_hold;
}

static const TypeInfo expeed6_i2c_info = {
    .name = TYPE_EXPEED6_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6I2cState),
    .class_init = expeed6_i2c_class_init,
};

static void expeed6_i2c_register_types(void)
{
    type_register_static(&expeed6_i2c_info);
}

type_init(expeed6_i2c_register_types)

void expeed6_install_i2c(MemoryRegion *root, DeviceState *gic)
{
    const unsigned acc_ch = 2, acc_addr = 0x1d;

    DeviceState *dev = qdev_new(TYPE_EXPEED6_I2C);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    for (unsigned n = 0; n < I2C_CHANNELS; n++) {
        expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), n, I2C_BASE + n * 0x1000);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), n, qdev_get_gpio_in(gic, I2C_SPI0 + n));
        if (n == acc_ch) {
            i2c_slave_create_simple(EXPEED6_I2C(dev)->ch[n].bus, TYPE_MMA8452Q, acc_addr);
        }
    }
}
