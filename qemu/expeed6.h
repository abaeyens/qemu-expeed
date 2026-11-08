/*
 * The Expeed 6 SoC
 *
 * What the Expeed 6 SoC its block files share: the includes they need,
 * the machine's diagnostics, and each block's constructor and cross-block calls
 * (one file per block, expeed6_<block>.c; the SoC itself is expeed6.c,
 * the camera's board with two of them z6ii.c).
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef EXPEED6_H
#define EXPEED6_H

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "sysemu/runstate.h"
#include "ui/console.h"
#include <math.h>

#include "hw/boards.h"
#include "target/arm/cpu.h"

#define PERIPHBASE    0x04300000ULL
#define GICD_IIDR_VAL 0x0100143bU
#define GICD_LSPI     31
#define NUM_IRQ       256
#define CNTFRQ_HZ     48000000ULL

#define FW_ENTRY 0x10020000
#define NUM_CPU  2

typedef struct Expeed6State {
    ARMCPU *cpu[NUM_CPU];
    int64_t sec_timer_ns[NUM_CPU], pmu_timer_ns[NUM_CPU];
} Expeed6State;

void expeed6_make_cpus(MachineState *machine, Expeed6State *st, MemoryRegion *root, bool el3);
DeviceState *expeed6_gic_new(bool el3, int cpu_base);
void expeed6_install_soc(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master);
void z6ii_cpu_reset(void *opaque);
extern MemoryRegion *s_slave_root;

uint32_t guest_pc(void);
const char *cluster_tag(void);

bool expeed6_fw_peek(AddressSpace *as, uint32_t addr, void *buf, size_t len);
bool expeed6_dma_guard_ok(uint64_t addr, size_t len, const char *who);
void mipu_mem_read(void *opaque, uint64_t addr, void *buf, size_t len);
void mipu_mem_write(void *opaque, uint64_t addr, const void *buf, size_t len);

typedef struct {
    const uint8_t *y, *c;
    uint32_t w, h;
    uint32_t gen;
} Expeed6LvDevelop;
bool expeed6_mipu_lv_develop(Expeed6LvDevelop *d);
bool expeed6_h264_is_input_plane(uint32_t addr);

uint64_t allones_read(void *opaque, hwaddr offset, unsigned size);
void allones_write(void *opaque, hwaddr offset, uint64_t value, unsigned size);

void expeed6_unmigrated(const char *what);
void expeed6_add_migration_blocker(void);
void expeed6_map_mmio(MemoryRegion *root, SysBusDevice *sbd, int n, hwaddr base);

typedef struct {
    uint32_t base, size;
    uint32_t *reg;
    uint32_t nregs;
    uint32_t *reset_val;
    const char *tag;
} Expeed6Regfile;
uint64_t regfile_read(void *opaque, hwaddr off, unsigned size);
void regfile_write(void *opaque, hwaddr off, uint64_t val, unsigned size);
void regfile_init(Expeed6Regfile *rf, uint32_t base, uint32_t size, const char *tag);
void regfile_set_reset(Expeed6Regfile *rf, uint32_t off, uint32_t val);
extern const VMStateDescription vmstate_expeed6_regfile;
Expeed6Regfile *install_regfile(MemoryRegion *root, uint32_t base, uint32_t size, const char *tag);

typedef struct Expeed6MipuState Expeed6MipuState;
typedef struct Expeed6DmaState Expeed6DmaState;
typedef struct Expeed6ExtIntState Expeed6ExtIntState;
typedef struct Expeed6GpioState Expeed6GpioState;
typedef struct Expeed6LliState Expeed6LliState;
typedef struct Z6iiExmcuState Z6iiExmcuState;

/* expeed6_jpeg.c: the JPEG codec (GIC 105) */
void expeed6_install_jpegco(MemoryRegion *root, AddressSpace *as, DeviceState *gic);

/* expeed6_sflt.c: the SFLT stage block (GIC 124) */
void expeed6_install_sflt(MemoryRegion *root, DeviceState *gic, AddressSpace *as);

/* expeed6_raw.c: the RAW stage block (GIC 121) */
void expeed6_install_rawb(MemoryRegion *root, AddressSpace *as, DeviceState *gic);

