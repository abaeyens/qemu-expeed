/*
 * The Expeed 6 SD_0 host controller at 0x2a100000
 *
 * QEMU's SDHCI with the Milbeaut vendor registers
 *
 * References:
 * - mainline Linux: drivers/mmc/host/sdhci-milbeaut.c, vendor registers above 0x100
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "hw/sd/sdhci.h"
#include "qapi/qmp/qdict.h"
#include "hw/sd/sd.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"
#include "sysemu/runstate.h"
#include "block/block-global-state.h"

#define SD0_BASE    0x2a100000
#define SD0_GIC_IRQ 230

typedef struct {
    char *path;
    uint8_t sha[32];
} Expeed6SdImage;

static Expeed6SdImage s_sd_image;

static bool sd_image_sha(const char *path, uint8_t out[32])
{
    g_autoptr(GChecksum) c = g_checksum_new(G_CHECKSUM_SHA256);
    FILE *f = fopen(path, "rb");
    static uint8_t buf[1 << 20];
    gsize len = 32;
    size_t n;

    if (!f) {
        error_report("Z6II_SD: cannot read the card image %s for the snapshot: %s", path, strerror(errno));
        return false;
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        g_checksum_update(c, buf, n);
    }
    fclose(f);
    g_checksum_get_digest(c, out, &len);
    return true;
}

static int sd_image_pre_save(void *opaque)
{
    return sd_image_sha(s_sd_image.path, s_sd_image.sha) ? 0 : -EIO;
}

static int sd_image_post_load(void *opaque, int version_id)
{
    uint8_t now[32];

    if (!sd_image_sha(s_sd_image.path, now)) {
        return -EIO;
    }
    if (memcmp(now, s_sd_image.sha, sizeof(now))) {
        error_report("Z6II_SD: the card image %s is not the one the snapshot was saved with (another sha256)",
                     s_sd_image.path);
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sd_image = {
    .name = "expeed6-sd-image",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = sd_image_pre_save,
    .post_load = sd_image_post_load,
    .fields = (const VMStateField[]){ VMSTATE_BUFFER(sha, Expeed6SdImage), VMSTATE_END_OF_LIST() }
};

static void sd_image_run_state(void *opaque, bool running, RunState state)
{
    Error *err = NULL;

    if (running) {
        bdrv_activate_all(&err);
        if (err) {
            error_report_err(err);
            exit(1);
        }
    }
}

void expeed6_install_sd0(MemoryRegion *root, DeviceState *gic, bool master)
{
    const char *path = getenv("Z6II_SD");
    const char *on = getenv("Z6II_SD_ON");
    bool want_slave = on && !strcmp(on, "slave");
    if (!path || want_slave == master) {
        return;
    }
    s_sd_image.path = g_strdup(path);
    vmstate_register_any(NULL, &vmstate_sd_image, &s_sd_image);
    qemu_add_vm_change_state_handler(sd_image_run_state, NULL);

    DeviceState *sd = qdev_new(TYPE_SYSBUS_SDHCI);
    object_property_set_link(OBJECT(sd), "dma", OBJECT(root), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sd), &error_fatal);
    if (!master) {
        memory_region_add_subregion_overlap(root, PCIE_SD_BAR0, sysbus_mmio_get_region(SYS_BUS_DEVICE(sd), 0), 2);
        if (s_slave_glue) {
            sysbus_connect_irq(SYS_BUS_DEVICE(sd), 0, qemu_allocate_irq(pcie_sd_irq, s_slave_glue, 0));
        }
    } else {
        memory_region_add_subregion_overlap(root, SD0_BASE, sysbus_mmio_get_region(SYS_BUS_DEVICE(sd), 0), 2);
        sysbus_connect_irq(SYS_BUS_DEVICE(sd), 0, qdev_get_gpio_in(gic, SD0_GIC_IRQ - 32));
        install_regfile(root, SD0_BASE + 0x100, 0x200, "expeed6.sd0.vendor");
    }

    QDict *opts = qdict_new();
    qdict_put_str(opts, "driver", "raw");
    Error *err = NULL;
    BlockBackend *blk = blk_new_open(path, NULL, opts, BDRV_O_RDWR, &err);
    if (!blk) {
        error_reportf_err(err, "Z6II_SD: cannot open '%s': ", path);
        exit(1);
    }
    DeviceState *card = qdev_new(TYPE_SD_CARD);
    qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
    qdev_realize_and_unref(card, qdev_get_child_bus(sd, "sd-bus"), &error_fatal);
    blk_unref(blk);
}
