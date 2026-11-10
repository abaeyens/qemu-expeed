/*
 * The Expeed 6 HDMAC, two 8-channel units at 0x2c020000/0x2c030000
 *
 * References:
 * - GoPro GP2 Linux 4.19.124: drivers/dma/milbeaut_hdmac.c
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "expeed6_hdmac.h"

Expeed6HdmacState *s_hdmac;

uint32_t *hdmac_ch(Expeed6HdmacUnit *u, unsigned n, unsigned r)
{
    return &u->reg[(0x10 + 0x10 * n + r) / 4];
}

static void hdmac_update_irq(Expeed6HdmacUnit *u, unsigned n)
{
    uint32_t b = *hdmac_ch(u, n, 4);
    qemu_set_irq(u->irq[n], (b & HDMAC_SS_MASK) && (b & (HDMAC_B_CI | HDMAC_B_EI)));
}

void hdmac_finish(Expeed6HdmacUnit *u, unsigned n)
{
    uint32_t *b = hdmac_ch(u, n, 4);
    *b = (*b & ~HDMAC_SS_MASK) | (HDMAC_SS_OK << HDMAC_SS_SHIFT);
    *hdmac_ch(u, n, 0) &= ~HDMAC_A_EB;
    hdmac_update_irq(u, n);
}

uint32_t hdmac_len(Expeed6HdmacUnit *u, unsigned n, unsigned *width)
{
    uint32_t a = *hdmac_ch(u, n, 0), b = *hdmac_ch(u, n, 4);
    *width = 1u << ((b >> 26) & 3);
    return ((a & 0xffff) + 1) * (((a >> 16) & 0xf) + 1) * *width;
}

static void hdmac_memcpy(Expeed6HdmacUnit *u, unsigned n)
{
    Expeed6HdmacState *s = u->s;
    unsigned w;
    uint32_t len = hdmac_len(u, n, &w), b = *hdmac_ch(u, n, 4);
    uint32_t src = *hdmac_ch(u, n, 8), dst = *hdmac_ch(u, n, 0xc);

    if (len > HDMAC_MAXLEN) {
        qemu_log_mask(LOG_UNIMP,
                      "expeed6.hdmac: unit %u ch%u copy of %u bytes exceeds the "
                      "model's %u-byte bound -- not copied\n",
                      u->unit, n, len, HDMAC_MAXLEN);
    } else if (!(b & (HDMAC_B_FS | HDMAC_B_FD))) {
        g_autofree uint8_t *buf = g_malloc(len);
        address_space_read(s->as, src, MEMTXATTRS_UNSPECIFIED, buf, len);
        address_space_write(s->as, dst, MEMTXATTRS_UNSPECIFIED, buf, len);
    } else {
        for (uint32_t o = 0; o < len; o += w) {
            uint8_t beat[4];
            address_space_read(s->as, src + ((b & HDMAC_B_FS) ? 0 : o), MEMTXATTRS_UNSPECIFIED, beat, w);
            address_space_write(s->as, dst + ((b & HDMAC_B_FD) ? 0 : o), MEMTXATTRS_UNSPECIFIED, beat, w);
        }
    }
    hdmac_finish(u, n);
}

static uint64_t hdmac_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6HdmacUnit *u = opaque;
    return off < sizeof(u->reg) ? u->reg[off / 4] : 0;
}


static void hdmac_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6HdmacUnit *u = opaque;
    if (off >= sizeof(u->reg)) {
        return;
    }
    uint32_t old = u->reg[off / 4];
    u->reg[off / 4] = (uint32_t)val;
    if (off < 0x10) {
        return;
    }
    unsigned n = (off - 0x10) / 0x10, r = (off - 0x10) % 0x10;
    if (r == 4) {
        hdmac_update_irq(u, n);
    } else if (r == 0 && (val & HDMAC_A_EB) && !(old & HDMAC_A_EB)) {
        if (((val >> 24) & 0x1f) == 0 && (val & HDMAC_A_ST)) {
            hdmac_memcpy(u, n);
        } else if (((val >> 24) & 0x1f) == 0) {
            qemu_log_mask(LOG_UNIMP,
                          "expeed6.hdmac: unit %u ch%u enabled with no trigger "
                          "(DMACA=0x%08x)\n",
                          u->unit, n, (uint32_t)val);
        } else if (audio_dma_kick(u, n)) {
        }
    } else if (r == 0 && !(val & HDMAC_A_EB) && (old & HDMAC_A_EB)) {
        audio_dma_cancel(u, n);
    }
}

static const MemoryRegionOps hdmac_ops = {
    .read = hdmac_read,
    .write = hdmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static bool hdmac_find(Expeed6HdmacState *s, uint32_t reg, bool want_dst, Expeed6HdmacUnit **pu, unsigned *pn)
{
    for (unsigned k = 0; k < 2; k++) {
        for (unsigned n = 0; n < 8; n++) {
            Expeed6HdmacUnit *u = &s->u[k];
            if ((*hdmac_ch(u, n, 0) & HDMAC_A_EB) && *hdmac_ch(u, n, want_dst ? 0xc : 8) == reg) {
                *pu = u;
                *pn = n;
                return true;
            }
        }
    }
    return false;
}

bool hdmac_sio_exchange(unsigned ch)
{
    Expeed6HdmacState *s = s_hdmac;
    Expeed6HdmacUnit *tu = NULL, *ru = NULL;
    unsigned tn = 0, rn = 0, w;
    uint32_t txreg = 0x2c0a0000 + 0x100 * ch;
    uint32_t len = 0;

    if (!s) {
        return false;
    }
    bool have_tx = hdmac_find(s, txreg, true, &tu, &tn);
    bool have_rx = hdmac_find(s, txreg + 4, false, &ru, &rn);
    if (have_tx) {
        len = hdmac_len(tu, tn, &w);
    } else if (have_rx) {
        len = hdmac_len(ru, rn, &w);
    }
    if (!len || len > HDMAC_MAXLEN) {
        return true;
    }
    g_autofree uint8_t *txb = g_malloc0(len);
    g_autofree uint8_t *rxb = g_malloc0(len);
    if (have_tx) {
        address_space_read(s->as, *hdmac_ch(tu, tn, 8), MEMTXATTRS_UNSPECIFIED, txb, len);
    }
    if (have_rx) {
        address_space_write(s->as, *hdmac_ch(ru, rn, 0xc), MEMTXATTRS_UNSPECIFIED, rxb, len);
    }
    if (have_tx) {
        hdmac_finish(tu, tn);
    }
    if (have_rx) {
        hdmac_finish(ru, rn);
    }
    return true;
}

static void hdmac_realize(DeviceState *dev, Error **errp)
{
    Expeed6HdmacState *s = EXPEED6_HDMAC(dev);

    for (unsigned k = 0; k < 2; k++) {
        s->u[k].s = s;
        s->u[k].unit = k;
        for (unsigned n = 0; n < 8; n++) {
            sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->u[k].irq[n]);
        }
        memory_region_init_io(&s->u[k].mmio, OBJECT(dev), &hdmac_ops, &s->u[k], "expeed6.hdmac", 0x1000);
        sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->u[k].mmio);
    }
}

static void hdmac_reset_enter(Object *obj, ResetType type)
{
    Expeed6HdmacState *s = EXPEED6_HDMAC(obj);

    for (unsigned k = 0; k < 2; k++) {
        memset(s->u[k].reg, 0, sizeof(s->u[k].reg));
    }
}

static void hdmac_reset_hold(Object *obj, ResetType type)
{
    Expeed6HdmacState *s = EXPEED6_HDMAC(obj);

    for (unsigned k = 0; k < 2; k++) {
        for (unsigned n = 0; n < 8; n++) {
            qemu_set_irq(s->u[k].irq[n], 0);
        }
    }
}

static const VMStateDescription vmstate_hdmac_unit = { .name = TYPE_EXPEED6_HDMAC "/unit",
                                                       .version_id = 1,
                                                       .minimum_version_id = 1,
                                                       .fields = (const VMStateField[]){
                                                           VMSTATE_UINT32_ARRAY(reg, Expeed6HdmacUnit, 0x90 / 4),
                                                           VMSTATE_END_OF_LIST() } };

static const VMStateDescription vmstate_hdmac = { .name = TYPE_EXPEED6_HDMAC,
                                                  .version_id = 1,
                                                  .minimum_version_id = 1,
                                                  .fields = (const VMStateField[]){
                                                      VMSTATE_STRUCT_ARRAY(u, Expeed6HdmacState, 2, 1,
                                                                           vmstate_hdmac_unit, Expeed6HdmacUnit),
                                                      VMSTATE_END_OF_LIST() } };

static void hdmac_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = hdmac_realize;
    dc->vmsd = &vmstate_hdmac;
    rc->phases.enter = hdmac_reset_enter;
    rc->phases.hold = hdmac_reset_hold;
}

static const TypeInfo hdmac_info = {
    .name = TYPE_EXPEED6_HDMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6HdmacState),
    .class_init = hdmac_class_init,
};

static void hdmac_register_types(void)
{
    type_register_static(&hdmac_info);
}

type_init(hdmac_register_types)

void expeed6_install_hdmac(MemoryRegion *root, AddressSpace *as, DeviceState *gic)
{
    static const hwaddr base[2] = { HDMAC_BASE0, HDMAC_BASE1 };
    static const int spi[2] = { HDMAC_SPI0, HDMAC_SPI1 };
    DeviceState *dev = qdev_new(TYPE_EXPEED6_HDMAC);
    Expeed6HdmacState *s = EXPEED6_HDMAC(dev);

    s->as = as;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    for (unsigned k = 0; k < 2; k++) {
        expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), k, base[k]);
        for (unsigned n = 0; n < 8; n++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(dev), k * 8 + n, qdev_get_gpio_in(gic, spi[k] + n));
        }
    }
    s_hdmac = s;
}
