/*
 * The Expeed 6 UART_0 (USIO) at 0x2c090000
 *
 * The firmware's serial console
 *
 * References:
 * - GoPro GP2 Linux 4.19.124: drivers/tty/serial/milbeaut_usio.c
 * - Nikon Z 9 Linux 4.14.224: drivers/tty/serial/nkj_uart.c
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "hw/qdev-properties.h"

#define UART0_BASE      0x2c090000
#define UART0_SIZE      0x1000
#define UART0_TX        0x04
#define UART0_LSR       0x03
#define UART0_LSR_TXRDY 0x02

#define TYPE_EXPEED6_UART0 "expeed6-uart0"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6UartState, EXPEED6_UART0)

struct Expeed6UartState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    char *path;
    FILE *out;
};

static uint64_t uart0_read(void *opaque, hwaddr off, unsigned size)
{
    if (off == UART0_LSR) {
        return UART0_LSR_TXRDY;
    }
    return allones_read((void *)(uintptr_t)UART0_BASE, off, size);
}

static void uart0_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6UartState *s = opaque;
    if (off == UART0_TX && s->out) {
        fputc((int)(val & 0xff), s->out);
        fflush(s->out);
        return;
    }
    allones_write((void *)(uintptr_t)UART0_BASE, off, val, size);
}

static const MemoryRegionOps uart0_ops = {
    .read = uart0_read,
    .write = uart0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void uart0_realize(DeviceState *dev, Error **errp)
{
    Expeed6UartState *u = EXPEED6_UART0(dev);

    if (u->path && *u->path) {
        u->out = strcmp(u->path, "-") ? fopen(u->path, "w") : stderr;
    }
    memory_region_init_io(&u->mmio, OBJECT(dev), &uart0_ops, u, "expeed6.uart0", UART0_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &u->mmio);
}

static Property uart0_properties[] = {
    DEFINE_PROP_STRING("path", Expeed6UartState, path),
    DEFINE_PROP_END_OF_LIST(),
};

static void uart0_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = uart0_realize;
    device_class_set_props(dc, uart0_properties);
}

static const TypeInfo uart0_info = {
    .name = TYPE_EXPEED6_UART0,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6UartState),
    .class_init = uart0_class_init,
};

static void uart0_register_types(void)
{
    type_register_static(&uart0_info);
}

type_init(uart0_register_types)

    void expeed6_install_uart0(MemoryRegion *root, bool master)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_UART0);
    if (master && getenv("EXPEED_UART")) {
        qdev_prop_set_string(dev, "path", getenv("EXPEED_UART"));
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, UART0_BASE);
}
