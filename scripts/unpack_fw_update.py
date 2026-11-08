#!/usr/bin/env python3
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unpack the firmware update file:
write the two components the emulator boots, the master and slave images.
Based on Simeon Pilgrim's Firmware.cs (https://github.com/simeonpilgrim/nikon-firmware-tools).
"""
import argparse
import hashlib
import os
import re
import struct

import numpy as np

# Z6_2_0170.bin
FIRMWARE_SHA256 = "5a9640b4c07abe1ecbf5a52655cbec6b893bba045bb2a1a1d21055b9f8f9d06d"
FIRMWARE_CS_PATH = "/opt/nikon-firmware-tools/Firmware.cs"  # fetched by the Dockerfile
DECODE_OFFSET = 0x20
COMPONENTS = ("eg1850_mas_01700", "eg1850_sla_01700")  # the master and slave Expeed images


def load_tables():
    """Firmware.cs's three 256-entry XOR tables, as uint8 arrays."""
    if not os.path.exists(FIRMWARE_CS_PATH):
        raise SystemExit(
            f"{FIRMWARE_CS_PATH} not found: it is fetched when the Docker image is built"
        )
    src = open(FIRMWARE_CS_PATH, encoding="utf-8", errors="replace").read()

    def table(name):
        m = re.search(name + r"\s*=\s*\{(.*?)\};", src, re.S)
        if not m:
            raise SystemExit(f"table {name} not found in {FIRMWARE_CS_PATH}")
        # & 0xFF: Ord3 holds a 0x100, which the decode truncates
        t = [int(x, 16) & 0xFF for x in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))]
        if len(t) != 256:
            raise SystemExit(f"table {name} in {FIRMWARE_CS_PATH} has {len(t)} entries, not 256")
        return np.array(t, np.uint8)

    return table("Xor_Ord1"), table("Xor_Ord2"), table("Xor_Ord3")


def decode(data, o1, o2, o3):
    i = np.arange(len(data) - DECODE_OFFSET, dtype=np.uint32)
    enc = np.frombuffer(data, np.uint8, offset=DECODE_OFFSET)
    return (enc ^ o1[i & 0xFF] ^ o2[(i >> 8) & 0xFF] ^ o3[(i >> 16) & 0xFF]).tobytes()


def write_components(pkg, dst):
    """Write the COMPONENTS out of the decoded package's table into dst."""
    count = struct.unpack_from(">I", pkg, 0x20)[0]
    if not 0 < count < 64:
        raise SystemExit(f"implausible component count {count}: the decode failed")
    found = {}
    for k in range(count):
        p = 0x30 + 32 * k
        name = pkg[p : p + 16].split(b"\x00")[0].decode("ascii", "replace")
        start, length = struct.unpack_from(">II", pkg, p + 16)
        found[name] = pkg[start : start + length]
    os.makedirs(dst, exist_ok=True)
    for name in COMPONENTS:
        if name not in found:
            raise SystemExit(f"no component {name} in the update")
        path = os.path.join(dst, name)
        with open(path, "wb") as f:
            f.write(found[name])
        print(f"  {name}  {len(found[name]):#x} bytes -> {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("firmware", help="the firmware update file, Z6_2_0170.bin")
    ap.add_argument("-o", "--out", default="out", help="output directory")
    args = ap.parse_args()

    data = open(args.firmware, "rb").read()
    if hashlib.sha256(data).hexdigest() != FIRMWARE_SHA256:
        raise SystemExit(
            f"{args.firmware} is not the Z6II firmware update 1.70 (Z6_2_0170.bin): its sha256 differs"
        )
    write_components(decode(data, *load_tables()), args.out)


if __name__ == "__main__":
    main()
