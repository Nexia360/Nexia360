#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Bits {
  const uint8_t* data = nullptr;
  size_t size = 0;

  uint32_t Get(uint64_t bit, uint32_t count) const {
    if (!count) {
      return 0;
    }
    uint64_t value = 0;
    const size_t first = size_t(bit >> 3);
    for (size_t k = 0; k < 8 && first + k < size; ++k) {
      value |= uint64_t(data[first + k]) << (8 * k);
    }
    value >>= (bit & 7);
    if (count < 32) {
      value &= (uint64_t(1) << count) - 1;
    }
    return uint32_t(value);
  }

  float Float(uint64_t bit) const {
    const uint32_t raw = Get(bit, 32);
    float f;
    std::memcpy(&f, &raw, 4);
    return f;
  }
};

struct Lattice {
  float cx = 2.0f;
  float cy = 0.0f;
  float cz = 0.0f;
  float k1 = 0.0f;
  float k2 = 0.0f;
};

struct Vertex {
  float pos[3];
  uint32_t raw[3];
  uint32_t normal;
  uint32_t weights;
  uint32_t indices;
  uint32_t color;
  uint16_t uv[12];
};

float Half(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31;
  int32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  uint32_t out;
  if (exp == 0) {
    if (!mant) {
      out = sign;
    } else {
      exp = 1;
      while (!(mant & 0x400)) {
        mant <<= 1;
        --exp;
      }
      mant &= 0x3FF;
      out = sign | uint32_t(exp + 112) << 23 | mant << 13;
    }
  } else if (exp == 31) {
    out = sign | 0x7F800000 | mant << 13;
  } else {
    out = sign | uint32_t(exp + 112) << 23 | mant << 13;
  }
  float f;
  std::memcpy(&f, &out, 4);
  return f;
}

bool DecodeVertices(const Bits& b, size_t at, uint32_t uv_sets,
                    const Lattice& lattice, std::vector<Vertex>* out,
                    size_t* consumed) {
  const uint64_t base = uint64_t(at) * 8;
  const uint32_t count = b.Get(base, 32);
  const float scale = b.Float(base + 32);
  const float min[3] = {b.Float(base + 64), b.Float(base + 96),
                        b.Float(base + 128)};
  const uint32_t width[3] = {b.Get(base + 160, 6), b.Get(base + 166, 6),
                             b.Get(base + 172, 6)};
  uint32_t int_base[4];
  uint32_t int_bits[4];
  for (int k = 0; k < 4; ++k) {
    int_base[k] = b.Get(base + 178 + 64 * k, 32);
    int_bits[k] = b.Get(base + 178 + 64 * k + 32, 32);
  }
  const uint32_t per_vertex = width[0] + width[1] + width[2] + int_bits[0] +
                              int_bits[1] + int_bits[2] + int_bits[3] +
                              32 * uv_sets;
  std::printf(
      "  vertices %u, scale %g, min (%g %g %g), widths %u/%u/%u\n"
      "  normal %08X/%u weights %08X/%u indices %08X/%u color %08X/%u, "
      "%u bits per vertex\n",
      count, scale, min[0], min[1], min[2], width[0], width[1], width[2],
      int_base[0], int_bits[0], int_base[1], int_bits[1], int_base[2],
      int_bits[2], int_base[3], int_bits[3], per_vertex);
  const float step[3] = {scale * lattice.cx, scale * lattice.cy,
                         scale * lattice.cz};
  uint64_t bit = base + 434;
  out->resize(count);
  for (uint32_t v = 0; v < count; ++v) {
    Vertex& vx = (*out)[v];
    for (int k = 0; k < 3; ++k) {
      vx.raw[k] = b.Get(bit, width[k]);
      bit += width[k];
    }
    vx.pos[1] = float(vx.raw[1]) * step[1] + min[1];
    vx.pos[2] = float(vx.raw[2]) * step[2] + min[2];
    if (vx.raw[1] & 1) {
      vx.pos[2] += step[2] * lattice.k1;
    }
    vx.pos[0] = float(vx.raw[0]) * step[0] + min[0];
    if ((vx.raw[2] ^ vx.raw[1]) & 1) {
      vx.pos[0] += step[0] * lattice.k2;
    }
    uint32_t* ints[4] = {&vx.normal, &vx.weights, &vx.indices, &vx.color};
    for (int k = 0; k < 4; ++k) {
      *ints[k] = int_base[k] + b.Get(bit, int_bits[k]);
      bit += int_bits[k];
    }
    for (uint32_t k = 0; k < uv_sets * 2 && k < 12; ++k) {
      vx.uv[k] = uint16_t(b.Get(bit, 16));
      bit += 16;
    }
  }
  *consumed = size_t((uint64_t(count) * per_vertex + 441) >> 3);
  return true;
}

