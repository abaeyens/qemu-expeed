/*
 * The Expeed 6 GPIO block at 0x04282000
 *
 * data and direction registers, and the board's input levels, echoes and links
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

#define GPIO_BASE     0x04282000
#define GPIO_DIR_OFF  0x100
#define GPIO_IN_LEVEL 0xffff
static const struct {
    unsigned off;
    uint16_t level;
} gpio_in_ports[] = {
    { 0x28, 0xefff },
    { 0x14, 0x47ff },
};

#define GPIO_ECHO_OFF 0x18
#define GPIO_ECHO_BIT 8
#define GPIO_ECHO_SRC 0
#define GPIO_ECHO_MS  20

typedef struct {
    unsigned out_off, out_bit, in_off, in_bit;
    bool invert;
} Expeed6GpioLink;
static const Expeed6GpioLink gpio_link_slave_card = { 0x1c, 12, 0x28, 2, false };
static const Expeed6GpioLink gpio_link_xqd = { 0x1c, 14, 0x28, 2, true };
#define GPIO_LINK_MAX 1

#define TYPE_EXPEED6_GPIO "expeed6-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6GpioState, EXPEED6_GPIO)

struct Expeed6GpioState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t reg[0x400];
    uint16_t in_level;
    uint16_t in_port[GPIO_DIR_OFF / 4];
    bool in_port_set[GPIO_DIR_OFF / 4];
    QEMUTimer *echo_timer;
    const Expeed6GpioLink *link[GPIO_LINK_MAX];
    unsigned n_link;
    void (*echo_fire)(void *opaque, int src);
    void *echo_opaque;
    bool master;
};

static void gpio_echo_cb(void *opaque)
{
    Expeed6GpioState *s = opaque;
    if (s->echo_fire) {
        s->echo_fire(s->echo_opaque, GPIO_ECHO_SRC);
    }
}

#define SD_CD_PORT 0x14
#define SD_CD_BIT  14
Expeed6GpioState *s_master_gpio, *s_slave_gpio;

void gpio_set_sd_card(Expeed6GpioState *s, bool present)
{
    unsigned i = SD_CD_PORT / 4;
    if (!s->in_port_set[i]) {
        s->in_port[i] = s->in_level;
        s->in_port_set[i] = true;
    }
    if (present) {
        s->in_port[i] &= (uint16_t) ~(1u << SD_CD_BIT);
    } else {
        s->in_port[i] |= (uint16_t)(1u << SD_CD_BIT);
    }
}

void gpio_set_input(Expeed6GpioState *s, unsigned off, unsigned bit, bool high)
{
    unsigned i = off / 4;
    if (!s->in_port_set[i]) {
        s->in_port[i] = s->in_level;
        s->in_port_set[i] = true;
    }
    if (high) {
        s->in_port[i] |= (uint16_t)(1u << bit);
    } else {
        s->in_port[i] &= (uint16_t) ~(1u << bit);
    }
}

#define GPIO_EXIU_PORT     0x14
#define GPIO_EXIU_BIT0     7
#define GPIO_EXIU_BIT_LAST 15

void gpio_drive_input(Expeed6GpioState *s, unsigned off, unsigned bit, bool high)
{
    const uint16_t before = s->in_port_set[off / 4] ? s->in_port[off / 4] : s->in_level;
    gpio_set_input(s, off, bit, high);
    if (off == GPIO_EXIU_PORT && bit >= GPIO_EXIU_BIT0 && bit <= GPIO_EXIU_BIT_LAST && s->echo_opaque &&
        ((before >> bit) & 1) != high) {
        extint_input_edge(s->echo_opaque, bit - GPIO_EXIU_BIT0, high);
    }
}

static uint64_t gpio_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6GpioState *s = opaque;
    uint32_t v = 0;
    if (off / 4 < 0x400) {
        v = s->reg[off / 4];
        if (off < GPIO_DIR_OFF) {
            uint16_t dir = (uint16_t)s->reg[(off + GPIO_DIR_OFF) / 4];
            uint16_t in = s->in_port_set[off / 4] ? s->in_port[off / 4] : s->in_level;
            for (unsigned i = 0; i < s->n_link; i++) {
                if (s->link[i]->in_off == off) {
                    uint16_t b = (uint16_t)(1u << s->link[i]->in_bit);
                    uint16_t lvl =
                        ((s->reg[s->link[i]->out_off / 4] >> s->link[i]->out_bit) & 1) ^ s->link[i]->invert;
                    in = lvl ? (uint16_t)(in | b) : (uint16_t)(in & ~b);
                }
            }
            v = (uint16_t)((v & dir) | (in & (uint16_t)~dir));
        }
    }
    return v;
}

static void gpio_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6GpioState *s = opaque;
    if (off / 4 < 0x400) {
        if (off < GPIO_DIR_OFF) {
            uint16_t mask = (uint16_t)(val >> 16), data = (uint16_t)val;
            uint16_t was = (uint16_t)s->reg[off / 4];
            s->reg[off / 4] = (uint16_t)((s->reg[off / 4] & ~mask) | (data & mask));
            const uint16_t b = (uint16_t)(1u << GPIO_ECHO_BIT);
            if (s->master && off == GPIO_ECHO_OFF && !(was & b) && (s->reg[off / 4] & b)) {
                timer_mod(s->echo_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + GPIO_ECHO_MS);
            }
        } else {
            s->reg[off / 4] = val;
        }
    }
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void gpio_instance_init(Object *obj)
{
    Expeed6GpioState *s = EXPEED6_GPIO(obj);

    s->in_level = GPIO_IN_LEVEL;
}

static void gpio_realize(DeviceState *dev, Error **errp)
{
    Expeed6GpioState *s = EXPEED6_GPIO(dev);

    s->echo_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, gpio_echo_cb, s);
    for (size_t i = 0; i < ARRAY_SIZE(gpio_in_ports); i++) {
        s->in_port[gpio_in_ports[i].off / 4] = gpio_in_ports[i].level;
        s->in_port_set[gpio_in_ports[i].off / 4] = true;
    }
    if (getenv("Z6II_SD") && *getenv("Z6II_SD")) {
        gpio_set_sd_card(s, true);
    }
    if (!s->master) {
        s->link[s->n_link++] = &gpio_link_slave_card;
    } else if (getenv("Z6II_XQD")) {
        s->link[s->n_link++] = &gpio_link_xqd;
    }
    memory_region_init_io(&s->mmio, OBJECT(dev), &gpio_ops, s, "expeed6.gpio", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void gpio_reset_enter(Object *obj, ResetType type)
{
    Expeed6GpioState *s = EXPEED6_GPIO(obj);

    memset(s->reg, 0, sizeof(s->reg));
    timer_del(s->echo_timer);
}

static const VMStateDescription vmstate_gpio = {
    .name = TYPE_EXPEED6_GPIO,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]){ VMSTATE_UINT32_ARRAY(reg, Expeed6GpioState, 0x400),
                                      VMSTATE_UINT16(in_level, Expeed6GpioState),
                                      VMSTATE_UINT16_ARRAY(in_port, Expeed6GpioState, GPIO_DIR_OFF / 4),
                                      VMSTATE_BOOL_ARRAY(in_port_set, Expeed6GpioState, GPIO_DIR_OFF / 4),
                                      VMSTATE_TIMER_PTR(echo_timer, Expeed6GpioState), VMSTATE_END_OF_LIST() }
};

static Property gpio_properties[] = {
    DEFINE_PROP_BOOL("master", Expeed6GpioState, master, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void gpio_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = gpio_realize;
    dc->vmsd = &vmstate_gpio;
    device_class_set_props(dc, gpio_properties);
    rc->phases.enter = gpio_reset_enter;
}

static const TypeInfo gpio_info = {
    .name = TYPE_EXPEED6_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6GpioState),
    .instance_init = gpio_instance_init,
    .class_init = gpio_class_init,
};

static void gpio_register_types(void)
{
    type_register_static(&gpio_info);
}

type_init(gpio_register_types)

    Expeed6GpioState *expeed6_install_gpio(MemoryRegion *root, bool master, Expeed6ExtIntState *extint)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_GPIO);
    Expeed6GpioState *gpio = EXPEED6_GPIO(dev);
    qdev_prop_set_bit(dev, "master", master);
    gpio->echo_opaque = extint; /* the controller its wired pins (and echoes) raise */
    if (master) {
        gpio->echo_fire = gpio_echo_fire_extint;
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    if (master) {
        s_master_gpio = gpio;
    } else {
        s_slave_gpio = gpio;
    }
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, GPIO_BASE);
    return gpio;
}
