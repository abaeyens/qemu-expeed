/*
 * The Expeed 6 PCIe glue
 *
 * The DesignWare root complex with its MSI controller, and the XQD/CFexpress card behind it.
 *
 * References:
 * - Socionext SynQuacer's EDK2 platform code, edk2-platforms before aaba5807: Silicon/Socionext/
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_bus.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"

#define PCIEGLUE_BASE 0x2b200000
#define PCIEGLUE_SIZE 0x1000

#define PCIEGLUE_INTX_STS  0x24
#define PCIEGLUE_INTX_MASK (3u << 28)
#define PCIE_EP_GIC_IRQ    243

typedef struct Expeed6PcieGlue {
    Expeed6Regfile rf;
    qemu_irq ep_irq;
    int level;
} Expeed6PcieGlue;

Expeed6PcieGlue *s_slave_glue;

static uint64_t pcieglue_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6Regfile *s = opaque;
    uint32_t v = (off / 4 < s->size / 4) ? s->reg[off / 4] : 0;
    if (off == 0x00) {
        v = (v & ~0xfu) | 4;                        /* bring-up state*/
    } else if (off == 0x0c) {
        v |= 1u << 4;                               /* PHY ready */
    } else if (off == 0x58) {
        v = (v & ~(0x3fu << 16)) | (0x11u << 16);   /* LTSSM = L0 */
    } else if (off == 0x210) {
        v |= 1;                                     /* link status, 1 if XQD card inserted*/
    }
    return v;
}

static void pcieglue_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6PcieGlue *g = opaque;
    if (off == PCIEGLUE_INTX_STS) {
        g->rf.reg[off / 4] &= ~(uint32_t)val;  /* write-1-to-clear */
        qemu_set_irq(g->ep_irq, (g->rf.reg[off / 4] & PCIEGLUE_INTX_MASK) != 0);
        return;
    }
    regfile_write(&g->rf, off, val, size);
}

static const MemoryRegionOps pcieglue_ops = {
    .read = pcieglue_read,
    .write = pcieglue_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

#define PCIE_DBI_BASE        0x2a900000
#define PCIE_DBI_MSI_ADDR_LO 0x820
#define PCIE_DBI_MSI_ADDR_HI 0x824
#define PCIE_DBI_MSI_EN      0x828
#define PCIE_DBI_MSI_STS     0x830
#define PCIE_MSI_GIC_IRQ     246

typedef struct Expeed6PcieDbi {
    Expeed6Regfile rf;
    qemu_irq gic;
    MemoryRegion *msi_catch;
} Expeed6PcieDbi;


static void pcie_dbi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6PcieDbi *d = opaque;
    if (off == PCIE_DBI_MSI_STS) {
        d->rf.reg[off / 4] &= ~(uint32_t)val; /* write-1-to-clear */
        qemu_set_irq(d->gic, d->rf.reg[off / 4] != 0);
        return;
    }
    regfile_write(&d->rf, off, val, size);
    if ((off == PCIE_DBI_MSI_ADDR_LO || off == PCIE_DBI_MSI_ADDR_HI) && d->msi_catch) {
        uint64_t a = d->rf.reg[PCIE_DBI_MSI_ADDR_LO / 4] | ((uint64_t)d->rf.reg[PCIE_DBI_MSI_ADDR_HI / 4] << 32);
        memory_region_transaction_begin();
        memory_region_set_address(d->msi_catch, a);
        memory_region_set_enabled(d->msi_catch, a != 0);
        memory_region_transaction_commit();
    }
}

static const MemoryRegionOps pcie_dbi_ops = {
    .read = regfile_read,
    .write = pcie_dbi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void pcie_intx_set(Expeed6PcieGlue *g, int level)
{
    if (level && !g->level) {
        g->rf.reg[PCIEGLUE_INTX_STS / 4] |= 1u << 28;
        qemu_set_irq(g->ep_irq, 1);
    }
    g->level = level;
}

void pcie_sd_irq(void *opaque, int n, int level)
{
    pcie_intx_set(opaque, level);
}

#define TYPE_EXPEED6_PCIE_HOST "expeed6-pcie-host"
#define NPCIE_CFG_BASE         0x2a800000
#define NPCIE_CFG_SIZE         0x10000
#define NPCIE_MEM_BASE         0x2a810000
#define NPCIE_MEM_SIZE         0xf0000

typedef struct Expeed6PcieHost {
    PCIHostState parent_obj;
    MemoryRegion bus_mem;
    MemoryRegion bus_io;
    MemoryRegion cfg;
    MemoryRegion mem_win;
    AddressSpace bus_as;
    BlockBackend *blk;
    bool settling;
    uint32_t msi_held;
    QEMUTimer *msi_timer;
    MemoryRegion dma_root;
    MemoryRegion dma_sys;
    MemoryRegion msi_catch;
    AddressSpace dma_as;
    Expeed6PcieDbi *dbi;
    Expeed6PcieGlue *glue;
    const char *tag;
} Expeed6PcieHost;

static PCIDevice *npcie_find(Expeed6PcieHost *h, hwaddr off)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(h);
    return pci_find_device(phb->bus, (off >> 20) & 0xff, PCI_DEVFN((off >> 15) & 0x1f, (off >> 12) & 7));
}

