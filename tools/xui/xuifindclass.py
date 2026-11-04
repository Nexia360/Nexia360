"""Locate a XUI class's Register() and report its base class and _GetPropDef.

Register() (XUI_IMPLEMENT_CLASS) builds XUIClass on the stack:
  +04 szClassName  +08 szBaseClassName  +1C pPropDefs  +20 dwPropDefCount
A class with no properties of its own stores 0 into both +1C and +20; otherwise
the value comes from the bl immediately preceding the +1C store.
"""
import re
import struct
import sys

BASE = 0x92000000
# XuiRegisterClass in dash.xex 17559 -- the call that ENDS a Register(), never
# the _GetPropDef that fills pPropDefs.
REGISTER_CLASS = 0x921D7EB0


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


def wstr(d, va):
    o = va - BASE
    if o < 0 or o + 4 > len(d):
        return None
    e = o
    while e + 1 < len(d) and d[e:e + 2] != b'\0\0' and e - o < 120:
        e += 2
    try:
        s = d[o:e].decode('utf-16-be')
    except Exception:
        return None
    return s if s and all(32 <= ord(c) < 127 for c in s) else None


def main():
    d = open(sys.argv[1], 'rb').read()
    n = len(d) // 4
    W = struct.unpack('>%dI' % n, d[:n * 4])
    targets = {}
    for nm in sys.argv[2:]:
        for m in re.finditer(re.escape(nm.encode('utf-16-be') + b'\x00\x00'), d):
            targets[BASE + m.start()] = nm
    regs = {}
    for i in range(n):
        w = W[i]
        op = w >> 26
        if op == 15:
            rD, rA, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            regs[rD] = ((regs.get(rA, 0) if rA else 0) + (imm << 16)) & 0xFFFFFFFF
        elif op == 14:
            rD, rA, imm = (w >> 21) & 31, (w >> 16) & 31, sx(w & 0xFFFF)
            regs[rD] = ((regs.get(rA, 0) if rA else 0) + imm) & 0xFFFFFFFF
        elif op == 36:
            rS, rA, disp = (w >> 21) & 31, (w >> 16) & 31, sx(w & 0xFFFF)
            if regs.get(rS) not in targets:
                continue
            base = propdefs = None
            zero = False
            for j in range(max(0, i - 10), min(n, i + 24)):
                w2 = W[j]
                if (w2 >> 26) != 36 or ((w2 >> 16) & 31) != rA:
                    continue
                dj = sx(w2 & 0xFFFF)
                rS2 = (w2 >> 21) & 31
                if dj == disp + 4:
                    base = regs.get(rS2)
                elif dj == disp + 0x18:              # pPropDefs
                    # A class with properties stores the return of _GetPropDef,
                    # so the value comes from a `bl` and is NOT in `regs` --
                    # look for that call first.  Only when there is none and
                    # the stored register is a tracked zero does the class
                    # really have no property map of its own.
                    for k in range(j - 1, max(0, j - 6), -1):
                        w3 = W[k]
                        if (w3 >> 26) == 18 and (w3 & 1):
                            t = (w3 & 0x3FFFFFC) - (0x4000000 if w3 & 0x2000000 else 0)
                            cand = (t if w3 & 2 else (BASE + k * 4) + t) & 0xFFFFFFFF
                            if cand != REGISTER_CLASS:
                                propdefs = cand
                            break
                    if propdefs is None and regs.get(rS2) == 0:
                        zero = True
            print("%-28s register@%08X  base=%-14s propdefs=%s"
                  % (targets[regs[rS]], BASE + i * 4,
                     wstr(d, base) if base else '?',
                     'NONE' if zero else (hex(propdefs) if propdefs else '?')))


if __name__ == "__main__":
    main()
