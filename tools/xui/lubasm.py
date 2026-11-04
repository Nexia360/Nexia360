#!/usr/bin/env python3
"""Emit Lua 5.1 BIG-ENDIAN bytecode - the form the 360 dashboard will load.

Why this exists: the dashboard's Lua will NOT take source. dash.xex carries
the parser's strings, so lua_load's source path is compiled in, but a chunk of
plain text handed to the app loader raised and the guest kernel stopped with
KeBugCheck(0x000000F7) - Lua's panic - even when the chunk called nothing at
all and could only fail at load. The console's own .lub files are all LuaQ with
endianness 0, so precompiled is what the loader wants.

Stock luac writes little-endian, and a big-endian Lua 5.1 compiler is not
something to go and find, so this emits the chunk directly. It is NOT a Lua
compiler - it builds one shape: a chunk that defines global functions whose
bodies return a constant. That is exactly what a PAM app's Main.lub is
(Dash.Kiosk.lex's is 233 bytes: two requires and one global assignment), and
it is enough to answer whether a chunk of ours loads at all.

Verify what it writes with lubdump.py, which reads the console's own files.

Format, from the console's chunks and confirmed by round-tripping:

  header  1B 4C 75 61 | 51 | 00 format | 00 big-endian
          04 int | 04 size_t | 04 Instruction | 08 lua_Number | 00 non-integral
  proto   source (size_t length incl NUL, or 0 for none), linedefined,
          lastlinedefined, nups, numparams, is_vararg, maxstacksize,
          code (int count + instructions), constants (int count + tagged),
          protos (int count), lineinfo (int count), locvars (int count),
          upvalues (int count)
  insn    OP bits 0-5, A 6-13, C 14-22, B 23-31, Bx 14-31
"""

import struct
import sys

# Opcodes used here; the full table is in lubdump.py.
OP_MOVE = 0
OP_LOADK = 1
OP_LOADBOOL = 2
OP_LOADNIL = 3
OP_GETGLOBAL = 5
OP_GETTABLE = 6
OP_SETGLOBAL = 7
OP_JMP = 22
OP_TEST = 26
OP_CALL = 28
OP_RETURN = 30
OP_CLOSURE = 36

# An RK operand is a register below 256 and a constant at or above it.
BITRK = 1 << 8

# Constant tags.
T_NIL, T_BOOL, T_NUMBER, T_STRING = 0, 1, 3, 4

# is_vararg flags (lobject.h): declaring ISVARARG lets a function be called
# with more arguments than it names, which is what we want when every body
# ignores its arguments.
VARARG_ISVARARG = 2


def iABC(op, a, b, c):
    return (op & 0x3F) | ((a & 0xFF) << 6) | ((c & 0x1FF) << 14) | \
           ((b & 0x1FF) << 23)


def iABx(op, a, bx):
    return (op & 0x3F) | ((a & 0xFF) << 6) | ((bx & 0x3FFFF) << 14)


def iAsBx(op, a, sbx):
    """JMP and the for-loops: Bx holds sBx biased by 131071."""
    return iABx(op, a, sbx + 131071)


def lstring(text):
    if text is None:
        return struct.pack(">I", 0)
    raw = text.encode("latin-1") + b"\0"
    return struct.pack(">I", len(raw)) + raw


def constant(value):
    if value is None:
        return bytes([T_NIL])
    if isinstance(value, bool):
        return bytes([T_BOOL, 1 if value else 0])
    if isinstance(value, (int, float)):
        return bytes([T_NUMBER]) + struct.pack(">d", float(value))
    if isinstance(value, str):
        return bytes([T_STRING]) + lstring(value)
    raise TypeError("not a Lua constant: %r" % (value,))


