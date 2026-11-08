#!/usr/bin/env bash
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the standard per-opcode Ex MCU reply directory for Z6II_EXMCU_REPLY_DIR.
# One 256-byte block per request opcode the master polls at boot;
# each is a header- correct status/identity block (see scripts/emu/make_exmcu_reply.py).
# With the machine's handshake edges this makes the master accept the replies
# and reach steady-state status polling.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DIR="${1:-out/exmcu_dir}"
MODE="${2:-fill}"   # fill = every boolean set; coherent = "powered on, mode 1"
G="python3 $HERE/make_exmcu_reply.py"
mkdir -p "$DIR"
$G -o "$DIR/0101.bin" --cmd 1 --cls 1 --subtype 1 --fill 1              # identity
$G -o "$DIR/0106.bin" --cmd 6 --cls 1 --subtype 1 --fill 1
$G -o "$DIR/0205.bin" --cmd 5 --cls 2 --subtype 1 --fill 1 --plen 5
# The Ex MCU's RTC read (command 7, opcode 0x0207): answered with one FIXED datetime,
# so the master's software clock is set and stable across runs. Not requested at idle boot.
# Change the value here if a run needs another date.
$G -o "$DIR/0207.bin" --datetime 2026-09-19T12:00:00                  # RTC datetime reply
if [ "$MODE" = coherent ]; then
  # A status a real camera could actually report:
  # main switch on (+0x04), mode dial 1 (+0x0f), every other window byte 0.
  # This puts exactly bit 9 in the master's status word,
  # where --fill 1 sets nearly every bit.
  $G -o "$DIR/0202.bin" --cmd 2 --cls 2 --subtype 1 --plen 0x7c --field 0x04=01 --field 0x0f=01
  $G -o "$DIR/0208.bin" --cmd 8 --cls 2 --subtype 1 --plen 0x7c --field 0x04=01 --field 0x0f=01
else
  $G -o "$DIR/0202.bin" --cmd 2 --cls 2 --subtype 1 --fill 1              # status
  $G -o "$DIR/0208.bin" --cmd 8 --cls 2 --subtype 1 --fill 1 --plen 0x70  # status poll
fi
echo "built $DIR ($MODE)"
