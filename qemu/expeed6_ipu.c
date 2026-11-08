/*
 * The Expeed 6 IPU mailbox at 0x24020000, GIC 101
 *
 * the master's channel to the image processing unit's jobs
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "migration/vmstate.h"

#define IPU_BASE    0x24020000
#define IPU_GIC_IRQ 101
#define IPU_RX_MAX  64

#define TYPE_EXPEED6_IPU_MBX "expeed6-ipu-mbx"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6IpuState, EXPEED6_IPU_MBX)

struct Expeed6IpuState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t reg[0x100 / 4];
    qemu_irq irq;
    QEMUTimer *timer;
    uint32_t tx[3];
    uint32_t ntx;
    uint32_t rx[IPU_RX_MAX][3];
    uint32_t rx_head, rx_count, rx_word;
    uint32_t due[IPU_RX_MAX][3];
    uint32_t due_count;
};

static void ipu_update_irq(Expeed6IpuState *s)
{
    uint32_t st = s->reg[0x14 / 4] | 0x04 | (s->rx_count ? 0x01 : 0);
    s->reg[0x14 / 4] = st;
    qemu_set_irq(s->irq, (st & s->reg[0x1c / 4]) != 0);
}

static void ipu_deliver(void *opaque)
{
    Expeed6IpuState *s = opaque;
    for (unsigned i = 0; i < s->due_count && s->rx_count < IPU_RX_MAX; i++) {
        unsigned slot = (s->rx_head + s->rx_count) % IPU_RX_MAX;
        memcpy(s->rx[slot], s->due[i], sizeof(s->rx[slot]));
        s->rx_count++;
    }
    s->due_count = 0;
    ipu_update_irq(s);
}

static void ipu_queue(Expeed6IpuState *s, uint32_t w0, uint32_t w1, uint32_t w2)
{
    if (s->due_count < IPU_RX_MAX) {
        s->due[s->due_count][0] = w0;
        s->due[s->due_count][1] = w1;
        s->due[s->due_count][2] = w2;
        s->due_count++;
    }
    timer_mod(s->timer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + 1000);
}

static void ipu_command(Expeed6IpuState *s)
{
    uint32_t w0 = s->tx[0], w1 = s->tx[1], w2 = s->tx[2];
    uint32_t op = w0 & 0x3f;
    if (w0 & 0x2000) {
        return;
    }
    ipu_queue(s, w0 | 0x2000, w1, w2);
    if ((w0 & 0xfff) < 0x3f && (op == 0x0e || op == 0x10 || op == 0x12 || op == 0x14)) {
        ipu_queue(s, op + 1, w1, w2);
    } else if ((w0 & 0xfff) >= 0x3f) {
        ipu_queue(s, w0 & 0x1fff, w1, w2);
    }
}

static uint64_t ipu_mbx_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6IpuState *s = opaque;
    switch (off) {
    case 0x00:
        if (!s->rx_count) {
            return 0;
        }
        return s->rx[s->rx_head][s->rx_word < 3 ? s->rx_word++ : 2];
    case 0x0c:
        return s->rx_count * 3;
    default:
        if (off >= 0x74) {
            return allones_read((void *)(uintptr_t)IPU_BASE, off, size);
        }
        return off < sizeof(s->reg) ? s->reg[off / 4] : 0;
    }
}

static void ipu_mbx_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6IpuState *s = opaque;
    switch (off) {
    case 0x04:
        if (s->ntx < 3) {
            s->tx[s->ntx++] = (uint32_t)val;
        }
        return;
    case 0x08:
        if (val == 2) {
            if (s->ntx == 3) {
                ipu_command(s);
            }
            s->ntx = 0;
        } else if (val == 4 && s->rx_count) {
            s->rx_head = (s->rx_head + 1) % IPU_RX_MAX;
            s->rx_count--;
            s->rx_word = 0;
        }
        ipu_update_irq(s);
        return;
    case 0x18:
        s->reg[0x14 / 4] &= ~(uint32_t)val;
        ipu_update_irq(s);
        return;
    case 0x1c:
        s->reg[off / 4] = (uint32_t)val;
        ipu_update_irq(s);
        return;
    default:
        if (off < sizeof(s->reg)) {
            s->reg[off / 4] = (uint32_t)val;
        }
    }
}

static const MemoryRegionOps ipu_mbx_ops = {
    .read = ipu_mbx_read,
    .write = ipu_mbx_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ipu_mbx_realize(DeviceState *dev, Error **errp)
{
    Expeed6IpuState *s = EXPEED6_IPU_MBX(dev);

    s->timer = timer_new_us(QEMU_CLOCK_VIRTUAL, ipu_deliver, s);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    memory_region_init_io(&s->mmio, OBJECT(dev), &ipu_mbx_ops, s, "expeed6.ipu-mbx", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void ipu_mbx_reset_enter(Object *obj, ResetType type)
{
    Expeed6IpuState *s = EXPEED6_IPU_MBX(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->ntx = s->rx_head = s->rx_count = s->rx_word = s->due_count = 0;
    timer_del(s->timer);
}

static void ipu_mbx_reset_hold(Object *obj, ResetType type)
{
    qemu_set_irq(EXPEED6_IPU_MBX(obj)->irq, 0);
}

static const VMStateDescription vmstate_ipu_mbx = {
    .name = TYPE_EXPEED6_IPU_MBX,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields =
        (const VMStateField[]){
            VMSTATE_UINT32_ARRAY(reg, Expeed6IpuState, 0x100 / 4), VMSTATE_TIMER_PTR(timer, Expeed6IpuState),
            VMSTATE_UINT32_ARRAY(tx, Expeed6IpuState, 3), VMSTATE_UINT32(ntx, Expeed6IpuState),
            VMSTATE_UINT32_2DARRAY(rx, Expeed6IpuState, IPU_RX_MAX, 3), VMSTATE_UINT32(rx_head, Expeed6IpuState),
            VMSTATE_UINT32(rx_count, Expeed6IpuState), VMSTATE_UINT32(rx_word, Expeed6IpuState),
            VMSTATE_UINT32_2DARRAY(due, Expeed6IpuState, IPU_RX_MAX, 3),
            VMSTATE_UINT32(due_count, Expeed6IpuState), VMSTATE_END_OF_LIST() }
};


static void ipu_mbx_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = ipu_mbx_realize;
    dc->vmsd = &vmstate_ipu_mbx;
    rc->phases.enter = ipu_mbx_reset_enter;
    rc->phases.hold = ipu_mbx_reset_hold;
}

static const TypeInfo ipu_mbx_info = {
    .name = TYPE_EXPEED6_IPU_MBX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6IpuState),
    .class_init = ipu_mbx_class_init,
};

static void ipu_mbx_register_types(void)
{
    type_register_static(&ipu_mbx_info);
}

type_init(ipu_mbx_register_types)

    void expeed6_install_ipu_mbx(MemoryRegion *root, DeviceState *gic)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_IPU_MBX);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, IPU_GIC_IRQ - 32));
    memory_region_add_subregion_overlap(root, IPU_BASE, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0), 2);
}

static uint64_t ipu_read(void *opaque, hwaddr off, unsigned size)
{
    if (off == 0x0c) {
        return 0;
    }
    return allones_read((void *)(uintptr_t)0x24020000, off, size);
}

static const MemoryRegionOps ipu_ops = {
    .read = ipu_read,
    .write = allones_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

void expeed6_install_ipu_engine(MemoryRegion *root)
{
    MemoryRegion *ipumr = g_new(MemoryRegion, 1);
    memory_region_init_io(ipumr, NULL, &ipu_ops, (void *)(uintptr_t)0x24020000, "expeed6.ipu", 0x1000);
    memory_region_add_subregion_overlap(root, 0x24020000, ipumr, 1);
}