bool DecodeIndices16(const Bits& b, size_t at, uint32_t expected,
                     std::vector<uint32_t>* out, size_t* consumed) {
  const uint64_t base = uint64_t(at) * 8;
  const uint32_t count = b.Get(base, 32);
  const uint32_t first = b.Get(base + 32, 16);
  const uint32_t bits = b.Get(base + 48, 16);
  std::printf("  indices %u (expected %u), base %u, %u bits\n", count,
              expected, first, bits);
  if (bits > 16) {
    return false;
  }
  out->resize(count);
  uint64_t bit = base + 64;
  for (uint32_t k = 0; k < count; ++k) {
    (*out)[k] = (first + b.Get(bit, bits)) & 0xFFFF;
    bit += bits;
  }
  *consumed = size_t((uint64_t(count) * bits + 64 + 7) >> 3);
  return true;
}

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      table[i] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) {
    crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

void PutBe32(std::vector<uint8_t>* v, uint32_t x) {
  v->push_back(uint8_t(x >> 24));
  v->push_back(uint8_t(x >> 16));
  v->push_back(uint8_t(x >> 8));
  v->push_back(uint8_t(x));
}

void Chunk(FILE* f, const char* tag, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out;
  PutBe32(&out, uint32_t(data.size()));
  out.insert(out.end(), tag, tag + 4);
  out.insert(out.end(), data.begin(), data.end());
  PutBe32(&out, Crc32(out.data() + 4, out.size() - 4));
  std::fwrite(out.data(), 1, out.size(), f);
}

bool WritePng(const std::string& path, uint32_t w, uint32_t h,
              const std::vector<uint8_t>& rgba) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    return false;
  }
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::fwrite(kSig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  PutBe32(&ihdr, w);
  PutBe32(&ihdr, h);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  Chunk(f, "IHDR", ihdr);
  std::vector<uint8_t> raw;
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4,
               rgba.begin() + size_t(y + 1) * w * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t at = 0; at < raw.size() || at == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    const bool last = at + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (last) {
      break;
    }
  }
  PutBe32(&z, (b << 16) | a);
  Chunk(f, "IDAT", z);
  Chunk(f, "IEND", {});
  std::fclose(f);
  return true;
}

void Rgb565(uint16_t c, uint8_t* out) {
  out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  out[2] = uint8_t((c & 31) * 255 / 31);
  out[3] = 255;
}

void DecodeColorBlock(const uint8_t* p, bool dxt1, uint8_t out[16][4]) {
  const uint16_t c0 = uint16_t(p[0] | p[1] << 8);
  const uint16_t c1 = uint16_t(p[2] | p[3] << 8);
  uint8_t pal[4][4];
  Rgb565(c0, pal[0]);
  Rgb565(c1, pal[1]);
  for (int k = 0; k < 3; ++k) {
    if (!dxt1 || c0 > c1) {
      pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
    } else {
      pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
      pal[3][k] = 0;
    }
  }
  pal[2][3] = 255;
  pal[3][3] = (dxt1 && c0 <= c1) ? 0 : 255;
  const uint32_t bits = uint32_t(p[4] | p[5] << 8 | p[6] << 16 | p[7] << 24);
  for (int i = 0; i < 16; ++i) {
    std::memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
  }
}

