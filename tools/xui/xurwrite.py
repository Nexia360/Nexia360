#!/usr/bin/env python3
"""Author a .xur scene - the inverse of xurview's reader.

Everything structural comes from xurview: the class chains, the property order
and the property types are read out of xam_ui_new.cc by xurview.Classes, the
same parser the viewer uses, so a scene written here cannot be encoded against
a table the reader disagrees with. Every scene is parsed back through
xurview.Scene before it is written out, and any complaint the reader would make
is a hard failure here.

Format, all big-endian, from xurview's reader:

  header   "XUIB" | u32 version=8 | u32 0 | u16 kind=0x0E | u32 total size
           | u16 section count | 12 packed pool hints
  sections u32 offset and u32 length per { char tag[4] }, offsets from "XUIB"
  STRN     u32 bytes-that-follow | u16 count | NUL-terminated strings
  VECT     f32 x3 each      FLOT  f32 each      COLR  u32 each
  DATA     the object stream

  object   packed class-name index | u8 flags
           flags&1: packed total property count, then ONE PACKED MASK PER
                    CLASS LEVEL, base first, values for set bits in order
           flags&2: packed child count, then that many objects
  value    1 bool=u8, 2/3 packed, 4 FLOT index, 5 STRN index (1-based),
           6 COLR index, 7 VECT index, 8/10 packed

A property whose flags have bit 0 is indexed: the value is preceded by a packed
count. Nothing here writes one.

  xurwrite.py out.xur --demo
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xurview

OBJECT_HAS_PROPERTIES = 0x01
OBJECT_HAS_CHILDREN = 0x02


def packed(value):
    """PackedReader's inverse: <0xF0 one byte, <0x1000 two, else 0xFF + u32."""
    if value < 0xF0:
        return bytes([value])
    if value < 0x1000:
        return bytes([0xF0 | (value >> 8), value & 0xFF])
    return bytes([0xFF]) + struct.pack(">I", value)


class Pools(object):
    """The shared tables a scene's values are indexed out of."""

    def __init__(self):
        self.strings = []
        self.floats = []
        self.vectors = []
        self.colors = []

    def string(self, text):
        # 1-based: index 0 is how the format says "no string".
        if text is None:
            return 0
        if text not in self.strings:
            self.strings.append(text)
        return self.strings.index(text) + 1

    def _pool(self, table, value):
        if value not in table:
            table.append(value)
        return table.index(value)

    def flot(self, value):
        return self._pool(self.floats, float(value))

    def vect(self, value):
        return self._pool(self.vectors, tuple(float(v) for v in value))

    def colr(self, value):
        return self._pool(self.colors, int(value) & 0xFFFFFFFF)


class Element(object):
    def __init__(self, class_name, children=None, **properties):
        self.class_name = class_name
        self.properties = properties
        self.children = list(children or [])


def encode_value(pools, prop, value):
    kind = prop["type"]
    if kind == 1:
        return bytes([1 if value else 0])
    if kind in (2, 3, 8, 10):
        return packed(int(value))
    if kind == 4:
        return packed(pools.flot(value))
    if kind == 5:
        return packed(pools.string(value))
    if kind == 6:
        return packed(pools.colr(value))
    if kind == 7:
        return packed(pools.vect(value))
    raise SystemExit("xurwrite: property '%s' has type %d, which this writer "
                     "does not encode" % (prop["name"], kind))