class Proto(object):
    def __init__(self, maxstack=2, is_vararg=VARARG_ISVARARG):
        self.code = []
        self.constants = []
        self.protos = []
        self.maxstack = maxstack
        self.is_vararg = is_vararg

    def k(self, value):
        for i, existing in enumerate(self.constants):
            if type(existing) is type(value) and existing == value:
                return i
        self.constants.append(value)
        return len(self.constants) - 1

    def encode(self):
        out = bytearray()
        out += lstring(None)                       # source: none, like theirs
        out += struct.pack(">ii", 0, 0)            # line defined / last
        out += bytes([0, 0, self.is_vararg, self.maxstack])  # nups, params
        out += struct.pack(">i", len(self.code))
        for instruction in self.code:
            out += struct.pack(">I", instruction & 0xFFFFFFFF)
        out += struct.pack(">i", len(self.constants))
        for value in self.constants:
            out += constant(value)
        out += struct.pack(">i", len(self.protos))
        for child in self.protos:
            out += child.encode()
        out += struct.pack(">i", 0)                # lineinfo
        out += struct.pack(">i", 0)                # locvars
        out += struct.pack(">i", 0)                # upvalues
        return bytes(out)


class Call(object):
    """Marks a body that calls a dotted global instead of returning a
    constant. See calls()."""

    def __init__(self, path, argument=None):
        self.path = path
        self.argument = argument


def returns(value):
    """A function body that returns one constant, or nothing for Ellipsis."""
    if isinstance(value, Probe):
        return probe(value.path, value.argument)
    if isinstance(value, TryCalls):
        return attempts(value.path, value.arguments)
    if isinstance(value, Call):
        return calls(value.path, value.argument)
    body = Proto()
    if value is Ellipsis:
        body.code.append(iABC(OP_RETURN, 0, 1, 0))          # return
        return body
    if value is None:
        body.code.append(iABC(OP_LOADNIL, 0, 0, 0))
    elif isinstance(value, bool):
        body.code.append(iABC(OP_LOADBOOL, 0, 1 if value else 0, 0))
    else:
        body.code.append(iABx(OP_LOADK, 0, body.k(value)))
    body.code.append(iABC(OP_RETURN, 0, 2, 0))              # return R0
    return body


def calls(path, argument):
    """A body that returns the result of calling a dotted global with one
    constant argument - `return Xbox.XuiScene.Create("Foo.xur")`.

    That is what a PAM app's Main does: Dash.Kiosk.lex's is
    `Main = function() return InstallScene.Create() end`, and returning the
    scene is how the app gets a picture on screen.

        GETGLOBAL R0, "Xbox"
        GETTABLE  R0, R0, K"XuiScene"      ; C is RK, so the constant bit is set
        GETTABLE  R0, R0, K"Create"
        LOADK     R1, "Foo.xur"
        CALL      R0, B=2, C=2             ; one argument, one result
        RETURN    R0, B=2
    """
    body = Proto(maxstack=2)
    head, rest = path[0], path[1:]
    body.code.append(iABx(OP_GETGLOBAL, 0, body.k(head)))
    for name in rest:
        body.code.append(iABC(OP_GETTABLE, 0, 0, BITRK | body.k(name)))
    argc = 0
    if argument is not None:
        body.code.append(iABx(OP_LOADK, 1, body.k(argument)))
        argc = 1
    body.code.append(iABC(OP_CALL, 0, argc + 1, 2))
    body.code.append(iABC(OP_RETURN, 0, 2, 0))
    return body


class TryCalls(object):
    """Marks a body that tries several calls and returns the first that gives
    a truthy result. See attempts()."""

    def __init__(self, path, arguments):
        self.path = path
        self.arguments = list(arguments)