void DecodeAlphaBlock(const uint8_t* p, uint8_t out[16][4]) {
  uint8_t pal[8];
  pal[0] = p[0];
  pal[1] = p[1];
  if (pal[0] > pal[1]) {
    for (int k = 1; k < 7; ++k) {
      pal[k + 1] = uint8_t(((7 - k) * pal[0] + k * pal[1]) / 7);
    }
  } else {
    for (int k = 1; k < 5; ++k) {
      pal[k + 1] = uint8_t(((5 - k) * pal[0] + k * pal[1]) / 5);
    }
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t bits = 0;
  for (int k = 0; k < 6; ++k) {
    bits |= uint64_t(p[2 + k]) << (8 * k);
  }
  for (int i = 0; i < 16; ++i) {
    out[i][3] = pal[(bits >> (3 * i)) & 7];
  }
}

bool DecodeTexture(const uint8_t* data, size_t size, uint32_t format,
                   uint32_t w, uint32_t h, uint32_t pitch, bool swap16,
                   std::vector<uint8_t>* rgba) {
  const uint32_t kind = format & 0x3F;
  const bool dxt1 = kind == 0x12;
  const bool dxt5 = kind == 0x14;
  const bool dxt3 = kind == 0x13;
  if (!dxt1 && !dxt3 && !dxt5 && kind != 0x06) {
    return false;
  }
  rgba->assign(size_t(w) * h * 4, 0);
  if (kind == 0x06) {
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const size_t s = size_t(y) * pitch + x * 4;
        if (s + 4 > size) {
          return false;
        }
        uint8_t* d = &(*rgba)[(size_t(y) * w + x) * 4];
        d[0] = data[s + 1];
        d[1] = data[s + 2];
        d[2] = data[s + 3];
        d[3] = data[s + 0];
      }
    }
    return true;
  }
  const uint32_t block = dxt1 ? 8 : 16;
  const uint32_t bw = (w + 3) / 4;
  const uint32_t bh = (h + 3) / 4;
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const size_t s = size_t(by) * pitch + size_t(bx) * block;
      if (s + block > size) {
        return false;
      }
      uint8_t blk[16];
      std::memcpy(blk, data + s, block);
      if (swap16) {
        for (uint32_t k = 0; k + 1 < block; k += 2) {
          std::swap(blk[k], blk[k + 1]);
        }
      }
      uint8_t px[16][4];
      if (dxt1) {
        DecodeColorBlock(blk, true, px);
      } else {
        DecodeColorBlock(blk + 8, false, px);
        if (dxt5) {
          DecodeAlphaBlock(blk, px);
        } else {
          for (int i = 0; i < 16; ++i) {
            const uint8_t nib = (blk[i / 2] >> ((i & 1) * 4)) & 15;
            px[i][3] = uint8_t(nib * 17);
          }
        }
      }
      for (int i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3);
        const uint32_t y = by * 4 + (i >> 2);
        if (x < w && y < h) {
          std::memcpy(&(*rgba)[(size_t(y) * w + x) * 4], px[i], 4);
        }
      }
    }
  }
  return true;
}

size_t ParseTextures(const Bits& b, const std::vector<uint8_t>& d, size_t at,
                     uint32_t count, const std::string& prefix,
                     bool standalone = false) {
  for (uint32_t t = 0; t < count; ++t) {
    if (at + (standalone ? 0 : 8) + 0x21 > d.size()) {
      std::printf("texture %u: truncated at %zX\n", t, at);
      return at;
    }
    uint32_t offset = 0;
    uint32_t gpu_size = 0;
    if (!standalone) {
      offset = b.Get(uint64_t(at) * 8, 32);
      gpu_size = b.Get(uint64_t(at) * 8 + 32, 32);
      at += 8;
    }
    const uint64_t hb = uint64_t(at) * 8;
    const uint32_t format = b.Get(hb, 32);
    const uint32_t w = b.Get(hb + 32, 32);
    const uint32_t h = b.Get(hb + 64, 32);
    const uint32_t f0c = b.Get(hb + 96, 32);
    const uint32_t f14 = b.Get(hb + 128, 32);
    const uint32_t levels = b.Get(hb + 160, 32);
    const uint32_t zero_fill = b.Get(hb + 192, 1);
    const uint32_t callback = b.Get(hb + 193, 1);
    const uint32_t pitch = b.Get(hb + 194, 32);
    const uint32_t rows = b.Get(hb + 226, 32);
    at += 0x21;
    const size_t bytes =
        zero_fill ? 0 : size_t(levels) * size_t(rows) * size_t(pitch);
    std::printf(
        "texture %u: gpu offset %X size %X | format %08X %ux%u c %X size %X "
        "count %u zero %u callback %u pitch %u rows %u -> %zX data bytes @%zX\n",
        t, offset, gpu_size, format, w, h, f0c, f14, levels, zero_fill,
        callback, pitch, rows, bytes, at);
    if (at + bytes > d.size()) {
      std::printf("  data runs past the end\n");
      return at;
    }
    const size_t slice = size_t(rows) * size_t(pitch);
    for (uint32_t s = 0; !prefix.empty() && bytes && s < levels; ++s) {
      std::vector<uint8_t> rgba;
      if (DecodeTexture(d.data() + at + s * slice, slice, format, w, h, pitch,
                        true, &rgba)) {
        WritePng(prefix + "_tex" + std::to_string(t) + "_" +
                     std::to_string(s) + ".png",
                 w, h, rgba);
      } else {
        std::printf("  format %X not decoded\n", format & 0x3F);
        break;
      }
    }
    at += bytes;
  }
  return at;
}

