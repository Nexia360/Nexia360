#!/usr/bin/env python3
"""Disassemble Xbox 360 dashboard Lua (.lub) - Lua 5.1 bytecode, BIG-ENDIAN.

The console's chunks are built with sizeof(int)=4, sizeof(size_t)=4,
sizeof(Instruction)=4, lua_Number=double, endianness=0 (big).  Stock luac on a
PC writes little-endian, so nothing off the shelf reads these; the header is
checked here rather than assumed.

  lubdump.py Avatars.lub                 every function, constants + code
  lubdump.py Avatars.lub --strings       just the string constants, in order
  lubdump.py Avatars.lub --calls         global/method call targets only
  lubdump.py Avatars.lub --func 3        one prototype by index
"""

import argparse
import struct
import sys

SIGNATURE = b"\x1bLua"

# Lua 5.1 opcodes, in opcode order, with their operand shape.
#   A   = register, B/C = register or RK, Bx/sBx = extended
OPCODES = [
    ("MOVE", "ABC"),      ("LOADK", "ABx"),     ("LOADBOOL", "ABC"),
    ("LOADNIL", "ABC"),   ("GETUPVAL", "ABC"),  ("GETGLOBAL", "ABx"),
    ("GETTABLE", "ABC"),  ("SETGLOBAL", "ABx"), ("SETUPVAL", "ABC"),
    ("SETTABLE", "ABC"),  ("NEWTABLE", "ABC"),  ("SELF", "ABC"),
    ("ADD", "ABC"),       ("SUB", "ABC"),       ("MUL", "ABC"),
    ("DIV", "ABC"),       ("MOD", "ABC"),       ("POW", "ABC"),
    ("UNM", "ABC"),       ("NOT", "ABC"),       ("LEN", "ABC"),
    ("CONCAT", "ABC"),    ("JMP", "AsBx"),      ("EQ", "ABC"),
    ("LT", "ABC"),        ("LE", "ABC"),        ("TEST", "ABC"),
    ("TESTSET", "ABC"),   ("CALL", "ABC"),      ("TAILCALL", "ABC"),
    ("RETURN", "ABC"),    ("FORLOOP", "AsBx"),  ("FORPREP", "AsBx"),
    ("TFORLOOP", "ABC"),  ("SETLIST", "ABC"),   ("CLOSE", "ABC"),
    ("CLOSURE", "ABx"),   ("VARARG", "ABC"),
]

# Which operands are RK (register-or-constant) per opcode name.
RK_B = {"GETTABLE", "SETTABLE", "ADD", "SUB", "MUL", "DIV", "MOD", "POW",
        "CONCAT", "EQ", "LT", "LE"}
RK_C = {"GETTABLE", "SETTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "MOD",
        "POW", "EQ", "LT", "LE"}

BITRK = 1 << 8


class Reader(object):
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def bytes(self, n):
        b = self.data[self.pos:self.pos + n]
        if len(b) != n:
            raise EOFError("chunk truncated at %d" % self.pos)
        self.pos += n
        return b

    def byte(self):
        return self.bytes(1)[0]

    def int(self):
        return struct.unpack(">i", self.bytes(4))[0]

    def size(self):
        return struct.unpack(">I", self.bytes(4))[0]

    def number(self):
        return struct.unpack(">d", self.bytes(8))[0]

    def string(self):
        n = self.size()
        if n == 0:
            return None
        raw = self.bytes(n)
        # The stored length includes the trailing NUL.
        return raw[:-1].decode("latin-1")


class Proto(object):
    def __init__(self):
        self.source = None
        self.line_defined = 0
        self.last_line_defined = 0
        self.nups = 0
        self.numparams = 0
        self.is_vararg = 0
        self.maxstacksize = 0
        self.code = []
        self.constants = []
        self.protos = []
        self.lineinfo = []
        self.locvars = []
        self.upvalues = []
        self.index = 0
        self.parent = None


def read_header(r):
    sig = r.bytes(4)
    if sig != SIGNATURE:
        raise ValueError("not a Lua chunk (got %r)" % sig)
    version = r.byte()
    fmt = r.byte()
    endian = r.byte()
    sizes = (r.byte(), r.byte(), r.byte(), r.byte())
    integral = r.byte()
    if version != 0x51:
        raise ValueError("expected Lua 5.1, got 0x%02X" % version)
    if endian != 0:
        raise ValueError("expected big-endian (0), got %d" % endian)
    if sizes != (4, 4, 4, 8) or integral != 0:
        raise ValueError("unexpected sizes %r integral=%d" % (sizes, integral))
    return {"version": version, "format": fmt}


def read_proto(r, counter, parent=None):
    p = Proto()
    p.parent = parent
    p.index = counter[0]
    counter[0] += 1
    p.source = r.string()
    p.line_defined = r.int()
    p.last_line_defined = r.int()
    p.nups = r.byte()
    p.numparams = r.byte()
    p.is_vararg = r.byte()
    p.maxstacksize = r.byte()

    n = r.int()
    p.code = [r.size() for _ in range(n)]

    n = r.int()
    for _ in range(n):
        t = r.byte()
        if t == 0:
            p.constants.append(None)
        elif t == 1:
            p.constants.append(bool(r.byte()))
        elif t == 3:
            p.constants.append(r.number())
        elif t == 4:
            p.constants.append(r.string())
        else:
            raise ValueError("bad constant tag %d" % t)

    n = r.int()
    p.protos = [read_proto(r, counter, p) for _ in range(n)]

    n = r.int()
    p.lineinfo = [r.int() for _ in range(n)]

    n = r.int()
    for _ in range(n):
        p.locvars.append((r.string(), r.int(), r.int()))

    n = r.int()
    p.upvalues = [r.string() for _ in range(n)]
    return p