def attempts(path, arguments):
    """`for each arg: r = f(arg); if r then return r end; return nil`

    One unknown per build cycle is the slow way to answer a question that has a
    handful of candidate answers - this asks all of them at once and the caller
    finds out which worked by what ends up on screen.

        <produce R0 = f(arg)>
        TEST   R0, C=1      ; falsy -> skip the RETURN and try the next one
        RETURN R0, B=2
        ... repeat ...
        RETURN R0, B=1      ; nothing worked

    Lua 5.1 TEST A C is `if boolean(R(A)) != C then PC++`, so C=1 skips the
    return exactly when the call came back nil or false.
    """
    body = Proto(maxstack=3)
    head, rest = path[0], path[1:]
    for argument in arguments:
        # pcall, because the callee RAISES on a bad argument rather than
        # returning nil - measured: six bare attempts produced one thrown
        # hr=0x8A020007 and never reached the second. Without pcall this is a
        # single test wearing a disguise.
        #
        #   pcall(Xbox.XuiScene.Create, "<path>")  -> R0 = ok, R1 = result
        #   TEST R0, C=0    ; truthy -> skip the JMP and return R1
        #   JMP  next
        #   MOVE R0, R1
        #   RETURN R0
        body.code.append(iABx(OP_GETGLOBAL, 0, body.k("pcall")))
        body.code.append(iABx(OP_GETGLOBAL, 1, body.k(head)))
        for name in rest:
            body.code.append(iABC(OP_GETTABLE, 1, 1, BITRK | body.k(name)))
        body.code.append(iABx(OP_LOADK, 2, body.k(argument)))
        body.code.append(iABC(OP_CALL, 0, 3, 3))
        body.code.append(iABC(OP_TEST, 0, 0, 0))
        body.code.append(iAsBx(OP_JMP, 0, 2))
        body.code.append(iABC(OP_MOVE, 0, 1, 0))
        body.code.append(iABC(OP_RETURN, 0, 2, 0))
    body.code.append(iABC(OP_RETURN, 0, 1, 0))
    return body


class Probe(object):
    """Marks a body that reports what Create() gave back, via error().

    Three indirect signals in a row said the wrong thing: "no error box" meant
    the pcall path returned ZERO VALUES rather than nil; "the xzp resource was
    read again" is invalid because add_package caches it; and an absent box is
    not success. error() is not indirect - the dashboard puts the message
    straight in the box, which is how ": 8A020007" was readable.
    """

    def __init__(self, path, argument):
        self.path = path
        self.argument = argument


def probe(path, argument):
    """Report Create()'s result through a call the EMULATOR logs.

    Neither route out of Lua works: a plain error() message never appears
    anywhere, and an App.Errors-shaped `hr=... msg=...` does not either - the
    dashboard pcalls Main, throws the error away and keeps the 0x80B04014 it
    stored before the call. So the signal has to leave Lua through something
    xam implements and we print.

    Xbox.User.GetGamertag(n) lands in xam as XamUserGetGamerTag and our shim
    logs its arguments, so the USER INDEX carries the answer:

        index 3 -> Create returned a scene
        index 5 -> Create returned nil

    Neither is a real user index; nothing is expected to come back. Grep the
    log for XamUserGetGamerTag(00000003 / 00000005).

         0 GETGLOBAL R1 Xbox / GETTABLE XuiScene / GETTABLE Create
         3 LOADK     R2 <path>
         4 CALL      R1                    ; R1 = scene or nil
         5 TEST      R1, C=0               ; truthy -> skip the JMP
         6 JMP       -> nil branch
         7 GetGamertag(3)                  ; OK
        12 JMP       -> end
        13 GetGamertag(5)                  ; NIL
        18 MOVE R0, R1 / RETURN R0         ; hand back whatever Create gave
    """
    body = Proto(maxstack=4)
    head, rest = path[0], path[1:]

    def signal(index):
        body.code.append(iABx(OP_GETGLOBAL, 2, body.k("Xbox")))
        body.code.append(iABC(OP_GETTABLE, 2, 2, BITRK | body.k("User")))
        body.code.append(iABC(OP_GETTABLE, 2, 2, BITRK | body.k("GetGamertag")))
        body.code.append(iABx(OP_LOADK, 3, body.k(float(index))))
        body.code.append(iABC(OP_CALL, 2, 2, 1))

    body.code.append(iABx(OP_GETGLOBAL, 1, body.k(head)))
    for name in rest:
        body.code.append(iABC(OP_GETTABLE, 1, 1, BITRK | body.k(name)))
    body.code.append(iABx(OP_LOADK, 2, body.k(argument)))
    body.code.append(iABC(OP_CALL, 1, 2, 2))
    body.code.append(iABC(OP_TEST, 1, 0, 0))
    body.code.append(iAsBx(OP_JMP, 0, 6))
    signal(3)
    body.code.append(iAsBx(OP_JMP, 0, 5))
    signal(5)
    body.code.append(iABC(OP_MOVE, 0, 1, 0))
    body.code.append(iABC(OP_RETURN, 0, 2, 0))
    return body


