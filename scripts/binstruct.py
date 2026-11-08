# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Binary layouts declared field by field: a type, `[count]` for an array, `@ offset` to place it.

    DEGC16 = scaled(U32, 16)
    TEMP = Tagged("TEMP", 1, i2c_limit=DEGC16[3] @ 0x24, thermistor_limit=U32[3] @ 0x30)
    TEMP(i2c_limit=(68, 68, 67))     # -> bytes; a field not given stays zero

A record refuses overlapping fields, a field it does not declare and a value that does not fit,
each with a `LayoutError` naming the field.
An array value is a sequence (at most `count` long) or a sparse {index: value}.
A scaled type takes the value in its unit: `scaled(U32, 192)` stores 1.0 as 192.

`T.leaves(buf, offset, value)` reads back what `value` would write: one (path, given, found, same)
per scalar it sets, with `found` in the same unit.
"""

import struct


class LayoutError(ValueError):
    pass


class Type:
    size = 0

    def __getitem__(self, count):
        return Array(self, count)

    def __matmul__(self, offset):
        return Field(self, offset)


class Scalar(Type):
    def __init__(self, fmt, scale=1):
        self.fmt, self.scale = "<" + fmt, scale
        self.size = struct.calcsize(self.fmt)

    def pack_into(self, buf, offset, value, path=""):
        try:
            struct.pack_into(self.fmt, buf, offset, round(value * self.scale))
        except (struct.error, TypeError) as e:
            raise LayoutError(f"{path or 'value'}: {value!r} does not fit ({e})") from e

    def leaves(self, buf, offset, value, path=""):
        (raw,) = struct.unpack_from(self.fmt, buf, offset)
        found = raw if self.scale == 1 else raw / self.scale
        yield path, value, found, round(value * self.scale) == raw


U8, U16, U32 = Scalar("B"), Scalar("H"), Scalar("I")


def scaled(t, scale):
    """`t`, holding its value in units of 1/scale."""
    return Scalar(t.fmt[1:], scale)


class Chars(Type):
    """A NUL-padded ASCII string."""

    def __init__(self, size):
        self.size = size

    def pack_into(self, buf, offset, value, path=""):
        if not isinstance(value, bytes) or len(value) > self.size:
            raise LayoutError(f"{path or 'value'}: {value!r} is not bytes of at most {self.size}")
        buf[offset : offset + len(value)] = value

    def leaves(self, buf, offset, value, path=""):
        found = bytes(buf[offset : offset + self.size]).rstrip(b"\0")
        yield path, value, found, found == value.rstrip(b"\0")


class Array(Type):
    def __init__(self, elem, count):
        self.elem, self.count = elem, count
        self.size = elem.size * count

    def _items(self, value, path):
        if isinstance(value, dict):
            items = list(value.items())
        elif isinstance(value, (list, tuple, bytes)):
            items = list(enumerate(value))
        else:
            raise LayoutError(
                f"{path or 'value'}: an array takes a sequence or a dict, not {value!r}"
            )
        for i, _ in items:
            if not 0 <= i < self.count:
                raise LayoutError(f"{path}[{i}]: past the end of an array of {self.count}")
        return items

    def pack_into(self, buf, offset, value, path=""):
        for i, v in self._items(value, path):
            self.elem.pack_into(buf, offset + i * self.elem.size, v, f"{path}[{i}]")

    def leaves(self, buf, offset, value, path=""):
        for i, v in self._items(value, path):
            yield from self.elem.leaves(buf, offset + i * self.elem.size, v, f"{path}[{i}]")


class Field:
    def __init__(self, type_, offset):
        self.type, self.offset = type_, offset
        self.end = offset + type_.size


class Record(Type):
    def __init__(self, size=None, **fields):
        spans = sorted((f.offset, f.end, name) for name, f in fields.items())
        for (_, end, a), (start, _, b) in zip(spans, spans[1:]):
            if start < end:
                raise LayoutError(f"fields {a} and {b} overlap")
        extent = max((f.end for f in fields.values()), default=0)
        if size is not None and extent > size:
            raise LayoutError(f"fields run to {extent:#x}, past the record's {size:#x}")
        self.fields, self.size = fields, extent if size is None else size

    def _field(self, name, path):
        if name not in self.fields:
            raise LayoutError(f"{path}.{name}: no such field")
        return self.fields[name]

    def pack_into(self, buf, offset, value, path=""):
        if not isinstance(value, dict):
            raise LayoutError(f"{path or 'value'}: a record takes a dict, not {value!r}")
        for name, v in value.items():
            field = self._field(name, path)
            field.type.pack_into(buf, offset + field.offset, v, f"{path}.{name}")

    def leaves(self, buf, offset, value, path=""):
        for name, v in value.items():
            field = self._field(name, path)
            yield from field.type.leaves(buf, offset + field.offset, v, f"{path}.{name}")

    def values(self, **values):
        """The values this record writes for `values`: for a plain record, `values` itself."""
        return values

    def __call__(self, **values):
        buf = bytearray(self.size)
        self.pack_into(buf, 0, self.values(**values))
        return bytes(buf)


class Tagged(Record):
    """A region that starts with its 16-byte ASCII name and a format version."""

    def __init__(self, name, version, **fields):
        super().__init__(tag=Chars(16) @ 0, version=U32 @ 0x10, **fields)
        self.tag, self.version = name.encode(), version

    def values(self, **values):
        if "tag" in values or "version" in values:
            raise LayoutError(f"{self.tag.decode()}: the tag and version are the declaration's")
        return {"tag": self.tag, "version": self.version, **values}
