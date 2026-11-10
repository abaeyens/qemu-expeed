/*
 * The Expeed 6 display engine at 0x20140000
 *
 * Register file, the frame interrupt and the panel scanout
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"

typedef struct {
    QemuConsole *con;
    AddressSpace *as;
    Expeed6Regfile *rf;
    uint32_t last_w, last_h;
} Expeed6DispEngScanout;

static inline uint32_t dispeng_reg(Expeed6DispEngScanout *s, uint32_t off)
{
    return s->rf->reg[off / 4];
}

static inline uint8_t clip8(float v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5f);
}

static inline uint32_t ycbcr_rgb(float y, float cb, float cr)
{
    return 0xff000000u | (clip8(y + 1.402f * cr) << 16) | (clip8(y - 0.344136f * cb - 0.714136f * cr) << 8) |
           clip8(y + 1.772f * cb);
}

static void expeed6_dispeng_gfx_update(void *opaque)
{
    Expeed6DispEngScanout *s = opaque;
    uint32_t w, h;
    uint32_t vsz = dispeng_reg(s, 0x4204), vw = vsz & 0xffff, vh = vsz >> 16;
    uint32_t vy = dispeng_reg(s, 0x4210), vc = dispeng_reg(s, 0x4220);
    uint32_t vyp = dispeng_reg(s, 0x4240) & 0xffff, vcp = dispeng_reg(s, 0x4250) & 0xffff;
    uint32_t vpos = dispeng_reg(s, 0x4600), vx0 = vpos & 0xffff, vy0 = (vpos >> 16) & 0x3fff;
    uint32_t gsz = dispeng_reg(s, 0x6204), gw = gsz & 0xffff, gh = gsz >> 16;
    uint32_t gc = dispeng_reg(s, 0x6440), gcp = dispeng_reg(s, 0x6500) & 0xffff;
    uint32_t ga = dispeng_reg(s, 0x64c0), gap = dispeng_reg(s, 0x6540) & 0xffff;
    bool video = vy && vc && vw && vh && vw <= 4096 && vh <= 4096 && vyp >= vw && vcp >= vw;
    bool overlay = gc && ga && gw && gh && gw <= 4096 && gh <= 4096 && gcp >= 2 * gw && gap >= gw;

    if (overlay) {
        w = gw, h = gh;
    } else if (video) {
        w = vw, h = vh;
    } else {
        return;  /* no plane programmed yet so show nothing */
    }
    if (!w || !h || w > 4096 || h > 4096) {
        return;
    }
    if (w != s->last_w || h != s->last_h) {
        qemu_console_resize(s->con, w, h);
        s->last_w = w;
        s->last_h = h;
    }
    DisplaySurface *ds = qemu_console_surface(s->con);
    if (!ds || surface_width(ds) != (int)w || surface_height(ds) != (int)h) {
        return;
    }
    uint32_t *dst = (uint32_t *)surface_data(ds);
    int dstride = surface_stride(ds) / 4;
    g_autofree uint8_t *yrow = g_malloc0(w), *crow = g_malloc0(w + 2);
    g_autofree uint8_t *orow = g_malloc0(2 * w + 4), *arow = g_malloc0(w);

    for (uint32_t y = 0; y < h; y++) {
        uint32_t *d = dst + (size_t)y * dstride;
        uint32_t vr = y - vy0;
        bool vrow = video && y >= vy0 && vr < vh, grow = overlay && y < gh;
        uint32_t vn = vx0 < w ? MIN(w - vx0, vw) : 0, gn = MIN(w, gw) & ~1u;
        if (vrow) {
            address_space_read(s->as, vy + (hwaddr)vr * vyp, MEMTXATTRS_UNSPECIFIED, yrow, vn);
            address_space_read(s->as, vc + (hwaddr)(vr / 2) * vcp, MEMTXATTRS_UNSPECIFIED, crow, (vn + 1) & ~1u);
        }
        if (grow) {
            address_space_read(s->as, gc + (hwaddr)y * gcp, MEMTXATTRS_UNSPECIFIED, orow, 2 * gn);
            address_space_read(s->as, ga + (hwaddr)y * gap, MEMTXATTRS_UNSPECIFIED, arow, gn);
        }
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = 0xff000000u;
            if (vrow && x >= vx0 && x - vx0 < vn) {
                uint32_t vx = x - vx0;
                px = ycbcr_rgb(yrow[vx], crow[vx & ~1u] - 128.0f, crow[(vx & ~1u) + 1] - 128.0f);
            }
            if (grow && x < gn) {
                const uint8_t *q = orow + (x & ~1u) * 2;
                uint32_t o = ycbcr_rgb(q[x & 1], q[2] - 128.0f, q[3] - 128.0f);
                uint32_t a = arow[x];
                uint32_t r = (((o >> 16) & 0xff) * a + ((px >> 16) & 0xff) * (255 - a)) / 255;
                uint32_t g = (((o >> 8) & 0xff) * a + ((px >> 8) & 0xff) * (255 - a)) / 255;
                uint32_t b = ((o & 0xff) * a + (px & 0xff) * (255 - a)) / 255;
                px = 0xff000000u | (r << 16) | (g << 8) | b;
            }
            d[x] = px;
        }
    }
    dpy_gfx_update_full(s->con);
}