def chunk(globals_, requires=()):
    """globals_: [(name, value)] -> the whole chunk as bytes.

    Each name becomes a global function returning that value; Ellipsis means a
    body that returns nothing, and None one that returns nil.

    `requires` are run at load, before the assignments:

        GETGLOBAL R0, "require"
        LOADK     R1, "Xbox.Xui"
        CALL      R0, B=2, C=1        ; one argument, no results

    Not optional despite an earlier round proving a chunk that calls nothing
    loads cleanly: `Xbox` is not in the globals until something requires it, so
    without these a body calling Xbox.XuiScene.Create indexes nil and Main
    hands back nothing. Dash.Kiosk.lex's Main.lub opens with exactly two of
    these, and hubapp's with four.
    """
    main = Proto()
    for module in requires:
        main.code.append(iABx(OP_GETGLOBAL, 0, main.k("require")))
        main.code.append(iABx(OP_LOADK, 1, main.k(module)))
        main.code.append(iABC(OP_CALL, 0, 2, 1))
    for name, value in globals_:
        index = len(main.protos)
        main.protos.append(returns(value))
        main.code.append(iABx(OP_CLOSURE, 0, index))
        main.code.append(iABx(OP_SETGLOBAL, 0, main.k(name)))
    main.code.append(iABC(OP_RETURN, 0, 1, 0))

    header = bytes([0x1B]) + b"Lua" + bytes([0x51, 0x00, 0x00,
                                             4, 4, 4, 8, 0])
    return header + main.encode()


def main(argv=None):
    argv = argv if argv is not None else sys.argv[1:]
    if not argv:
        print(__doc__)
        return 1
    out = argv[0]

    # The PAM entry points, from App.Main.lub's global assignments. Main is the
    # one that matters - Dash.Kiosk.lex's returns a scene - and nil is the
    # honest answer until there is a scene to return.
    # Main returns the scene, the way Dash.Kiosk.lex's does. Everything else
    # is the neutral reply so that whichever global the dashboard reaches for
    # next answers instead of erroring.
    data = chunk([
        ("SetAppConfig", True),
        # Every candidate alias in ONE build. Each A<n>.xur names itself on
        # screen, so whichever heading appears IS the answer - no inference
        # from the log, which has now lied three times (a missing box is not
        # success, a re-read section is not a resolve, and a tracer the native
        # binding rejects before the export is not a measurement).
        ("Main", TryCalls(("Xbox", "XuiScene", "Create"), [
            "ui#A1.xur",
            "nexia.probe#A2.xur",
            "UI#A3.xur",
            "A4.xur",
            "xzp#A5.xur",
            "Nexia.Probe#A6.xur",
        ])),
        ("SetNuiEnabled", Ellipsis),
        ("IsNuiEnabled", False),
        ("GetUserIndex", 0),
        ("IsCurrentUserOnline", False),
        ("EnterLRM", Ellipsis),
        ("ContinueEnterLRM", Ellipsis),
        ("OnLeaveLRM", Ellipsis),
        ("GetAppName", "Nexia.Probe"),
    # EXACTLY Dash.Kiosk.lex's Main.lub: Xbox.Xui and Xbox.XuiBase, nothing
    # else. Xbox.XuiScene is NOT required by name anywhere in the console's own
    # apps - XuiBase pulls the class chain in, and `Xbox.XuiScene` is then
    # reached as a table field. A require of a name the loader cannot map
    # raises at LOAD, which leaves Main undefined and surfaces as the dash's
    # 0x80B04014 on frame 0 - indistinguishable from a scene that would not
    # open, which is what sent the last three builds after the wrong thing.
    ], requires=("Xbox.Xui", "Xbox.XuiBase"))

    with open(out, "wb") as f:
        f.write(data)
    print("%s: %d bytes" % (out, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
