#!/usr/bin/env python3
"""Offline viewer for XUI packages (.xzp) and scenes (.xur).

Why this exists: a XUR's object stream is a packed byte stream with no
self-describing structure. Each object's property block is sized by the
property TABLES of its class, so a class whose table does not match the file
desynchronises the reader - and everything read after that point comes out of
the middle of something else. Inside the emulator that shows up as a scene
with the wrong number of children and no way to see why.

So this reads the format the same way src/xenia/kernel/xam/xam_ui_new.cc does,
and it takes the class tables FROM THAT FILE rather than keeping its own copy.
A viewer with its own tables could disagree with the emulator and be useless
for exactly the bug it is meant to find.

  xurview.py vk.xzp                     list a package
  xurview.py vk.xzp KeyboardMain.xur    dump one scene out of a package
  xurview.py KeyboardMain.xur           dump a loose scene
  xurview.py vk.xzp KeyboardMain.xur --trace
                                        show every read, with byte offsets and
                                        the class levels each property block
                                        consumed - this is what identifies a
                                        desync
  xurview.py vk.xzp KeyboardMain.xur --hex
                                        hex dump of the DATA section as well
"""

import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SOURCE = os.path.normpath(
    os.path.join(HERE, "..", "..", "src", "xenia", "kernel", "xam",
                 "xam_ui_new.cc"))

TYPE_NAMES = {
    0: "none", 1: "bool", 2: "int", 3: "uint", 4: "float", 5: "string",
    6: "color", 7: "vector", 8: "quat", 9: "object", 10: "custom",
}
TYPE_VALUES = {
    "kNone": 0, "kBool": 1, "kInteger": 2, "kUnsigned": 3, "kFloat": 4,
    "kString": 5, "kColor": 6, "kVector": 7, "kQuaternion": 8, "kObject": 9,
    "kCustom": 10,
}

# --------------------------------------------------------------------------
# The class tables, lifted out of the emulator's own source.
# --------------------------------------------------------------------------

