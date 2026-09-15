import struct
import sys
import zlib


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    pos = 8
    width = height = 0
    idat = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif tag == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    pixels = bytearray()
    for y in range(height):
        row = raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
        pixels.extend(row)
    return width, height, pixels


def write_png(path, width, height, rgb):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(rgb[y * width * 3:(y + 1) * width * 3])

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) < 4:
        print("usage: png_sheet.py <out.png> <cell_px> <in.png>...")
        return 1
    out = sys.argv[1]
    cell = int(sys.argv[2])
    images = [read_png(p) for p in sys.argv[3:]]
    columns = min(len(images), 4)
    rows = (len(images) + columns - 1) // columns
    gap = 8
    width = columns * (cell + gap) + gap
    height = rows * (cell + gap) + gap
    sheet = bytearray([60, 60, 60] * (width * height))
    for index, (w, h, px) in enumerate(images):
        ox = gap + (index % columns) * (cell + gap)
        oy = gap + (index // columns) * (cell + gap)
        scale = min(cell / w, cell / h)
        for y in range(int(h * scale)):
            sy = min(int(y / scale), h - 1)
            for x in range(int(w * scale)):
                sx = min(int(x / scale), w - 1)
                s = (sy * w + sx) * 4
                a = px[s + 3] / 255.0
                checker = 110 if ((x // 8 + y // 8) & 1) else 150
                d = ((oy + y) * width + ox + x) * 3
                for k in range(3):
                    sheet[d + k] = int(px[s + k] * a + checker * (1 - a))
    write_png(out, width, height, sheet)
    print("%d images -> %s (%dx%d)" % (len(images), out, width, height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