static const GraphicHwOps expeed6_dispeng_gfx_ops = {
    .gfx_update = expeed6_dispeng_gfx_update,
};

#define DISPENG_LINE0_SPI  78
#define DISPENG_VSYNC_MS   16
#define DISPENG_VSYNC_MASK 0x10000

#define TYPE_EXPEED6_DISPENG_VSYNC "expeed6-dispeng-vsync"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6DispEngVsync, EXPEED6_DISPENG_VSYNC)

struct Expeed6DispEngVsync {
    SysBusDevice parent_obj;
    MemoryRegion st[2];
    Expeed6Regfile *rf;
    QEMUTimer *t;
    qemu_irq line[4];
    uint64_t frames;
};

static void dispeng_vsync_update(Expeed6DispEngVsync *v)
{
    uint32_t *r = v->rf->reg;
    int level[4] = { 0 };
    static const uint32_t unit[2] = { 0x1000, 0x5000 };
    for (int i = 0; i < 2; i++) {
        uint32_t sel = r[(unit[i] + 0x100) / 4] & 0xff;
        if (sel >= 1 && sel <= 3) {
            level[sel] |= (r[(unit[i] + 0x214) / 4] & r[(unit[i] + 0x210) / 4]) != 0;
        }
    }
    for (int l = 1; l <= 3; l++) {
        qemu_set_irq(v->line[l], level[l]);
    }
}

static const uint32_t dispeng_st_off[2] = { 0x1214, 0x5214 };

static uint64_t dispeng_st_read(void *opaque, hwaddr off, unsigned size, int unit)
{
    Expeed6DispEngVsync *v = opaque;
    return v->rf->reg[dispeng_st_off[unit] / 4];
}

static void dispeng_st_write(void *opaque, hwaddr off, uint64_t val, unsigned size, int unit)
{
    Expeed6DispEngVsync *v = opaque;
    v->rf->reg[dispeng_st_off[unit] / 4] &= ~(uint32_t)val;  /* write 1 to clear */
    dispeng_vsync_update(v);
}

static uint64_t dispeng_st0_read(void *opaque, hwaddr off, unsigned size)
{
    return dispeng_st_read(opaque, off, size, 0);
}

static void dispeng_st0_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    dispeng_st_write(opaque, off, val, size, 0);
}

static uint64_t dispeng_st1_read(void *opaque, hwaddr off, unsigned size)
{
    return dispeng_st_read(opaque, off, size, 1);
}

static void dispeng_st1_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    dispeng_st_write(opaque, off, val, size, 1);
}

static const MemoryRegionOps dispeng_st_ops[2] = {
    {
        .read = dispeng_st0_read,
        .write = dispeng_st0_write,
        .endianness = DEVICE_LITTLE_ENDIAN,
        .valid = { .min_access_size = 4, .max_access_size = 4 },
    },
    {
        .read = dispeng_st1_read,
        .write = dispeng_st1_write,
        .endianness = DEVICE_LITTLE_ENDIAN,
        .valid = { .min_access_size = 4, .max_access_size = 4 },
    },
};

