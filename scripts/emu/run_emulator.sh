#!/usr/bin/env bash
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Boot the emulator, ready for manual interaction. Usage:
#
#   scripts/emu/run_emulator.sh [gtk|vnc=:N|none] [OUTDIR] [drive_poweron.py args...]
#
# Environment variables:
#   NEF            the raw picture the sensor sees
#   SETTINGS_DUMP  write the user settings region from this settings-store dump
#   SETTINGS       with SETTINGS_DUMP: KEY=VALUE settings stored over it, space-separated

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DISPLAY_ARG="${1:-gtk}"
OUT="${2:-$ROOT/out/run_emulator}"
shift $(( $# > 2 ? 2 : $# ))
mkdir -p "$OUT"
cd "$ROOT"

# The status fields the mock Ex MCU reports in every status block
STATUS_FIELDS=(0x39=01)
DEFAULT_DUMP=out/default_settings_store.bin
CLOCK_SETTINGS="0x3700=hex:ea0701010c0000000000 0x3701=3 0x0508=0"
export SETTINGS_DUMP="${SETTINGS_DUMP-$DEFAULT_DUMP}"
if [ -n "$SETTINGS_DUMP" ]; then
    if [ ! -f "$SETTINGS_DUMP" ]; then
        echo "run_emulator.sh: no settings dump at $SETTINGS_DUMP" >&2
        if [ "$SETTINGS_DUMP" = "$DEFAULT_DUMP" ]; then
            echo "  make it once with: scripts/emu/default_settings.sh" >&2
        fi
        echo "  (or SETTINGS_DUMP= to boot with the firmware's default settings)" >&2
        exit 1
    fi
    SETTINGS="${SETTINGS-$CLOCK_SETTINGS 0x1701=3 0x057a=1 0x0205=0xa8 0x0208=0xe0 0x020c=0xa8 0x0c08=0xe0 0x0c07=0 0x0525=6 0x050f=3 0x0c02=5 0x4805=5 0x0303=1 0x0306=1}"
    STATUS_FIELDS+=(0x0f=03)
fi
FIELD_ARGS=()
STATUS_ARGS=()
for f in "${STATUS_FIELDS[@]}"; do FIELD_ARGS+=(--field "$f"); STATUS_ARGS+=(--status-field "$f"); done
python3 "$HERE/exmcu_replies.py" "$OUT/exmcu" "${FIELD_ARGS[@]}"
export Z6II_EXMCU_REPLY_DIR="${Z6II_EXMCU_REPLY_DIR:-$OUT/exmcu}"

# The flash image the machine boots, assembled for this session:
# the firmware image, the parameter banks and, with SETTINGS_DUMP, the user settings region
if [ ! -f out/eg1850_mas_01700 ]; then
    echo "run_emulator.sh: no firmware image at out/eg1850_mas_01700 -- run scripts/unpack_fw_update.py Z6_2_0170.bin" >&2
    exit 1
fi
FLASH_ARGS=(--image out/eg1850_mas_01700 -o "$OUT/flash.img")
if [ -n "$SETTINGS_DUMP" ]; then
    FLASH_ARGS+=(--settings-dump "$SETTINGS_DUMP")
    for kv in ${SETTINGS:-}; do FLASH_ARGS+=(--setting "$kv"); done
fi
python3 scripts/make_flash_image.py "${FLASH_ARGS[@]}" >/dev/null
export Z6II_FLASH="$OUT/flash.img"

# The scene the front end makes every raw frame from
NEF="${NEF:-sample_pictures/canoe.NEF}"
if [ ! -f "$NEF" ]; then
    echo "run_emulator.sh: no picture at NEF=$NEF -- fetch the sample with sample_pictures/fetch_pictures.sh" >&2
    exit 1
fi
SCENE="out/scene_$(basename "$NEF" .NEF).bin"
if [ ! -f "$SCENE" ]; then
    python3 scripts/fe_raw.py --scene "$NEF" "$SCENE.tmp" >/dev/null && mv "$SCENE.tmp" "$SCENE"
fi
export EXPEED_SCENE="$SCENE"
Z6II_SENSOR_EXPOSURE="$(exiftool -s3 -ExposureTime -ISO "$NEF" |
    python3 -c 'import sys; from fractions import Fraction as F; t, iso = sys.stdin.read().split(); print(f"{float(F(t)):g},{iso}")')"
export Z6II_SENSOR_EXPOSURE

VTIME_DEFAULT=3
SLAVE_DEFAULT=1
[ "${REC:-0}" = 1 ] && SLAVE_DEFAULT=0
if [ "${SLAVE:-$SLAVE_DEFAULT}" = 1 ]; then
    if [ ! -f out/eg1850_sla_01700 ]; then
        echo "run_emulator.sh: no slave image at out/eg1850_sla_01700 -- run scripts/unpack_fw_update.py Z6_2_0170.bin" >&2
        echo "  (or SLAVE=0)" >&2
        exit 1
    fi
    python3 scripts/make_flash_image.py --image out/eg1850_sla_01700 --banks none -o "$OUT/flash_sla.img" >/dev/null
    # with the slave, the machine also runs the DualEngine data link,
    # the slave's PCIe link to the SD host and its card-subsystem power-good loop
    export Z6II_SLAVE="$OUT/flash_sla.img"
    VTIME_DEFAULT=1
fi
VTIME="${VTIME:-$VTIME_DEFAULT}"
if [ "${CARD:-1}" = 1 ]; then
    [ -f "$OUT/card.img" ] ||
        python3 scripts/emu/make_sd_image.py "$OUT/card.img" >/dev/null
    export Z6II_XQD="$OUT/card.img"
fi
if [ "${REC:-0}" = 1 ]; then
    [ "${CARD:-1}" = 1 ] || { echo "run_emulator.sh: REC=1 needs the card (CARD=1)" >&2; exit 1; }
    export EXPEED_H264=fe EXPEED_H264_ENCODE=1
fi
# the IIP pixel output for stills and movies
export EXPEED_IIP_PIXELS=1
REL_ARGS=(--exmcu-frame 7:1:0x0a:18:0x04=1a08120c0000)
INTERACTIVE=()
[ "$DISPLAY_ARG" = none ] || INTERACTIVE=(--interactive "$DISPLAY_ARG")

python3 "$HERE/drive_poweron.py" \
    --exmcu-frame "13:0x91:0x10:6:0x04=01+0x09=01+0x0c=00" \
    --exmcu-frame "1:3:0x6a:10:0x0f=01+0x1c=02+0x68=02" \
    --exmcu-frame "16:1:0x5c:6:0x04=01+0x0c=34000000+0x10=34000000+0x14=10000000" \
    --exmcu-frame "9:6:5:11+2" --exmcu-frame "9:1:5:11.3+2" --exmcu-frame "9:3:4:11.6+2" \
    --pid-file "$OUT/qemu.pid" --shot-dir "$OUT" \
    --settle "${HANDOVER:-12}" --vtime "$VTIME" ${INTERACTIVE[@]+"${INTERACTIVE[@]}"} \
    "${REL_ARGS[@]}" "${STATUS_ARGS[@]}" "$@" 2>&1 |
    if [ ${#INTERACTIVE[@]} -gt 0 ]; then tee "$OUT/run.txt"; else cat > "$OUT/run.txt"; fi
