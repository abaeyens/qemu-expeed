#!/usr/bin/env bash
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Play DOOM on the emulated camera:
#
#   scripts/emu/doom.sh [gtk|vnc=:N|none] [OUTDIR]   # default: gtk, out/doom_play
#
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"
DISPLAY_ARG="${1:-gtk}"
OUT="${2:-out/doom_play}"
[ -f DOOM1.WAD ] || { echo "doom.sh: needs DOOM1.WAD (the shareware WAD) in the repository root" >&2; exit 1; }
[ -f out/doom_port/doom.bin ] ||
    { echo "doom.sh: needs out/doom_port/doom.bin: build it with programs/doom/build.sh" >&2; exit 1; }
mkdir -p "$OUT"
[ -f "$OUT/card.img" ] ||
    python3 scripts/emu/make_sd_image.py "$OUT/card.img" >/dev/null
mcopy -o -i "$OUT/card.img@@4M" DOOM1.WAD ::DOOM1.WAD   # the partition starts at 4 MB (make_sd_image.py)
SLAVE="${SLAVE:-0}" HANDOVER="${HANDOVER:-20}" Z6II_KEYS="${Z6II_KEYS:-m@22}" exec "$HERE/run_emulator.sh" \
    "$DISPLAY_ARG" "$OUT" --poke-before-at 25 --load-before "0xd04c0000:$ROOT/out/doom_port/doom.bin" \
    --late-call 26:0xd04c0000 "${@:3}"
