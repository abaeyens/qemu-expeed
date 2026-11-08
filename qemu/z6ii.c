/*
 * The QEMU machine `z6ii`
 *
 * Two Expeed 6 clusters (expeed6.c), each booted from its flash image
 * (Z6II_FLASH and Z6II_SLAVE, see scripts/make_flash_image.py),
 * joined by the DualEngine ring; the image window NOR flash (z6ii_nor.c),
 * the image sensor (z6ii_sensor.c) and the Ex MCU (z6ii_exmcu.c) around them.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "qapi/visitor.h"
#include "qapi/qapi-types-ui.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/misc/unimp.h"
#include "expeed6.h"
#include "expeed6_sio.h"
#include "expeed6_hdmac.h"
#include "z6ii.h"
#include "sysemu/reset.h"
#include "exec/address-spaces.h"
#include "exec/exec-all.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "hw/sysbus.h"
#include "hw/sd/sdhci.h"
#include "hw/sd/sd.h"
#include "hw/qdev-properties-system.h"
#include "sysemu/block-backend.h"
#include "block/aio.h"
#include "qemu/main-loop.h"
#include "qapi/qmp/qdict.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "sysemu/runstate.h"
#include "sysemu/cpus.h"
#include "sysemu/cpu-timers.h"
#include "qemu/log.h"
#include "ui/console.h"
#include "ui/input.h"
#include "gevg_model.h"
#include <math.h>

#include "mipu_model.h"
#include "fe_model.h"
#include "fe_regs.h"
#include "jpeg_model.h"
#include "nef_model.h"
#include "nor_model.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/mma845x.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_bus.h"
#include "sysemu/blockdev.h"
#include <glib/gstdio.h>


static void z6ii_machine_reset(MachineState *machine, ShutdownCause reason)
{
    if (reason != SHUTDOWN_CAUSE_NONE && reason != SHUTDOWN_CAUSE_SNAPSHOT_LOAD) {
        warn_report("z6ii: a system reset keeps the RAM, so this is not a clean boot; start a new QEMU for one");
    }
    qemu_devices_reset(reason);
}

static void load_flash(AddressSpace *as, const char *path)
{
    g_autofree char *buf = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &buf, &len, NULL)) {
        error_report("z6ii: failed to read the flash image %s", path);
        exit(1);
    }
    if (len > IMAGE_SIZE) {
        error_report("z6ii: %s is %zu bytes, more than the 0x%llx-byte flash", path, (size_t)len, IMAGE_SIZE);
        exit(1);
    }
    if (address_space_write_rom(as, IMAGE_BASE, MEMTXATTRS_UNSPECIFIED, buf, len) != MEMTX_OK) {
        error_report("z6ii: failed to load %s", path);
        exit(1);
    }
}

static const char *z6ii_slave_flash(void)
{
    const char *flash = getenv("Z6II_SLAVE");
    return flash && *flash ? flash : NULL;
}

static void z6ii_build_slave(MachineState *machine, bool el3)
{
    const char *flash = z6ii_slave_flash();
    if (!flash) {
        return;
    }
    Expeed6State *sl = g_new0(Expeed6State, 1);
    MemoryRegion *root = g_new(MemoryRegion, 1);
    memory_region_init(root, NULL, "expeed6.sla", UINT64_MAX);
    s_slave_root = root;

    expeed6_make_cpus(machine, sl, root, el3);

    DeviceState *gic = expeed6_gic_new(el3, CPU(sl->cpu[0])->cpu_index);
    SysBusDevice *sbd = SYS_BUS_DEVICE(gic);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion_overlap(root, PERIPHBASE + 0x1000, sysbus_mmio_get_region(sbd, 0), 1);
    memory_region_add_subregion_overlap(root, PERIPHBASE + 0x2000, sysbus_mmio_get_region(sbd, 1), 1);
    for (int c = 0; c < NUM_CPU; c++) {
        DeviceState *cd = DEVICE(sl->cpu[c]);
        sysbus_connect_irq(sbd, c, qdev_get_gpio_in(cd, ARM_CPU_IRQ));
        sysbus_connect_irq(sbd, c + NUM_CPU, qdev_get_gpio_in(cd, ARM_CPU_FIQ));
        sysbus_connect_irq(sbd, c + 2 * NUM_CPU, qdev_get_gpio_in(cd, ARM_CPU_VIRQ));
        sysbus_connect_irq(sbd, c + 3 * NUM_CPU, qdev_get_gpio_in(cd, ARM_CPU_VFIQ));
        static const int timer_ppi[] = { 30, 27, 26, 29, 28 };
        for (int t = 0; t < ARRAY_SIZE(timer_ppi); t++) {
            qdev_connect_gpio_out(cd, t, qdev_get_gpio_in(gic, (NUM_IRQ - 32) + c * 32 + timer_ppi[t]));
        }
    }

    MemoryRegion *mr;
    mr = g_new(MemoryRegion, 1);
    memory_region_init_ram(mr, NULL, "expeed6.sla.image", IMAGE_SIZE, &error_fatal);
    memory_region_add_subregion(root, IMAGE_BASE, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_ram(mr, NULL, "expeed6.sla.ram", RAM_SIZE, &error_fatal);
    memory_region_add_subregion(root, RAM_BASE, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_ram(mr, NULL, "expeed6.sla.low", 0x04000000, &error_fatal);
    memory_region_add_subregion(root, 0x0, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_ram(mr, NULL, "expeed6.sla.sram04", 0x00040000, &error_fatal);
    memory_region_add_subregion(root, 0x04000000, mr);
    AddressSpace *as = g_new(AddressSpace, 1);
    address_space_init(as, root, "expeed6.sla.as");
    expeed6_install_soc(root, as, gic, false);

    load_flash(as, flash);
    qemu_register_reset(z6ii_cpu_reset, sl);
    fprintf(stderr, "z6ii: slave cluster up from %s\n", flash);
}

static bool z6ii_virtual_timers_may_run(void)
{
    return !icount_enabled() || qemu_in_vcpu_thread() || all_cpu_threads_idle();
}

static void z6ii_pause_at_cb(void *opaque)
{
    QEMUTimer *t = opaque;
    fprintf(stderr, "z6ii: EMU_PAUSE_AT: pausing at virtual %" PRId64 " ms\n",
            qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    timer_free(t);
    vm_stop(RUN_STATE_PAUSED);
}

static void z6ii_pause_at_schedule(const char *spec)
{
    gchar **parts = g_strsplit(spec, ",", -1);
    for (int i = 0; parts[i]; i++) {
        char *end;
        long long ms = strtoll(parts[i], &end, 0);
        if (end == parts[i] || *end || ms <= 0) {
            error_report("EMU_PAUSE_AT: '%s' is not a positive number of ms", parts[i]);
            exit(1);
        }
        QEMUTimer *t = g_new0(QEMUTimer, 1);
        timer_init_ms(t, QEMU_CLOCK_VIRTUAL, z6ii_pause_at_cb, t);
        timer_mod(t, ms);
    }
    g_strfreev(parts);
}

static void z6ii_init(MachineState *machine)
{
    qemu_timer_virtual_may_run = z6ii_virtual_timers_may_run;
    if (getenv("EMU_PAUSE_AT")) {
        z6ii_pause_at_schedule(getenv("EMU_PAUSE_AT"));
    }
    Expeed6State *s = g_new0(Expeed6State, 1);
    object_property_add_str(OBJECT(machine), "exmcu-push", NULL, exmcu_push_prop_set);
    object_property_add_str(OBJECT(machine), "exmcu-schedule", NULL, exmcu_schedule_prop_set);
    object_property_add_str(OBJECT(machine), "cd-present", NULL, cd_present_prop_set);
    object_property_add_str(OBJECT(machine), "sd-card-present", NULL, sd_card_present_prop_set);
    object_property_add_str(OBJECT(machine), "cd-insert", NULL, cd_insert_prop_set);
    z6ii_keys_init();
    const bool el3 = true;
    MemoryRegion *sysmem = g_new(MemoryRegion, 1);
    memory_region_init(sysmem, NULL, "expeed6.mas", UINT64_MAX);
    AddressSpace *as = g_new(AddressSpace, 1);
    address_space_init(as, sysmem, "expeed6.mas.as");
    MemoryRegion *image = g_new(MemoryRegion, 1);
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    const char *flash = getenv("Z6II_FLASH") ? getenv("Z6II_FLASH") : "out/flash.img";

    expeed6_make_cpus(machine, s, sysmem, el3);

    z6ii_image_init(image);
    memory_region_add_subregion(sysmem, IMAGE_BASE, image);
    memory_region_init_ram(ram, NULL, "expeed6.ram", RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, RAM_BASE, ram);
    MemoryRegion *low = g_new(MemoryRegion, 1);
    memory_region_init_ram(low, NULL, "expeed6.low", 0x04000000, &error_fatal);
    memory_region_add_subregion(sysmem, 0x0, low);

    load_flash(as, flash);

    MemoryRegion *sram = g_new(MemoryRegion, 1);
    memory_region_init_ram(sram, NULL, "expeed6.sram04", 0x00040000, &error_fatal);
    memory_region_add_subregion(sysmem, 0x04000000, sram);

    MemoryRegion *sram29 = g_new(MemoryRegion, 1);
    memory_region_init_ram(sram29, NULL, "expeed6.sram0429", 0x00010000, &error_fatal);
    memory_region_add_subregion_overlap(sysmem, 0x04290000, sram29, 1);

    DeviceState *gic = expeed6_gic_new(el3, CPU(s->cpu[0])->cpu_index);
    SysBusDevice *gicsbd = SYS_BUS_DEVICE(gic);
    sysbus_realize_and_unref(gicsbd, &error_fatal);
    memory_region_add_subregion_overlap(sysmem, PERIPHBASE + 0x1000, sysbus_mmio_get_region(gicsbd, 0), 1);
    memory_region_add_subregion_overlap(sysmem, PERIPHBASE + 0x2000, sysbus_mmio_get_region(gicsbd, 1), 1);
    for (int c = 0; c < NUM_CPU; c++) {
        DeviceState *cd = DEVICE(s->cpu[c]);
        sysbus_connect_irq(gicsbd, c, qdev_get_gpio_in(cd, ARM_CPU_IRQ));
        sysbus_connect_irq(gicsbd, c + NUM_CPU, qdev_get_gpio_in(cd, ARM_CPU_FIQ));
        sysbus_connect_irq(gicsbd, c + 2 * NUM_CPU, qdev_get_gpio_in(cd, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicsbd, c + 3 * NUM_CPU, qdev_get_gpio_in(cd, ARM_CPU_VFIQ));
        static const int timer_ppi[] = {
            [GTIMER_PHYS] = 30,
            [GTIMER_VIRT] = 27,
            [GTIMER_HYP] = 26,
            [GTIMER_SEC] = 29,
        };
        int ppibase = NUM_IRQ - 32 + c * 32;
        for (int t = 0; t < ARRAY_SIZE(timer_ppi); t++) {
            qdev_connect_gpio_out(cd, t, qdev_get_gpio_in(gic, ppibase + timer_ppi[t]));
        }
    }

    expeed6_install_soc(sysmem, as, gic, true);

    MemoryRegion *ring = g_new(MemoryRegion, 1);
    memory_region_init_ram(ring, NULL, "expeed6.dualring", RING_SIZE, &error_fatal);
    MemoryRegion *ringmas = g_new(MemoryRegion, 1);
    memory_region_init_alias(ringmas, NULL, "expeed6.dualring.mas", ring, 0, RING_SIZE);
    if (!z6ii_slave_flash()) {
        memory_region_add_subregion_overlap(sysmem, RING_MAS, ringmas, 2);
    }
    z6ii_build_slave(machine, el3);

    expeed6_lli_link_clusters(sysmem);

    qemu_register_reset(z6ii_cpu_reset, s);
    expeed6_add_migration_blocker();
}

static void z6ii_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Nikon Expeed master (eg1850_mas), boots from reset";
    mc->init = z6ii_init;
    mc->reset = z6ii_machine_reset;
    mc->max_cpus = 2 * NUM_CPU;
    mc->default_cpus = 2 * NUM_CPU;
    mc->min_cpus = 2 * NUM_CPU;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    mc->ignore_memory_transaction_failures = true;
}

static const TypeInfo z6ii_machine_type = {
    .name = MACHINE_TYPE_NAME("z6ii"),
    .parent = TYPE_MACHINE,
    .class_init = z6ii_machine_class_init,
};

static void z6ii_machine_register_types(void)
{
    type_register_static(&z6ii_machine_type);
}

type_init(z6ii_machine_register_types)
