import struct
import sys

# Mirrors xna_effect.cc: an XNB whose content is a compiled console effect holds
# a container tagged BCF00BCF with an offset to the body, and the body is a D3DX
# effect tagged FEFF0901. Everything in the body is big-endian on the console.

D3DX_TAG = 0xFEFF0901
CONTAINER_TAG = 0xBCF00BCF


def be32(data, at):
    return struct.unpack_from(">I", data, at)[0]


def le32(data, at):
    return struct.unpack_from("<I", data, at)[0]


def find_body(data):
    for at in range(0, min(len(data), 8192) - 4):
        if be32(data, at) == CONTAINER_TAG:
            body = at + be32(data, at + 4)
            if body + 8 <= len(data) and be32(data, body) == D3DX_TAG:
                return body, True
        if be32(data, at) == D3DX_TAG:
            return at, True
        if le32(data, at) == D3DX_TAG:
            return at, False
    return None, None


def plausible(parameters, techniques, objects, words):
    if parameters == 0 or parameters > 4096:
        return False
    if techniques == 0 or techniques > 256:
        return False
    if objects > 8192:
        return False
    return (parameters + techniques) < words


def read_name(body, offset, big):
    r = be32 if big else le32
    if offset == 0 or offset + 4 > len(body):
        return ""
    length = r(body, offset)
    if length == 0 or length > 256 or offset + 4 + length > len(body):
        return ""
    return body[offset + 4:offset + 4 + length].split(b"\0")[0].decode(
        "ascii", "replace")


def main(path):
    data = open(path, "rb").read()
    at, big = find_body(data)
    if at is None:
        print(f"{path}: no D3DX body found")
        return
    body = data[at:]
    r = be32 if big else le32
    offset = r(body, 4)
    words = len(body) // 4

    header = None
    for candidate in (8 + offset, offset, 8):
        if candidate + 16 > len(body):
            continue
        p, t, o = r(body, candidate), r(body, candidate + 4), r(body,
                                                               candidate + 12)
        if plausible(p, t, o, words):
            header = (candidate, p, t, o)
            break
    if header is None:
        print(f"{path}: no plausible header")
        return
    header_at, parameters, techniques, objects = header
    print(f"\n=== {path}")
    print(f"body at {at}, {'big' if big else 'little'}-endian, "
          f"{len(body)} bytes, header at {header_at}")
    print(f"{parameters} parameter(s), {techniques} technique(s), "
          f"{objects} object(s)")

    # Parameter records follow the header + 16, four words each.
    cursor = header_at + 16
    print(f"{'name':32} {'class':>5} {'type':>4} {'rows':>4} {'cols':>4} "
          f"{'elems':>5} {'value_offset':>12} {'/16':>6}")
    for _ in range(parameters):
        if cursor + 16 > len(body):
            break
        type_offset = r(body, cursor)
        value_offset = r(body, cursor + 4)
        cursor += 16
        if type_offset + 28 > len(body):
            continue
        ptype = r(body, type_offset)
        pclass = r(body, type_offset + 4)
        name = read_name(body, r(body, type_offset + 8), big)
        elements = r(body, type_offset + 16)
        columns = r(body, type_offset + 20)
        rows = r(body, type_offset + 24)
        quotient = value_offset / 16.0
        print(f"{name:32} {pclass:5} {ptype:4} {rows:4} {columns:4} "
              f"{elements:5} {value_offset:12} {quotient:6.2f}")


if __name__ == "__main__":
    for argument in sys.argv[1:]:
        main(argument)