def encode_object(classes, pools, element):
    chain = classes.chain(element.class_name)
    if not chain:
        raise SystemExit("xurwrite: no class table for '%s' - add it to "
                         "xam_ui_new.cc first" % element.class_name)

    remaining = dict(element.properties)
    levels = []
    written = 0
    for level in chain:
        mask = 0
        body = bytearray()
        for i, prop in enumerate(level["props"]):
            if prop["name"] not in remaining:
                continue
            if prop["flags"] & 1:
                raise SystemExit("xurwrite: '%s' is indexed; not supported"
                                 % prop["name"])
            mask |= 1 << i
            body += encode_value(pools, prop, remaining.pop(prop["name"]))
            written += 1
        levels.append((mask, bytes(body)))
    if remaining:
        raise SystemExit(
            "xurwrite: '%s' has no property %s in its class chain"
            % (element.class_name, ", ".join(sorted(remaining))))

    flags = 0
    if written:
        flags |= OBJECT_HAS_PROPERTIES
    if element.children:
        flags |= OBJECT_HAS_CHILDREN

    out = bytearray()
    out += packed(pools.string(element.class_name))
    out += bytes([flags])
    if written:
        # The count the file states is the TOTAL across every level; the
        # emulator ignores it and reads one mask per level, and xurview
        # compares the two as its class-table check.
        out += packed(written)
        for mask, body in levels:
            out += packed(mask)
            out += body
    if element.children:
        out += packed(len(element.children))
        for child in element.children:
            out += encode_object(classes, pools, child)
    return bytes(out)


def build(root, classes):
    pools = Pools()
    # The class names are strings too, and the root's is the first one the
    # reader will ask for, so pool them before anything else touches the table.
    data = encode_object(classes, pools, root)

    sections = []
    if pools.strings:
        body = b"".join(s.encode("latin-1") + b"\0" for s in pools.strings)
        sections.append(("STRN",
                         struct.pack(">IH", len(body), len(pools.strings)) + body))
    if pools.vectors:
        sections.append(("VECT", b"".join(struct.pack(">fff", *v)
                                          for v in pools.vectors)))
    if pools.floats:
        sections.append(("FLOT", b"".join(struct.pack(">f", f)
                                          for f in pools.floats)))
    if pools.colors:
        sections.append(("COLR", b"".join(struct.pack(">I", c)
                                          for c in pools.colors)))
    sections.append(("DATA", data))

    # 12 pool hints. The emulator reads them and does not depend on them, so
    # the honest value is what each pool actually holds, and zero for the ones
    # this writer never fills.
    hints = [len(pools.strings), len(pools.vectors), 0, 0, 0,
             len(pools.floats), len(pools.colors), 0, 0, 0, 0, 0]
    hint_bytes = b"".join(packed(h) for h in hints)

    prefix = 0x14 + len(hint_bytes) + 12 * len(sections)
    table = bytearray()
    offset = prefix
    for tag, body in sections:
        table += tag.encode("latin-1") + struct.pack(">II", offset, len(body))
        offset += len(body)
    total = offset

    header = bytearray()
    header += b"XUIB"
    header += struct.pack(">I", 8)          # version
    header += struct.pack(">I", 0)          # reserved
    header += struct.pack(">H", 0x0E)       # kind: a scene
    header += struct.pack(">I", total)
    header += struct.pack(">H", len(sections))
    return bytes(header) + hint_bytes + bytes(table) + \
        b"".join(body for _tag, body in sections)


def verify(data, classes):
    """Parse it back with the reader. Any note it raises is a failure."""
    scene = xurview.Scene(data, classes)
    if scene.notes:
        for note in scene.notes:
            print("  ! %s" % note, file=sys.stderr)
        raise SystemExit("xurwrite: the reader rejected what we wrote")
    return scene


# --------------------------------------------------------------------------
# Cell placement
#
# One call per thing on screen. A cell is a XuiText or, when it is given an
# image, a XuiImage - the two leaves a screen is actually made of - and every
# argument maps to a real property in the class tables, so anything expressible
# here is something the console will draw.
#
# Not a tile: the Epix slot format gives you a name, a caption and one picture
# in a position the TEMPLATE chooses. This places anything anywhere.
# --------------------------------------------------------------------------

WHITE = 0xFFFFFFFF
DIM = 0xFFB8C8B0
GREEN = 0xFF92C83E
SHADOW = 0x80202020
CLEAR = 0x00000000

