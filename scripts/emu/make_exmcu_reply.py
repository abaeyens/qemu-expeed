#!/usr/bin/env python3
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build an Ex MCU reply block: an <op>.bin of the emulator's Z6II_EXMCU_REPLY_DIR peer.

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
    [0xff]   checksum: sum(block[0..0xff]) & 0xff == 0xff  (i.e. chk = 0xff - sum(0..0xfe))

A status message is command 2, class 2. Its payload[0] selects the status subtype:
subtype 1 is the main status, whose boolean bytes at payload offsets +0x04..+0x73
the master packs into its status word.
Set those with --fill (blanket) or --field CODE=HEXBYTES (specific offsets).

For the status poll (opcode 0x0208 / 0x0202) the payload is a 4-byte header + a 120-byte
state block, so master payload offset +0x04+K = Ex state byte K.
Key fields to set with --field: +0x04 = power/main-switch (→ status word bit 9),
+0x0f = mode dial (0-6), +0x22..+0x32 = other dials/levers;
all other window offsets are momentary buttons.
Example coherent powered-on status:
--cmd 8 --cls 2 --plen 0x7c --field 0x04=01 --field 0x0f=01.

Emit one or more concatenated 256-byte blocks; the peer serves them on successive
exchanges and repeats the last one.
"""
import argparse


def build_block(cmd, cls, payload, total=1, index=1):
    b = bytearray(256)
    b[0] = cmd & 0xFF
    b[1] = cls & 0xFF
    b[2] = len(payload) & 0xFF
    b[3] = (len(payload) >> 8) & 0xFF
    b[8 : 8 + len(payload)] = payload
    b[0xFC] = total & 0xFF
    b[0xFD] = index & 0xFF
    b[0xFF] = (0xFF - sum(b[:0xFF])) & 0xFF
    assert sum(b) & 0xFF == 0xFF
    return bytes(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", required=True, help="output .bin path")
    ap.add_argument(
        "--cmd", type=lambda x: int(x, 0), default=2, help="command byte (default 2 = status)"
    )
    ap.add_argument("--cls", type=lambda x: int(x, 0), default=2, help="class byte (default 2)")
    ap.add_argument(
        "--subtype", type=lambda x: int(x, 0), default=1, help="payload[0] status subtype"
    )
    ap.add_argument(
        "--fill", type=lambda x: int(x, 0), default=0, help="value for payload[1..0x77] (blanket)"
    )
    ap.add_argument("--plen", type=lambda x: int(x, 0), default=0x79, help="payload length field")
    ap.add_argument(
        "--field",
        action="append",
        default=[],
        metavar="OFF=HEXBYTES",
        help="set payload bytes at OFF, e.g. 0x40=01 (repeatable)",
    )
    ap.add_argument(
        "--datetime",
        metavar="YYYY-MM-DDThh:mm:ss",
        help="build the command-7 DATETIME reply (the Ex MCU's RTC read): wire subtype 1, "
        "10-byte payload with {year-2000, month-1, day-1, hour, minute, second} at [4..9], "
        "as the master parses it and as the real camera answers (length 10). "
        "Implies --cmd 7 --cls 2 --subtype 1 --plen 0x0a.",
    )
    a = ap.parse_args()
    if a.datetime:
        import datetime as _dt

        t = _dt.datetime.strptime(a.datetime, "%Y-%m-%dT%H:%M:%S")
        a.cmd, a.cls, a.subtype, a.plen = 7, 2, 1, 0x0A
        a.field.append(
            "0x04="
            + bytes([t.year - 2000, t.month - 1, t.day - 1, t.hour, t.minute, t.second]).hex()
        )
    payload = bytearray(a.plen)
    payload[0] = a.subtype
    if a.fill:
        for off in range(1, min(0x78, a.plen)):
            payload[off] = a.fill
    for f in a.field:
        off, hexb = f.split("=")
        off = int(off, 0)
        data = bytes.fromhex(hexb)
        payload[off : off + len(data)] = data
    open(a.o, "wb").write(build_block(a.cmd, a.cls, bytes(payload)))
    print(
        f"{a.o}: cmd={a.cmd:#x} cls={a.cls:#x} subtype={a.subtype} fill={a.fill:#x} fields={a.field or '-'}"
    )


if __name__ == "__main__":
    main()
