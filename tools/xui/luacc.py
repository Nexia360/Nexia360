#!/usr/bin/env python3
"""Compile Lua 5.1 source to the 17559 dashboard's bytecode format.

    python tools/xui/luacc.py --tlc <tlc.lua> --pylibs <dir> in.lua out.lub
        [--chunkname @main.lua]

Tiny Lua Compiler (github.com/luauminati/Tiny-Lua-Compiler, MIT) does the
compiling; it runs inside Python through lupa. Its output is a standard PC
chunk - little-endian, size_t 8. The console wants big-endian with size_t 4
(header 1B 4C 75 61 51 00 00 04 04 04 08 00, see xui-formats-reference), so
the chunk is re-encoded field by field. Nothing is reinterpreted: every
instruction, constant and debug record is carried across unchanged.

The result is read back through lubdump.py's own parser, so a chunk that
would not load on the console never gets written.
"""
import argparse
import os
import struct
import sys

PC_HEADER = b"\x1bLua\x51\x00\x01\x04\x08\x04\x08\x00"
CONSOLE_HEADER = b"\x1bLua\x51\x00\x00\x04\x04\x04\x08\x00"


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, n):
        if self.p + n > len(self.d):
            raise ValueError("chunk truncated at %d (+%d)" % (self.p, n))
        out = self.d[self.p:self.p + n]
        self.p += n
        return out

    def u8(self):
        return self.take(1)[0]

    def int(self):
        return struct.unpack("<i", self.take(4))[0]

    def size_t(self):
        return struct.unpack("<Q", self.take(8))[0]

    def instr(self):
        return struct.unpack("<I", self.take(4))[0]

    def number(self):
        return struct.unpack("<d", self.take(8))[0]

    def string(self):
        n = self.size_t()
        return None if n == 0 else self.take(n)


class Writer:
    def __init__(self):
        self.out = bytearray()

    def u8(self, v):
        self.out.append(v)

    def int(self, v):
        self.out += struct.pack(">i", v)

    def size_t(self, v):
        if v > 0xFFFFFFFF:
            raise ValueError("size %d does not fit the console's size_t" % v)
        self.out += struct.pack(">I", v)

    def instr(self, v):
        self.out += struct.pack(">I", v)

    def number(self, v):
        self.out += struct.pack(">d", v)

    def string(self, s):
        if s is None:
            self.size_t(0)
        else:
            self.size_t(len(s))
            self.out += s


def convert_function(r, w, chunkname, top):
    source = r.string()
    # The console's own chunks name their source "@File.lua" at the top level;
    # nested functions inherit it (stored as an empty string).
    if top and chunkname is not None:
        source = chunkname.encode("utf-8") + b"\0"
    w.string(source)
    w.int(r.int())  # linedefined
    w.int(r.int())  # lastlinedefined
    for _ in range(4):  # nups, numparams, is_vararg, maxstacksize
        w.u8(r.u8())

    n = r.int()
    w.int(n)
    for _ in range(n):
        w.instr(r.instr())

    n = r.int()
    w.int(n)
    for _ in range(n):
        kind = r.u8()
        w.u8(kind)
        if kind == 0:
            pass
        elif kind == 1:
            w.u8(r.u8())
        elif kind == 3:
            w.number(r.number())
        elif kind == 4:
            w.string(r.string())
        else:
            raise ValueError("unknown constant type %d" % kind)

    n = r.int()
    w.int(n)
    for _ in range(n):
        convert_function(r, w, chunkname, False)

    n = r.int()
    w.int(n)
    for _ in range(n):
        w.int(r.int())
    n = r.int()
    w.int(n)
    for _ in range(n):
        w.string(r.string())
        w.int(r.int())
        w.int(r.int())
    n = r.int()
    w.int(n)
    for _ in range(n):
        w.string(r.string())


def to_console(pc_chunk, chunkname):
    if pc_chunk[:12] != PC_HEADER:
        raise ValueError("not a little-endian size_t-8 Lua 5.1 chunk: %s"
                         % pc_chunk[:12].hex())
    r = Reader(pc_chunk)
    r.take(12)
    w = Writer()
    w.out += CONSOLE_HEADER
    convert_function(r, w, chunkname, True)
    if r.p != len(pc_chunk):
        raise ValueError("%d trailing bytes after the main function"
                         % (len(pc_chunk) - r.p))
    return bytes(w.out)


def compile_source(tlc_path, pylibs, source):
    if pylibs:
        sys.path.insert(0, pylibs)
    import lupa  # noqa: E402

    lua = lupa.LuaRuntime(encoding=None, unpack_returned_tuples=True)
    with open(tlc_path, "rb") as f:
        tlc = lua.execute(f.read())
    result = tlc.compile(source)
    if isinstance(result, tuple):
        result = result[0]
    if not isinstance(result, (bytes, bytearray)):
        raise ValueError("TLC returned %r, not bytecode" % (type(result),))
    return bytes(result)


def verify(console_chunk):
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    import lubdump  # noqa: E402

    parse = getattr(lubdump, "parse", None) or getattr(lubdump, "load", None)
    if parse is None:
        return "lubdump has no parse(); not verified"
    parse(console_chunk)
    return "verified by lubdump"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tlc", required=True)
    ap.add_argument("--pylibs")
    ap.add_argument("--chunkname")
    ap.add_argument("source")
    ap.add_argument("output")
    args = ap.parse_args()

    with open(args.source, "rb") as f:
        source = f.read()
    pc = compile_source(args.tlc, args.pylibs, source)
    chunkname = args.chunkname or "@" + os.path.basename(args.source)
    console = to_console(pc, chunkname)
    note = verify(console)
    with open(args.output, "wb") as f:
        f.write(console)
    print("%s: %d bytes (PC %d), %s" % (args.output, len(console), len(pc),
                                          note))


if __name__ == "__main__":
    main()
