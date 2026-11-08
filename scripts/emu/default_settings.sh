#!/usr/bin/env bash
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Make out/default_settings_store.bin, the dump run_emulator.sh builds the user settings region from.
#
#   scripts/emu/default_settings.sh [OUTFILE]      # default: out/default_settings_store.bin
#
# The firmware's default settings exist only in the running firmware: this boots it once with its default settings
# (master alone, no card, no settings region), and dumps the settings store (0x6c000 bytes at 0xc253d000) at 20 s
# virtual time, after the boot has restored it. The boot's output goes to out/default_boot/run.txt.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"
OUTFILE="${1:-out/default_settings_store.bin}"
mkdir -p "$(dirname "$OUTFILE")"
rm -f "$OUTFILE.tmp"
SETTINGS_DUMP= SLAVE=0 CARD=0 "$HERE/run_emulator.sh" none out/default_boot \
    --snap "20:0xc253d000:0x6c000:$OUTFILE.tmp" >/dev/null
if [ ! -s "$OUTFILE.tmp" ]; then
    echo "default_settings.sh: the dump was not written; see out/default_boot/run.txt" >&2
    exit 1
fi
mv "$OUTFILE.tmp" "$OUTFILE"
echo "wrote $OUTFILE"
