import struct
import sys
import zlib


def load_obj(path):
    verts = []
    faces = []
    lines = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "v":
                verts.append(tuple(float(p) for p in parts[1:4]))
            elif parts[0] == "f":
                faces.append(tuple(int(p.split("/")[0]) - 1 for p in parts[1:4]))
            elif parts[0] == "l":
                lines.append((int(parts[1]) - 1, int(parts[2]) - 1))
    return verts, faces, lines


def overlay(pixels, size, verts, joints, bones, axis_h):
    lo_h = min(v[axis_h] for v in verts)
    hi_h = max(v[axis_h] for v in verts)
    lo_y = min(v[1] for v in verts)
    hi_y = max(v[1] for v in verts)
    span = max(hi_h - lo_h, hi_y - lo_y) * 1.05
    cx = (lo_h + hi_h) / 2
    cy = (lo_y + hi_y) / 2
    scale = size / span

    def project(v):
        return ((v[axis_h] - cx) * scale + size / 2, size / 2 - (v[1] - cy) * scale)

    def put(x, y, color):
        if 0 <= x < size and 0 <= y < size:
            pixels[(y * size + x) * 3:(y * size + x) * 3 + 3] = bytes(color)

    for a, b in bones:
        (x0, y0), (x1, y1) = project(joints[a]), project(joints[b])
        steps = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for i in range(steps + 1):
            t = i / steps
            put(int(x0 + (x1 - x0) * t), int(y0 + (y1 - y0) * t), (30, 90, 200))
    for j in joints:
        x, y = project(j)
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                put(int(x) + dx, int(y) + dy, (200, 40, 40))


def write_png(path, width, height, pixels):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(pixels[y * width * 3:(y + 1) * width * 3])

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def normal(a, b, c):
    u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    length = (n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5 or 1.0
    return (n[0] / length, n[1] / length, n[2] / length)


def render(verts, faces, axis_h, axis_d, size, light):
    lo_h = min(v[axis_h] for v in verts)
    hi_h = max(v[axis_h] for v in verts)
    lo_y = min(v[1] for v in verts)
    hi_y = max(v[1] for v in verts)
    span = max(hi_h - lo_h, hi_y - lo_y) * 1.05
    cx = (lo_h + hi_h) / 2
    cy = (lo_y + hi_y) / 2
    scale = size / span
    pixels = bytearray([40] * (size * size * 3))
    depth = [-1e9] * (size * size)

    def project(v):
        return ((v[axis_h] - cx) * scale + size / 2, size / 2 - (v[1] - cy) * scale, v[axis_d])

    for face in faces:
        a, b, c = (verts[i] for i in face)
        n = normal(a, b, c)
        shade = abs(n[0] * light[0] + n[1] * light[1] + n[2] * light[2])
        value = int(70 + 170 * shade)
        p = [project(a), project(b), project(c)]
        min_x = max(int(min(q[0] for q in p)), 0)
        max_x = min(int(max(q[0] for q in p)) + 1, size - 1)
        min_y = max(int(min(q[1] for q in p)), 0)
        max_y = min(int(max(q[1] for q in p)) + 1, size - 1)
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = p
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            continue
        for y in range(min_y, max_y + 1):
            for x in range(min_x, max_x + 1):
                px = x + 0.5
                py = y + 0.5
                w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area
                w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area
                w2 = 1.0 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w0 * z0 + w1 * z1 + w2 * z2
                idx = y * size + x
                if z > depth[idx]:
                    depth[idx] = z
                    pixels[idx * 3:idx * 3 + 3] = bytes((value, value, value))
    return pixels


def main():
    if len(sys.argv) < 3:
        print("usage: render_obj.py <in.obj> <out.png> [size] [skeleton.obj]")
        return 1
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 420
    verts, faces, own_bones = load_obj(sys.argv[1])
    if not faces and len(sys.argv) <= 4:
        sys.argv.append(sys.argv[1])
    front = render(verts, faces, 0, 2, size, (0.3, 0.4, 0.87))
    side = render(verts, faces, 2, 0, size, (0.87, 0.4, 0.3))
    if len(sys.argv) > 4:
        joints, _, bones = load_obj(sys.argv[4])
        overlay(front, size, verts, joints, bones, 0)
        overlay(side, size, verts, joints, bones, 2)
    combined = bytearray()
    for y in range(size):
        combined.extend(front[y * size * 3:(y + 1) * size * 3])
        combined.extend(side[y * size * 3:(y + 1) * size * 3])
    write_png(sys.argv[2], size * 2, size, combined)
    print("%d vertices, %d faces -> %s" % (len(verts), len(faces), sys.argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