/* expeed6_dafi.c: the DAFI stage block (GIC 123) */
void expeed6_install_dafi(MemoryRegion *root, DeviceState *gic);

/* expeed6_mvd.c: the MVD stage block (GIC 116) */
void expeed6_install_mvd(MemoryRegion *root, DeviceState *gic);

/* expeed6_ipu.c: the IPU mailbox (GIC 101) */
void expeed6_install_ipu_mbx(MemoryRegion *root, DeviceState *gic);

/* expeed6_hdmac.c: the HDMAC (GIC 142-157) */
void expeed6_install_hdmac(MemoryRegion *root, AddressSpace *as, DeviceState *gic);
bool hdmac_sio_exchange(unsigned ch);

/* expeed6_audio.c: the audio interface (GIC 167/168) */
#define AUDIO_CODEC_CH 6
typedef struct Expeed6Audio Expeed6Audio;
extern Expeed6Audio *s_audio;
void expeed6_install_audio(MemoryRegion *root);
void audio_codec_tx(Expeed6Audio *a, uint16_t w, bool rx_inhibit);
uint16_t audio_codec_rx(Expeed6Audio *a);

/* expeed6_sd.c: the SD_0 host (GIC 230) */
void expeed6_install_sd0(MemoryRegion *root, DeviceState *gic, bool master);

/* expeed6_pcie.c: the PCIe root complex and the XQD card */
typedef struct Expeed6PcieGlue Expeed6PcieGlue;
typedef struct Expeed6PcieDbi Expeed6PcieDbi;
typedef struct Expeed6PcieHost Expeed6PcieHost;
extern Expeed6PcieGlue *s_slave_glue;
#define PCIE_SD_BAR0 0x2a810000
void pcie_sd_irq(void *opaque, int n, int level);
Expeed6PcieDbi *expeed6_install_pcie(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master);
void expeed6_install_xqd(Expeed6PcieHost *h, const char *path);

/* expeed6_i2c.c: the I2C controllers (GIC 214-217), the MMA8452Q on channel 2 */
void expeed6_install_i2c(MemoryRegion *root, DeviceState *gic);

/* expeed6_disp.c: the display engine's register file, frame interrupt and scanout */
void expeed6_install_disp(MemoryRegion *root, AddressSpace *as, DeviceState *gic);

/* expeed6_timer.c: TIMER_0..11 */
void expeed6_install_soctimer(MemoryRegion *root, DeviceState *gic);

/* expeed6_h264.c: the H.264 encoder (GIC 110) */
void expeed6_install_h264(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master);

/* expeed6_gevg.c: the vector-graphics engine (GIC 129-131) */
void expeed6_install_gevg(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master);

/* expeed6_mipu.c: the MIPU (GIC 99/100) */
typedef struct Expeed6MipuState Expeed6MipuState;
Expeed6MipuState *expeed6_install_mipu(MemoryRegion *root, AddressSpace *as, bool master);
void expeed6_mipu_connect(Expeed6MipuState *s, DeviceState *gic);
/* the front end's DGAIN0..3 as fe_model gains (expeed6_mipu.c) */
void fe_dgains(const uint32_t *fe, float *g);
uint64_t fe_hash(uint64_t h, const void *p, size_t n);
void expeed6_mipu_connect_fe(Expeed6MipuState *s, const uint32_t *fe_regs, void (*refresh)(void *, uint64_t),
                             void *opaque);


/* expeed6_sif.c: the sensor interface, PPC and front end (SIF_0/1/6 = GIC 86/87/92, PPC_0..4 = GIC 94-98) */
#define FE_OFF 0x8000
void expeed6_install_sif(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master, Expeed6DmaState *dma,
                         Expeed6MipuState *mipu);
int64_t expeed6_fe_frame_ns(AddressSpace *as, int64_t fallback_ns);

/* expeed6_counter.c: the free-running counter */
void expeed6_install_counter(MemoryRegion *root);

/* expeed6_crg.c: the clock-and-reset generator and the event FIFO (GIC 250) */
void expeed6_install_crg(MemoryRegion *root, DeviceState *gic);