struct Vector3dContext {
  float scale;
  float min[3];
  uint32_t width[3];
};

Vector3dContext ReadVector3dContext(const Bits& b, uint64_t bit,
                                    const Lattice& lattice) {
  Vector3dContext c;
  c.scale = b.Float(bit);
  for (int k = 0; k < 3; ++k) {
    c.min[k] = b.Float(bit + 32 + 32 * k);
    c.width[k] = b.Get(bit + 128 + 6 * k, 6);
  }
  (void)lattice;
  return c;
}

void DecodeLattice(const Bits& b, uint64_t* bit, const Vector3dContext& c,
                   const Lattice& lattice, float out[3]) {
  uint32_t raw[3];
  for (int k = 0; k < 3; ++k) {
    raw[k] = b.Get(*bit, c.width[k]);
    *bit += c.width[k];
  }
  const float step[3] = {c.scale * lattice.cx, c.scale * lattice.cy,
                         c.scale * lattice.cz};
  out[1] = float(raw[1]) * step[1] + c.min[1];
  out[2] = float(raw[2]) * step[2] + c.min[2];
  if (raw[1] & 1) {
    out[2] += step[2] * lattice.k1;
  }
  out[0] = float(raw[0]) * step[0] + c.min[0];
  if ((raw[2] ^ raw[1]) & 1) {
    out[0] += step[0] * lattice.k2;
  }
}

void RotationVectorToQuaternion(const float v[3], float half_factor,
                                float q[4]) {
  const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  const float angle = length * half_factor;
  const float s = length > 0.0f ? std::sin(angle) / length : half_factor;
  q[0] = v[0] * s;
  q[1] = v[1] * s;
  q[2] = v[2] * s;
  q[3] = std::cos(angle);
}

int DecodeSkeleton(const std::vector<uint8_t>& d, const Lattice& lattice,
                   float half_factor, const char* obj_path) {
  Bits b{d.data(), d.size()};
  const uint32_t count = b.Get(0, 32);
  const Vector3dContext pos = ReadVector3dContext(b, 32, lattice);
  const Vector3dContext rot = ReadVector3dContext(b, 178, lattice);
  const uint32_t per_joint = 8 + pos.width[0] + pos.width[1] + pos.width[2] +
                             rot.width[0] + rot.width[1] + rot.width[2];
  const size_t bytes = size_t((uint64_t(count) * per_joint + 331) >> 3);
  std::printf(
      "skeleton: %u joints, pos scale %g min (%g %g %g) widths %u/%u/%u, rot "
      "scale %g min (%g %g %g) widths %u/%u/%u, %u bits per joint, %zX of "
      "%zX bytes\n",
      count, pos.scale, pos.min[0], pos.min[1], pos.min[2], pos.width[0],
      pos.width[1], pos.width[2], rot.scale, rot.min[0], rot.min[1],
      rot.min[2], rot.width[0], rot.width[1], rot.width[2], per_joint, bytes,
      d.size());
  FILE* obj = obj_path ? std::fopen(obj_path, "w") : nullptr;
  uint64_t bit = 324;
  std::vector<uint32_t> parents(count);
  for (uint32_t j = 0; j < count; ++j) {
    parents[j] = b.Get(bit, 8);
    bit += 8;
    float p[3];
    float r[3];
    float q[4];
    DecodeLattice(b, &bit, pos, lattice, p);
    DecodeLattice(b, &bit, rot, lattice, r);
    RotationVectorToQuaternion(r, half_factor, q);
    const float qlen =
        std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    std::printf(
        "  %2u parent %3u pos %8.4f %8.4f %8.4f rotvec %8.4f %8.4f %8.4f "
        "quat %7.4f %7.4f %7.4f %7.4f |q| %.4f\n",
        j, parents[j], p[0], p[1], p[2], r[0], r[1], r[2], q[0], q[1], q[2],
        q[3], qlen);
    if (obj) {
      std::fprintf(obj, "v %f %f %f\n", p[0], p[1], p[2]);
    }
  }
  if (obj) {
    for (uint32_t j = 0; j < count; ++j) {
      if (parents[j] < count) {
        std::fprintf(obj, "l %u %u\n", parents[j] + 1, j + 1);
      }
    }
    std::fclose(obj);
  }
  return bytes == d.size() ? 0 : 4;
}

