#!/usr/bin/env python3
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble ARM/Thumb with Keystone.
"""
import argparse
import subprocess
import sys

CHILD = r"""
import sys
from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM, KS_MODE_THUMB
thumb, addr = int(sys.argv[1]), int(sys.argv[2], 0)
code, _ = Ks(KS_ARCH_ARM, KS_MODE_THUMB if thumb else KS_MODE_ARM).asm(sys.stdin.read(), addr)
sys.stdout.write(bytes(code).hex())
"""


def assemble(asm, thumb=True, addr=0, who="ks_asm"):
    """The machine code for `asm` as bytes. Raises SystemExit if every attempt fails."""
    from keystone import Ks, KsError, KS_ARCH_ARM, KS_MODE_ARM, KS_MODE_THUMB

    try:
        mode = KS_MODE_THUMB if thumb else KS_MODE_ARM
        code, _ = Ks(KS_ARCH_ARM, mode).asm(asm, addr)
        again, _ = Ks(KS_ARCH_ARM, mode).asm(asm, addr)
        if code is not None and code == again:  # an unresolved symbol gives None, not an exception
            return bytes(code)
        why = "no code" if code is None else "two different results from two engines"
        print(f"{who}: keystone returned {why} in-process; retrying in a fresh process",
              file=sys.stderr)  # fmt: skip
    except KsError as e:
        print(
            f"{who}: keystone failed in-process ({e}); retrying in a fresh process", file=sys.stderr
        )
    for attempt in range(3):
        r = subprocess.run(
            [sys.executable, "-c", CHILD, str(int(thumb)), hex(addr)],
            input=asm,
            capture_output=True,
            text=True,
        )
        if r.returncode == 0 and r.stdout:
            return bytes.fromhex(r.stdout)
        err = r.stderr.strip().splitlines()[-1:] or ["no output"]
        print(f"{who}: keystone failed in child {attempt + 1} of 3 ({err[0]})", file=sys.stderr)
    print(f"{who}: the text that failed:\n{asm}", file=sys.stderr)
    raise SystemExit(f"{who}: keystone could not assemble the text above")


def selftest():
    ok = True
    want = bytes.fromhex("40f20100")  # movw r0, #1 (Thumb-2)
    for name, fn in (("in-process", lambda: assemble("movw r0, #1\n")),
                     ("child", lambda: _child_only("movw r0, #1\n"))):  # fmt: skip
        got = fn()
        good = got == want
        print(("ok    " if good else "FAIL  ") + f"{name}: {got.hex()}")
        ok = ok and good
    try:
        assemble("movw r0, #bogus\n", who="selftest")
        print("FAIL  a bad operand was accepted")
        ok = False
    except SystemExit:
        print("ok    a bad operand ends in SystemExit after the retries")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


def _child_only(asm):
    r = subprocess.run(
        [sys.executable, "-c", CHILD, "1", "0"],
        input=asm,
        capture_output=True,
        text=True,
        check=True,
    )
    return bytes.fromhex(r.stdout)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--selftest", action="store_true")
    sys.exit(selftest() if ap.parse_args().selftest else 0)