static void dispeng_vsync_tick(void *opaque)
{
    Expeed6DispEngVsync *v = opaque;
    uint32_t bits = DISPENG_VSYNC_MASK & v->rf->reg[0x5210 / 4];
    if (bits) {
        v->rf->reg[0x5214 / 4] |= bits;
        v->frames++;
    }
    dispeng_vsync_update(v);
    timer_mod(v->t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DISPENG_VSYNC_MS * 1000000LL);
}

static void dispeng_vsync_realize(DeviceState *dev, Error **errp)
{
    Expeed6DispEngVsync *v = EXPEED6_DISPENG_VSYNC(dev);

    if (!v->rf) {
        error_setg(errp, "expeed6-dispeng-vsync: needs its register file");
        return;
    }
    for (int l = 1; l <= 3; l++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &v->line[l]);
    }
    for (int i = 0; i < 2; i++) {
        memory_region_init_io(&v->st[i], OBJECT(dev), &dispeng_st_ops[i], v, "expeed6.dispeng-status", 4);
        sysbus_init_mmio(SYS_BUS_DEVICE(dev), &v->st[i]);
    }
    v->t = timer_new_ns(QEMU_CLOCK_VIRTUAL, dispeng_vsync_tick, v);
}

static void dispeng_vsync_reset_enter(Object *obj, ResetType type)
{
    Expeed6DispEngVsync *v = EXPEED6_DISPENG_VSYNC(obj);

    v->frames = 0;
    timer_mod(v->t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DISPENG_VSYNC_MS * 1000000LL);
}

static void dispeng_vsync_reset_hold(Object *obj, ResetType type)
{
    Expeed6DispEngVsync *v = EXPEED6_DISPENG_VSYNC(obj);

    for (int l = 1; l <= 3; l++) {
        qemu_set_irq(v->line[l], 0);
    }
}

static const VMStateDescription vmstate_dispeng_vsync = {
    .name = TYPE_EXPEED6_DISPENG_VSYNC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_TIMER_PTR(t, Expeed6DispEngVsync),
                                      VMSTATE_UINT64(frames, Expeed6DispEngVsync), VMSTATE_END_OF_LIST() }
};


static void dispeng_vsync_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = dispeng_vsync_realize;
    dc->vmsd = &vmstate_dispeng_vsync;
    rc->phases.enter = dispeng_vsync_reset_enter;
    rc->phases.hold = dispeng_vsync_reset_hold;
}

static const TypeInfo dispeng_vsync_info = {
    .name = TYPE_EXPEED6_DISPENG_VSYNC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6DispEngVsync),
    .class_init = dispeng_vsync_class_init,
};

static void dispeng_vsync_register_types(void)
{
    type_register_static(&dispeng_vsync_info);
}

type_init(dispeng_vsync_register_types)

void expeed6_install_disp(MemoryRegion *root, AddressSpace *as, DeviceState *gic)
{
    Expeed6Regfile *disp = install_regfile(root, 0x20140000, 0x10000, "expeed6.dsi");
    DeviceState *dev = qdev_new(TYPE_EXPEED6_DISPENG_VSYNC);
    Expeed6DispEngVsync *v = EXPEED6_DISPENG_VSYNC(dev);
    v->rf = disp;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    for (int l = 1; l <= 3; l++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), l - 1, qdev_get_gpio_in(gic, DISPENG_LINE0_SPI + l));
    }
    for (int i = 0; i < 2; i++) {
        memory_region_add_subregion_overlap(root, 0x20140000 + dispeng_st_off[i],
                                            sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), i), 2);
    }
    Expeed6DispEngScanout *de = g_new0(Expeed6DispEngScanout, 1);
    de->as = as;
    de->rf = disp;
    de->con = graphic_console_init(NULL, 0, &expeed6_dispeng_gfx_ops, de);
}