static uint64_t npcie_cfg_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6PcieHost *h = opaque;
    PCIDevice *d = npcie_find(h, off);
    uint64_t v = size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1;
    if (d) {
        v = pci_host_config_read_common(d, off & 0xfff, pci_config_size(d), size);
    }
    return v;
}

static void npcie_cfg_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6PcieHost *h = opaque;
    PCIDevice *d = npcie_find(h, off);
    if (d) {
        pci_host_config_write_common(d, off & 0xfff, pci_config_size(d), val, size);
    }
}

static const MemoryRegionOps npcie_cfg_ops = {
    .read = npcie_cfg_read,
    .write = npcie_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static AddressSpace *npcie_dma_as(PCIBus *bus, void *opaque, int devfn)
{
    return &((Expeed6PcieHost *)opaque)->dma_as;
}

static const PCIIOMMUOps npcie_iommu_ops = {
    .get_address_space = npcie_dma_as,
};

static uint64_t npcie_msi_read(void *opaque, hwaddr off, unsigned size)
{
    return 0;
}

#define NPCIE_MSI_US 20

static void npcie_msi_raise(Expeed6PcieHost *h, uint32_t bits)
{
    Expeed6PcieDbi *d = h->dbi;
    bits &= d->rf.reg[PCIE_DBI_MSI_EN / 4];
    if (bits) {
        d->rf.reg[PCIE_DBI_MSI_STS / 4] |= bits;
        qemu_set_irq(d->gic, 1);
    }
}

static void npcie_msi_deliver(void *opaque)
{
    Expeed6PcieHost *h = opaque;
    uint32_t bits = h->msi_held;
    h->msi_held = 0;
    npcie_msi_raise(h, bits);
}

static void npcie_msi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6PcieHost *h = opaque;
    uint32_t bit = 1u << (val & 0x1f);
    if (h->settling) {
        h->msi_held |= bit;
        timer_mod(h->msi_timer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + NPCIE_MSI_US);
        return;
    }
    npcie_msi_raise(h, bit);
}

static const MemoryRegionOps npcie_msi_ops = {
    .read = npcie_msi_read,
    .write = npcie_msi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void npcie_set_irq(void *opaque, int irq, int level)
{
    Expeed6PcieHost *h = opaque;
    pcie_intx_set(h->glue, level);
}

static int npcie_map_irq(PCIDevice *d, int pin)
{
    return pin;
}

static const TypeInfo expeed6_pcie_host_info = {
    .name = TYPE_EXPEED6_PCIE_HOST,
    .parent = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(Expeed6PcieHost),
};

static void expeed6_pcie_host_register(void)
{
    type_register_static(&expeed6_pcie_host_info);
}
type_init(expeed6_pcie_host_register)

    static void npcie_settle(Expeed6PcieHost *h)
{
    if (!h->blk) {
        return;
    }
    h->settling = true;
    for (int i = 0; i < 64; i++) {
        blk_drain(h->blk);
        if (!aio_poll(qemu_get_aio_context(), false)) {
            break;
        }
    }
    h->settling = false;
}

static uint64_t npcie_win_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6PcieHost *h = opaque;
    uint8_t buf[8] = { 0 };
    address_space_read(&h->bus_as, NPCIE_MEM_BASE + off, MEMTXATTRS_UNSPECIFIED, buf, size);
    return ldn_le_p(buf, size);
}

static void npcie_win_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6PcieHost *h = opaque;
    uint8_t buf[8];
    stn_le_p(buf, size, val);
    address_space_write(&h->bus_as, NPCIE_MEM_BASE + off, MEMTXATTRS_UNSPECIFIED, buf, size);
    npcie_settle(h);
}

