#!/usr/bin/env python3
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the Ex MCU mock peer's reply directories (Z6II_EXMCU_REPLY_DIR) with the main switch's OFF->ON edge.

The master fires the power-on edge only when the status record's changed-mask bit is set, never on the level, so the
switch has to go 0 -> 1 across two consecutive status frames the master receives. The status comes from opcode 0x0202,
which the master requests only twice, early in the boot. The peer serves block i of a multi-block reply file on the
i-th request for that opcode (last block sticky), so `cat off/0202.bin on/0202.bin > edge/0202.bin` puts the
transition where the master looks.

Usage:
    python3 scripts/emu/exmcu_switch_edge.py                  # out/exmcu_{off,on,edge}
    python3 scripts/emu/exmcu_switch_edge.py --build-inhibit  # out/exmcu_edge_inhibit (what run_emulator.sh boots with without the settings)
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def build_dirs(fields=(), suffix=""):
    """off/ = switch clear, on/ = switch set, edge/ = the two concatenated per opcode.

    `fields` (OFF=HEX) are set in every status block as well, into dirs named with `suffix`:
    --build-inhibit makes out/exmcu_edge_inhibit with the standby inhibit +0x39 = 1, without which
    the standby timer powers the emulated camera off 30 s after boot; a stand-in for what keeps
    the USB-connected body awake.
    """
    gen = [sys.executable, os.path.join(HERE, "make_exmcu_reply.py")]
    for name, on in (("out/exmcu_off" + suffix, False), ("out/exmcu_on" + suffix, True)):
        d = os.path.join(ROOT, name)
        os.makedirs(d, exist_ok=True)
        subprocess.run(
            ["bash", os.path.join(HERE, "make_exmcu_dir.sh"), name, "coherent"],
            cwd=ROOT,
            check=True,
            stdout=subprocess.DEVNULL,
        )
        for op, cmd, plen in (("0202", 2, 0x7C), ("0208", 8, 0x7C)):
            args = gen + [
                "-o",
                f"{name}/{op}.bin",
                "--cmd",
                str(cmd),
                "--cls",
                "2",
                "--subtype",
                "1",
                "--plen",
                hex(plen),
                "--field",
                "0x0f=01",
            ]
            if on:
                args += ["--field", "0x04=01"]
            for f in fields:
                args += ["--field", f]
            subprocess.run(args, cwd=ROOT, check=True, stdout=subprocess.DEVNULL)

    edge = os.path.join(ROOT, "out", "exmcu_edge" + suffix)
    shutil.rmtree(edge, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "out", "exmcu_off" + suffix), edge)
    for op in ("0202.bin", "0208.bin"):
        with open(os.path.join(edge, op), "wb") as out:
            for d in ("exmcu_off" + suffix, "exmcu_on" + suffix):
                with open(os.path.join(ROOT, "out", d, op), "rb") as f:
                    out.write(f.read())


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--build-inhibit",
        action="store_true",
        help="build out/exmcu_edge_inhibit: the edge dir with the standby inhibit +0x39 = 1 in "
        "every status block (what run_emulator.sh boots with without the settings)",
    )
    a = ap.parse_args()
    if a.build_inhibit:
        build_dirs(("0x39=01",), "_inhibit")
    else:
        build_dirs()
    return 0


if __name__ == "__main__":
    sys.exit(main())
