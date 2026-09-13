#!/usr/bin/env python3
"""Generates xna_exports_generated.cc from the console native surface.

The surface comes from dump-surface.ps1 as one row per entry point:

    module <TAB> entry <TAB> returnType <TAB> argTypes <TAB> verdict

Two facts decide what a shim may return, and both come out of the assemblies
rather than from judgement:

  * The RETURN TYPE. `Microsoft.Xna.Framework.ErrorCodes` and
    `KernelReturnCode` are error codes where zero means success - the caller
    passes them to ThrowExceptionFromResult and throws on anything else. That
    settles 122 of the 231 entry points outright.
  * The VERDICT, for the ones typed as a bare UInt32, which is what both a
    handle and an error code look like. dump-surface.ps1 reports what the
    CALLER does with the value: compares it against 0xFFFFFFFF (a handle) or
    throws on non-zero (an error).

Where neither is conclusive the entry point keeps FAILING. That is not
timidity: a stub that answers a question it does not understand is how an
unimplemented display-mode enumerator ran until memory was exhausted and looked
exactly like a hang. Failure is the one answer that always terminates a loop.

Entry points implemented by hand elsewhere are detected and skipped, so this can
be re-run whenever another one moves out of here.
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
XNA = os.path.join(ROOT, "src", "xenia", "kernel", "xna")
SURFACE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "surface.tsv")
OUTPUT = os.path.join(XNA, "xna_exports_generated.cc")
DEF_OUTPUT = os.path.join(XNA, "xna_exports.def")

# Exports that are not part of the console surface: Nexia's own entry points,
# found by name from managed code with NativeLibrary.TryGetExport. They are not
# in surface.tsv and never will be, so they are listed here instead of being
# appended to the .def by hand.
NEXIA_EXPORTS = [
    "Nexia_XnaExportUsage",
    "Nexia_XnaPrepareEffect",
    "Nexia_XnaLog",
    "Nexia_XnaStorageRoot",
    "Nexia_XnaDraw",
    "Nexia_XnaDrawUserPrimitives",
    "Nexia_XnaEffectApply",
    "Nexia_XnaReadTitleFile",
    "Nexia_XnaIndexElementSize",
]

# Files carrying hand-written implementations. Anything defined in one of these
# must not be generated as well, or the symbol is defined twice.
HAND_WRITTEN = [
    "xna_exports_impl.cc",
    "xna_exports_gpu.cc",
    "xna_exports_input.cc",
    "xna_exports_content.cc",
]

# Scalars, by the name Cecil reports. Everything else that is not a pointer is
# an enum, which is always 32-bit here.
SCALARS = {
    "System.Void": "void",
    "System.Boolean": "uint32_t",  # marshals as a 4-byte BOOL under Winapi
    "System.Byte": "uint8_t",
    "System.SByte": "int8_t",
    "System.Int16": "int16_t",
    "System.UInt16": "uint16_t",
    "System.Int32": "int32_t",
    "System.UInt32": "uint32_t",
    "System.Int64": "int64_t",
    "System.UInt64": "uint64_t",
    "System.IntPtr": "uint64_t",
    "System.UIntPtr": "uint64_t",
    "System.Single": "float",
    "System.Double": "double",
    "System.String": "const char*",
    "System.Char": "uint16_t",
}

# Pointees worth zeroing when a shim answers with a default. A single scalar
# behind a pointer is an out parameter; a byte or void pointer is a buffer whose
# length is not knowable from the signature, and writing into one blind would
# corrupt whatever the caller passed.
NOT_AN_OUT_PARAMETER = {"System.Void", "System.Byte", "System.SByte", "System.Char"}


def cpp_type(token):
    """The C++ spelling of one Cecil type token from the surface dump."""
    if token == "sb":
        return "char*"  # StringBuilder, marshalled as ANSI with no CharSet set
    if token.startswith("struct:"):
        # By-value structs are passed by hidden reference on x64 above 8 bytes,
        # and in registers below. Nothing generated here reads one, so a
        # pointer is safe either way as long as it is never dereferenced.
        return "const void*"
    for prefix in ("ptr:", "ref:"):
        if token.startswith(prefix):
            inner = token[len(prefix):]
            if inner == "System.Void":
                return "void*"
            return scalar(inner) + "*"
    return scalar(token)


def scalar(name):
    if name in SCALARS:
        return SCALARS[name]
    return "uint32_t"  # an enum


def pointee(token):
    for prefix in ("ptr:", "ref:"):
        if token.startswith(prefix):
            return token[len(prefix):]
    return None


def symbol(module, entry):
    return "xna_" + module + "_" + entry.replace("::", "__")


def implemented_elsewhere():
    found = set()
    pattern = re.compile(r'extern "C"[^(]*?\b(xna_[A-Za-z0-9_]+)\s*\(', re.S)
    for name in HAND_WRITTEN:
        path = os.path.join(XNA, name)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as handle:
            found.update(pattern.findall(handle.read()))
    return found


def main():
    done = implemented_elsewhere()
    rows = []
    seen = set()
    with open(SURFACE, encoding="utf-8") as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 4:
                continue
            key, ret, args, verdict = parts
            module, entry = key.split("!", 1)
            # Imports from real Windows libraries bind to the real thing.
            if module.lower().endswith(".dll"):
                continue
            if key in seen:
                continue
            seen.add(key)
            rows.append((module, entry, ret, args, verdict))

    body = []
    counts = {"void": 0, "error": 0, "handle": 0, "failing": 0, "skipped": 0}

    for module, entry, ret, args, verdict in sorted(rows):
        name = symbol(module, entry)
        if name in done:
            counts["skipped"] += 1
            continue

        tokens = [a for a in args.split(",") if a]
        params = []
        for index, token in enumerate(tokens):
            params.append("{} a{}".format(cpp_type(token), index))
        signature = ", ".join(params) if params else ""

        is_error = ret.endswith("ErrorCodes") or ret.endswith("KernelReturnCode")
        if verdict == "ERROR":
            is_error = True

        if ret == "System.Void":
            kind, return_type = "void", "void"
        elif is_error:
            kind, return_type = "error", "uint32_t"
        elif verdict == "HANDLE":
            kind, return_type = "handle", "uint32_t"
        else:
            kind, return_type = "failing", scalar(ret)
        counts[kind] += 1

        lines = []
        lines.append('extern "C" {} {}({}) {{'.format(return_type, name, signature))

        reporter = "XnaExportDefaulted" if kind != "failing" else "XnaExportUnimplemented"
        lines.append('  xe::kernel::xna::{}("{}!{}");'.format(
            reporter, module, entry.replace("::", "::")))

        if kind in ("void", "error", "handle"):
            for index, token in enumerate(tokens):
                inner = pointee(token)
                if token == "sb":
                    lines.append("  if (a{0}) {{ a{0}[0] = 0; }}".format(index))
                elif inner and inner not in NOT_AN_OUT_PARAMETER:
                    lines.append("  if (a{0}) {{ *a{0} = {1}; }}".format(
                        index, "0" if scalar(inner) != "float" else "0.0f"))

        if kind == "error":
            lines.append("  return 0;")
        elif kind == "handle":
            lines.append("  return xe::kernel::xna::XnaAllocateHandle();")
        elif kind == "failing":
            if return_type == "float":
                lines.append("  return 0.0f;")
            elif return_type == "const char*":
                lines.append("  return nullptr;")
            else:
                lines.append("  return kNotImplemented;")

        lines.append("}")
        body.append("\n".join(lines))

    header = '''/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The console native XNA surface, exported so the console assemblies bind
// straight to this executable.
//
// GENERATED by tools/xna/gen-exports.py from tools/xna/surface.tsv. Do not
// hand-edit: implement an entry point in one of the files beside this one and
// re-run the generator, which detects it and stops emitting it here.
//
// Three answers appear below, and which one an entry point gets is decided by
// the assemblies rather than by judgement:
//
//   DEFAULTED  the return type is ErrorCodes or KernelReturnCode - an error
//              code where zero means success - or the caller was seen passing
//              the result to ThrowExceptionFromResult. Success is reported and
//              scalar out parameters are zeroed, which is what "nothing to
//              report" looks like to the caller. Buffers are left alone: their
//              length is not in the signature, and writing into one blind
//              would corrupt whatever was passed.
//   HANDLE     the caller compares the result against 0xFFFFFFFF and raises
//              OutOfMemoryException, so a live handle is issued.
//   FAILING    neither is conclusive. These keep returning E_NOTIMPL, because
//              failure is the only answer that always terminates a loop - an
//              unimplemented display-mode enumerator that answered "success"
//              once ran until memory was exhausted and looked exactly like a
//              hang.
//
// Every entry point here still reports itself the first time it is reached, so
// a run names precisely which defaults a title actually relied on.
//
// A stub that ignores its arguments is safe on x64: one calling convention,
// arguments in registers, and the caller cleans up - so only the return value
// and any out parameter have to be right.

#include <cstdint>

#include "xenia/kernel/xna/xna_exports.h"

namespace {

// HRESULT-shaped, and zero means success.
constexpr uint32_t kNotImplemented = 0x80004001;  // E_NOTIMPL

}  // namespace

'''

    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(header)
        handle.write("\n\n".join(body))
        handle.write("\n")

    exports = ["    {}={}".format(entry, symbol(module, entry))
               for module, entry, _, _, _ in sorted(rows)]
    exports += ["    {0}={0}".format(name) for name in NEXIA_EXPORTS]
    with open(DEF_OUTPUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("; GENERATED by tools/xna/gen-exports.py from surface.tsv\n"
                     "; and its NEXIA_EXPORTS list. Do not hand-edit.\n"
                     ";\n"
                     "; Maps the console export names - C++-shaped, and not\n"
                     "; legal C identifiers - onto the symbols in\n"
                     "; xna_exports_generated.cc and the hand-written\n"
                     "; implementations beside it.\n"
                     "EXPORTS\n")
        handle.write("\n".join(exports))
        handle.write("\n")

    total = sum(counts.values())
    print("generated {} entry points from {} rows".format(
        total - counts["skipped"], total))
    for kind in ("error", "handle", "void", "failing", "skipped"):
        print("  {:<8} {}".format(kind, counts[kind]))


if __name__ == "__main__":
    main()
