"""Recover a XUI class's property table from the code that builds it.

XUI_BEGIN_PROPERTY_MAP puts the XUIElementPropDef array in a FUNCTION-LOCAL
static with a non-constant initializer (nIndex++), so the console image has no
static table to read -- the array is filled by code on first call.  Each entry
is 0x30 bytes:

    +00 dwFlags  +04 dwId  +08 dwOffset  +0C dwExtra
    +10 szPropName  +14 Type  +18 pDefaultVal  +1C pCustomHandler ...

This walks the initializer, tracking register values from lis/addi/li, and
reads every stw whose displacement lands on a known field.

Usage: propmap.py <flat> <codeVA start> <codeVA end>
"""
import struct
import sys

BASE = 0x92000000
TYPES = ["EMPTY", "BOOL", "INTEGER", "UNSIGNED", "FLOAT", "STRING",
         "COLOR", "VECTOR", "QUATERNION", "OBJECT", "CUSTOM"]
OUR = {"BOOL": "kBool", "INTEGER": "kInteger", "UNSIGNED": "kUnsigned",
       "FLOAT": "kFloat", "STRING": "kString", "COLOR": "kColor",
       "VECTOR": "kVector", "QUATERNION": "kQuaternion",
       "OBJECT": "kObject", "CUSTOM": "kCustom"}


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


def wstr(d, va):
    o = va - BASE
    if o < 0 or o + 2 > len(d):
        return None
    e = o
    while e + 1 < len(d) and d[e:e + 2] != b'\0\0':
        e += 2
    try:
        s = d[o:e].decode('utf-16-be')
    except Exception:
        return None
    return s if s and all(32 <= ord(c) < 127 for c in s) else None


def main():
    path, lo, hi = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
    d = open(path, 'rb').read()
    regs = {}
    entries = {}
    for va in range(lo, hi, 4):
        w = struct.unpack_from('>I', d, va - BASE)[0]
        op = w >> 26
        if op == 15:                                   # addis / lis
            rD, rA, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            regs[rD] = (regs.get(rA, 0) if rA else 0) + (imm << 16)
        elif op == 14:                                 # addi / li
            rD, rA, imm = (w >> 21) & 31, (w >> 16) & 31, sx(w & 0xFFFF)
            regs[rD] = (regs.get(rA, 0) if rA else 0) + imm
        elif op == 36:                                 # stw rS, d(rA)
            rS, disp = (w >> 21) & 31, sx(w & 0xFFFF)
            if disp < 0:
                continue
            idx, field = divmod(disp, 0x30)
            val = regs.get(rS)
            if val is None:
                continue
            e = entries.setdefault(idx, {})
            if field == 0x00:
                e['flags'] = val
            elif field == 0x04:
                e['id'] = val
            elif field == 0x08:
                e['offset'] = val
            elif field == 0x0C:
                e['extra'] = val
            elif field == 0x10:
                e['name'] = wstr(d, val & 0xFFFFFFFF)
            elif field == 0x14:
                e['type'] = val
            elif field == 0x18:
                e['default'] = val

    named = {i: e for i, e in entries.items() if e.get('name')}
    if not named:
        print("no property entries found in %08X..%08X" % (lo, hi))
        return
    top = max(named)
    print("%d entries" % (top + 1))
    for i in range(top + 1):
        e = entries.get(i, {})
        t = e.get('type')
        tn = TYPES[t] if t is not None and t < len(TYPES) else '?'
        print("  [%2d] %-26s %-10s flags=%s off=%s extra=%s"
              % (i, e.get('name'), tn,
                 ('%04X' % e['flags']) if 'flags' in e else '-',
                 ('%04X' % e['offset']) if 'offset' in e else '-',
                 ('%X' % e['extra']) if 'extra' in e else '-'))
    print()
    for i in range(top + 1):
        e = entries.get(i, {})
        t = e.get('type')
        tn = TYPES[t] if t is not None and t < len(TYPES) else '?'
        print('    {"%s", PropertyType::%s, 0x%X, nullptr},'
              % (e.get('name'), OUR.get(tn, '???'), e.get('flags', 0)))


if __name__ == "__main__":
    main()