# XUI_FONT_STYLE_*, from the XDK's xui.h. The dashboard's own scenes use 272
# (LEFT_ALIGN | NO_WORDWRAP) and 16656 (that plus ELLIPSIS), which is what a
# single-line row wants.
STYLE_DROPSHADOW = 0x0001
STYLE_ITALIC = 0x0002
STYLE_BOLD = 0x0004
STYLE_UNDERLINE = 0x0008
STYLE_NO_WORDWRAP = 0x0010
STYLE_SINGLE_LINE = 0x0020
STYLE_LEFT = 0x0100
STYLE_RIGHT = 0x0200
STYLE_CENTER = 0x0400
STYLE_VCENTER = 0x1000
STYLE_ELLIPSIS = 0x4000

STYLE_ROW = STYLE_LEFT | STYLE_NO_WORDWRAP | STYLE_ELLIPSIS

# XUI_IMAGE_SIZE_MODE_*: how a picture fills its box.
SIZE_STRETCH = 0
SIZE_CENTER = 1
SIZE_SCALE = 16


def cell(name, text=None, color=WHITE, dropshadow=True,
         x=0.0, y=0.0, w=None, h=None,
         image=None, tint=None, size_mode=None,
         point_size=None, font=None, style=None,
         opacity=None, show=None, anchor=None, element_id=None):
    """Place one thing on a screen.

      name          the element's Id, and the text when none is given
      text          what it reads; None means use name, "" means an empty box
      color         TextColor, 0xAARRGGBB
      dropshadow    emit DropShadowColor (the bool; shadow colour is fixed)
      x, y          Position
      w, h          Width, Height
      image         ImagePath - giving one makes this a XuiImage, not a XuiText
      tint          ColorFactor, an image's colour multiplier
      size_mode     SIZE_STRETCH / SIZE_CENTER / SIZE_SCALE
      point_size    font size
      font          font name, e.g. "Segoe Light"
      style         TextStyle, the STYLE_* flags above
      opacity       0..1
      show          False hides it without removing it
      anchor        edge anchoring
      element_id    Id, when it should differ from name
    """
    props = {"Id": element_id or name}
    if w is not None:
        props["Width"] = float(w)
    if h is not None:
        props["Height"] = float(h)
    props["Position"] = (float(x), float(y), 0.0)
    if opacity is not None:
        props["Opacity"] = float(opacity)
    if show is not None:
        props["Show"] = bool(show)
    if anchor is not None:
        props["Anchor"] = int(anchor)

    if image is not None:
        props["ImagePath"] = image
        if size_mode is not None:
            props["SizeMode"] = int(size_mode)
        if tint is not None:
            props["ColorFactor"] = int(tint)
        return Element("XuiImage", **props)

    props["Text"] = name if text is None else text
    props["TextColor"] = int(color)
    # A text element with no font has nothing to rasterise with, and every
    # scene the console ships names one - "Segoe Light" in
    # meavatarslotscene.xur. Ours had none, which is the likeliest reason a
    # scene that Create() accepts still draws nothing.
    props["Font"] = font if font is not None else "Segoe Light"
    if dropshadow:
        props["DropShadowColor"] = SHADOW
    if point_size is not None:
        props["PointSize"] = float(point_size)
    if style is not None:
        props["TextStyle"] = int(style)
    return Element("XuiText", **props)


def group(name, cells, x=0.0, y=0.0, w=None, h=None, show=None):
    """Cells that move together. XuiGroup adds nothing of its own, so this is
    purely the transform and the Show of everything inside it."""
    props = {"Id": name, "Position": (float(x), float(y), 0.0)}
    if w is not None:
        props["Width"] = float(w)
    if h is not None:
        props["Height"] = float(h)
    if show is not None:
        props["Show"] = bool(show)
    return Element("XuiGroup", children=cells, **props)


