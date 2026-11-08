#!/usr/bin/env python3
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Load the DOOM binary onto a Nikon Z 6II v1.70 over USB/PTP

WARNING: RUNNING THIS SCRIPT MAY IRREVERSIBLY DAMAGE YOUR CAMERA
AND/OR MAY VOID ITS WARRANTY. EXCLUSIVELY USE AT YOUR OWN RISK.

Uploads the DOOM binary to high DRAM and then hooks a GIC software-generated
interrupt (SGI) slot, which triggers the binary immediately.
Designed to be compatible with the Nikon Z 6II firmware version v1.70.
Run on the HOST (as the Docker container lacks USB passthrough).
You may have to install some Python dependencies.

Usage::

    ./load_doom.py --load-sgi DOOM_BINARY_FILE [--sgi N]

``N`` selects the GIC IRQ table slot (0–15; default 0).

Preconditions:
- Camera ON, main switch ON
- Camera in menu (not photo or video live view, or playback)
"""

import argparse
import struct
import sys
import time
import usb.core
import usb.util

NIKON_VID = 0x04B0

CONTAINER_CMD = 1
CONTAINER_DATA = 2
CONTAINER_RESP = 3

OP_OPEN_SESSION = 0x1002
OP_CLOSE_SESSION = 0x1003

OP_SERVICE_WRAP_RECV = 0x94FF
OP_SERVICE_WRAP_SEND = 0x94FE

SUBOP_F001 = 0xF001
SUBOP_F036 = 0xF036
SUBOP_F037 = 0xF037

BLOB_ADDR = 0xD0000000  # high DRAM, executable
IRQTAB_BASE = 0xC2B47260  # GIC second-level ISR table; slot n is at IRQTAB_BASE + 4*n
GICD_SGIR = 0x04301F00  # write 0x02000000 | n to trigger SGI n to self


# ---------------------------------------------------------------------------
# USB/PTP transport
# ---------------------------------------------------------------------------


def find_camera():
    dev = usb.core.find(idVendor=NIKON_VID)
    if dev is None:
        print("No Nikon USB device found (vendor id 0x04b0).  Camera plugged in and awake?")
        sys.exit(1)
    cfg = dev.get_active_configuration()
    intf = next((i for i in cfg if i.bInterfaceClass == 6), None)
    if intf is None:
        print("No USB Still Image (PTP) interface found.")
        sys.exit(1)
    ep_out = usb.util.find_descriptor(
        intf,
        custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)
        == usb.util.ENDPOINT_OUT,
    )
    ep_in = usb.util.find_descriptor(
        intf,
        custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)
        == usb.util.ENDPOINT_IN,
    )
    if ep_out is None or ep_in is None:
        print("Could not find bulk IN/OUT endpoints.")
        sys.exit(1)
    if dev.is_kernel_driver_active(intf.bInterfaceNumber):
        dev.detach_kernel_driver(intf.bInterfaceNumber)
    usb.util.claim_interface(dev, intf.bInterfaceNumber)
    return dev, intf, ep_out, ep_in


def release_camera(dev, intf):
    try:
        usb.util.release_interface(dev, intf.bInterfaceNumber)
        dev.attach_kernel_driver(intf.bInterfaceNumber)
    except Exception:
        pass


class Ptp:
    """Minimal PTP-over-USB (Bulk-Only, USB Still Image class) transport."""

    def __init__(self, ep_out, ep_in):
        self.ep_out = ep_out
        self.ep_in = ep_in
        self.txn = 0

    def _next_txn(self):
        self.txn += 1
        return self.txn

    def _write_container(self, ctype, code, txn, payload=b""):
        length = 12 + len(payload)
        header = struct.pack("<IHHI", length, ctype, code, txn)
        self.ep_out.write(header + payload, timeout=10000)

    def _read_container(self, timeout=10000):
        chunk = 1 << 20
        raw = bytes(self.ep_in.read(chunk, timeout=timeout))
        if len(raw) < 12:
            raise IOError(f"short PTP container ({len(raw)} bytes)")
        length = struct.unpack_from("<I", raw, 0)[0]
        while len(raw) < length:
            raw += bytes(self.ep_in.read(chunk, timeout=timeout))
        ctype, code, _ = struct.unpack_from("<HHI", raw, 4)
        payload = raw[12:length]
        return ctype, code, payload

    def command(self, opcode, params=(), data_out=None):
        """Send Command (+ optional Data-out phase); return (response_code, data_in_or_None)."""
        txn = self._next_txn()
        param_bytes = b"".join(struct.pack("<I", p) for p in params)
        self._write_container(CONTAINER_CMD, opcode, txn, param_bytes)
        if data_out is not None:
            self._write_container(CONTAINER_DATA, opcode, txn, data_out)
        ctype, code, payload = self._read_container()
        data_in = None
        if ctype == CONTAINER_DATA:
            data_in = bytes(payload)
            ctype, code, payload = self._read_container()
        if ctype != CONTAINER_RESP:
            raise IOError(f"expected Response container, got type={ctype} code=0x{code:04x}")
        return code, data_in


# ---------------------------------------------------------------------------
# Service-access primitives
# ---------------------------------------------------------------------------


def enable_service_access(ptp):
    code, _ = ptp.command(OP_SERVICE_WRAP_RECV, params=[SUBOP_F001, 0, 0, 0, 0])
    return code


def f036_read(ptp, addr, length):
    code, data = ptp.command(OP_SERVICE_WRAP_RECV, params=[SUBOP_F036, addr, length, 0, 0])
    return code, bytes(data) if data else None


def f037_write(ptp, addr, data):
    code, _ = ptp.command(
        OP_SERVICE_WRAP_SEND,
        params=[SUBOP_F037, addr, len(data), 0, 0],
        data_out=bytes(data),
    )
    return code


# ---------------------------------------------------------------------------
# Load
# ---------------------------------------------------------------------------


def do_load_sgi(ptp, blob_path, sgi):
    with open(blob_path, "rb") as fh:
        blob = fh.read()

    irqtab_slot = IRQTAB_BASE + 4 * sgi
    sgi_trigger = 0x02000000 | sgi

    print(f"\n--- code load (SGI {sgi}) ---")
    print(f"  blob      : {blob_path}  ({len(blob)} bytes)")
    print(f"  Write 1   : {len(blob)} bytes to 0x{BLOB_ADDR:08x}")
    print(f"  Write 2   : 0x{BLOB_ADDR | 1:08x} to 0x{irqtab_slot:08x}  (IRQTAB slot {sgi})")
    print(f"  Write 3   : 0x{sgi_trigger:08x} to 0x{GICD_SGIR:08x}  (triggers immediately)")
    print()
    print("  Preconditions:")
    print("    - Camera ON, main switch ON")
    print("    - Camera in menu (not in live view)")
    print()

    ans = input("  Type YES to proceed with Write 1 (blob upload): ").strip()
    if ans != "YES":
        print("  Aborted.")
        return False

    print()
    print("  [1/5] enable service access...", flush=True)
    code = enable_service_access(ptp)
    print(f"         -> 0x{code:04x}  ", end="")
    if code == 0x2001:
        print("OK")
    else:
        print("not granted; aborting")
        return False

    print(f"  [2/5] write blob {len(blob)} bytes to 0x{BLOB_ADDR:08x}...", flush=True)
    code = f037_write(ptp, BLOB_ADDR, blob)
    print(f"         -> 0x{code:04x}  ", end="")
    if code != 0x2001:
        print("FAILED -- aborting, IRQTAB not touched")
        return False
    print("OK")

    print(f"  [3/5] verify: read first 16 bytes back from 0x{BLOB_ADDR:08x}...", flush=True)
    code, readback = f036_read(ptp, BLOB_ADDR, 16)
    readback = bytes(readback or b"")
    expected = blob[:16]
    print(f"         -> 0x{code:04x}  data={readback.hex()}")
    print(f"         expected          {expected.hex()}")
    if readback != expected:
        print("         MISMATCH -- blob may not have landed; aborting, IRQTAB NOT touched")
        return False
    print("         MATCH")

    print(f"\n  [3.5/5] read IRQTAB slot {sgi} before writing...", flush=True)
    code, current = f036_read(ptp, irqtab_slot, 4)
    current = bytes(current or b"")
    current_val = struct.unpack("<I", current)[0] if len(current) == 4 else None
    print(f"          -> 0x{code:04x}  data={current.hex()}")
    if current_val == 0:
        print(
            f"          Slot {sgi} is zero -- an unclaimed slot does not dispatch when triggered.\n"
            f"          The blob will very likely NOT run.  Use sgi=0 on a running camera."
        )
    else:
        print(
            f"          Slot {sgi} holds a live handler -- overwriting it is recoverable by power-cycle.\n"
            f"          Do not use slot 2 (the timer inter-processor interrupt)."
        )

    print()
    print(f"  Blob is at 0x{BLOB_ADDR:08x}.  About to hook IRQTAB slot {sgi} and trigger it.")
    ans = input("  Type YES to proceed with Write 2+3 (hook IRQTAB and trigger): ").strip()
    if ans != "YES":
        print(f"  Aborted.  Blob is at 0x{BLOB_ADDR:08x} but IRQTAB is NOT hooked.")
        return False

    print()
    print(f"  [4/5] write handler ptr to IRQTAB slot {sgi} (0x{irqtab_slot:08x})...", flush=True)
    code = f037_write(ptp, irqtab_slot, struct.pack("<I", BLOB_ADDR | 1))
    print(f"         -> 0x{code:04x}  ", end="")
    if code != 0x2001:
        print("FAILED")
        return False
    print("OK")

    print(
        f"  [5/5] trigger SGI {sgi} via GICD_SGIR...",
        flush=True,
    )
    code = f037_write(ptp, GICD_SGIR, struct.pack("<I", sgi_trigger))
    print(f"         -> 0x{code:04x}")

    # Restore the slot so subsequent inter-processor interrupts hit the original handler.
    if len(current) == 4:
        print(f"\n  [5.5/5] restore IRQTAB slot {sgi}...", flush=True)
        code = f037_write(ptp, irqtab_slot, current)
        print(f"           -> 0x{code:04x}  ", end="")
        if code == 0x2001:
            print("OK")
        else:
            print("FAILED -- slot NOT restored until power-cycle")
    else:
        print(f"\n  [5.5/5] SKIP restore -- original slot value was not read cleanly")

    print()
    print("  Triggered.  If the blob ran, its effect should be visible already.")
    print("  Recovery: press any camera button, or pull battery.")
    return True


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(
        description="Load the DOOM binary onto a Nikon Z 6II v1.70 over USB/PTP",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--load-sgi",
        metavar="FILE",
        required=True,
        help="DOOM binary to load and execute through SGI.",
    )
    ap.add_argument(
        "--sgi",
        type=int,
        default=0,
        choices=range(16),
        help="GIC IRQ table slot / SGI id (default 0).",
    )
    args = ap.parse_args()

    print()
    print("  WARNING: RUNNING THIS SCRIPT MAY IRREVERSIBLY DAMAGE YOUR CAMERA")
    print("  AND/OR MAY VOID ITS WARRANTY. EXCLUSIVELY USE AT YOUR OWN RISK.")
    print()
    ans = input('  To continue, type "YES I TAKE THE FULL RISK": ').strip()
    if ans.lower() != "yes i take the full risk":
        print("  Aborted")
        sys.exit(0)
    print()

    dev, intf, ep_out, ep_in = find_camera()
    try:
        ptp = Ptp(ep_out, ep_in)
        code, _ = ptp.command(OP_OPEN_SESSION, params=[1])
        if code != 0x2001:
            print(f"OpenSession failed: 0x{code:04x}")
            sys.exit(1)

        hooked = do_load_sgi(ptp, args.load_sgi, args.sgi)
        if hooked:
            print()
            print("  Waiting 1.5 s before releasing the USB interface.")
            time.sleep(1.5)
        else:
            ptp.command(OP_CLOSE_SESSION)
    finally:
        release_camera(dev, intf)


if __name__ == "__main__":
    main()
