/*
 * The Z 6II main board NOR flash behind the master's 64 MB image window
 *
 * Uses qemu/nor_model.c
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "z6ii.h"
#include "exec/exec-all.h"
#include "nor_model.h"
#include "migration/vmstate.h"
#include "sysemu/reset.h"
typedef struct {
    NorFlash f;
    MemoryRegion *mr;
    int32_t mode_saved;
} Z6iiNor;

static Z6iiNor z6ii_nor;

static void z6ii_nor_changed(Z6iiNor *n, uint64_t lo, uint64_t hi)
{
    ram_addr_t ra = memory_region_get_ram_addr(n->mr);
    tb_invalidate_phys_range(ra + lo, ra + hi - 1);
    memory_region_set_dirty(n->mr, lo, hi - lo);
}

static uint64_t z6ii_nor_read(void *opaque, hwaddr addr, unsigned size)
{
    Z6iiNor *n = opaque;
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        uint16_t w = nor_read(&n->f, (addr + i) & ~(hwaddr)1);
        v |= (uint64_t)((w >> (((addr + i) & 1) * 8)) & 0xff) << (8 * i);
    }
    return v;
}

static void z6ii_nor_write16(Z6iiNor *n, hwaddr addr, uint16_t v)
{
    NorMode was = n->f.mode;
    NorWrite r = nor_write(&n->f, addr, v);
    if (r == NOR_W_PLAIN) {
        return;
    }
    if (r == NOR_W_CHANGED) {
        z6ii_nor_changed(n, n->f.dirty_lo, n->f.dirty_hi);
    }
    if (n->f.mode != was) {

        memory_region_rom_device_set_romd(n->mr, n->f.mode == NOR_READ);
    }
}

static void z6ii_nor_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Z6iiNor *n = opaque;
    if (size >= 2 && !(addr & 1)) {

        for (unsigned i = 0; i < size; i += 2) {
            z6ii_nor_write16(n, addr + i, val >> (8 * i));
        }
    }
}

static const MemoryRegionOps z6ii_nor_ops = {
    .read = z6ii_nor_read,
    .write = z6ii_nor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};


static int z6ii_nor_pre_save(void *opaque)
{
    Z6iiNor *n = opaque;
    n->mode_saved = n->f.mode;
    return 0;
}

static int z6ii_nor_post_load(void *opaque, int version_id)
{
    Z6iiNor *n = opaque;
    n->f.mode = (NorMode)n->mode_saved;
    memory_region_rom_device_set_romd(n->mr, n->f.mode == NOR_READ);
    return 0;
}

static const VMStateDescription vmstate_z6ii_nor = {
    .name = "z6ii-nor",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = z6ii_nor_pre_save,
    .post_load = z6ii_nor_post_load,
    .fields = (const VMStateField[]){ VMSTATE_INT32(mode_saved, Z6iiNor), VMSTATE_INT32(f.cycle, Z6iiNor),
                                      VMSTATE_UINT64(f.wb_base, Z6iiNor), VMSTATE_UINT32(f.wb_count, Z6iiNor),
                                      VMSTATE_UINT32(f.wb_n, Z6iiNor),
                                      VMSTATE_UINT64_ARRAY(f.wb_addr, Z6iiNor, NOR_BUF_WORDS_MAX),
                                      VMSTATE_UINT16_ARRAY(f.wb_data, Z6iiNor, NOR_BUF_WORDS_MAX),
                                      VMSTATE_END_OF_LIST() }
};

static void z6ii_nor_reset(void *opaque)
{
    Z6iiNor *n = opaque;
    n->f.mode = NOR_READ;
    n->f.cycle = 0;
    n->f.wb_count = n->f.wb_n = 0;
    memory_region_rom_device_set_romd(n->mr, true);
}

void z6ii_image_init(MemoryRegion *image)
{
    memory_region_init_rom_device(image, NULL, &z6ii_nor_ops, &z6ii_nor, "expeed6.image", IMAGE_SIZE,
                                  &error_fatal);
    z6ii_nor.mr = image;
    nor_init(&z6ii_nor.f, memory_region_get_ram_ptr(image), IMAGE_SIZE, 0x20000, 512);
    vmstate_register_any(NULL, &vmstate_z6ii_nor, &z6ii_nor);
    qemu_register_reset(z6ii_nor_reset, &z6ii_nor);
}
