#!/usr/bin/env python3
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble the flash image the emulated camera boots from (Z6II_FLASH, Z6II_SLAVE).

The image is the content of the camera's 64 MB NOR flash at 0x10000000: the firmware image at 0x10020000 and, above
it, the synthetic parameter banks (synth_param_banks.py) the boot restores its reserved regions from, with,
optionally, the user settings region written from a settings-store dump with KEY=VALUE settings stored over it
(settings_region.py; default_settings.py makes the dump).

If desired, one can provide a dumped bootloader, otherwise the stand-in bootloader
which just jumps directly to the image base gets used.

  python3 scripts/make_flash_image.py --image out/eg1850_mas_01700 -o out/flash.img \\
      --settings-dump out/default_settings_store.bin --setting 0x1701=3
  python3 scripts/make_flash_image.py --image out/eg1850_sla_01700 --banks none -o out/flash_sla.img
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import settings_region  # noqa: E402
import synth_param_banks  # noqa: E402
from param_flash import Layout  # noqa: E402

FLASH_BASE, FLASH_SIZE = 0x10000000, 0x04000000
IMAGE_ADDR = 0x10020000  # the firmware image's load base
# Bootloader stand-in: ldr pc, [pc, #-4] to IMAGE_ADDR (sits by definition at FLASH_BASE)
BOOT_STUB = struct.pack("<II", 0xE51FF004, IMAGE_ADDR)


def assemble(out_path, image, banks, boot=BOOT_STUB):
    """Assemble a flash image from several parts and write to a sparse file

    Write `boot` at the flash start (0x0), `image` at `IMAGE_ADDR - FLASH_BASE`,
    and each of `banks` {address: bytes} at its address minus FLASH_BASE.
    """
    max_bootloader_size = IMAGE_ADDR - FLASH_BASE
    if len(boot) > max_bootloader_size:
        sys.exit(
            f"bootloader is {len(boot):#x} bytes while must be <={max_bootloader_size:#x} bytes"
        )
    parts = {FLASH_BASE: boot, IMAGE_ADDR: image, **banks}
    with open(out_path, "wb") as f:
        f.truncate(FLASH_SIZE)
        for addr in sorted(parts):
            data = parts[addr]
            if not (FLASH_BASE <= addr and addr + len(data) <= FLASH_BASE + FLASH_SIZE):
                sys.exit(f"{addr:#x}+{len(data):#x} is outside the flash")
            f.seek(addr - FLASH_BASE)
            f.write(data)


def read_boot(path):
    """Read bootloader in from a dump of it"""
    with open(path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--image", required=True, help="the firmware image (unpack_fw_update.py)")
    ap.add_argument("-o", "--out", required=True, help="the flash image to write")
    ap.add_argument(
        "--banks",
        choices=("synthetic", "none"),
        default="synthetic",
        help="the parameter banks: the synthetic ones (default), or none (the slave has no banks)",
    )
    ap.add_argument(
        "--settings-dump",
        metavar="DUMP",
        help="write the user settings region (0x0f) from this settings-store dump (default_settings.py)",
    )
    ap.add_argument(
        "--setting",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="with --settings-dump: store VALUE in InfoData key KEY first (repeatable; KEY=hex:BYTES "
        "for a cell wider than a word)",
    )
    ap.add_argument(
        "--bootloader",
        metavar="DUMP",
        help="bootloader dump file (default: jump to the image)",
    )
    a = ap.parse_args()
    if a.setting and not a.settings_dump:
        sys.exit("--setting needs --settings-dump")
    if a.settings_dump and a.banks == "none":
        sys.exit("--settings-dump needs the parameter banks")

    with open(a.image, "rb") as f:
        image = f.read()
    banks = {}
    if a.banks == "synthetic":
        try:
            layout = Layout.from_image(image)
            flash = synth_param_banks.flash(layout)
            if a.settings_dump:
                with open(a.settings_dump, "rb") as f:
                    store = f.read()
                settings = [settings_region.parse_setting(t) for t in a.setting]
                for base, content in settings_region.journal(
                    layout, image, store, settings
                ).items():
                    flash.put_bank(base, content)
        except ValueError as e:
            sys.exit(f"{a.image}: {e}")
        banks = flash.images()
    boot = read_boot(a.bootloader) if a.bootloader else BOOT_STUB
    assemble(a.out, image, banks, boot)
    print(f"{a.out}: the image and {len(banks)} banks" + "".join(f", {t}" for t in a.setting))


if __name__ == "__main__":
    main()