static const MemoryRegionOps npcie_win_ops = {
    .read = npcie_win_read,
    .write = npcie_win_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static Expeed6PcieHost *expeed6_pcie_host_create(MemoryRegion *root, Expeed6PcieDbi *dbi, Expeed6PcieGlue *glue,
                                                 bool master)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_PCIE_HOST);
    Expeed6PcieHost *h = (Expeed6PcieHost *)dev;
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    h->tag = master ? "M-PCIE" : "S-PCIE";
    h->dbi = dbi;
    h->glue = glue;
    memory_region_init(&h->bus_mem, OBJECT(dev), "expeed6-pcie-mem", UINT64_MAX);
    memory_region_init(&h->bus_io, OBJECT(dev), "expeed6-pcie-io", 0x10000);
    phb->bus = pci_register_root_bus(dev, master ? "expeed6-pcie-m" : "expeed6-pcie-s", npcie_set_irq,
                                     npcie_map_irq, h, &h->bus_mem, &h->bus_io, 0, 4, TYPE_PCIE_BUS);
    memory_region_init(&h->dma_root, OBJECT(dev), "expeed6-pcie-dma", UINT64_MAX);
    memory_region_init_alias(&h->dma_sys, OBJECT(dev), "expeed6-pcie-dma-sys", root, 0, memory_region_size(root));
    memory_region_add_subregion_overlap(&h->dma_root, 0, &h->dma_sys, 0);
    memory_region_init_io(&h->msi_catch, OBJECT(dev), &npcie_msi_ops, h, "expeed6-pcie-msi", 4);
    memory_region_set_enabled(&h->msi_catch, false);
    memory_region_add_subregion_overlap(&h->dma_root, 0, &h->msi_catch, 1);
    address_space_init(&h->dma_as, &h->dma_root, "expeed6-pcie-dma");
    dbi->msi_catch = &h->msi_catch;
    pci_setup_iommu(phb->bus, &npcie_iommu_ops, h);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_init_io(&h->cfg, OBJECT(dev), &npcie_cfg_ops, h, "expeed6-pcie-cfg", NPCIE_CFG_SIZE);
    memory_region_add_subregion_overlap(root, NPCIE_CFG_BASE, &h->cfg, 3);
    address_space_init(&h->bus_as, &h->bus_mem, "expeed6-pcie-bus");
    memory_region_init_io(&h->mem_win, OBJECT(dev), &npcie_win_ops, h, "expeed6-pcie-memwin", NPCIE_MEM_SIZE);
    h->mem_win.disable_reentrancy_guard = true;
    memory_region_add_subregion_overlap(root, NPCIE_MEM_BASE, &h->mem_win, 3);
    return h;
}

void expeed6_install_xqd(Expeed6PcieHost *h, const char *path)
{
    QemuOpts *o = drive_add(IF_NONE, -1, path, "format=raw,id=expeed6-xqd");
    DriveInfo *di = drive_new(o, IF_NONE, &error_fatal);
    DeviceState *nvme = qdev_new("nvme");
    qdev_prop_set_string(nvme, "serial", "Z6II-XQD");
    h->blk = blk_by_legacy_dinfo(di);
    h->msi_timer = timer_new_us(QEMU_CLOCK_VIRTUAL, npcie_msi_deliver, h);
    qdev_prop_set_drive_err(nvme, "drive", h->blk, &error_fatal);
    qdev_realize_and_unref(nvme, BUS(PCI_HOST_BRIDGE(h)->bus), &error_fatal);
}

static const VMStateDescription vmstate_pcie_glue = {
    .name = "expeed6-pcie-glue",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (const VMStateField[]){ VMSTATE_STRUCT(rf, Expeed6PcieGlue, 1, vmstate_expeed6_regfile, Expeed6Regfile),
                                VMSTATE_INT32(level, Expeed6PcieGlue), VMSTATE_END_OF_LIST() }
};

static const VMStateDescription vmstate_pcie_dbi = {
    .name = "expeed6-pcie-dbi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (const VMStateField[]){ VMSTATE_STRUCT(rf, Expeed6PcieDbi, 1, vmstate_expeed6_regfile, Expeed6Regfile),
                                VMSTATE_END_OF_LIST() }
};

Expeed6PcieDbi *expeed6_install_pcie(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master)
{
    Expeed6PcieGlue *glue = g_new0(Expeed6PcieGlue, 1);
    regfile_init(&glue->rf, PCIEGLUE_BASE, PCIEGLUE_SIZE, master ? "M-PCIEGLUE" : "S-PCIEGLUE");
    glue->ep_irq = qdev_get_gpio_in(gic, PCIE_EP_GIC_IRQ - 32);
    MemoryRegion *mr = g_new(MemoryRegion, 1);
    memory_region_init_io(mr, NULL, &pcieglue_ops, glue, "expeed6.pcie.glue", PCIEGLUE_SIZE);
    memory_region_add_subregion_overlap(root, PCIEGLUE_BASE, mr, 2);
    Expeed6PcieDbi *dbi = g_new0(Expeed6PcieDbi, 1);
    regfile_init(&dbi->rf, PCIE_DBI_BASE, 0x10000, master ? "M-PCIE-DBI" : "S-PCIE-DBI");
    dbi->gic = qdev_get_gpio_in(gic, PCIE_MSI_GIC_IRQ - 32);
    MemoryRegion *dmr = g_new(MemoryRegion, 1);
    memory_region_init_io(dmr, NULL, &pcie_dbi_ops, dbi, "expeed6.pcie.dbi", 0x10000);
    memory_region_add_subregion_overlap(root, PCIE_DBI_BASE, dmr, 1);
    vmstate_register_any(NULL, &vmstate_pcie_glue, glue);
    vmstate_register_any(NULL, &vmstate_pcie_dbi, dbi);
    if (master) {
        msi_nonbroken = true;
        expeed6_unmigrated("the XQD card (QEMU's NVMe controller cannot be migrated)");
        expeed6_install_xqd(expeed6_pcie_host_create(root, dbi, glue, true), getenv("Z6II_XQD"));
        return dbi;
    }
    Expeed6Regfile *ep = install_regfile(root, 0x2a800000, 0x100000, "S-PCIE-EP");
    {
        s_slave_glue = glue;
        regfile_set_reset(ep, 0x64, 0xc);
    }
    return dbi;
}
