#!/usr/bin/env bash
# Apply the Nikon custom machine to a QEMU 9.1.0 source tree and build qemu-system-arm
set -euo pipefail
QEMU_SRC="${1:-/opt/qemu-9.1.0}"
HERE="$(cd "$(dirname "$0")" && pwd)"

QEMU_PRISTINE="${QEMU_PRISTINE:-/opt/qemu-9.1.0}"
if [ ! -f "$QEMU_SRC/hw/arm/meson.build" ]; then
  if [ ! -f "$QEMU_PRISTINE/hw/arm/meson.build" ]; then
    echo "error: no QEMU source at $QEMU_SRC, and no pristine tree at $QEMU_PRISTINE" >&2
    echo "       set QEMU_PRISTINE=/path/to/qemu-9.1.0 (an unpacked 9.1.0 source tree)" >&2
    exit 1
  fi
  echo "seeding $QEMU_SRC from $QEMU_PRISTINE (~850 MB, one time)"
  mkdir -p "$QEMU_SRC"
  tar -C "$QEMU_PRISTINE" --exclude=./build -cf - . | tar -C "$QEMU_SRC" -xf -
fi

EG_FILES=(
  z6ii.c z6ii.h z6ii_fw.h z6ii_sensor.c z6ii_exmcu.c z6ii_nor.c
  expeed6.c expeed6.h
  expeed6_iip.c
  expeed6_jpeg.c
  expeed6_counter.c expeed6_crg.c expeed6_uart.c expeed6_sysctl.c expeed6_cd.c expeed6_hdmi.c
  expeed6_xdmac.c expeed6_dsp.c
  expeed6_extint.c expeed6_gpio.c expeed6_idmac.c expeed6_lli.c expeed6_sio.c expeed6_sio.h
  expeed6_sif.c
  expeed6_mipu.c
  expeed6_h264.c
  expeed6_gevg.c
  expeed6_disp.c
  expeed6_timer.c
  expeed6_i2c.c
  expeed6_pcie.c
  expeed6_sd.c
  expeed6_hdmac.c expeed6_hdmac.h
  expeed6_audio.c
  expeed6_ipu.c
  expeed6_mvd.c
  expeed6_dafi.c
  expeed6_raw.c
  expeed6_sflt.c
  gevg_model.c gevg_model.h
  mipu_model.c mipu_model.h mipu_regs.h
  fe_model.c fe_model.h fe_regs.h
  sensor_model.c sensor_model.h
  iip_model.c iip_model.h iip_regs.h
  nor_model.c nor_model.h
  jpeg_model.c jpeg_model.h
  nef_model.c nef_model.h
)
for f in "${EG_FILES[@]}"; do
  cp "$HERE/$f" "$QEMU_SRC/hw/arm/$f"
done

cp "$HERE/mma845x.c"          "$QEMU_SRC/hw/i2c/mma845x.c"
cp "$HERE/mma845x.h"          "$QEMU_SRC/include/hw/i2c/mma845x.h"

grep -q dist_iidr "$QEMU_SRC/include/hw/intc/arm_gic_common.h" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/gic-ident-props.patch"

grep -q eoi_idle_deactivates "$QEMU_SRC/include/hw/intc/arm_gic_common.h" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/gic-eoi-idle.patch"

grep -q first_cpu_index "$QEMU_SRC/include/hw/intc/arm_gic_common.h" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/gic-cpu-index-base.patch"

grep -q "nikon: PS <= NPSS accepted" "$QEMU_SRC/hw/nvme/ctrl.c" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/nvme-power-state.patch"

grep -q "nikon: MSI as well as MSI-X" "$QEMU_SRC/hw/nvme/ctrl.c" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/nvme-msi.patch"

grep -q qemu_timer_virtual_may_run "$QEMU_SRC/include/qemu/timer.h" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/icount-deterministic.patch"

grep -q "instructions charged per exception (default 1; 0: none)" "$QEMU_SRC/accel/tcg/cpu-exec.c" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/icount-exception-cost.patch"