void QuatMul(const float a[4], const float b[4], float out[4]) {
  const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
  const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
  const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
  const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
  out[0] = x;
  out[1] = y;
  out[2] = z;
  out[3] = w;
}

void QuatRotate(const float q[4], const float v[3], float out[3]) {
  const float p[4] = {v[0], v[1], v[2], 0.0f};
  const float c[4] = {-q[0], -q[1], -q[2], q[3]};
  float t[4];
  float r[4];
  QuatMul(q, p, t);
  QuatMul(t, c, r);
  out[0] = r[0];
  out[1] = r[1];
  out[2] = r[2];
}

std::vector<uint32_t> SkeletonParents(const std::vector<uint8_t>& d,
                                      const Lattice& lattice,
                                      std::vector<float>* bind = nullptr) {
  std::vector<uint32_t> parents;
  if (d.size() < 0x29) {
    return parents;
  }
  Bits b{d.data(), d.size()};
  const uint32_t count = b.Get(0, 32);
  const Vector3dContext pos = ReadVector3dContext(b, 32, lattice);
  const Vector3dContext rot = ReadVector3dContext(b, 178, lattice);
  uint64_t bit = 324;
  for (uint32_t j = 0; j < count && j < 256; ++j) {
    parents.push_back(b.Get(bit, 8));
    bit += 8;
    float p[3];
    float r[3];
    DecodeLattice(b, &bit, pos, lattice, p);
    DecodeLattice(b, &bit, rot, lattice, r);
    if (bind) {
      bind->insert(bind->end(), p, p + 3);
    }
  }
  return parents;
}

struct JointPose {
  float p[3];
  float r[3];
  float s[3];
};

