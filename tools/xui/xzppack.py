#!/usr/bin/env python3
"""Build a XUIZ (.xzp) package - the container the 360 dashboard's apps use.

The reader is xurview.open_package; this is its inverse, and --verify reads the
result back through that reader so a package is never shipped unchecked.

Layout, big-endian, taken from the dashboard's own packages (soclua, hubapp,
dashlua, Dash.Kiosk.lex's `lua` resource) and confirmed on all four:

    +0x00  "XUIZ"
    +0x04  u32  version, always 3
    +0x08  u32  TOTAL size of the package, header included
    +0x0C  u32  0
    +0x10  u32  directory size in bytes
    +0x14  u16  entry count
    +0x16       directory: per entry  u32 size, u32 offset, u8 name length,
                name bytes (no terminator); offset is relative to the content
                block, which starts at 0x16 + directory size

Names keep the separator the dashboard uses in a package ("de-de\\Strings.xus"),
so a path given on the command line is stored with backslashes.

  xzppack.py out.xzp main.lub Manifest.xml
  xzppack.py out.xzp --root src --recurse
  xzppack.py out.xzp ... --verify
"""

import argparse
import os
import struct
import sys

SIGNATURE = b"XUIZ"
VERSION = 3


def build(entries):
    """entries: [(stored_name, bytes)] -> the package as bytes."""
    directory = bytearray()
    content = bytearray()
    for name, payload in entries:
        encoded = name.encode("latin-1")
        if len(encoded) > 0xFF:
            raise ValueError("name too long for a one-byte length: %s" % name)
        directory += struct.pack(">II", len(payload), len(content))
        directory += bytes([len(encoded)]) + encoded
        content += payload

    total = 0x16 + len(directory) + len(content)
    header = bytearray()
    header += SIGNATURE
    header += struct.pack(">I", VERSION)
    header += struct.pack(">I", total)
    header += struct.pack(">I", 0)
    header += struct.pack(">I", len(directory))
    header += struct.pack(">H", len(entries))
    return bytes(header + directory + content)


def verify(data, entries):
    """Read the package back through the viewer's own parser."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import xurview

    read = xurview.open_package(data)
    if read is None:
        raise SystemExit("verify: not a package")
    if len(read) != len(entries):
        raise SystemExit("verify: %d entries written, %d read"
                         % (len(entries), len(read)))
    for (name, payload), (got_name, offset, size) in zip(entries, read):
        if got_name != name:
            raise SystemExit("verify: name %r != %r" % (got_name, name))
        if size != len(payload):
            raise SystemExit("verify: %s size %d != %d"
                             % (name, size, len(payload)))
        if data[offset:offset + size] != payload:
            raise SystemExit("verify: %s content differs" % name)
    return len(read)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output")
    ap.add_argument("files", nargs="*")
    ap.add_argument("--root", help="directory to take files from")
    ap.add_argument("--recurse", action="store_true",
                    help="with --root, include subdirectories")
    ap.add_argument("--verify", action="store_true",
                    help="read the result back through xurview's parser")
    args = ap.parse_args(argv)

    entries = []
    if args.root:
        root = os.path.abspath(args.root)
        walk = os.walk(root) if args.recurse else [
            (root, [], sorted(os.listdir(root)))]
        for base, _dirs, names in walk:
            for name in sorted(names):
                path = os.path.join(base, name)
                if not os.path.isfile(path):
                    continue
                rel = os.path.relpath(path, root).replace("/", chr(92))
                entries.append((rel, open(path, "rb").read()))
    for path in args.files:
        entries.append((os.path.basename(path), open(path, "rb").read()))

    if not entries:
        raise SystemExit("xzppack: nothing to pack")

    data = build(entries)
    if args.verify:
        verify(data, entries)

    with open(args.output, "wb") as f:
        f.write(data)

    print("%s: %d entries, %d bytes%s"
          % (args.output, len(entries), len(data),
             " (verified)" if args.verify else ""))
    for name, payload in entries:
        print("   %-34s %d" % (name, len(payload)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