def load(data):
    r = Reader(data)
    header = read_header(r)
    counter = [0]
    return header, read_proto(r, counter)


def walk(p):
    yield p
    for child in p.protos:
        for q in walk(child):
            yield q


def decode(ins):
    op = ins & 0x3F
    a = (ins >> 6) & 0xFF
    c = (ins >> 14) & 0x1FF
    b = (ins >> 23) & 0x1FF
    bx = (ins >> 14) & 0x3FFFF
    return op, a, b, c, bx


def const_str(p, k):
    if k >= len(p.constants):
        return "K%d?" % k
    v = p.constants[k]
    if isinstance(v, str):
        return '"%s"' % v.replace("\n", "\\n")
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float) and v.is_integer():
        return "%d" % int(v)
    return repr(v)


def rk(p, x):
    if x & BITRK:
        return const_str(p, x & (BITRK - 1))
    return "R%d" % x


def disasm_line(p, pc, ins):
    op, a, b, c, bx = decode(ins)
    if op >= len(OPCODES):
        return "%-10s %d" % ("BAD(%d)" % op, a)
    name, shape = OPCODES[op]
    sbx = bx - 131071

    if name in ("LOADK", "GETGLOBAL", "SETGLOBAL"):
        return "%-10s R%d %s" % (name, a, const_str(p, bx))
    if name == "CLOSURE":
        return "%-10s R%d proto#%d" % (name, a, bx)
    if shape == "AsBx":
        return "%-10s R%d -> %d" % (name, a, pc + 1 + sbx)
    if name == "SELF":
        return "%-10s R%d R%d %s" % (name, a, b, rk(p, c))
    if name == "CALL":
        return "%-10s R%d in=%d out=%d" % (name, a, b - 1, c - 1)
    if name == "TAILCALL":
        return "%-10s R%d in=%d" % (name, a, b - 1)
    if name == "RETURN":
        return "%-10s R%d n=%d" % (name, a, b - 1)
    ops = ["R%d" % a]
    ops.append(rk(p, b) if name in RK_B else "R%d" % b)
    ops.append(rk(p, c) if name in RK_C else "R%d" % c)
    return "%-10s %s" % (name, " ".join(ops))


def proto_name(p):
    where = "main" if p.parent is None else "proto#%d" % p.index
    return "%s  (line %d-%d, %d param%s%s, %d upval%s)" % (
        where, p.line_defined, p.last_line_defined, p.numparams,
        "" if p.numparams == 1 else "s",
        ", vararg" if p.is_vararg else "", p.nups,
        "" if p.nups == 1 else "s")


def print_proto(p, show_code=True):
    print("=" * 78)
    print(proto_name(p))
    if p.upvalues:
        print("  upvalues: %s" % ", ".join(u or "?" for u in p.upvalues))
    if p.constants:
        print("  constants:")
        for i, v in enumerate(p.constants):
            print("    K%-3d %s" % (i, const_str(p, i)))
    if not show_code:
        return
    print("  code:")
    for pc, ins in enumerate(p.code):
        line = p.lineinfo[pc] if pc < len(p.lineinfo) else 0
        print("    %4d  [%4d]  %s" % (pc, line, disasm_line(p, pc, ins)))


def collect_calls(p):
    """GETGLOBAL/SELF targets: what this prototype reaches out to."""
    out = []
    for pc, ins in enumerate(p.code):
        op, a, b, c, bx = decode(ins)
        name = OPCODES[op][0] if op < len(OPCODES) else None
        if name == "GETGLOBAL":
            out.append(("global", p.constants[bx] if bx < len(p.constants) else "?"))
        elif name == "SELF" and (c & BITRK):
            k = c & (BITRK - 1)
            out.append(("method", p.constants[k] if k < len(p.constants) else "?"))
        elif name == "GETTABLE" and (c & BITRK):
            k = c & (BITRK - 1)
            v = p.constants[k] if k < len(p.constants) else "?"
            if isinstance(v, str):
                out.append(("field", v))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--strings", action="store_true",
                    help="string constants only, in prototype order")
    ap.add_argument("--calls", action="store_true",
                    help="global / method / field names referenced")
    ap.add_argument("--func", type=int, default=None,
                    help="dump one prototype by index")
    ap.add_argument("--no-code", action="store_true",
                    help="constants and upvalues, no disassembly")
    args = ap.parse_args(argv)

    data = open(args.file, "rb").read()
    header, main_proto = load(data)

    protos = list(walk(main_proto))

    if args.strings:
        for p in protos:
            strs = [v for v in p.constants if isinstance(v, str)]
            if strs:
                print("-- %s" % proto_name(p))
                for s in strs:
                    print("   %s" % s)
        return 0

    if args.calls:
        seen = {}
        for p in protos:
            for kind, name in collect_calls(p):
                if isinstance(name, str):
                    seen.setdefault((kind, name), 0)
                    seen[(kind, name)] += 1
        for (kind, name), n in sorted(seen.items()):
            print("%-7s %-40s x%d" % (kind, name, n))
        return 0

    if args.func is not None:
        for p in protos:
            if p.index == args.func:
                print_proto(p, not args.no_code)
                return 0
        print("no prototype #%d (have 0..%d)" % (args.func, len(protos) - 1),
              file=sys.stderr)
        return 1

    print("%s: Lua 5.1 big-endian, %d prototypes, source %s"
          % (args.file, len(protos), main_proto.source))
    for p in protos:
        print_proto(p, not args.no_code)
    return 0


if __name__ == "__main__":
    sys.exit(main())
