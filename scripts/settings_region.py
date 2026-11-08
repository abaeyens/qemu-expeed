# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""The user settings in the parameter flash: the settings store's persistent image as a journal.

The persistent image is 0x5e records that tile one stretch of the settings store, listed by a
static record table (a pointer, a size and a flag each). Record 0 is a 0x180-byte header whose
word 0 is the total size and word 1 the format version.
The boot restore takes the newer of the region's two banks and copies the last occurrence of each
record out of it; one record missing, or a header that disagrees, and every persistent page goes
back to the firmware's defaults, which is what a zero-filled bank gives.

The bank is a journal: entries {u32 index, u32 length, data padded to 4} (the index must be below
0x5e and the length the record's size), each write session closed by {u32 "ENDM", u32 sequence},
erased flash (0xff) after the last session. So it carries no generic "ENDM" at the end of the
bank, which is why ParamFlash.put_bank keeps the terminator out of it.

The image comes from a dump of the settings store (0x6c000 bytes from 0xc253d000) taken from an
emulated boot, with KEY=VALUE settings stored over it. A key is an InfoData key: a page in its
high byte, a cell of the page in its low byte; a cell wider than a word is given as
KEY=hex:BYTES, its raw bytes.
"""
import struct

from param_flash import CRT_FILE, CRT_VA, ENDM, LOAD_BASE

SETTINGS_REGION = 0x0F
RECORDS_VA, RECORD_COUNT = 0xC1873968, 0x5E
IMAGE_VA, IMAGE_SIZE, IMAGE_VERSION = 0xC25614E4, 0x18D94, 0x14
STORE_VA, STORE_SIZE = 0xC253D000, 0x6C000  # the settings store: where a dump starts, its length
PAGES_VA, PAGE_STRIDE, PAGE_COUNT = 0x11917284, 0x1C, 0x52  # InfoData's page table
DESCS_VA, DESC_STRIDE = 0x1190B944, 0x10  # its cell descriptors


def records(image):
    """The persistent image's [(address, size)] from the static record table, checked to tile it."""
    off = CRT_FILE + RECORDS_VA - CRT_VA
    recs = [struct.unpack_from("<II", image, off + 12 * i) for i in range(RECORD_COUNT)]
    addr = IMAGE_VA
    for i, (ptr, size) in enumerate(recs):
        if ptr != addr:
            raise ValueError(f"settings record {i:#x} at {ptr:#x}, expected {addr:#x}")
        addr += size
    if addr - IMAGE_VA != IMAGE_SIZE:
        raise ValueError(f"settings records cover {addr - IMAGE_VA:#x} bytes, not the image's")
    return recs


def cell(image, key):
    """InfoData key -> (value address, size), refusing a key past its page's count. The page table
    does not bound the index, so an out-of-range key silently names another page's cell.
    """
    page, sub = key >> 8, key & 0xFF
    if page >= PAGE_COUNT:
        raise ValueError(f"InfoData key {key:#06x}: no page {page:#x}")
    entry = PAGES_VA - LOAD_BASE + page * PAGE_STRIDE
    base, count = struct.unpack_from("<HH", image, entry + 0xC)
    if sub >= count:
        raise ValueError(f"InfoData key {key:#06x}: page {page:#x} has only {count:#x} keys")
    size, ptr = struct.unpack_from(
        "<II", image, DESCS_VA - LOAD_BASE + (base + sub) * DESC_STRIDE + 4
    )
    return ptr, size


def parse_setting(text):
    """KEY=VALUE, both in any base int() accepts, or KEY=hex:BYTES for the cell's raw bytes."""
    key, _, value = text.partition("=")
    if not value:
        raise ValueError(f"setting {text!r}: want KEY=VALUE")
    if value.startswith("hex:"):
        return int(key, 0), bytes.fromhex(value[4:])
    return int(key, 0), int(value, 0)


def journal(layout, image, store, settings=()):
    """{bank: content} for the settings region: bank A one write session holding every record of
    the persistent image (from the store dump `store`, with `settings` [(key, value)] stored over
    it, little-endian in the key's size), sequence 0, then erased flash; bank B erased."""
    lo = IMAGE_VA - STORE_VA
    if len(store) < lo + IMAGE_SIZE:
        raise ValueError(f"the dump's {len(store):#x} bytes do not reach the settings image")
    persistent = bytearray(store[lo : lo + IMAGE_SIZE])
    if struct.unpack_from("<II", persistent, 0) != (IMAGE_SIZE, IMAGE_VERSION):
        raise ValueError("the dump's settings header is not a restored image's")
    for key, value in settings:
        ptr, size = cell(image, key)
        if not IMAGE_VA <= ptr <= ptr + size <= IMAGE_VA + IMAGE_SIZE:
            raise ValueError(f"InfoData key {key:#06x} ({ptr:#x}) is not in the persistent image")
        if isinstance(value, bytes):  # a cell wider than a word, such as the clock's
            if len(value) > size:
                raise ValueError(f"InfoData key {key:#06x}: {len(value)} bytes for {size}")
            data = value
        else:
            data = value.to_bytes(4, "little")[: min(size, 4)]
        persistent[ptr - IMAGE_VA : ptr - IMAGE_VA + len(data)] = data
    session = bytearray()
    for i, (ptr, size) in enumerate(records(image)):
        session += struct.pack("<II", i, size) + persistent[ptr - IMAGE_VA : ptr - IMAGE_VA + size]
        session += bytes(-size % 4)
    session += struct.pack("<II", ENDM, 0)
    holder = layout.holders[layout.regions[SETTINGS_REGION].holder]
    (bank_a, bank_b), size = holder.banks, holder.size
    if len(session) > size:
        raise ValueError(f"settings journal {len(session):#x} bytes overflows its {size:#x} bank")
    return {bank_a: bytes(session) + b"\xff" * (size - len(session)), bank_b: b"\xff" * size}