grep -q "z6ii: of the CPSR, the hflags read only" "$QEMU_SRC/target/arm/tcg/op_helper.c" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/hflags-cpsr-changed.patch"

grep -q "z6ii: EMU_IDLE_WFI" "$QEMU_SRC/target/arm/tcg/translate.c" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/idle-wfi.patch"

grep -q "Only the I mask can change, in the same mode" "$QEMU_SRC/target/arm/tcg/op_helper.c" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/cpsr-write-chain.patch"

grep -q "z6ii: breakpoints are recognised when a TB is looked up" "$QEMU_SRC/accel/tcg/tcg-accel-ops.c" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/gdb-break-flush.patch"

grep -q "z6ii: 14, not 12" "$QEMU_SRC/accel/tcg/tb-jmp-cache.h" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/tb-jmp-cache-bits.patch"

grep -q "z6ii: EMU_SPIN_YIELD" "$QEMU_SRC/target/arm/tcg/translate.c" ||
  patch -d "$QEMU_SRC" -p1 < "$HERE/spin-yield.patch"

EG_C=""
for f in "${EG_FILES[@]}"; do
  case "$f" in *.c) EG_C="$EG_C${EG_C:+, }'$f'" ;; esac
done
EG_LINE="arm_ss.add(when: 'CONFIG_Z6II', if_true: [files($EG_C), jpeg])"
if ! grep -qxF "$EG_LINE" "$QEMU_SRC/hw/arm/meson.build"; then
  sed -i "/CONFIG_Z6II/d" "$QEMU_SRC/hw/arm/meson.build"
  sed -i "/arm_ss.add(when: 'CONFIG_SABRELITE'/a $EG_LINE" "$QEMU_SRC/hw/arm/meson.build"
fi

sed -i "/mma845x/d" "$QEMU_SRC/hw/i2c/meson.build"
sed -i "/CONFIG_BITBANG_I2C/a i2c_ss.add(when: 'CONFIG_MMA845X', if_true: files('mma845x.c'))" \
    "$QEMU_SRC/hw/i2c/meson.build"

kconfig_put() {
  local file="$1" name="$2"; shift 2
  awk -v n="$name" '/^config /{skip = ($2 == n)} {if (!skip) print}' "$file" > "$file.tmp"
  mv "$file.tmp" "$file"
  printf '\nconfig %s\n' "$name" >> "$file"
  printf '    %s\n' "$@" >> "$file"
}
kconfig_put "$QEMU_SRC/hw/i2c/Kconfig"  MMA845X bool "select I2C"

kconfig_put "$QEMU_SRC/hw/arm/Kconfig" Z6II \
    bool \
    "default y" \
    "depends on TCG && ARM" \
    "select UNIMP" \
    "select SDHCI" \
    "select I2C" \
    "select MMA845X" \
    "select PCI_EXPRESS" \
    "select NVME_PCI"

cd "$QEMU_SRC"

if [ -d build ] && [ ! -w build ]; then
  echo "error: $QEMU_SRC/build is not writable by $(id -un)." >&2
  echo "       The container image rebuilt it as root. Fix with:" >&2
  echo "         sudo chown -R \"$(id -un)\":\"$(id -gn)\" $QEMU_SRC" >&2
  exit 1
fi

[ -d build ] || ./configure --target-list=arm-softmmu --disable-docs --disable-werror

EG_OBJS=""
for f in "${EG_FILES[@]}"; do
  case "$f" in *.c) EG_OBJS="$EG_OBJS ${f%.c}" ;; esac
done
for obj in $EG_OBJS mma845x; do
  if [ -d build ] && ! ninja -C build -t targets all 2>/dev/null | grep -q "$obj\.c\.o"; then
    meson setup --reconfigure build
    break
  fi
done

ninja -C build qemu-system-arm
echo "built: $QEMU_SRC/build/qemu-system-arm  (machine: -M z6ii)"