def screen(name, cells, width=1280.0, height=720.0):
    """A whole screen: the canvas the file needs at its root, and the scene
    the app's Main hands back."""
    scene = Element("XuiScene", Id=name, Width=float(width),
                    Height=float(height), Position=(0.0, 0.0, 0.0),
                    children=list(cells))
    return Element("XuiCanvas", Width=float(width), Height=float(height),
                   children=[scene])


def from_description(description):
    """Build a screen from plain data - a dict of the same argument names.

    This is what makes a screen CONTENT: the same file can be authored by
    hand, generated, or one day served by the hub, without anything here
    knowing what it is for.

        {"name": "NexiaProbeScene", "width": 1280, "height": 720,
         "cells": [{"name": "Heading", "text": "...", "x": 96, "y": 84}]}
    """
    def build(entry):
        entry = dict(entry)
        kind = entry.pop("kind", "cell")
        if kind == "group":
            children = [build(c) for c in entry.pop("cells", [])]
            return group(cells=children, **entry)
        if "style" in entry and isinstance(entry["style"], str):
            entry["style"] = sum(
                globals()["STYLE_" + part.strip().upper()]
                for part in entry["style"].split("|"))
        for key in ("color", "tint"):
            if isinstance(entry.get(key), str):
                entry[key] = int(entry[key], 0)
        return cell(**entry)

    return screen(description.get("name", "Scene"),
                  [build(c) for c in description.get("cells", [])],
                  description.get("width", 1280),
                  description.get("height", 720))


def demo_scene():
    """A friends panel: heading, rows, footer. Built with cell() so the demo
    and anything authored later go through the same code."""
    cells = [
        cell("Heading", "Nexia Friends", x=96, y=84, w=900, h=64,
             point_size=40, style=STYLE_ROW),
        cell("Subheading", "Served from the hub, drawn by our own scene",
             color=DIM, x=96, y=146, w=900, h=36, point_size=18,
             style=STYLE_ROW),
    ]
    for i, (who, detail) in enumerate([
            ("unrevoked", "Playing Minecraft"),
            ("Dink", "Online"),
            ("MrFrags", "Online"),
            ("Rosaparks", "Last seen 2 hours ago"),
            ("Yeppers", "Offline"),
    ]):
        top = 196.0 + i * 62.0
        cells.append(cell("Row%dName" % i, who, x=96, y=top, w=560, h=44,
                          point_size=24, style=STYLE_ROW))
        cells.append(cell("Row%dDetail" % i, detail, color=DIM, x=680,
                          y=top + 4, w=460, h=40, point_size=19,
                          style=STYLE_ROW))
    cells.append(cell("Footer", "B  Back", color=DIM, x=96, y=622, w=400,
                      h=36, point_size=18, style=STYLE_ROW))
    return screen("NexiaProbeScene", cells)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output")
    ap.add_argument("--source", default=xurview.DEFAULT_SOURCE,
                    help="xam_ui_new.cc to take the class tables from")
    ap.add_argument("--demo", action="store_true",
                    help="write the built-in panel scene")
    ap.add_argument("--from", dest="source_json",
                    help="build the screen from a JSON description")
    ap.add_argument("--print", dest="show", action="store_true",
                    help="print the tree the reader gets back")
    args = ap.parse_args(argv)

    if args.source_json:
        import json
        with open(args.source_json, "r", encoding="utf-8") as f:
            root = from_description(json.load(f))
    elif args.demo:
        root = demo_scene()
    else:
        raise SystemExit("xurwrite: nothing to write (try --demo or --from)")

    classes = xurview.Classes(args.source)
    data = build(root, classes)
    scene = verify(data, classes)

    with open(args.output, "wb") as f:
        f.write(data)
    print("%s: %d bytes, %d string(s), verified by the reader"
          % (args.output, len(data), len(scene.strings)))
    if args.show:
        xurview.print_tree(scene.root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