int DecodeAnimation(const std::vector<uint8_t>& d, const Lattice& lattice,
                    const std::vector<uint32_t>& parents,
                    const std::vector<float>& bind,
                    const std::string& prefix) {
  if (d.size() < 0x28) {
    return 1;
  }
  Bits hb{d.data(), d.size()};
  uint32_t hdr[10];
  for (int k = 0; k < 10; ++k) {
    hdr[k] = hb.Get(uint64_t(k) * 32, 32);
  }
  float rate;
  std::memcpy(&rate, &hdr[1], 4);
  std::printf(
      "animation: frames %u rate %g joints %u f60 %u motion %u f5 %u | offA "
      "%X offC %X offB %X total %X (file body %zX)\n",
      hdr[0], rate, hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7], hdr[8],
      hdr[9], d.size() - 0x28);
  const uint8_t* body = d.data() + 0x28;
  const uint32_t section = hdr[6];
  if (0x28 + size_t(section) > d.size()) {
    std::printf("joint section runs past the end\n");
    return 2;
  }
  Bits b{body, section};
  const uint32_t frames = b.Get(0, 32);
  const uint32_t joints = b.Get(32, 32);
  if (joints > 72) {
    std::printf("joint count %u\n", joints);
    return 2;
  }
  std::vector<Vector3dContext> ctx(size_t(joints) * 3);
  uint64_t bit = 64;
  uint32_t per_frame = 0;
  uint32_t animated = 0;
  for (uint32_t s = 0; s < joints; ++s) {
    const uint32_t j = joints - 1 - s;
    uint32_t joint_bits = 0;
    for (int k = 0; k < 3; ++k) {
      ctx[j * 3 + k] = ReadVector3dContext(b, bit, lattice);
      bit += 146;
      joint_bits += ctx[j * 3 + k].width[0] + ctx[j * 3 + k].width[1] +
                    ctx[j * 3 + k].width[2];
    }
    per_frame += joint_bits;
    animated += joint_bits ? 1 : 0;
  }
  const uint64_t header_bits = 64 + 72 * 438;
  const uint64_t used = header_bits + uint64_t(frames) * per_frame;
  std::printf(
      "joints: %u frames x %u joints, %u animated, %u bits per frame, header "
      "%llu bits, data ends at byte %llu of section %u (%s)\n",
      frames, joints, animated, per_frame, (unsigned long long)header_bits,
      (unsigned long long)((used + 7) / 8), section,
      (used + 7) / 8 == section       ? "exact"
      : (used + 7) / 8 + 3 >= section && (used + 7) / 8 <= section ? "padded"
                                                                    : "MISMATCH");
  const uint32_t picks[3] = {0, frames / 2, frames ? frames - 1 : 0};
  for (int pi = 0; pi < 3; ++pi) {
    const uint32_t f = picks[pi];
    std::vector<JointPose> pose(joints);
    uint64_t fb = header_bits + uint64_t(f) * per_frame;
    for (uint32_t j = 0; j < joints; ++j) {
      DecodeLattice(b, &fb, ctx[j * 3 + 0], lattice, pose[j].p);
      DecodeLattice(b, &fb, ctx[j * 3 + 1], lattice, pose[j].r);
      DecodeLattice(b, &fb, ctx[j * 3 + 2], lattice, pose[j].s);
    }
    if (pi == 0) {
      for (uint32_t j = 0; j < joints && j < 6; ++j) {
        std::printf(
            "  f0 j%u pos %.4f %.4f %.4f rot %.4f %.4f %.4f scale %.4f %.4f "
            "%.4f\n",
            j, pose[j].p[0], pose[j].p[1], pose[j].p[2], pose[j].r[0],
            pose[j].r[1], pose[j].r[2], pose[j].s[0], pose[j].s[1],
            pose[j].s[2]);
      }
    }
    if (prefix.empty() || parents.size() < joints) {
      continue;
    }
    std::vector<float> wp(size_t(joints) * 3);
    std::vector<float> wq(size_t(joints) * 4);
    for (uint32_t j = 0; j < joints; ++j) {
      float lq[4];
      RotationVectorToQuaternion(pose[j].r, 1.0f, lq);
      const uint32_t parent = parents[j];
      float local[3];
      for (int k = 0; k < 3; ++k) {
        const float rest =
            bind.size() >= size_t(joints) * 3
                ? bind[j * 3 + k] - (parent < j ? bind[parent * 3 + k] : 0.0f)
                : 0.0f;
        local[k] = rest + pose[j].p[k];
      }
      if (parent >= j) {
        std::memcpy(&wp[j * 3], local, 12);
        std::memcpy(&wq[j * 4], lq, 16);
        continue;
      }
      float offset[3];
      QuatRotate(&wq[parent * 4], local, offset);
      for (int k = 0; k < 3; ++k) {
        wp[j * 3 + k] = wp[parent * 3 + k] + offset[k];
      }
      QuatMul(&wq[parent * 4], lq, &wq[j * 4]);
    }
    const std::string path = prefix + "_f" + std::to_string(f) + ".obj";
    FILE* obj = std::fopen(path.c_str(), "w");
    if (!obj) {
      continue;
    }
    for (uint32_t j = 0; j < joints; ++j) {
      std::fprintf(obj, "v %f %f %f\n", wp[j * 3], wp[j * 3 + 1],
                   wp[j * 3 + 2]);
    }
    for (uint32_t j = 0; j < joints; ++j) {
      if (parents[j] < j) {
        std::fprintf(obj, "l %u %u\n", parents[j] + 1, j + 1);
      }
    }
    std::fclose(obj);
    std::printf("  frame %u -> %s\n", f, path.c_str());
  }
  return 0;
}

