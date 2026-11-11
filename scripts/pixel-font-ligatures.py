#!/usr/bin/env python3
"""Turns off the default ligatures of the pixel font.

Pixelify Sans joins "fi" and "fl" into one glyph that, at the sizes the
interface uses, reads as another letter ("first" looks like "Arst"). Slint
cannot turn a font feature off, so this renames the GSUB feature `liga`,
which shapers apply by default, to `hlig` (historical ligatures), which they
apply only when asked. The glyphs and every other feature stay as they are;
the feature list stays in alphabetical order. The font's license, the SIL
Open Font License 1.1, allows modified versions; it declares no reserved
font name.

Usage: scripts/pixel-font-ligatures.py FONT.ttf...   (changes the files)
       scripts/pixel-font-ligatures.py --check FONT.ttf...
"""
import struct
import sys


def feature_records(data):
    """The offset of each GSUB feature record, with its tag."""
    count = struct.unpack(">H", data[4:6])[0]
    tables = {}
    for i in range(count):
        tag, _, offset, _ = struct.unpack(">4sIII", data[12 + 16 * i : 28 + 16 * i])
        tables[tag] = offset
    if b"GSUB" not in tables:
        return []
    gsub = tables[b"GSUB"]
    feature_list = gsub + struct.unpack(">H", data[gsub + 6 : gsub + 8])[0]
    features = struct.unpack(">H", data[feature_list : feature_list + 2])[0]
    return [(feature_list + 2 + 6 * i, data[feature_list + 2 + 6 * i : feature_list + 6 + 6 * i]) for i in range(features)]


def checksum(data):
    padded = bytes(data) + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(padded) // 4}I", padded)) & 0xFFFFFFFF


def fix_checksums(data):
    """The GSUB table's checksum in the directory, then the whole font's in
    the head table, as the OpenType specification computes them."""
    count = struct.unpack(">H", data[4:6])[0]
    head = None
    for i in range(count):
        entry = 12 + 16 * i
        tag, _, offset, length = struct.unpack(">4sIII", data[entry : entry + 16])
        if tag == b"head":
            head = offset
        if tag == b"GSUB":
            data[entry + 4 : entry + 8] = struct.pack(">I", checksum(data[offset : offset + length]))
    data[head + 8 : head + 12] = b"\0\0\0\0"
    data[head + 8 : head + 12] = struct.pack(">I", (0xB1B0AFBA - checksum(data)) & 0xFFFFFFFF)


def main():
    check = "--check" in sys.argv[1:]
    status = 0
    for path in [arg for arg in sys.argv[1:] if arg != "--check"]:
        data = bytearray(open(path, "rb").read())
        records = feature_records(data)
        tags = [tag for _, tag in records]
        if b"liga" not in tags:
            fixed = bytearray(data)
            fix_checksums(fixed)
            if fixed != data and not check:
                open(path, "wb").write(bytes(fixed))
                print(f"{path}: checksums fixed")
            elif fixed != data:
                print(f"{path}: wrong checksums")
                status = 1
            else:
                print(f"{path}: no default ligatures")
            continue
        if check:
            print(f"{path}: still has default ligatures")
            status = 1
            continue
        for at, tag in records:
            if tag == b"liga":
                data[at : at + 4] = b"hlig"
        renamed = [tag for _, tag in feature_records(data)]
        if renamed != sorted(renamed):
            sys.exit(f"{path}: the feature list would not stay in order")
        fix_checksums(data)
        open(path, "wb").write(bytes(data))
        print(f"{path}: liga renamed to hlig")
    return status


if __name__ == "__main__":
    sys.exit(main())
