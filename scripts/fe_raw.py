#!/usr/bin/env python3
"""Write the scene file the emulator's sensor model makes every raw frame from (EXPEED_SCENE).

The scene is a NEF's whole raw image, 14-bit linear, read with rawpy (LibRaw).
The sensor model (`qemu/sensor_model.c`) reads it out as the firmware programs the sensor,
and the front-end model (`qemu/fe_model.c`) turns that into the front end's raw outputs.

  python3 scripts/fe_raw.py --scene sample_pictures/canoe.NEF out/scene_canoe.bin
"""

import argparse
import struct
import sys

import rawpy

MAGIC = b"NKSCENE1"  # then u32 width, u32 height, u16 LE pixels row by row (qemu/sensor_model.h)


def write_scene(nef, out):
    """The NEF's whole raw image (6064 x 4040 on the Z6II), 14-bit linear, as a scene file."""
    with rawpy.imread(nef) as raw:
        px = raw.raw_image.copy()
        bits = int(raw.white_level).bit_length()
    if bits < 14:
        px = px << (14 - bits)
    h, w = px.shape
    with open(out, "wb") as f:
        f.write(MAGIC + struct.pack("<II", w, h))
        f.write(px.astype("<u2").tobytes())
    return w, h


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--scene",
        nargs=2,
        metavar=("NEF", "OUT"),
        required=True,
        help="write a NEF's raw as a scene file",
    )
    a = ap.parse_args()
    w, h = write_scene(*a.scene)
    print(f"wrote {a.scene[1]}: {w} x {h}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