class Classes:
    def __init__(self, source_path):
        with open(source_path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        self.arrays = {}      # kFoo_Properties -> [property, ...]
        self.table_array = {} # kFoo_Table -> kFoo_Properties
        self.classes = {}     # name -> {"base": str, "props": [property, ...]}
        self._read_arrays(text)
        self._read_tables(text)
        self._read_classes(text)
        self._link_subtables()

    def _read_arrays(self, text):
        pattern = re.compile(
            r"static const Property (\w+)\[\]\s*=\s*\{(.*?)\n\};", re.S)
        entry = re.compile(
            r'\{\s*"([^"]*)"\s*,\s*PropertyType::(\w+)\s*,\s*'
            r'(0[xX][0-9a-fA-F]+|\d+)\s*,\s*(nullptr|&(\w+))\s*\}', re.S)
        for name, body in pattern.findall(text):
            props = []
            for prop_name, type_name, flags, _, sub in entry.findall(body):
                props.append({
                    "name": prop_name,
                    "type": TYPE_VALUES.get(type_name, 0),
                    "flags": int(flags, 0),
                    "sub_table": sub or None,
                    "sub": None,
                })
            self.arrays[name] = props

    def _read_tables(self, text):
        pattern = re.compile(
            r"static const PropertyTable (\w+)\s*=\s*\{\s*(\w+)\s*,\s*(\d+)",
            re.S)
        for table, array, _count in pattern.findall(text):
            self.table_array[table] = array

    def _read_classes(self, text):
        block = re.search(r"static const XuiClass kClasses\[\]\s*=\s*\{(.*?)\n\};",
                          text, re.S)
        if not block:
            raise SystemExit("xurview: kClasses[] not found in " + DEFAULT_SOURCE)
        entry = re.compile(
            r'\{\s*"([^"]*)"\s*,\s*(?:"([^"]*)"|nullptr)\s*,\s*'
            r'\{\s*(?:(\w+)\s*,\s*(\d+)|nullptr\s*,\s*0)\s*\}\s*\}', re.S)
        for name, base, array, count in entry.findall(block.group(1)):
            props = []
            if array:
                # The count in kClasses wins: a class may expose only the
                # first N of a shared array.
                props = self.arrays.get(array, [])[:int(count)]
            self.classes[name] = {"base": base or None, "props": props}

    def _link_subtables(self):
        for props in self.arrays.values():
            for prop in props:
                if prop["sub_table"]:
                    array = self.table_array.get(prop["sub_table"])
                    prop["sub"] = self.arrays.get(array)

    def chain(self, name):
        """Base class first, exactly as ClassChain() orders it."""
        chain = []
        current = self.classes.get(name)
        seen = set()
        while current is not None:
            key = id(current)
            if key in seen:
                break
            seen.add(key)
            chain.append(current)
            current = self.classes.get(current["base"]) if current["base"] else None
        chain.reverse()
        return chain

    def known(self, name):
        return name in self.classes

# --------------------------------------------------------------------------
# Containers
# --------------------------------------------------------------------------

def open_package(data):
    """XUIZ -> {name: (offset, size)} in file order."""
    base = data.find(b"XUIZ")
    if base < 0:
        return None
    version = struct.unpack_from(">I", data, base + 4)[0]
    if version != 3:
        raise SystemExit("xurview: unsupported package version %d" % version)
    dir_size = struct.unpack_from(">I", data, base + 16)[0]
    count = struct.unpack_from(">H", data, base + 20)[0]
    content = base + 22 + dir_size
    at = base + 22
    entries = []
    for _ in range(count):
        size, offset = struct.unpack_from(">II", data, at)
        name_length = data[at + 8]
        at += 9
        name = data[at:at + name_length].decode("latin-1")
        at += name_length
        entries.append((name, content + offset, size))
    return entries


class Reader:
    """PackedReader, byte for byte."""

    def __init__(self, data):
        self.data = data
        self.at = 0
        self.failed = False

    def eof(self):
        return self.at >= len(self.data)

    def byte(self):
        if self.at >= len(self.data):
            self.failed = True
            return 0
        value = self.data[self.at]
        self.at += 1
        return value

    def uint32(self):
        if self.at + 4 > len(self.data):
            self.failed = True
            self.at = len(self.data)
            return 0
        value = struct.unpack_from(">I", self.data, self.at)[0]
        self.at += 4
        return value

    def packed(self):
        lead = self.byte()
        if lead == 0xFF:
            return self.uint32()
        if lead >= 0xF0:
            return ((lead & 0x0F) << 8) | self.byte()
        return lead


OBJECT_HAS_PROPERTIES = 0x01
OBJECT_HAS_CHILDREN = 0x02
OBJECT_HAS_TIMELINES = 0x04
OBJECT_IS_SHARED = 0x08


class Scene:
    SCENE_HEADER_SIZE = 20
    POOL_COUNT = 12

    def __init__(self, data, classes, trace=False):
        self.classes = classes
        self.trace = trace
        self.notes = []
        self.sections = {}
        self.strings = []
        self.floats = []
        self.colors = []
        self.vectors = []
        self.pool = {}
        self.root = None
        self._load(data)

    def _say(self, text):
        if self.trace:
            print(text)

    def _load(self, data):
        base = data.find(b"XUIB")
        if base < 0:
            raise SystemExit("xurview: not a XUR (no XUIB)")
        version = struct.unpack_from(">I", data, base + 4)[0]
        kind = struct.unpack_from(">H", data, base + 0x0C)[0]
        total = struct.unpack_from(">I", data, base + 0x0E)[0]
        count = struct.unpack_from(">H", data, base + 0x12)[0]
        self.version, self.kind, self.total, self.section_count = (
            version, kind, total, count)
        if version != 8 or kind != 0x0E:
            self.notes.append("unsupported scene %d/%d" % (version, kind))
        if base + total != len(data):
            self.notes.append(
                "size %d does not match %d" % (total, len(data) - base))

# 12 packed pool hints sit between the header and the section table.
        header = Reader(data[base + self.SCENE_HEADER_SIZE:])
        self.pool_hints = [header.packed() for _ in range(self.POOL_COUNT)]
        at = base + self.SCENE_HEADER_SIZE + header.at
        self.section_list = []
        for _ in range(count):
            tag = data[at:at + 4].decode("latin-1")
            offset, length = struct.unpack_from(">II", data, at + 4)
            at += 12
            self.section_list.append((tag, offset, length))
            self.sections[tag] = data[base + offset:base + offset + length]

        self._load_strings()
        self._load_numbers()

        body = self.sections.get("DATA")
        if body is None:
            raise SystemExit("xurview: no DATA section")
        self.data_size = len(body)
        reader = Reader(body)
        self.root = self._object(reader, 0)
        self.trailing = len(body) - reader.at
        if reader.failed:
            self.notes.append("object stream overran")
        if self.trailing:
            self.notes.append(
                "%d byte(s) of DATA left unread - the reader lost sync"
                % self.trailing)

    def _load_strings(self):
        section = self.sections.get("STRN")
        if not section or len(section) < 6:
            return
        count = struct.unpack_from(">H", section, 4)[0]
        at = 6
        while at < len(section) and len(self.strings) < count:
            end = section.find(b"\0", at)
            if end < 0:
                end = len(section)
            self.strings.append(section[at:end].decode("latin-1"))
            at = end + 1

    def _load_numbers(self):
        section = self.sections.get("FLOT")
        if section:
            for at in range(0, len(section) - 3, 4):
                self.floats.append(struct.unpack_from(">f", section, at)[0])
        section = self.sections.get("VECT")
        if section:
            for at in range(0, len(section) - 11, 12):
                self.vectors.append(struct.unpack_from(">fff", section, at))
        section = self.sections.get("COLR")
        if section:
            for at in range(0, len(section) - 3, 4):
                self.colors.append(struct.unpack_from(">I", section, at)[0])

    def string_at(self, index):
        if not index or index > len(self.strings):
            return "<str %d/%d>" % (index, len(self.strings))
        return self.strings[index - 1]

    # -- the object stream --------------------------------------------------

    def _value(self, reader, prop):
        kind = prop["type"]
        if kind == 1:
            return reader.byte() != 0
        if kind in (2, 3):
            return reader.packed()
        if kind == 4:
            index = reader.packed()
            return self.floats[index] if index < len(self.floats) else 0.0
        if kind == 5:
            return self.string_at(reader.packed())
        if kind == 6:
            index = reader.packed()
            return self.colors[index] if index < len(self.colors) else 0
        if kind == 7:
            index = reader.packed()
            return self.vectors[index] if index < len(self.vectors) else None
        if kind in (8, 10):
            return reader.packed()
        if kind == 9:
            return self._nested(reader, prop)
        reader.failed = True
        return None

    def _nested(self, reader, prop):
        index = reader.packed()
        if index in self.pool:
            return {"shared": index}
        reader.packed()
        values = {}
        if prop["sub"]:
            values = self._block(reader, prop["sub"])
        self.pool[index] = values
        return values

    def _block(self, reader, props):
        values = {}
        if reader.eof():
            return values
        mask = reader.packed()
        for i, prop in enumerate(props):
            if not (mask & (1 << i)):
                continue
            count = reader.packed() if (prop["flags"] & 1) else 1
            items = [self._value(reader, prop) for _ in range(count)]
            values[prop["name"]] = items if count != 1 else items[0]
        return values

    def _properties(self, reader, class_name, depth):
        values = {}
        chain = self.classes.chain(class_name)
        if not chain:
            # Nothing consumed. The next read is taken as the child count,
            # which is how an unknown class derails the whole stream.
            self._say("%s  ! no class table for '%s' - 0 masks read"
                      % ("  " * depth, class_name))
            return values, 0
        for level in chain:
            if reader.eof():
                break
            start = reader.at
            mask = reader.packed()
            if not mask:
                self._say("%s  level %-22s mask=0 (@%d)"
                          % ("  " * depth, self._level_name(level), start))
                continue
            taken = []
            for i, prop in enumerate(level["props"]):
                if not (mask & (1 << i)):
                    continue
                count = reader.packed() if (prop["flags"] & 1) else 1
                items = [self._value(reader, prop) for _ in range(count)]
                values[prop["name"]] = items if count != 1 else items[0]
                taken.append(prop["name"])
            self._say("%s  level %-22s mask=%#x -> %s (@%d..%d)"
                      % ("  " * depth, self._level_name(level), mask,
                         ", ".join(taken) or "-", start, reader.at))
              # A mask with bits above the table's property count means the
              # table is SHORTER than the one the file was written against.
            if level["props"] and mask >> len(level["props"]):
                self.notes.append(
                    "'%s': mask %#x has bits past the %d propert(ies) in our "
                    "table - the class table is out of date"
                    % (self._level_name(level), mask, len(level["props"])))
        return values, len(chain)

    def _level_name(self, level):
        for name, entry in self.classes.classes.items():
            if entry is level:
                return name
        return "?"

    def _track(self, reader):
        head = reader.byte()
        depth = head & 0x7F
        if depth:
            reader.byte()  # levels
            reader.byte()  # property
            for _ in range(1, depth):
                reader.byte()  # path
        if head & 0x80:
            reader.packed()  # data
        return head

    def _timelines(self, reader, has_children):
        """Objects with flags & 0x04 carry this. Skipping it desynchronises
        everything after, which is why it is read in full even though nothing
        here displays it."""
        named_count = reader.packed()
        if named_count:
            reader.packed()  # base index into the shared NAME run
        if not has_children:
            return 0
        count = reader.packed()
        for _ in range(count):
            if reader.eof():
                break
            reader.packed()  # target string
            tracks = reader.packed()
            for _ in range(tracks):
                if reader.eof():
                    break
                self._track(reader)
            reader.packed()  # key count
            reader.packed()  # key block
        return count

    def _object(self, reader, depth):
        start = reader.at
        class_index = reader.packed()
        flags = reader.byte()
        name = self.string_at(class_index)
        node = {
            "class": name, "flags": flags, "offset": start,
            "properties": {}, "children": [], "shared": None,
            "known": self.classes.known(name),
        }
        self._say("%s@%-5d class=%-24s flags=%02X"
                  % ("  " * depth, start, name, flags))
        if flags & OBJECT_HAS_PROPERTIES:
            declared = reader.packed()
            node["declared_property_count"] = declared
            props, _levels = self._properties(reader, name, depth)
            node["properties"] = props
            # The count the file states is the TOTAL number of properties in
            # the block, across every class level. The emulator ignores it and
            # reads one mask per level instead, which works out to the same
            # thing only while our class tables match the ones the file was
            # authored against - so comparing the two is the cheapest possible
            # check that a class table is still correct.
            if declared != len(props):
                self.notes.append(
                    "@%d '%s': file declares %d propert(ies), we read %d - "
                    "the class table for this class is wrong"
                    % (start, name, declared, len(props)))
        elif flags & OBJECT_IS_SHARED:
            node["shared"] = reader.packed()
        if flags & OBJECT_HAS_CHILDREN:
            at = reader.at
            count = reader.packed()
            node["child_count"] = count
            self._say("%s  children=%d (@%d)" % ("  " * depth, count, at))
            for _ in range(count):
                if reader.eof():
                    self.notes.append(
                        "@%d '%s': ran out of DATA with children left"
                        % (start, name))
                    break
                node["children"].append(self._object(reader, depth + 1))
        if flags & OBJECT_HAS_TIMELINES:
            at = reader.at
            node["timelines"] = self._timelines(reader, bool(node["children"]))
            self._say("%s  timelines=%d (@%d..%d)"
                      % ("  " * depth, node["timelines"], at, reader.at))
        return node

# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------

def describe(value):
    if isinstance(value, float):
        return "%g" % value
    if isinstance(value, str):
        return '"%s"' % value
    if isinstance(value, dict):
        return "{%s}" % ", ".join(sorted(value))
    if isinstance(value, tuple):
        return "(%s)" % ", ".join("%g" % v for v in value)
    return str(value)


def print_tree(node, depth=0, show_properties=True):
    mark = "" if node["known"] else "   <- no class table"
    head = "%s%s" % ("  " * depth, node["class"])
    print("%-52s flags=%02X props=%d children=%d%s"
          % (head, node["flags"], len(node["properties"]),
             len(node["children"]), mark))
    if show_properties:
        for key in sorted(node["properties"]):
            print("%s  . %s = %s"
                  % ("  " * depth, key, describe(node["properties"][key])))
    for child in node["children"]:
        print_tree(child, depth + 1, show_properties)


def flatten(node, depth=0, ox=0.0, oy=0.0, out=None, path="0"):
    """Absolute boxes for every node that has a size.

    Position is relative to the parent in XUI, so it accumulates down the
    tree. A node with no Width/Height of its own (a scene, a group) still
    contributes its offset to its children."""
    if out is None:
        out = []
    props = node["properties"]

    def number(name, fallback=0.0):
        value = props.get(name, fallback)
        return float(value) if isinstance(value, (int, float)) else fallback

    position = props.get("Position")
    px, py = (position[0], position[1]) if isinstance(position, tuple) else (0.0, 0.0)
    x, y = ox + px, oy + py
    width, height = number("Width"), number("Height")
    out.append({
        "path": path,
        "depth": depth,
        "cls": node["class"],
        "id": props.get("Id", ""),
        "x": x, "y": y, "w": width, "h": height,
        "shown": props.get("Show", True) is not False,
        "known": node["known"],
        "props": {k: describe(v) for k, v in sorted(props.items())},
    })
    for i, child in enumerate(node["children"]):
        flatten(child, depth + 1, x, y, out, "%s.%d" % (path, i))
    return out


HTML_HEAD = """<!doctype html>
<meta charset="utf-8">
<title>{title}</title>
<style>
:root{{--bg:#15171c;--panel:#1d2026;--edge:#2c313a;--fg:#d6dae2;--dim:#7e8794;
--acc:#4ec9b0;--warn:#e0a458;--hidden:#a065a0}}
*{{box-sizing:border-box}}
body{{margin:0;font:13px/1.5 Consolas,Menlo,monospace;background:var(--bg);
color:var(--fg)}}
/* One column, PICTURE FIRST. A side-by-side layout put a fixed-width tree
   in front of an 852px canvas, so in any pane narrower than the two together
   the drawing was pushed off the right-hand edge and the page looked
   empty. */
#side{{border-top:1px solid var(--edge);background:var(--panel);padding:8px 0}}
header{{padding:10px 14px;border-bottom:1px solid var(--edge);
background:var(--panel)}}
header b{{color:var(--acc);font-size:15px}}
header span{{color:var(--dim);margin-left:10px}}
#stage{{padding:16px;overflow:auto}}
#canvas{{position:relative;background:#0c0e11;outline:1px solid var(--edge);
background-image:linear-gradient(#ffffff08 1px,transparent 1px),
linear-gradient(90deg,#ffffff08 1px,transparent 1px);background-size:40px 40px}}
.box{{position:absolute;border:1px solid var(--acc);color:var(--acc);
font-size:10px;overflow:hidden;background:#4ec9b012}}
.box.hidden{{border-style:dashed;border-color:var(--hidden);
color:var(--hidden);background:#a065a018}}
.box.unknown{{border-color:var(--warn);color:var(--warn);background:#e0a45812}}
.box>span{{display:block;padding:1px 3px;white-space:nowrap}}
.box:hover{{outline:2px solid #fff;z-index:99;background:#ffffff22}}
details{{padding:1px 12px}}
details>summary{{cursor:pointer;white-space:pre;list-style:none}}
details>summary::-webkit-details-marker{{display:none}}
details:hover>summary{{background:#ffffff0d}}
.cls{{color:var(--acc)}}
.hid .cls{{color:var(--hidden)}}
.unk .cls{{color:var(--warn)}}
.id{{color:var(--dim)}}
.kv{{padding:2px 0 6px 0;color:var(--dim)}}
.kv b{{color:var(--fg);font-weight:normal}}
#legend{{padding:8px 14px;color:var(--dim);border-top:1px solid var(--edge)}}
#legend i{{font-style:normal;padding:0 4px}}
#notes{{padding:8px 14px;color:var(--warn)}}
</style>
<header><b>{title}</b><span>{subtitle}</span></header>
<div id="stage"><div id="canvas" style="width:{width:g}px;height:{height:g}px">
"""

HTML_MID = """  </div></div>
  <div id="legend">
    <i style="color:#4ec9b0">&#9632;</i>visible
    <i style="color:#a065a0">&#9632;</i>Show=False
    <i style="color:#e0a458">&#9632;</i>no class table
    &nbsp;&mdash;&nbsp; hover a box for its properties, or open a row below
  </div>
<div id="side">
"""

HTML_TAIL = """</div>{notes}
"""


def escape(text):
    return (str(text).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def to_html(scene, title, subtitle, path):
    """A STATIC page.

    Everything is real markup - no script builds the content. These get looked
    at through viewers and previews that do not run JavaScript, and a page
    that assembles itself in JS shows up blank in every one of them."""
    nodes = flatten(scene.root)
    width = height = 0.0
    for node in nodes:
        width = max(width, node["x"] + node["w"])
        height = max(height, node["y"] + node["h"])
    width = width or 852.0
    height = height or 480.0

    # Drawing first, tree second - see the layout note in the stylesheet.
    out = [HTML_HEAD.format(title=escape(title), subtitle=escape(subtitle),
                            width=width, height=height)]
    for node in nodes:
        if node["w"] <= 0 or node["h"] <= 0:
            continue
        kind = ("box" + ("" if node["shown"] else " hidden")
                + ("" if node["known"] else " unknown"))
        tip = "%s%s  %gx%g at (%g,%g)\n%s" % (
            node["cls"], " #" + node["id"] if node["id"] else "",
            node["w"], node["h"], node["x"], node["y"],
            "\n".join("%s = %s" % (k, v) for k, v in node["props"].items()))
        out.append(
            '    <div class="%s" style="left:%gpx;top:%gpx;width:%gpx;'
            'height:%gpx" title="%s"><span>%s</span></div>\n'
            % (kind, node["x"], node["y"], node["w"], node["h"],
               escape(tip), escape(node["id"] or node["cls"])))

    out.append(HTML_MID)
    for node in nodes:
        kind = ("" if node["shown"] else " hid") + ("" if node["known"] else " unk")
        head = ("  " * node["depth"] + '<span class="cls">%s</span>'
                % escape(node["cls"]))
        if node["id"]:
            head += ' <span class="id">#%s</span>' % escape(node["id"])
        rows = "".join(
            "<div>%s = <b>%s</b></div>" % (escape(k), escape(v))
            for k, v in node["props"].items())
        out.append(
            '<details class="%s"><summary>%s</summary><div class="kv">'
            '<div>%g x %g at (%g, %g)</div>%s</div></details>\n'
            % (kind.strip(), head, node["w"], node["h"], node["x"], node["y"],
               rows or "<div>no properties</div>"))

    notes = ""
    if scene.notes:
        notes = '\n  <div id="notes">! ' + "<br>! ".join(
            escape(n) for n in scene.notes) + "</div>"
    out.append(HTML_TAIL.format(notes=notes))

    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(out))
    return len(nodes)


def hexdump(data, base=0):
    for at in range(0, len(data), 16):
        row = data[at:at + 16]
        print("%04X  %-47s  %s"
              % (base + at, " ".join("%02X" % b for b in row),
                 "".join(chr(b) if 32 <= b < 127 else "." for b in row)))


def main():
    parser = argparse.ArgumentParser(
        description="Offline viewer for XUI packages and scenes.")
    parser.add_argument("container", help=".xzp package or a loose .xur")
    parser.add_argument("entry", nargs="?",
                        help="scene inside the package, e.g. KeyboardMain.xur")
    parser.add_argument("--trace", action="store_true",
                        help="show every read with byte offsets")
    parser.add_argument("--hex", action="store_true",
                        help="hex dump the DATA section")
    parser.add_argument("--no-properties", action="store_true",
                        help="tree only, without property values")
    parser.add_argument("--source", default=DEFAULT_SOURCE,
                        help="xam_ui_new.cc to take the class tables from")
    parser.add_argument("--html", metavar="FILE", nargs="?", const="",
                        help="write a visual viewer to FILE and open it "
                             "(default: next to the scene, then opened)")
    args = parser.parse_args()

    # Scene strings carry Japanese, Cyrillic and Greek key labels, which the
    # Windows console's default code page cannot encode - print them as best
    # it can rather than dying halfway through a dump.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    with open(args.container, "rb") as f:
        blob = f.read()

    entries = open_package(blob)
    if entries is not None and not args.entry:
        print("%s: %d entries" % (args.container, len(entries)))
        for name, offset, size in entries:
            print("  %-32s %7d bytes at %d" % (name, size, offset))
        return 0

    if entries is not None:
        match = [e for e in entries if e[0].lower() == args.entry.lower()]
        if not match:
            print("xurview: '%s' is not in the package" % args.entry,
                  file=sys.stderr)
            return 1
        _, offset, size = match[0]
        blob = blob[offset:offset + size]

    classes = Classes(args.source)
    scene = Scene(blob, classes, trace=args.trace)

    if args.html is not None:
        name = args.entry or os.path.basename(args.container)
        out = args.html or os.path.join(
            os.path.dirname(os.path.abspath(args.container)),
            os.path.basename(name).replace("\\", "_") + ".html")
        subtitle = "%d bytes, %d string(s)%s" % (
            scene.total, len(scene.strings),
            "  -  " + "; ".join(scene.notes) if scene.notes else "")
        count = to_html(scene, name, subtitle, out)
        print("%s: %d element(s) -> %s" % (name, count, out))
        if not args.html:
            import webbrowser
            webbrowser.open("file:///" + out.replace("\\", "/"))
        return 0

    print("version %d kind %#x, %d bytes, %d section(s)"
          % (scene.version, scene.kind, scene.total, scene.section_count))
    for tag, offset, length in scene.section_list:
        print("  %-6s offset=%-6d length=%d" % (tag, offset, length))
    print("pool hints: %s" % ", ".join(str(h) for h in scene.pool_hints))
    print("%d string(s), %d float(s), %d vector(s), %d colour(s)"
          % (len(scene.strings), len(scene.floats), len(scene.vectors),
             len(scene.colors)))
    for i, text in enumerate(scene.strings, 1):
        print("  [%2d] %s" % (i, text))
    print()
    print_tree(scene.root, 0, not args.no_properties)

    if args.hex:
        print()
        print("DATA (%d bytes):" % scene.data_size)
        hexdump(scene.sections["DATA"])

    if scene.notes:
        print()
        print("PROBLEMS")
        for note in scene.notes:
            print("  ! %s" % note)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
