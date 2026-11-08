#!/usr/bin/env python3
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build an SD card image the camera can mount, MBR with one FAT32-LBA partition
"""
import argparse
import struct
import subprocess
import sys

SECTOR = 512
PART_START = 8192  # sectors: 4 MB, the SD spec's allocation-unit alignment for small cards
FAT32_LBA = 0x0C


def chs(lba):
    """The legacy CHS triple for an LBA (255 heads, 63 sectors), clamped as tools do."""
    c, rem = divmod(lba, 255 * 63)
    h, s = divmod(rem, 63)
    if c > 1023:
        return bytes([0xFE, 0xFF, 0xFF])
    return bytes([h, ((c >> 2) & 0xC0) | (s + 1), c & 0xFF])


def mbr(total_sectors, start=PART_START, ptype=FAT32_LBA):
    """A 512-byte MBR with one partition covering [start, total_sectors)."""
    count = total_sectors - start
    entry = (
        bytes([0x00])
        + chs(start)
        + bytes([ptype])
        + chs(total_sectors - 1)
        + struct.pack("<II", start, count)
    )
    sector = bytearray(SECTOR)
    struct.pack_into("<I", sector, 0x1B8, 0x5A364E6B)  # disk signature, arbitrary but fixed
    sector[0x1BE : 0x1BE + 16] = entry
    sector[0x1FE:0x200] = b"\x55\xaa"
    return bytes(sector)


LABEL = "NIKON Z6II"
DEFAULT_SIZE_MB = 1024
DEFAULT_VOLUME_ID = 0x11111111


def build(path, size_mb=DEFAULT_SIZE_MB, volume_id=DEFAULT_VOLUME_ID):
    total = size_mb * 1024 * 1024 // SECTOR
    if total <= PART_START + 65536:
        raise SystemExit("make_sd_image: --size-mb too small for FAT32 (use >= 64)")
    with open(path, "wb") as f:
        f.truncate(total * SECTOR)
        f.write(mbr(total))
    subprocess.run(
        [
            "mkfs.vfat",
            "--invariant",
            "-i",
            "%08x" % volume_id,
            "-F",
            "32",
            "-n",
            LABEL,
            "--offset=%d" % PART_START,
            path,
            str((total - PART_START) * SECTOR // 1024),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    return "%s@@%d" % (path, PART_START * SECTOR)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("--size-mb", type=int, default=DEFAULT_SIZE_MB, help="default %(default)d")
    ap.add_argument(
        "--volume-id",
        type=lambda x: int(x, 0),
        default=DEFAULT_VOLUME_ID,
        help="FAT32 volume ID (default %#x; a fixed value keeps the image reproducible)"
        % DEFAULT_VOLUME_ID,
    )
    a = ap.parse_args()
    img = build(a.image, a.size_mb, a.volume_id)
    print(
        "wrote %s (%d MB, FAT32 partition at %d sectors); mtools: -i %s"
        % (a.image, a.size_mb, PART_START, img)
    )


if __name__ == "__main__":
    sys.exit(main())