/* expeed6_uart.c: UART_0, the serial console (EXPEED_UART) */
void expeed6_install_uart0(MemoryRegion *root, bool master);

/* expeed6_sysctl.c: the clock-gate array */
void expeed6_install_sysctl(MemoryRegion *root);

/* expeed6_cd.c: card detect (GIC 237) */
void expeed6_install_cd(MemoryRegion *root, DeviceState *gic, bool master);
void cd_present_prop_set(Object *obj, const char *value, Error **errp);
void sd_card_present_prop_set(Object *obj, const char *value, Error **errp);
void cd_insert_prop_set(Object *obj, const char *value, Error **errp);

/* expeed6_hdmi.c: the HDMI hot-plug detect */
void expeed6_install_hdmi(MemoryRegion *root);

/* expeed6_xdmac.c: the FASP XDMAC (GIC 45-48) */
void expeed6_install_xdmac(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master);

/* expeed6_dsp.c: the core B IPCU peer */
void expeed6_install_dsp_peers(MemoryRegion *root, DeviceState *gic);

/* expeed6_ipu.c, also: the IPU engine, idle */
void expeed6_install_ipu_engine(MemoryRegion *root);

/* expeed6_extint.c: the external-interrupt controller */
typedef struct Expeed6ExtIntState Expeed6ExtIntState;
Expeed6ExtIntState *expeed6_install_extint(MemoryRegion *root, DeviceState *gic);
#define EXTINT_N 16
void extint_raise(Expeed6ExtIntState *s, int idx);
void gpio_echo_fire_extint(void *opaque, int src);
void extint_input_edge(Expeed6ExtIntState *s, int idx, bool level);

/* expeed6_gpio.c: the GPIO block; each cluster's, for the keys, the LLI handshake pin and the SD socket */
typedef struct Expeed6GpioState Expeed6GpioState;
extern Expeed6GpioState *s_master_gpio, *s_slave_gpio;
Expeed6GpioState *expeed6_install_gpio(MemoryRegion *root, bool master, Expeed6ExtIntState *extint);
void gpio_set_input(Expeed6GpioState *s, unsigned off, unsigned bit, bool high);
void gpio_drive_input(Expeed6GpioState *s, unsigned off, unsigned bit, bool high);
void gpio_set_sd_card(Expeed6GpioState *s, bool present);

/* expeed6_idmac.c: the IDMAC (GIC 103/104) */
typedef struct Expeed6DmaState Expeed6DmaState;
extern Expeed6DmaState *s_master_idmac, *s_slave_idmac;
Expeed6DmaState *expeed6_install_idmac(MemoryRegion *root, AddressSpace *as, DeviceState *gic, bool master);
AddressSpace *expeed6_idmac_as(Expeed6DmaState *d);
void expeed6_idmac_set_as(Expeed6DmaState *d, AddressSpace *as);

/* expeed6_lli.c: the DualEngine/LLI link (GIC 127) */
typedef struct Expeed6LliState Expeed6LliState;
extern Expeed6LliState *s_master_lli, *s_slave_lli;
Expeed6LliState *expeed6_install_lli(MemoryRegion *root, DeviceState *gic, bool master);
void expeed6_lli_connect_gpio(Expeed6LliState *lli, Expeed6GpioState *gpio);
void expeed6_lli_link_clusters(MemoryRegion *sysmem);

/* expeed6_sio.c: the SIO block and the Ex MCU link's peer stand-in (SIO_n = SPI 160 + 2n RX, 161 + 2n TX) */
typedef struct Z6iiExmcuState Z6iiExmcuState;
extern Z6iiExmcuState *s_exmcu;
void expeed6_install_sio(MemoryRegion *root, AddressSpace *as, DeviceState *gic, Expeed6ExtIntState *extint);

/* expeed6_iip.c: the IIP (GIC 102) */
#define EXPEED_IIP_SPI 70
typedef struct Expeed6IipState Expeed6IipState;
Expeed6IipState *expeed6_iip_new(MemoryRegion *root, AddressSpace *as, bool master);
void expeed6_iip_connect(Expeed6IipState *s, qemu_irq irq, bool master);

#endif /* EXPEED6_H */