std::vector<uint8_t> ReadFile(const char* path) {
  std::vector<uint8_t> d;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    return d;
  }
  std::fseek(f, 0, SEEK_END);
  d.resize(size_t(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  std::fread(d.data(), 1, d.size(), f);
  std::fclose(f);
  return d;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf(
        "usage: avatar_model_decode <model.bin> [out.obj] [cy cz k1 k2 "
        "[cx]]\n"
        "       avatar_model_decode --skeleton <skeleton.bin> <out.obj> "
        "<half_factor>\n");
    return 1;
  }
  if (std::string(argv[1]) == "--anim" && argc >= 3) {
    Lattice lattice;
    lattice.cy = 1.6329932f;
    lattice.cz = 1.7320508f;
    lattice.k1 = 1.0f / 3.0f;
    lattice.k2 = 0.5f;
    std::vector<uint32_t> parents;
    std::vector<float> bind;
    if (argc >= 4) {
      parents = SkeletonParents(ReadFile(argv[3]), lattice, &bind);
    }
    return DecodeAnimation(ReadFile(argv[2]), lattice, parents, bind,
                           argc >= 5 ? std::string(argv[4]) : std::string());
  }
  if (std::string(argv[1]) == "--skeleton" && argc >= 5) {
    Lattice lattice;
    lattice.cy = 1.6329932f;
    lattice.cz = 1.7320508f;
    lattice.k1 = 1.0f / 3.0f;
    lattice.k2 = 0.5f;
    const std::vector<uint8_t> sk = ReadFile(argv[2]);
    return DecodeSkeleton(sk, lattice, float(std::atof(argv[4])), argv[3]);
  }
  const std::vector<uint8_t> d = ReadFile(argv[1]);
  if (d.size() < 0x30) {
    std::printf("cannot read %s\n", argv[1]);
    return 1;
  }
  Lattice lattice;
  if (argc >= 7) {
    lattice.cy = float(std::atof(argv[3]));
    lattice.cz = float(std::atof(argv[4]));
    lattice.k1 = float(std::atof(argv[5]));
    lattice.k2 = float(std::atof(argv[6]));
  }
  if (argc >= 8) {
    lattice.cx = float(std::atof(argv[7]));
  }
  Bits b{d.data(), d.size()};
  if ((b.Get(0, 32) & 0xFFFFFE00u) == 0x1A200000u) {
    std::string prefix;
    if (argc >= 3) {
      prefix = argv[2];
      const size_t dot = prefix.rfind('.');
      if (dot != std::string::npos) {
        prefix.resize(dot);
      }
    }
    const size_t end = ParseTextures(b, d, 0, 1, prefix, true);
    std::printf("standalone texture ends @%zX of %zX\n", end, d.size());
    return end == d.size() ? 0 : 3;
  }
  uint32_t h[12];
  for (int k = 0; k < 12; ++k) {
    h[k] = b.Get(uint64_t(k) * 32, 32);
  }
  std::printf(
      "model: cpu %X gpu %X tex %X vb %X ib %X batches %u textures %u | "
      "vb@%X ib@%X batches@%X textures@%X scratch %X\n",
      h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10],
      h[11]);
  if (h[5] > 64 || h[6] > 64) {
    std::printf("not a model: %u batches, %u textures\n", h[5], h[6]);
    return 5;
  }
  size_t at = 0x30;
  FILE* obj = argc >= 3 ? std::fopen(argv[2], "w") : nullptr;
  uint32_t obj_base = 1;
  for (uint32_t batch = 0; batch < h[5]; ++batch) {
    const uint64_t bb = uint64_t(at) * 8;
    const uint32_t word0 = b.Get(bb, 32);
    const uint32_t params = b.Get(bb + 32, 5);
    const uint32_t tris = b.Get(bb + 37, 32);
    const uint32_t verts = b.Get(bb + 69, 32);
    const uint32_t uvs = b.Get(bb + 101, 32);
    const uint32_t stride = b.Get(bb + 133, 32);
    const uint32_t unk = b.Get(bb + 165, 32);
    const uint32_t vb = b.Get(bb + 197, 32);
    const uint32_t ib = b.Get(bb + 229, 32);
    std::printf(
        "batch %u @%zX: word0 %X params %u tris %u verts %u uvs %u stride %u "
        "unk %u vb %X ib %X\n",
        batch, at, word0, params, tris, verts, uvs, stride, unk, vb, ib);
    at += 0x21;
    for (uint32_t p = 0; p < params; ++p) {
      const uint64_t pb = uint64_t(at) * 8;
      std::printf("  param %u: type %u usage %u data %08X %08X %08X %08X\n", p,
                  b.Get(pb, 32), b.Get(pb + 32, 32), b.Get(pb + 64, 32),
                  b.Get(pb + 96, 32), b.Get(pb + 128, 32), b.Get(pb + 160, 32));
      at += 0x18;
    }
    if (uvs == 7 || uvs > 6) {
      std::printf("  uv set count %u uses the full vertex packer: not handled\n",
                  uvs);
      return 2;
    }
    std::vector<Vertex> vertices;
    size_t used = 0;
    DecodeVertices(b, at, uvs, lattice, &vertices, &used);
    std::printf("  vertex stream %zX bytes, ends @%zX\n", used, at + used);
    at += used;
    std::vector<uint32_t> indices;
    if (!DecodeIndices16(b, at, tris * 3, &indices, &used)) {
      std::printf("  index context looks wrong\n");
      return 2;
    }
    std::printf("  index stream %zX bytes, ends @%zX\n", used, at + used);
    at += used;
    uint32_t max_index = 0;
    for (uint32_t i : indices) {
      max_index = i > max_index ? i : max_index;
    }
    std::printf("  max index %u of %zu vertices\n", max_index, vertices.size());
    float lo[3] = {1e9f, 1e9f, 1e9f};
    float hi[3] = {-1e9f, -1e9f, -1e9f};
    for (const Vertex& v : vertices) {
      for (int k = 0; k < 3; ++k) {
        lo[k] = v.pos[k] < lo[k] ? v.pos[k] : lo[k];
        hi[k] = v.pos[k] > hi[k] ? v.pos[k] : hi[k];
      }
    }
    std::printf("  bounds x %g..%g y %g..%g z %g..%g\n", lo[0], hi[0], lo[1],
                hi[1], lo[2], hi[2]);
    for (size_t v = 0; v < vertices.size() && v < 6; ++v) {
      const Vertex& vx = vertices[v];
      std::printf(
          "  v%zu raw %u %u %u pos %.4f %.4f %.4f n %08X w %08X i %08X c %08X "
          "uv0 %.4f %.4f\n",
          v, vx.raw[0], vx.raw[1], vx.raw[2], vx.pos[0], vx.pos[1], vx.pos[2],
          vx.normal, vx.weights, vx.indices, vx.color, Half(vx.uv[0]),
          Half(vx.uv[1]));
    }
    if (obj) {
      for (const Vertex& v : vertices) {
        std::fprintf(obj, "v %f %f %f\n", v.pos[0], v.pos[1], v.pos[2]);
      }
      for (const Vertex& v : vertices) {
        std::fprintf(obj, "vt %f %f\n", Half(v.uv[0]), 1.0f - Half(v.uv[1]));
      }
      for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        std::fprintf(obj, "f %u/%u %u/%u %u/%u\n", indices[t] + obj_base,
                     indices[t] + obj_base, indices[t + 1] + obj_base,
                     indices[t + 1] + obj_base, indices[t + 2] + obj_base,
                     indices[t + 2] + obj_base);
      }
      obj_base += uint32_t(vertices.size());
    }
  }
  if (obj) {
    std::fclose(obj);
  }
  std::printf("after batches @%zX of %zX\n", at, d.size());
  std::string prefix;
  if (argc >= 3) {
    prefix = argv[2];
    const size_t dot = prefix.rfind('.');
    if (dot != std::string::npos) {
      prefix.resize(dot);
    }
  }
  at = ParseTextures(b, d, at, h[6], prefix);
  std::printf("after textures @%zX of %zX, tail", at, d.size());
  for (size_t k = at; k < d.size() && k < at + 24; ++k) {
    std::printf(" %02X", d[k]);
  }
  std::printf("\n");
  return 0;
}
