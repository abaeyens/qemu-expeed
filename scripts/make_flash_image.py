#!/usr/bin/env python3
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble the flash image the emulated camera boots from (Z6II_FLASH, Z6II_SLAVE).

The image is the content of the camera's 64 MB NOR flash at 0x10000000: the firmware image at 0x10020000 and, above
it, the synthetic parameter banks (synth_param_banks.py) the boot restores its reserved regions from, with,
optionally, the user settings region written from a settings-store dump with KEY=VALUE settings stored over it
(settings_region.py; default_settings.py makes the dump). The first-stage bootloader's 128 KB at the start stays
empty, as do the gaps (zeros). The file is sparse.

  python3 scripts/make_flash_image.py --image out/eg1850_mas_01700 -o out/flash.img \\
      --settings-dump out/default_settings_store.bin --setting 0x1701=3
  python3 scripts/make_flash_image.py --image out/eg1850_sla_01700 --banks none -o out/flash_sla.img
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import settings_region  # noqa: E402
import synth_param_banks  # noqa: E402
from param_flash import Layout  # noqa: E402

FLASH_BASE, FLASH_SIZE = 0x10000000, 0x04000000
IMAGE_ADDR = 0x10020000  # the firmware image's load base


def assemble(out_path, image, banks):
    """Write `image` at IMAGE_ADDR and each of `banks` {address: bytes} at its address, as a sparse file."""
    parts = {IMAGE_ADDR: image, **banks}
    with open(out_path, "wb") as f:
        f.truncate(FLASH_SIZE)
        for addr in sorted(parts):
            data = parts[addr]
            if not (FLASH_BASE <= addr and addr + len(data) <= FLASH_BASE + FLASH_SIZE):
                sys.exit(f"{addr:#x}+{len(data):#x} is outside the flash")
            f.seek(addr - FLASH_BASE)
            f.write(data)


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
    assemble(a.out, image, banks)
    print(f"{a.out}: the image and {len(banks)} banks" + "".join(f", {t}" for t in a.setting))


if __name__ == "__main__":
    main()
