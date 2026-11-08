#!/usr/bin/env python3
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Ex MCU's replies to the master

The Ex MCU link is a synchronous full-duplex 256-byte block exchange on SIO channel 0
(request) with the reply clocked back on channel 1.
The block framing is the one the master's link driver expects:

    [0]      command byte
    [1]      class byte
    [2..3]   u16 payload length
    [4..7]   reserved (0)
    [8..]    payload (<=240 bytes)
    [0xfc]   total block count
    [0xfd]   block index (1-based)
    [0xfe]   reserved (0)
    [0xff]   checksum: sum(block[0..0xff]) & 0xff == 0xff

A status message is command 2 (or 8, the status poll), class 2, and its payload[0]
selects the subtype: subtype 1 is the main status, a 4-byte header
and a 120-byte state block whose bytes the master packs into its status word.
In it, +0x04 is the main switch, +0x0f the mode dial (0-6),
+0x22..+0x32 other dials and levers, +0x39 the standby inhibit;
the other bytes are momentary buttons.

The reply directory (Z6II_EXMCU_REPLY_DIR) holds one <opcode>.bin per request
the master makes at boot; the machine serves block i of a file
on the i-th request for that opcode, and repeats the last one.
The master powers on only on the main switch's OFF->ON edge, never on its level,
and it requests the status (0x0202, 0x0208) only twice, early in the boot:
so each status file is two blocks, switch off, then on.

  python3 scripts/emu/exmcu_replies.py OUTDIR [--field OFF=HEX ...]
"""
import argparse
import os

RTC = (2026, 9, 19, 12, 0, 0)  # RTC time read answer


def build_block(cmd, cls, payload):
    """One 256-byte block around `payload`."""
    b = bytearray(256)
    b[0] = cmd & 0xFF
    b[1] = cls & 0xFF
    b[2] = len(payload) & 0xFF
    b[3] = (len(payload) >> 8) & 0xFF
    b[8 : 8 + len(payload)] = payload
    b[0xFC] = 1
    b[0xFD] = 1
    b[0xFF] = (0xFF - sum(b[:0xFF])) & 0xFF
    return bytes(b)


def block(cmd, cls, plen, subtype=1, fields=(), fill=0):
    """A block with a `plen`-byte payload: payload[0] = `subtype`, payload[1..0x77] = `fill`, then
    each of `fields` ("OFF=HEXBYTES") written over it at its offset, in order."""
    payload = bytearray(plen)
    payload[0] = subtype
    for off in range(1, min(0x78, plen)):
        payload[off] = fill
    for f in fields:
        off, hexb = f.split("=")
        data = bytes.fromhex(hexb)
        payload[int(off, 0) : int(off, 0) + len(data)] = data
    return build_block(cmd, cls, bytes(payload))


def write_reply_dir(path, fields=()):
    """The reply directory, `fields` ("OFF=HEX") set in every status block as well."""
    y, mo, d, h, mi, s = RTC
    replies = {
        "0101": block(1, 1, 0x79, fill=1),  # identity
        "0106": block(6, 1, 0x79, fill=1),
        "0205": block(5, 2, 5, fill=1),
        "0207": block(
            7, 2, 0x0A, fields=["0x04=" + bytes([y - 2000, mo - 1, d - 1, h, mi, s]).hex()]
        ),
    }
    for op, cmd in (("0202", 2), ("0208", 8)):
        off = block(cmd, 2, 0x7C, fields=["0x0f=01", *fields])
        on = block(cmd, 2, 0x7C, fields=["0x0f=01", "0x04=01", *fields])
        replies[op] = off + on
    os.makedirs(path, exist_ok=True)
    for op, data in replies.items():
        with open(os.path.join(path, op + ".bin"), "wb") as f:
            f.write(data)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("outdir", help="the reply directory to write")
    ap.add_argument(
        "--field",
        action="append",
        default=[],
        metavar="OFF=HEX",
        help="also set this payload field in every status block; repeatable",
    )
    a = ap.parse_args()
    write_reply_dir(a.outdir, a.field)


if __name__ == "__main__":
    main()
