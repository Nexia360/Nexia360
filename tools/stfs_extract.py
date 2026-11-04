#!/usr/bin/env python3
"""Extract an STFS (CON/LIVE/PIRS) package without building the emulator.

    python tools/stfs_extract.py <package> <dest>
    python tools/stfs_extract.py --list <package>

The block and hash-chain math mirrors
src/xenia/vfs/devices/xcontent_devices/stfs_container_device.cc. SVOD packages
are not handled.
"""
import os
import struct
import sys

BLOCK = 0x1000
LEVELS = (170, 28900, 4913000)
END_OF_CHAIN = 0xFFFFFF
HASH_ENTRY = 0x18
DIR_ENTRY = 64

# XContentHeader::header_size, then XContentMetadata at 0x344.
OFF_HEADER_SIZE = 0x340
OFF_VOLUME_TYPE = 0x3A1
OFF_VOLUME_DESCRIPTOR = 0x379


def u24le(b):
    return b[0] | (b[1] << 8) | (b[2] << 16)


def round_up(value, to):
    return (value + to - 1) // to * to


class Stfs:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        magic = self.d[0:4]
        if magic not in (b"CON ", b"LIVE", b"PIRS"):
            raise SystemExit("not an STFS package: %r" % magic)
        if struct.unpack_from(">I", self.d, OFF_VOLUME_TYPE)[0] == 1:
            raise SystemExit("SVOD package, not STFS")

        self.header_size = struct.unpack_from(">I", self.d, OFF_HEADER_SIZE)[0]
        self.data_base = round_up(self.header_size, BLOCK)

        vd = OFF_VOLUME_DESCRIPTOR
        flags = self.d[vd + 2]
        self.read_only = bool(flags & 1)
        self.root_active = bool(flags & 2)
        self.file_table_block_count = struct.unpack_from("<H", self.d, vd + 3)[0]
        self.file_table_block = u24le(self.d[vd + 5:vd + 8])
        self.total_block_count = struct.unpack_from(">I", self.d, vd + 0x1C)[0]

        self.blocks_per_hash_table = 1 if self.read_only else 2
        self.step = (
            LEVELS[0] + self.blocks_per_hash_table,
            LEVELS[1] + (LEVELS[0] + 1) * self.blocks_per_hash_table,
        )
        self.hash_tables = {}

    def block_to_offset(self, index):
        block = index
        for level in range(3):
            base = LEVELS[level]
            block += ((index + base) // base) * self.blocks_per_hash_table
            if index < base:
                break
        return self.data_base + (block << 12)

    def hash_block_number(self, index, level):
        if level == 2:
            return self.step[1]
        if index < LEVELS[level]:
            return 0 if level == 0 else self.step[level - 1]
        block = (index // LEVELS[level]) * self.step[level]
        if level == 0:
            block += ((index // LEVELS[1]) + 1) * self.blocks_per_hash_table
            if index < LEVELS[1]:
                return block
        return block + self.blocks_per_hash_table

    def hash_block_offset(self, index, level):
        return self.data_base + (self.hash_block_number(index, level) << 12)

    def levels_to_check(self):
        for level in range(3):
            if self.total_block_count < LEVELS[level]:
                return level
        return 0

    def block_info(self, index):
        # Each level says which of the two backing blocks the next level down
        # is currently using. A read-only package has one of each, so level 0
        # answers on its own.
        secondary = BLOCK if self.root_active else 0
        levels = self.levels_to_check()
        if self.read_only:
            secondary = 0
            levels = 0
        for level in range(levels, -1, -1):
            offset = self.hash_block_offset(index, level)
            if offset not in self.hash_tables:
                start = offset + secondary
                self.hash_tables[offset] = self.d[start:start + BLOCK]
            table = self.hash_tables[offset]
            if level:
                record = (index // LEVELS[level - 1]) % LEVELS[0]
            else:
                record = index % LEVELS[0]
            info = struct.unpack_from(">I", table, record * HASH_ENTRY + 0x14)[0]
            secondary = BLOCK if info & 0x40000000 else 0
        table = self.hash_tables[self.hash_block_offset(index, 0)]
        record = index % LEVELS[0]
        return struct.unpack_from(">I", table, record * HASH_ENTRY + 0x14)[0]

    def next_block(self, index):
        return self.block_info(index) & 0xFFFFFF

    def entries(self):
        out = []
        block = self.file_table_block
        for _ in range(self.file_table_block_count):
            base = self.block_to_offset(block)
            for slot in range(BLOCK // DIR_ENTRY):
                at = base + slot * DIR_ENTRY
                if self.d[at] == 0:
                    break
                flags = self.d[at + 40]
                out.append({
                    "name": self.d[at:at + (flags & 0x3F)].decode(
                        "cp1252", "replace"),
                    "directory": bool(flags & 0x80),
                    "start": u24le(self.d[at + 47:at + 50]),
                    "parent": struct.unpack_from(">H", self.d, at + 50)[0],
                    "length": struct.unpack_from(">I", self.d, at + 52)[0],
                })
            block = self.next_block(block)
            if block == END_OF_CHAIN:
                break
        return out

    def read(self, entry):
        data = bytearray()
        block = entry["start"]
        remaining = entry["length"]
        while remaining and block != END_OF_CHAIN:
            take = min(BLOCK, remaining)
            offset = self.block_to_offset(block)
            data += self.d[offset:offset + take]
            remaining -= take
            block = self.next_block(block)
        return bytes(data), remaining


def relative_paths(entries):
    paths = []
    for entry in entries:
        parts = [entry["name"]]
        parent = entry["parent"]
        seen = set()
        while parent != 0xFFFF and parent < len(entries) and parent not in seen:
            seen.add(parent)
            parts.append(entries[parent]["name"])
            parent = entries[parent]["parent"]
        clean = [p for p in reversed(parts) if p not in ("", ".", "..")]
        paths.append(os.path.join(*clean) if clean else "")
    return paths


def main(argv):
    listing = argv[1:2] == ["--list"]
    args = argv[2:] if listing else argv[1:]
    if len(args) != (1 if listing else 2):
        raise SystemExit(__doc__)

    stfs = Stfs(args[0])
    print("header_size %X data_base %X read_only %s blocks %d table_blocks %d"
          % (stfs.header_size, stfs.data_base, stfs.read_only,
             stfs.total_block_count, stfs.file_table_block_count))

    entries = stfs.entries()
    failed = 0
    for entry, rel in zip(entries, relative_paths(entries)):
        if not rel:
            continue
        if entry["directory"]:
            if not listing:
                os.makedirs(os.path.join(args[1], rel), exist_ok=True)
            continue
        if listing:
            print("%-60s %9d" % (rel, entry["length"]))
            continue
        target = os.path.join(args[1], rel)
        os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
        data, short = stfs.read(entry)
        with open(target, "wb") as f:
            f.write(data)
        if short:
            failed += 1
        print("%-60s %9d%s" % (rel, len(data), "  SHORT" if short else ""))

    if failed:
        print("%d file(s) came up short - bad hash chain?" % failed)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
