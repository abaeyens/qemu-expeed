/*
 * The Expeed 6 SoC cluster
 *
 * Its two Cortex-A7 cores and GIC-400, the stall-breaker's all-ones windows
 * over the unmodelled peripherals, the models' shared diagnostics,
 * and the installer that puts every block's model
 * into the cluster's address space (one file per block, expeed6_<block>.c).
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "expeed6_hdmac.h"
#include "hw/qdev-properties.h"
#include "sysemu/reset.h"
#include "migration/blocker.h"

void z6ii_cpu_reset(void *opaque)
{
    Expeed6State *s = opaque;

    for (int c = 0; c < NUM_CPU; c++) {
        cpu_reset(CPU(s->cpu[c]));
        s->cpu[c]->env.regs[15] = FW_ENTRY;
    }
}

uint32_t guest_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

MemoryRegion *s_slave_root;

const char *cluster_tag(void)
{
    return current_cpu && s_slave_root && current_cpu->memory == s_slave_root ? ".sla" : "";
}

#define RTOS_MEM_LO 0xc1800000u
#define RTOS_MEM_HI 0xc6100000u

bool expeed6_fw_peek(AddressSpace *as, uint32_t addr, void *buf, size_t len)
{
    if (address_space_read(as, addr, MEMTXATTRS_UNSPECIFIED, buf, len) != MEMTX_OK) {
        memset(buf, 0, len);
        return false;
    }
    return true;
}

bool expeed6_dma_guard_ok(uint64_t addr, size_t len, const char *who)
{
    static GHashTable *warned;

    if (addr >= RTOS_MEM_HI || addr + len <= RTOS_MEM_LO) {
        return true;
    }
    if (!warned) {
        warned = g_hash_table_new(g_str_hash, g_str_equal);
    }
    if (!g_hash_table_contains(warned, who)) {
        g_hash_table_add(warned, (gpointer)who);
        warn_report("z6ii: %s writes %zu bytes at %#" PRIx64 " at %.3f s, inside the RTOS's own memory "
                    "(0x%08x-0x%08x): a buffer the firmware did not hand out? refused (first of this "
                    "model's, not repeated)",
                    who, len, addr, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, RTOS_MEM_LO, RTOS_MEM_HI);
    }
    return false;
}

void mipu_mem_read(void *opaque, uint64_t addr, void *buf, size_t len)
{
    address_space_read(opaque, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

void mipu_mem_write(void *opaque, uint64_t addr, const void *buf, size_t len)
{
    if (expeed6_dma_guard_ok(addr, len, "the MIPU/NEF model")) {
        address_space_write(opaque, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
    }
}

#define STALL_N 64

static GHashTable *stall_tbl(void)
{
    static GHashTable *t;
    if (!t) {
        t = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    return t;
}

#define STALL_REPORTED 0x80000000u

static inline int expeed6_side(void)
{
    return (current_cpu && current_cpu->cpu_index >= NUM_CPU) ? 1 : 0;
}

static inline gpointer stall_key(uint64_t addr)
{
    return (gpointer)(uintptr_t)(addr | ((uint64_t)expeed6_side() << 40));
}

uint64_t allones_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t addr = (uint64_t)(uintptr_t)opaque + offset;
    uint64_t all = (size >= 8) ? ~0ULL : ((1ULL << (size * 8)) - 1);

    gpointer key = stall_key(addr);
    uint32_t st = GPOINTER_TO_UINT(g_hash_table_lookup(stall_tbl(), key));
    uint32_t cnt = (st & ~STALL_REPORTED) + 1;
    if (cnt >= STALL_N && !(st & STALL_REPORTED)) {
        fprintf(stderr,
                "z6ii: MMIO %08" PRIx64 " read %d times without a write "
                "(pc=%08x, %s) — stall-breaker engaged; this register needs a model\n",
                addr, (int)cnt, guest_pc(), expeed6_side() ? "slave" : "master");
        st |= STALL_REPORTED;
    }
    g_hash_table_insert(stall_tbl(), key, GUINT_TO_POINTER(cnt | (st & STALL_REPORTED)));

    return ((cnt - 1) / STALL_N) & 1 ? 0 : all;
}

void allones_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    uint64_t addr = (uint64_t)(uintptr_t)opaque + offset;
    gpointer key = stall_key(addr);
    uint32_t st = GPOINTER_TO_UINT(g_hash_table_lookup(stall_tbl(), key));
    g_hash_table_insert(stall_tbl(), key, GUINT_TO_POINTER(st & STALL_REPORTED));
}

static const MemoryRegionOps allones_ops = {
    .read = allones_read,
    .write = allones_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

static GPtrArray *s_unmigrated;
static Error *s_migration_blocker;

void expeed6_unmigrated(const char *what)
{
    if (!s_unmigrated) {
        s_unmigrated = g_ptr_array_new();
    }
    for (guint i = 0; i < s_unmigrated->len; i++) {
        if (!strcmp(g_ptr_array_index(s_unmigrated, i), what)) {
            return;
        }
    }
    g_ptr_array_add(s_unmigrated, (gpointer)what);
}

void expeed6_add_migration_blocker(void)
{
    if (!s_unmigrated || !s_unmigrated->len) {
        return;
    }
    g_autoptr(GString) names = g_string_new(NULL);
    for (guint i = 0; i < s_unmigrated->len; i++) {
        g_string_append_printf(names, "%s%s", i ? ", " : "", (const char *)g_ptr_array_index(s_unmigrated, i));
    }
    error_setg(&s_migration_blocker, "z6ii: no saved state for %s", names->str);
    migrate_add_blocker(&s_migration_blocker, &error_fatal);
}

void expeed6_map_mmio(MemoryRegion *root, SysBusDevice *sbd, int n, hwaddr base)
{
    memory_region_add_subregion_overlap(root, base, sysbus_mmio_get_region(sbd, n), 1);
}

static struct {
    uint32_t n;
    uint64_t *keys;
    uint32_t *vals;
} s_stall_save;

static int stall_pre_save(void *opaque)
{
    GHashTableIter it;
    gpointer k, v;
    uint32_t i = 0;

    s_stall_save.n = g_hash_table_size(stall_tbl());
    g_free(s_stall_save.keys);
    g_free(s_stall_save.vals);
    s_stall_save.keys = g_new(uint64_t, s_stall_save.n);
    s_stall_save.vals = g_new(uint32_t, s_stall_save.n);
    g_hash_table_iter_init(&it, stall_tbl());
    while (g_hash_table_iter_next(&it, &k, &v)) {
        s_stall_save.keys[i] = (uint64_t)(uintptr_t)k;
        s_stall_save.vals[i] = GPOINTER_TO_UINT(v);
        i++;
    }
    return 0;
}

static int stall_post_load(void *opaque, int version_id)
{
    g_hash_table_remove_all(stall_tbl());
    for (uint32_t i = 0; i < s_stall_save.n; i++) {
        g_hash_table_insert(stall_tbl(), (gpointer)(uintptr_t)s_stall_save.keys[i],
                            GUINT_TO_POINTER(s_stall_save.vals[i]));
    }
    return 0;
}

static const VMStateDescription vmstate_stall = {
    .name = "expeed6-stall-breaker",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = stall_pre_save,
    .post_load = stall_post_load,
    .fields =
        (const VMStateField[]){
            VMSTATE_UINT32(n, typeof(s_stall_save)),
            VMSTATE_VARRAY_UINT32_ALLOC(keys, typeof(s_stall_save), n, 0, vmstate_info_uint64, uint64_t),
            VMSTATE_VARRAY_UINT32_ALLOC(vals, typeof(s_stall_save), n, 0, vmstate_info_uint32, uint32_t),
            VMSTATE_END_OF_LIST() }
};

static void stall_reset(void *opaque)
{
    g_hash_table_remove_all(stall_tbl());
}

static void map_allones(MemoryRegion *sysmem, const char *name, hwaddr base, uint64_t size)
{
    static bool registered;
    if (!registered) {
        registered = true;
        vmstate_register_any(NULL, &vmstate_stall, &s_stall_save);
        qemu_register_reset(stall_reset, NULL);
    }
    MemoryRegion *mr = g_new(MemoryRegion, 1);
    memory_region_init_io(mr, NULL, &allones_ops, (void *)(uintptr_t)base, name, size);
    memory_region_add_subregion(sysmem, base, mr);
}

uint64_t regfile_read(void *opaque, hwaddr off, unsigned size)
{
    Expeed6Regfile *s = opaque;
    uint64_t v = (off / 4 < s->size / 4) ? s->reg[off / 4] : 0;
    return v;
}

void regfile_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Expeed6Regfile *s = opaque;
    if (off / 4 < s->size / 4) {
        s->reg[off / 4] = (uint32_t)val;
    }
}

static const MemoryRegionOps regfile_ops = {
    .read = regfile_read,
    .write = regfile_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void regfile_init(Expeed6Regfile *rf, uint32_t base, uint32_t size, const char *tag)
{
    rf->base = base;
    rf->size = size;
    rf->nregs = size / 4;
    rf->reg = g_new0(uint32_t, rf->nregs);
    rf->tag = tag;
}

void regfile_set_reset(Expeed6Regfile *rf, uint32_t off, uint32_t val)
{
    assert(off / 4 < rf->nregs);
    if (!rf->reset_val) {
        rf->reset_val = g_new0(uint32_t, rf->nregs);
    }
    rf->reset_val[off / 4] = val;
    rf->reg[off / 4] = val;
}

const VMStateDescription vmstate_expeed6_regfile = { .name = "expeed6-regfile-state",
                                                     .version_id = 1,
                                                     .minimum_version_id = 1,
                                                     .fields = (const VMStateField[]){
                                                         VMSTATE_VARRAY_UINT32(reg, Expeed6Regfile, nregs, 1,
                                                                               vmstate_info_uint32, uint32_t),
                                                         VMSTATE_END_OF_LIST() } };

#define TYPE_EXPEED6_REGFILE "expeed6-regfile"
OBJECT_DECLARE_SIMPLE_TYPE(Expeed6RegfileState, EXPEED6_REGFILE)

struct Expeed6RegfileState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    Expeed6Regfile rf;
    uint32_t base, size;
    char *tag;
};

static void regfile_realize(DeviceState *dev, Error **errp)
{
    Expeed6RegfileState *s = EXPEED6_REGFILE(dev);

    if (!s->size || s->size % 4 || !s->tag) {
        error_setg(errp, "expeed6-regfile: size must be a non-zero multiple of 4, and tag set");
        return;
    }
    regfile_init(&s->rf, s->base, s->size, s->tag);
    memory_region_init_io(&s->mmio, OBJECT(dev), &regfile_ops, &s->rf, s->tag, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void regfile_reset_enter(Object *obj, ResetType type)
{
    Expeed6RegfileState *s = EXPEED6_REGFILE(obj);

    if (s->rf.reset_val) {
        memcpy(s->rf.reg, s->rf.reset_val, s->rf.size);
    } else {
        memset(s->rf.reg, 0, s->rf.size);
    }
}

static const VMStateDescription vmstate_regfile = { .name = TYPE_EXPEED6_REGFILE,
                                                    .version_id = 1,
                                                    .minimum_version_id = 1,
                                                    .fields = (const VMStateField[]){
                                                        VMSTATE_STRUCT(rf, Expeed6RegfileState, 1,
                                                                       vmstate_expeed6_regfile, Expeed6Regfile),
                                                        VMSTATE_END_OF_LIST() } };

static Property regfile_properties[] = {
    DEFINE_PROP_UINT32("base", Expeed6RegfileState, base, 0),
    DEFINE_PROP_UINT32("size", Expeed6RegfileState, size, 0),
    DEFINE_PROP_STRING("tag", Expeed6RegfileState, tag),
    DEFINE_PROP_END_OF_LIST(),
};

static void regfile_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = regfile_realize;
    dc->vmsd = &vmstate_regfile;
    device_class_set_props(dc, regfile_properties);
    rc->phases.enter = regfile_reset_enter;
}

static const TypeInfo regfile_info = {
    .name = TYPE_EXPEED6_REGFILE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Expeed6RegfileState),
    .class_init = regfile_class_init,
};

static void regfile_register_types(void)
{
    type_register_static(&regfile_info);
}

type_init(regfile_register_types)

Expeed6Regfile *install_regfile(MemoryRegion *root, uint32_t base, uint32_t size, const char *tag)
{
    DeviceState *dev = qdev_new(TYPE_EXPEED6_REGFILE);
    qdev_prop_set_uint32(dev, "base", base);
    qdev_prop_set_uint32(dev, "size", size);
    qdev_prop_set_string(dev, "tag", tag);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    expeed6_map_mmio(root, SYS_BUS_DEVICE(dev), 0, base);
    return &EXPEED6_REGFILE(dev)->rf;
}

static int expeed6_cpu_extra_pre_save(void *opaque)
{
    Expeed6State *st = opaque;

    for (int c = 0; c < NUM_CPU; c++) {
        ARMCPU *cpu = st->cpu[c];
        st->sec_timer_ns[c] = timer_expire_time_ns(cpu->gt_timer[GTIMER_SEC]);
        st->pmu_timer_ns[c] = cpu->pmu_timer ? timer_expire_time_ns(cpu->pmu_timer) : -1;
    }
    return 0;
}

static int expeed6_cpu_extra_post_load(void *opaque, int version_id)
{
    Expeed6State *st = opaque;

    for (int c = 0; c < NUM_CPU; c++) {
        ARMCPU *cpu = st->cpu[c];
        QEMUTimer *t[2] = { cpu->gt_timer[GTIMER_SEC], cpu->pmu_timer };
        int64_t ns[2] = { st->sec_timer_ns[c], st->pmu_timer_ns[c] };
        for (int i = 0; i < 2; i++) {
            if (!t[i]) {
                continue;
            } else if (ns[i] >= 0) {
                timer_mod_ns(t[i], ns[i]);
            } else {
                timer_del(t[i]);
            }
        }
    }
    return 0;
}

static const VMStateDescription vmstate_expeed6_cpu_extra = {
    .name = "expeed6-cpu-extra",
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_save = expeed6_cpu_extra_pre_save,
    .post_load = expeed6_cpu_extra_post_load,
    .fields =
        (const VMStateField[]){ VMSTATE_INT64_ARRAY(sec_timer_ns, Expeed6State, NUM_CPU),
                                VMSTATE_INT64_ARRAY(pmu_timer_ns, Expeed6State, NUM_CPU), VMSTATE_END_OF_LIST() }
};

void expeed6_make_cpus(MachineState *machine, Expeed6State *st, MemoryRegion *root, bool el3)
{
    for (int c = 0; c < NUM_CPU; c++) {
        Object *cpuobj = object_new(machine->cpu_type);
        object_property_set_bool(cpuobj, "has_el3", el3, &error_abort);
        if (object_property_find(cpuobj, "has_el2")) {
            object_property_set_bool(cpuobj, "has_el2", false, &error_abort);
        }
        object_property_set_int(cpuobj, "cntfrq", CNTFRQ_HZ, &error_abort);
        if (object_property_find(cpuobj, "reset-cbar")) {
            object_property_set_int(cpuobj, "reset-cbar", PERIPHBASE, &error_abort);
        }
        object_property_set_bool(cpuobj, "start-powered-off", false, &error_abort);
        object_property_set_int(cpuobj, "mp-affinity", c, &error_abort);
        if (root) {
            object_property_set_link(cpuobj, "memory", OBJECT(root), &error_abort);
        }
        qdev_realize(DEVICE(cpuobj), NULL, &error_abort);
        st->cpu[c] = ARM_CPU(cpuobj);
    }
    vmstate_register_any(NULL, &vmstate_expeed6_cpu_extra, st);
}

void expeed6_install_soc(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master)
{
    map_allones(root, "expeed6.periph04", 0x04040000, 0x00FC0000);
    map_allones(root, "expeed6.periph20", 0x20000000, 0x05000000);

    expeed6_install_counter(root);

    map_allones(root, "expeed6.periph2c", 0x2C000000, 0x01000000);

    map_allones(root, "expeed6.periph25", 0x25000000, 0x07000000);

    expeed6_install_crg(root, gic);

    Expeed6IipState *iip = expeed6_iip_new(root, as, master);

    Expeed6LliState *lli = expeed6_install_lli(root, gic, master);

    expeed6_install_uart0(root, master);

    expeed6_install_sysctl(root);

    expeed6_install_cd(root, gic, master);

    expeed6_install_hdmi(root);

    expeed6_install_xdmac(root, as, gic, master);

    expeed6_install_dsp_peers(root, gic);

    Expeed6ExtIntState *extint = expeed6_install_extint(root, gic);

    Expeed6MipuState *mipu = expeed6_install_mipu(root, as, master);
    expeed6_mipu_connect(mipu, gic);

    expeed6_install_ipu_engine(root);

    expeed6_install_jpegco(root, as, gic);
    expeed6_install_rawb(root, as, gic);
    if (master) {
        expeed6_install_dafi(root, gic);
        expeed6_install_sflt(root, gic, as);
    }
    expeed6_install_mvd(root, gic);
    if (master) {
        expeed6_install_ipu_mbx(root, gic);
    }

    if (master) {
        expeed6_install_disp(root, as, gic);
        expeed6_install_soctimer(root, gic);
        expeed6_install_i2c(root, gic);
    }

    Expeed6GpioState *gpio = expeed6_install_gpio(root, master, extint);
    expeed6_lli_connect_gpio(lli, gpio);

    Expeed6DmaState *dma = expeed6_install_idmac(root, as, gic, master);

    if (!master || getenv("Z6II_XQD") != NULL) {
        expeed6_install_pcie(root, as, gic, master);
    }
    expeed6_install_sd0(root, gic, master);

    if (master) {
        expeed6_install_hdmac(root, as, gic);
        expeed6_install_audio(root);

        expeed6_install_sio(root, as, gic, extint);
    }

    expeed6_iip_connect(iip, qdev_get_gpio_in(gic, EXPEED_IIP_SPI), master);

    expeed6_install_sif(root, as, gic, master, dma, mipu);

    expeed6_install_h264(root, as, gic, master);
    expeed6_install_gevg(root, as, gic, master);
}

DeviceState *expeed6_gic_new(bool el3, int cpu_base)
{
    DeviceState *gic = qdev_new("arm_gic");

    qdev_prop_set_uint32(gic, "num-cpu", NUM_CPU);
    qdev_prop_set_uint32(gic, "num-irq", NUM_IRQ);
    qdev_prop_set_uint32(gic, "revision", 2);
    qdev_prop_set_bit(gic, "has-security-extensions", el3);
    qdev_prop_set_uint32(gic, "dist-iidr", GICD_IIDR_VAL);
    qdev_prop_set_uint32(gic, "lspi", GICD_LSPI);
    qdev_prop_set_bit(gic, "eoi-idle-deactivates", true);
    qdev_prop_set_uint32(gic, "first-cpu-index", cpu_base);
    return gic;
}
