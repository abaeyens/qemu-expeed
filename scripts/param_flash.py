# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""The parameter flash: the banks above the firmware image, and the regions restored from them.

The layout is two static tables in the image's initialised data:

    holders      one flash bank each: its base (and a second, B, for a dual bank), its size,
                 and whether the restore wants "ENDM" in its last word
    descriptors  one region each: its holder, its size, and, for the child of a container,
                 the parent and its offset in the parent

At boot each region with a static size is copied from its bank into a DRAM buffer; a container
is copied whole, its children at the same offsets. A region whose static size is 0 is sized at
run time by the service that owns it and keeps its own format in the bank (the user settings are
a journal), so it cannot be written as a plain image.

`ParamFlash` is the flash's parameter banks. Fill it region by region, then take the banks:

    flash = ParamFlash(Layout.from_image(image))   # every bank zero
    flash.put_region(0x11, data)                   # at the region's place, in each of its banks
    banks = flash.images()                         # {address: bytes}, the terminators stamped

A region replaces its whole extent, so where two overlap (a container and its child; regions
0x4f and 0x50, which share their bank's start) the one written last wins.
"""
import struct
from dataclasses import dataclass

LOAD_BASE = 0x10020000  # the image's address = its file offset + this
ENDM = 0x4D444E45  # "ENDM", the terminator word of a bank
FIRMWARE_LO, FIRMWARE_HI = 0x10000000, 0x12960000  # boot block + image: never a parameter bank

CRT_FILE, CRT_VA = 0x2381C14, 0xC1800000  # the initialised-data segment the loader copies
HOLDERS_VA, HOLDER_STRIDE = 0xC185E5FC, 0x2C
DESCS_VA, DESC_STRIDE, DESC_COUNT = 0xC185EBA8, 0x1C, 0x51
HOLDER_COUNT = (DESCS_VA - HOLDERS_VA) // HOLDER_STRIDE  # 33: the descriptors follow the holders


@dataclass(frozen=True)
class Holder:
    index: int
    banks: tuple  # bank A's base, then B's for a dual bank
    size: int
    endm: bool

    @classmethod
    def unpack(cls, index, record):
        a, b, size = struct.unpack_from("<3I", record, 0x00)
        (endm,) = struct.unpack_from("<I", record, 0x20)
        banks = tuple(x for x in (a, b) if x and not FIRMWARE_LO <= x < FIRMWARE_HI)
        return cls(index, banks, size, endm == 1)


@dataclass(frozen=True)
class Region:
    id: int
    offset: int  # in the parent; for a top-level region the holder's base includes it
    size: int  # 0: sized at run time, not a plain image
    holder: int
    parent: int  # 0 for a top-level region

    @classmethod
    def unpack(cls, record):
        rid, offset, _addr, size, _w4, holder, parent = struct.unpack_from("<7I", record)
        return cls(rid, offset, size, holder, parent)


@dataclass(frozen=True)
class Bank:
    """One file of the flash: holders that overlap (a parent spanning its children, two holders
    on one base) share it, since QEMU refuses overlapping ROM regions."""

    base: int
    size: int
    terminators: tuple  # offsets of the "ENDM" words in the bank


@dataclass(frozen=True)
class Layout:
    holders: dict  # index -> Holder, only those with a bank and a size
    regions: dict  # id -> Region

    @classmethod
    def from_image(cls, image):
        def table(va, stride, count):
            off = CRT_FILE + va - CRT_VA
            return [image[off + i * stride : off + (i + 1) * stride] for i in range(count)]

        if len(image) < CRT_FILE + DESCS_VA - CRT_VA + DESC_STRIDE * DESC_COUNT:
            raise ValueError(f"{len(image):#x} bytes: too short for the Z6II master image")
        records = table(HOLDERS_VA, HOLDER_STRIDE, HOLDER_COUNT)
        if struct.unpack_from("<I", records[2])[0] != LOAD_BASE:  # holder 2 is the image itself
            raise ValueError(f"no holder table at {HOLDERS_VA:#x}: not the Z6II master image?")
        holders = [Holder.unpack(i, r) for i, r in enumerate(records)]
        regions = [Region.unpack(r) for r in table(DESCS_VA, DESC_STRIDE, DESC_COUNT)]
        return cls(
            {h.index: h for h in holders if h.banks and h.size},
            {r.id: r for r in regions if r.id},
        )

    def banks(self):
        """The holders' banks, merged where they overlap, in address order."""
        spans = sorted((base, h.size) for h in self.holders.values() for base in h.banks)
        merged = []
        for base, size in spans:
            if merged and base < merged[-1][0] + merged[-1][1]:
                last = merged[-1]
                merged[-1] = (last[0], max(last[1], base + size - last[0]))
            else:
                merged.append((base, size))
        ends = {b + h.size for h in self.holders.values() if h.endm for b in h.banks}
        return [
            Bank(base, size, tuple(sorted(e - 4 - base for e in ends if base < e <= base + size)))
            for base, size in merged
        ]

    def _holder_and_offset(self, rid):
        """A child sits at its offset in its container's holder; a top-level region at its own
        holder's base (a few carry offsets into their holder's span, which their bases include).
        """
        region = self.regions[rid]
        if region.parent:
            if self.regions[region.parent].parent:
                raise ValueError(f"region {rid:#x}: a container's child has a child")
            holder, offset = self.regions[region.parent].holder, region.offset
        else:
            holder, offset = region.holder, 0
        if holder not in self.holders:
            raise KeyError(f"region {rid:#x}: holder {holder} has no flash bank")
        return self.holders[holder], offset

    def place(self, rid):
        """region id -> [(bank base, offset in it)], one per bank of its holder."""
        holder, offset = self._holder_and_offset(rid)
        return [(base, offset) for base in holder.banks]

    def capacity(self, rid):
        """How many bytes of region `rid` its bank holds: its size, or less where the DRAM
        buffer is larger than the bank (region 0x4f)."""
        holder, offset = self._holder_and_offset(rid)
        return max(0, min(self.regions[rid].size, holder.size - offset))


class ParamFlash:
    def __init__(self, layout):
        self.layout = layout
        self.banks = {b.base: bytearray(b.size) for b in layout.banks()}
        self.own_format = set()  # banks written whole by put_bank: they get no terminator

    def _bank_of(self, addr):
        for base, data in self.banks.items():
            if base <= addr < base + len(data):
                return base
        raise KeyError(f"{addr:#x} is in no parameter bank")

    def put(self, addr, data):
        """Write `data` at flash address `addr`, which must lie inside one bank."""
        base = self._bank_of(addr)
        if addr + len(data) > base + len(self.banks[base]):
            raise ValueError(f"{len(data):#x} bytes at {addr:#x} run past bank {base:#x}")
        self.banks[base][addr - base : addr - base + len(data)] = data
        self.own_format.discard(base)  # regions written into it: the bank takes its terminator

    def put_bank(self, base, data):
        """Replace bank `base` whole with `data`, which keeps its own format: no "ENDM" is stamped
        into it (the settings journal ends in its own sessions and erased flash)."""
        if len(data) != len(self.banks[base]):
            raise ValueError(f"bank {base:#x}: {len(data):#x} bytes for {len(self.banks[base]):#x}")
        self.banks[base][:] = data
        self.own_format.add(base)

    def put_region(self, rid, data):
        """Write region `rid`'s image at its place in each of its banks. It replaces the region's
        whole extent: what `data` does not cover becomes zero."""
        capacity = self.layout.capacity(rid)
        if len(data) > capacity:
            raise ValueError(f"region {rid:#x}: {len(data):#x} bytes, its bank holds {capacity:#x}")
        for base, offset in self.layout.place(rid):
            self.put(base + offset, bytes(data) + bytes(capacity - len(data)))

    def put_regions(self, *sources):
        """Write each source's {region: bytes} in turn: a later source wins where two overlap."""
        for source in sources:
            for rid, data in source.items():
                self.put_region(rid, data)

    def images(self):
        """{bank address: bytes}, "ENDM" stamped where each holder asks for it, except into the
        banks of their own format."""
        out = {}
        for bank in self.layout.banks():
            data = bytearray(self.banks[bank.base])
            if bank.base not in self.own_format:
                for t in bank.terminators:
                    struct.pack_into("<I", data, t, ENDM)
            out[bank.base] = bytes(data)
        return out
