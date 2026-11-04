"""Find every XUI property-map initializer in a flattened XEX and print it.

Looks for the shape XUI_BEGIN_PROPERTY_MAP compiles to: a run of stw into a
0x30-strided array where +0x10 of each entry gets a pointer to a UTF-16 string.
"""
import struct
import sys

BASE = 0x92000000
TYPES = ["EMPTY", "BOOL", "INTEGER", "UNSIGNED", "FLOAT", "STRING",
         "COLOR", "VECTOR", "QUATERNION", "OBJECT", "CUSTOM"]


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


def wstr(d, va):
    o = va - BASE
    if o < 0 or o + 4 > len(d):
        return None
    e = o
    while e + 1 < len(d) and d[e:e + 2] != b'\0\0' and e - o < 120:
        e += 2
    if e == o:
        return None
    try:
        s = d[o:e].decode('utf-16-be')
    except Exception:
        return None
    if not s or not all(32 <= ord(c) < 127 for c in s):
        return None
    return s


def scan(d, want_count=None):
    n = len(d) // 4
    W = struct.unpack('>%dI' % n, d[:n * 4])
    regs = {}
    cur = {}
    start = None
    out = []

    def flush():
        if cur and max(cur) >= 0:
            named = sorted(k for k in cur if cur[k].get('name'))
            if named and named[0] == 0:
                out.append((start, dict(cur)))
        cur.clear()

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
            rS, disp = (w >> 21) & 31, sx(w & 0xFFFF)
            if disp < 0 or rS not in regs:
                continue
            idx, field = divmod(disp, 0x30)
            if field == 0x10:
                s = wstr(d, regs[rS])
                if s:
                    if idx == 0:
                        flush()
                        start = BASE + i * 4
                    cur.setdefault(idx, {})['name'] = s
            elif field in (0x00, 0x04, 0x08, 0x0C, 0x14):
                if idx in cur or field == 0x14:
                    cur.setdefault(idx, {})[
                        {0x00: 'flags', 0x04: 'id', 0x08: 'off',
                         0x0C: 'extra', 0x14: 'type'}[field]] = regs[rS]
        elif op in (16, 18):        # branch: end of basic block, keep going
            pass
    flush()
    return out


def main():
    d = open(sys.argv[1], 'rb').read()
    want = int(sys.argv[2]) if len(sys.argv) > 2 else None
    filt = sys.argv[3].lower() if len(sys.argv) > 3 else None
    for start, ent in scan(d):
        top = max(ent)
        if want is not None and top + 1 != want:
            continue
        names = [ent.get(i, {}).get('name') for i in range(top + 1)]
        if filt and not any(nm and filt in nm.lower() for nm in names):
            continue
        print("map @ %08X  %d entries" % (start, top + 1))
        for i in range(top + 1):
            e = ent.get(i, {})
            t = e.get('type')
            tn = TYPES[t] if t is not None and t < len(TYPES) else '?'
            print("   [%2d] %-28s %-10s flags=%s off=%s"
                  % (i, e.get('name'), tn,
                     ('%04X' % e['flags']) if 'flags' in e else '-',
                     ('%04X' % e['off']) if 'off' in e else '-'))
        print()


if __name__ == "__main__":
    main()
