/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_avatar_format.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "third_party/mspack/lzx.h"
#include "third_party/mspack/mspack.h"
#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xna {
namespace avatar {

namespace {

constexpr uint32_t kDescriptionMagic = 0x5641584Eu;
constexpr uint32_t kDescriptionVersion = 0x32305641u;
constexpr size_t kItemsOffset = 12;
constexpr size_t kColorsOffset = 48;
constexpr size_t kCustomOffset = 84;
constexpr float kMinHeight = 1.4120882f;
constexpr float kHeightRange = 0.25442278f;
constexpr float kLatticeX = 2.0f;
constexpr float kLatticeY = 1.6329932f;
constexpr float kLatticeZ = 1.7320508f;
constexpr float kRowOffset = 1.0f / 3.0f;
constexpr float kColumnOffset = 0.5f;
constexpr uint64_t kClipHeaderBits = 64 + 72 * 438;
constexpr uint32_t kCoverageMask = 0x7FFFCu;
constexpr uint32_t kOutfitBit = 0x800000u;
constexpr uint32_t kHeadEntry = 2;
constexpr uint32_t kMaleBodyEntry = 0;
constexpr uint32_t kFemaleBodyEntry = 1;
constexpr uint32_t kParamTexture = 1;

const uint8_t kMainSkeletonData[] = {
    0x47, 0x00, 0x00, 0x00, 0x02, 0x44, 0xED, 0x36, 0x5E, 0x39, 0x2E, 0xBF,
    0x6B, 0xB0, 0x80, 0x3C, 0xF2, 0x7B, 0x88, 0xBD, 0x51, 0xE4, 0x08, 0x10,
    0xB5, 0xDB, 0x00, 0x00, 0x00, 0x93, 0x02, 0x00, 0xD0, 0x96, 0x02, 0x00,
    0xE0, 0x94, 0x02, 0x00, 0xF0, 0xBF, 0xBF, 0x6B, 0x30, 0xA0, 0x9F, 0x04,
    0xB0, 0xBF, 0x8B, 0x34, 0xA1, 0x9F, 0x04, 0xB0, 0x4D, 0x4D, 0xA7, 0x5B,
    0x50, 0x05, 0xB0, 0x31, 0x4A, 0xA7, 0x5B, 0x50, 0x05, 0xB0, 0xBF, 0xAB,
    0x7B, 0xA1, 0x9F, 0x14, 0xB0, 0xBF, 0x8B, 0x1A, 0x26, 0x02, 0x24, 0xB0,
    0x4D, 0xCD, 0x6F, 0x4F, 0x33, 0x24, 0xB0, 0x4D, 0x8D, 0x8B, 0xD5, 0xCB,
    0x34, 0xB0, 0x31, 0xCA, 0x6F, 0x4F, 0x33, 0x34, 0xB0, 0x31, 0x8A, 0x8B,
    0xD5, 0xCB, 0x14, 0xB0, 0xBF, 0x2B, 0xCB, 0xA3, 0x36, 0x64, 0x40, 0x69,
    0xAD, 0xA1, 0x43, 0x06, 0x52, 0x60, 0xE0, 0xAB, 0x47, 0x6B, 0x25, 0x65,
    0xD0, 0x59, 0x0D, 0xA3, 0x49, 0x3D, 0x53, 0xB0, 0xBF, 0xAB, 0x73, 0xED,
    0x81, 0x82, 0x20, 0x16, 0xAA, 0xA1, 0x43, 0x06, 0x52, 0x00, 0x9F, 0xAB,
    0x47, 0x6B, 0x25, 0x85, 0x80, 0x25, 0x0A, 0xA3, 0x49, 0x3D, 0x53, 0xB0,
    0xBF, 0xCB, 0xF0, 0x27, 0xA2, 0xE3, 0xB0, 0xBF, 0x8B, 0xB4, 0x31, 0x0E,
    0xC4, 0x30, 0xE8, 0x6D, 0x48, 0x6B, 0xEB, 0xB2, 0xC0, 0x86, 0x0D, 0x00,
    0xC0, 0x1F, 0x0B, 0x31, 0x97, 0x69, 0x48, 0x6B, 0xEB, 0xF2, 0x90, 0xF8,
    0x09, 0x00, 0xC0, 0x1F, 0xEB, 0xB0, 0xBF, 0xAB, 0xDE, 0xEE, 0x05, 0x43,
    0x01, 0xB5, 0x31, 0x10, 0xEB, 0xFA, 0x42, 0x91, 0xCE, 0xCF, 0x33, 0x2B,
    0xF1, 0x42, 0xA1, 0xFB, 0x6D, 0x48, 0x6B, 0xEB, 0x62, 0x61, 0xCA, 0x25,
    0x10, 0xEB, 0xFA, 0x62, 0xD1, 0xB0, 0xC7, 0x33, 0x2B, 0xF1, 0x62, 0xC1,
    0x83, 0x69, 0x48, 0x6B, 0xEB, 0x92, 0xB1, 0xA8, 0x73, 0xF3, 0x2A, 0x1E,
    0x93, 0xC1, 0x2B, 0x93, 0xFD, 0xEA, 0x11, 0x93, 0x91, 0xA2, 0xF4, 0xE4,
    0xAA, 0x2F, 0xC3, 0xA1, 0xD6, 0x63, 0xF3, 0x2A, 0x1E, 0xC3, 0x91, 0x53,
    0x84, 0xFD, 0xEA, 0x11, 0xC3, 0xC1, 0xDC, 0xE2, 0xE4, 0xAA, 0x2F, 0x13,
    0xB2, 0x39, 0x76, 0xDD, 0xA9, 0x0F, 0x16, 0x82, 0x40, 0xB6, 0xE7, 0x29,
    0xF1, 0x13, 0x32, 0x27, 0xF6, 0xCA, 0xA9, 0xEC, 0x11, 0xC2, 0xF5, 0x95,
    0xA2, 0x69, 0x06, 0x10, 0x92, 0xB8, 0xB5, 0x3E, 0xA4, 0x2F, 0x13, 0x92,
    0x43, 0x96, 0xE7, 0xA6, 0x2F, 0x13, 0x92, 0xA2, 0xF4, 0xE4, 0x6A, 0xDB,
    0x43, 0xA2, 0x45, 0x61, 0xDD, 0xA9, 0x0F, 0x46, 0xD2, 0x3E, 0xA1, 0xE7,
    0x29, 0xF1, 0x43, 0x22, 0x58, 0xE1, 0xCA, 0xA9, 0xEC, 0x41, 0xA2, 0x89,
    0x81, 0xA2, 0x69, 0x06, 0x40, 0xC2, 0xC6, 0xA1, 0x3E, 0xA4, 0x2F, 0x43,
    0xD2, 0x3B, 0x81, 0xE7, 0xA6, 0x2F, 0x43, 0xD2, 0xDC, 0xE2, 0xE4, 0x6A,
    0xDB, 0x53, 0xF2, 0xE9, 0x76, 0xDD, 0x69, 0x16, 0x66, 0x52, 0xFB, 0xB6,
    0xE7, 0xE9, 0xF0, 0x73, 0x52, 0xDA, 0xF6, 0xCA, 0xA9, 0xE6, 0x81, 0x12,
    0x8D, 0x96, 0xA2, 0x29, 0x00, 0xB0, 0x52, 0x98, 0x75, 0x8E, 0x29, 0x36,
    0xC7, 0x72, 0x95, 0x60, 0xDD, 0x69, 0x16, 0xD6, 0x12, 0x84, 0xA0, 0xE7,
    0xE9, 0xF0, 0xE3, 0x12, 0xA5, 0xE0, 0xCA, 0xA9, 0xE6, 0xF1, 0x52, 0xF2,
    0x80, 0xA2, 0x29, 0x00, 0x20, 0x03, 0xE7, 0x61, 0x8E, 0x29, 0x36, 0x37,
    0xF3, 0x68, 0x77, 0xDD, 0x69, 0x16, 0x46, 0x63, 0x7F, 0xB7, 0xE7, 0x29,
    0xF1, 0x53, 0x03, 0x5C, 0xF7, 0xCA, 0xA9, 0xE6, 0x61, 0x43, 0xFC, 0x96,
    0xA2, 0x29, 0x00, 0x70, 0xE3, 0x10, 0x76, 0xEE, 0xA8, 0x88, 0x88, 0x73,
    0x16, 0x60, 0xDD, 0x69, 0x16, 0x96, 0x03, 0x00, 0xA0, 0xE7, 0x29, 0xF1,
    0xA3, 0x53, 0x23, 0xE0, 0xCA, 0xA9, 0xE6, 0xB1, 0x23, 0x83, 0x80, 0xA2,
    0x29, 0x00, 0xC0, 0x73, 0x6E, 0x61, 0xEE, 0xA8, 0x88, 0x08,
};

constexpr int32_t kPresetClips[] = {3,  4,  5,  39, 40, 41, 42, 43, 6,  7,  8,
                                    9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                    20, 21, 38, 22, 23, 24, 25, 26, 27};

constexpr uint32_t kSkinPalette[] = {0xFFF3D5C0, 0xFFE8BE9C, 0xFFD9A77F,
                                     0xFFC68A62, 0xFFA96E4A, 0xFF8A5638,
                                     0xFF6B3F27, 0xFF4E2C1B};
constexpr uint32_t kHairPalette[] = {0xFF1B1511, 0xFF3B2618, 0xFF5E3A20,
                                     0xFF8A5A2B, 0xFFB98546, 0xFFE2C07A,
                                     0xFFA03A1C, 0xFF6F6F6F, 0xFFD9D9D9};
constexpr uint32_t kLipPalette[] = {0xFFB5655E, 0xFFC4726C, 0xFF9E4F4A,
                                    0xFFD08A84, 0xFF8C3F3F};
constexpr uint32_t kIrisPalette[] = {0xFF3D6FA8, 0xFF4E8A4A, 0xFF6B4423,
                                     0xFF3A2A1C, 0xFF7C8C98, 0xFF2F8A8A};
constexpr uint32_t kShadowPalette[] = {0xFF6A4C8C, 0xFF3C6E9E, 0xFF8C4C6A,
                                       0xFF4A4A4A};
constexpr uint32_t kClothingPalette[] = {
    0xFFC0392B, 0xFF2E86C1, 0xFF28B463, 0xFFF1C40F, 0xFF8E44AD, 0xFFE67E22,
    0xFF1ABC9C, 0xFF34495E, 0xFFECF0F1, 0xFF2C2C2C, 0xFF7F8C8D, 0xFFD35400};

const char* const kSlotNames[kSlotCount] = {
    "Hair",     "Shirt",     "Trousers",    "Shoes",      "Hat",       "Gloves",
    "Glasses",  "Wristwear", "Earrings",    "Ring",       "Carryable", "Eyes",
    "Eyebrows", "Mouth",     "Facial hair", "Face paint", "Eye shadow"};

const char* const kColorNames[kColorCount] = {
    "Skin",       "Hair",        "Lips",      "Eyes",     "Eyebrows",
    "Eye shadow", "Facial hair", "Feature 1", "Feature 2"};

struct HeadLayer {
  uint32_t usage;
  uint32_t feature;
  uint32_t kind;
  int32_t color;
  int32_t expression;
};

constexpr HeadLayer kHeadLayers[] = {
    {5, kFeatureFacePaint, kLayerDecal, -1, -1},
    {6, kFeatureFacialHair, kLayerFeature, kColorFacialHair, -1},
    {7, kFeatureEyebrows, kLayerFeature, kColorEyebrow, 1},
    {8, kFeatureEyebrows, kLayerFeature, kColorEyebrow, 2},
    {9, kFeatureEyes, kLayerFeature, kColorIris, 3},
    {10, kFeatureEyes, kLayerFeature, kColorIris, 4},
    {11, kFeatureEyeShadow, kLayerFeature, kColorEyeShadow, -1},
    {12, kFeatureMouth, kLayerFeature, kColorLips, 0},
};

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
    float value;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
  }

  bool Holds(uint64_t end_bit) const { return (end_bit + 7) / 8 <= size; }
};

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

void PutLe32(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value);
  p[1] = uint8_t(value >> 8);
  p[2] = uint8_t(value >> 16);
  p[3] = uint8_t(value >> 24);
}

float Half(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31;
  int32_t exponent = (h >> 10) & 0x1F;
  uint32_t mantissa = h & 0x3FF;
  uint32_t out;
  if (exponent == 0) {
    if (!mantissa) {
      out = sign;
    } else {
      exponent = 1;
      while (!(mantissa & 0x400)) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FF;
      out = sign | uint32_t(exponent + 112) << 23 | mantissa << 13;
    }
  } else if (exponent == 31) {
    out = sign | 0x7F800000u | mantissa << 13;
  } else {
    out = sign | uint32_t(exponent + 112) << 23 | mantissa << 13;
  }
  float value;
  std::memcpy(&value, &out, sizeof(value));
  return value;
}

struct Lattice {
  float scale = 0.0f;
  float min[3] = {};
  uint32_t width[3] = {};
};

Lattice ReadLattice(const Bits& b, uint64_t bit) {
  Lattice lattice;
  lattice.scale = b.Float(bit);
  for (int k = 0; k < 3; ++k) {
    lattice.min[k] = b.Float(bit + 32 + 32 * k);
    lattice.width[k] = b.Get(bit + 128 + 6 * k, 6);
  }
  return lattice;
}

bool LatticeValid(const Lattice& lattice) {
  return lattice.width[0] <= 32 && lattice.width[1] <= 32 &&
         lattice.width[2] <= 32;
}

uint32_t LatticeBits(const Lattice& lattice) {
  return lattice.width[0] + lattice.width[1] + lattice.width[2];
}

void ReadPoint(const Bits& b, uint64_t* bit, const Lattice& lattice,
               float out[3]) {
  uint32_t raw[3];
  for (int k = 0; k < 3; ++k) {
    raw[k] = b.Get(*bit, lattice.width[k]);
    *bit += lattice.width[k];
  }
  const float step[3] = {lattice.scale * kLatticeX, lattice.scale * kLatticeY,
                         lattice.scale * kLatticeZ};
  out[1] = float(raw[1]) * step[1] + lattice.min[1];
  out[2] = float(raw[2]) * step[2] + lattice.min[2];
  if (raw[1] & 1) {
    out[2] += step[2] * kRowOffset;
  }
  out[0] = float(raw[0]) * step[0] + lattice.min[0];
  if ((raw[2] ^ raw[1]) & 1) {
    out[0] += step[0] * kColumnOffset;
  }
}

void RotationToQuaternion(const float v[3], float q[4]) {
  const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  const float scale = length > 0.0f ? std::sin(length) / length : 1.0f;
  q[0] = v[0] * scale;
  q[1] = v[1] * scale;
  q[2] = v[2] * scale;
  q[3] = std::cos(length);
}

void DecodeNormal(uint32_t packed, float out[3]) {
  const int32_t x = int32_t(packed << 21) >> 21;
  const int32_t y = int32_t(packed << 10) >> 21;
  const int32_t z = int32_t(packed) >> 22;
  out[0] = float(x) / 1023.0f;
  out[1] = float(y) / 1023.0f;
  out[2] = float(z) / 511.0f;
  const float length =
      std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
  if (length > 0.0f) {
    out[0] /= length;
    out[1] /= length;
    out[2] /= length;
  } else {
    out[1] = 1.0f;
  }
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
  uint8_t palette[4][4];
  Rgb565(c0, palette[0]);
  Rgb565(c1, palette[1]);
  for (int k = 0; k < 3; ++k) {
    if (!dxt1 || c0 > c1) {
      palette[2][k] = uint8_t((2 * palette[0][k] + palette[1][k]) / 3);
      palette[3][k] = uint8_t((palette[0][k] + 2 * palette[1][k]) / 3);
    } else {
      palette[2][k] = uint8_t((palette[0][k] + palette[1][k]) / 2);
      palette[3][k] = 0;
    }
  }
  palette[2][3] = 255;
  palette[3][3] = (dxt1 && c0 <= c1) ? 0 : 255;
  const uint32_t bits = uint32_t(p[4] | p[5] << 8 | p[6] << 16 | p[7] << 24);
  for (int i = 0; i < 16; ++i) {
    std::memcpy(out[i], palette[(bits >> (2 * i)) & 3], 4);
  }
}

void DecodeAlphaBlock(const uint8_t* p, uint8_t out[16][4]) {
  uint8_t palette[8];
  palette[0] = p[0];
  palette[1] = p[1];
  if (palette[0] > palette[1]) {
    for (int k = 1; k < 7; ++k) {
      palette[k + 1] = uint8_t(((7 - k) * palette[0] + k * palette[1]) / 7);
    }
  } else {
    for (int k = 1; k < 5; ++k) {
      palette[k + 1] = uint8_t(((5 - k) * palette[0] + k * palette[1]) / 5);
    }
    palette[6] = 0;
    palette[7] = 255;
  }
  uint64_t bits = 0;
  for (int k = 0; k < 6; ++k) {
    bits |= uint64_t(p[2 + k]) << (8 * k);
  }
  for (int i = 0; i < 16; ++i) {
    out[i][3] = palette[(bits >> (3 * i)) & 7];
  }
}

bool DecodeSlice(const uint8_t* data, size_t size, uint32_t format,
                 uint32_t width, uint32_t height, uint32_t pitch,
                 uint8_t* rgba) {
  const uint32_t kind = format & 0x3F;
  const bool dxt1 = kind == 0x12;
  const bool dxt3 = kind == 0x13;
  const bool dxt5 = kind == 0x14;
  if (kind == 0x06) {
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const size_t s = size_t(y) * pitch + size_t(x) * 4;
        if (s + 4 > size) {
          return false;
        }
        uint8_t* d = rgba + (size_t(y) * width + x) * 4;
        d[0] = data[s + 1];
        d[1] = data[s + 2];
        d[2] = data[s + 3];
        d[3] = data[s + 0];
      }
    }
    return true;
  }
  if (!dxt1 && !dxt3 && !dxt5) {
    return false;
  }
  const uint32_t block = dxt1 ? 8 : 16;
  const uint32_t blocks_wide = (width + 3) / 4;
  const uint32_t blocks_high = (height + 3) / 4;
  for (uint32_t by = 0; by < blocks_high; ++by) {
    for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
      const size_t s = size_t(by) * pitch + size_t(bx) * block;
      if (s + block > size) {
        return false;
      }
      uint8_t swapped[16];
      std::memcpy(swapped, data + s, block);
      for (uint32_t k = 0; k + 1 < block; k += 2) {
        std::swap(swapped[k], swapped[k + 1]);
      }
      uint8_t pixels[16][4];
      if (dxt1) {
        DecodeColorBlock(swapped, true, pixels);
      } else {
        DecodeColorBlock(swapped + 8, false, pixels);
        if (dxt5) {
          DecodeAlphaBlock(swapped, pixels);
        } else {
          for (int i = 0; i < 16; ++i) {
            const uint8_t nibble = (swapped[i / 2] >> ((i & 1) * 4)) & 15;
            pixels[i][3] = uint8_t(nibble * 17);
          }
        }
      }
      for (int i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3);
        const uint32_t y = by * 4 + (i >> 2);
        if (x < width && y < height) {
          std::memcpy(rgba + (size_t(y) * width + x) * 4, pixels[i], 4);
        }
      }
    }
  }
  return true;
}

bool ReadTexture(const Bits& b, const std::vector<uint8_t>& d, size_t* at,
                 std::shared_ptr<const Texture>* out) {
  if (*at + 0x21 > d.size()) {
    return false;
  }
  const uint64_t hb = uint64_t(*at) * 8;
  const uint32_t format = b.Get(hb, 32);
  const uint32_t width = b.Get(hb + 32, 32);
  const uint32_t height = b.Get(hb + 64, 32);
  const uint32_t slices = b.Get(hb + 160, 32);
  const uint32_t zero_fill = b.Get(hb + 192, 1);
  const uint32_t pitch = b.Get(hb + 194, 32);
  const uint32_t rows = b.Get(hb + 226, 32);
  *at += 0x21;
  out->reset();
  if (zero_fill) {
    return true;
  }
  if (!width || !height || width > 4096 || height > 4096 || !slices ||
      slices > 64 || !pitch || pitch > 65536 || rows > 4096) {
    return false;
  }
  const size_t slice_bytes = size_t(rows) * pitch;
  const size_t bytes = slice_bytes * slices;
  if (*at + bytes > d.size()) {
    return false;
  }
  auto texture = std::make_shared<Texture>();
  texture->width = width;
  texture->height = height;
  texture->slices = slices;
  const size_t pixels = size_t(width) * height * 4;
  texture->rgba.assign(pixels * slices, 0);
  for (uint32_t s = 0; s < slices; ++s) {
    if (!DecodeSlice(d.data() + *at + s * slice_bytes, slice_bytes, format,
                     width, height, pitch, texture->rgba.data() + s * pixels)) {
      return false;
    }
  }
  *at += bytes;
  *out = texture;
  return true;
}

bool ReadRawVertices(const Bits& b, size_t at, uint32_t uv_sets,
                     std::vector<RawVertex>* out, size_t* consumed) {
  const uint64_t base = uint64_t(at) * 8;
  if (!b.Holds(base + 434)) {
    return false;
  }
  const uint32_t count = b.Get(base, 32);
  const Lattice lattice = ReadLattice(b, base + 32);
  if (count > 65536 || !LatticeValid(lattice)) {
    return false;
  }
  uint32_t int_base[4];
  uint32_t int_bits[4];
  uint64_t per_vertex = LatticeBits(lattice) + 32ull * uv_sets;
  for (int k = 0; k < 4; ++k) {
    int_base[k] = b.Get(base + 178 + 64 * k, 32);
    int_bits[k] = b.Get(base + 178 + 64 * k + 32, 32);
    if (int_bits[k] > 32) {
      return false;
    }
    per_vertex += int_bits[k];
  }
  if (!b.Holds(base + 434 + uint64_t(count) * per_vertex)) {
    return false;
  }
  out->resize(count);
  uint64_t bit = base + 434;
  for (uint32_t i = 0; i < count; ++i) {
    RawVertex& vertex = (*out)[i];
    ReadPoint(b, &bit, lattice, vertex.position);
    uint32_t* values[4] = {&vertex.normal, &vertex.weights, &vertex.bindings,
                           &vertex.color};
    for (int k = 0; k < 4; ++k) {
      *values[k] = int_base[k] + b.Get(bit, int_bits[k]);
      bit += int_bits[k];
    }
    for (uint32_t k = 0; k < uv_sets * 2 && k < kLayerCount * 2; ++k) {
      vertex.uv[k] = uint16_t(b.Get(bit, 16));
      bit += 16;
    }
  }
  *consumed = size_t((uint64_t(count) * per_vertex + 441) >> 3);
  return true;
}

bool ReadVertices(const Bits& b, size_t at, uint32_t uv_sets,
                  std::vector<Vertex>* out, size_t* consumed) {
  std::vector<RawVertex> raw;
  if (!ReadRawVertices(b, at, uv_sets, &raw, consumed)) {
    return false;
  }
  out->resize(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    const RawVertex& source = raw[i];
    Vertex& vertex = (*out)[i];
    std::memcpy(vertex.position, source.position, sizeof(vertex.position));
    DecodeNormal(source.normal, vertex.normal);
    float sum = 0.0f;
    for (int k = 0; k < 4; ++k) {
      vertex.weights[k] = float((source.weights >> (8 * k)) & 0xFF) / 255.0f;
      vertex.bones[k] = uint8_t(source.bindings >> (8 * k));
      sum += vertex.weights[k];
    }
    if (sum > 0.0f) {
      for (int k = 0; k < 4; ++k) {
        vertex.weights[k] /= sum;
      }
    } else {
      vertex.weights[0] = 1.0f;
    }
    for (uint32_t s = 0; s < kLayerCount; ++s) {
      if (s < uv_sets) {
        vertex.uv[s][0] = Half(source.uv[s * 2]);
        vertex.uv[s][1] = Half(source.uv[s * 2 + 1]);
      } else {
        vertex.uv[s][0] = vertex.uv[0][0];
        vertex.uv[s][1] = vertex.uv[0][1];
      }
    }
  }
  return true;
}

bool ReadRawTexture(const Bits& b, const std::vector<uint8_t>& d, size_t* at,
                    RawTexture* out) {
  if (*at + 0x21 > d.size()) {
    return false;
  }
  const uint64_t hb = uint64_t(*at) * 8;
  RawTexture texture;
  texture.format = b.Get(hb, 32);
  texture.width = b.Get(hb + 32, 32);
  texture.height = b.Get(hb + 64, 32);
  texture.total_size = b.Get(hb + 96, 32);
  texture.slice_size = b.Get(hb + 128, 32);
  texture.slices = b.Get(hb + 160, 32);
  texture.zero_fill = b.Get(hb + 192, 1) != 0;
  texture.tiled = b.Get(hb + 193, 1) != 0;
  texture.pitch = b.Get(hb + 194, 32);
  texture.rows = b.Get(hb + 226, 32);
  *at += 0x21;
  if (!texture.width || !texture.height || texture.width > 4096 ||
      texture.height > 4096 || !texture.slices || texture.slices > 64) {
    return false;
  }
  if (!texture.zero_fill) {
    if (!texture.pitch || texture.pitch > 65536 || texture.rows > 4096) {
      return false;
    }
    const size_t bytes = size_t(texture.pitch) * texture.rows * texture.slices;
    if (*at + bytes > d.size()) {
      return false;
    }
    texture.data.assign(d.begin() + *at, d.begin() + *at + bytes);
    *at += bytes;
  }
  *out = std::move(texture);
  return true;
}

bool ReadIndices(const Bits& b, size_t at, std::vector<uint16_t>* out,
                 size_t* consumed) {
  const uint64_t base = uint64_t(at) * 8;
  if (!b.Holds(base + 64)) {
    return false;
  }
  const uint32_t count = b.Get(base, 32);
  const uint32_t first = b.Get(base + 32, 16);
  const uint32_t bits = b.Get(base + 48, 16);
  if (bits > 16 || count > (1u << 20) ||
      !b.Holds(base + 64 + uint64_t(count) * bits)) {
    return false;
  }
  out->resize(count);
  uint64_t bit = base + 64;
  for (uint32_t k = 0; k < count; ++k) {
    (*out)[k] = uint16_t((first + b.Get(bit, bits)) & 0xFFFF);
    bit += bits;
  }
  *consumed = size_t((uint64_t(count) * bits + 64 + 7) >> 3);
  return true;
}

struct LzxContext {
  const uint8_t* input = nullptr;
  size_t input_size = 0;
  size_t input_at = 0;
  std::vector<uint8_t> output;
};

int LzxRead(mspack_file* file, void* buffer, int bytes) {
  auto* context = reinterpret_cast<LzxContext*>(file);
  const size_t take =
      std::min<size_t>(size_t(bytes), context->input_size - context->input_at);
  std::memcpy(buffer, context->input + context->input_at, take);
  context->input_at += take;
  return int(take);
}

int LzxWrite(mspack_file* file, void* buffer, int bytes) {
  auto* context = reinterpret_cast<LzxContext*>(file);
  auto* in = static_cast<uint8_t*>(buffer);
  context->output.insert(context->output.end(), in, in + bytes);
  return bytes;
}

void* LzxAlloc(mspack_system*, size_t bytes) { return std::malloc(bytes); }
void LzxFree(void* pointer) { std::free(pointer); }
void LzxCopy(void* source, void* destination, size_t bytes) {
  std::memcpy(destination, source, bytes);
}
void LzxMessage(mspack_file*, const char*, ...) {}

bool Inflate(const uint8_t* input, size_t input_size, size_t output_size,
             std::vector<uint8_t>* out) {
  LzxContext context;
  context.input = input;
  context.input_size = input_size;
  mspack_system system = {};
  system.read = LzxRead;
  system.write = LzxWrite;
  system.alloc = LzxAlloc;
  system.free = LzxFree;
  system.copy = LzxCopy;
  system.message = LzxMessage;
  lzxd_stream* lzx =
      lzxd_init(&system, reinterpret_cast<mspack_file*>(&context),
                reinterpret_cast<mspack_file*>(&context), 15, 0, 32 * 1024,
                off_t(output_size), 0);
  if (!lzx) {
    return false;
  }
  const int status = lzxd_decompress(lzx, off_t(output_size));
  lzxd_free(lzx);
  *out = std::move(context.output);
  return status == MSPACK_ERR_OK && out->size() == output_size;
}

void AppendUtf8(std::string* out, uint32_t code) {
  if (code < 0x80) {
    out->push_back(char(code));
  } else if (code < 0x800) {
    out->push_back(char(0xC0 | (code >> 6)));
    out->push_back(char(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out->push_back(char(0xE0 | (code >> 12)));
    out->push_back(char(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(char(0x80 | (code & 0x3F)));
  } else {
    out->push_back(char(0xF0 | (code >> 18)));
    out->push_back(char(0x80 | ((code >> 12) & 0x3F)));
    out->push_back(char(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(char(0x80 | (code & 0x3F)));
  }
}

std::string Utf16BeName(const std::vector<uint8_t>& d, size_t at) {
  std::string name;
  while (at + 1 < d.size() && name.size() < 160) {
    uint32_t unit = uint32_t(d[at] << 8) | d[at + 1];
    at += 2;
    if (!unit) {
      break;
    }
    if (unit >= 0xD800 && unit < 0xDC00 && at + 1 < d.size()) {
      const uint32_t low = uint32_t(d[at] << 8) | d[at + 1];
      if (low >= 0xDC00 && low < 0xE000) {
        unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        at += 2;
      }
    }
    AppendUtf8(&name, unit);
  }
  return name;
}

void ColorToFloat(uint32_t argb, float out[4]) {
  out[0] = float((argb >> 16) & 0xFF) / 255.0f;
  out[1] = float((argb >> 8) & 0xFF) / 255.0f;
  out[2] = float(argb & 0xFF) / 255.0f;
  out[3] = 1.0f;
}

Matrix Translation(float x, float y, float z) {
  Matrix m = Identity();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  return m;
}

Matrix FromQuaternion(const float q[4], const float t[3]) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  Matrix m;
  m[0] = 1.0f - 2.0f * (y * y + z * z);
  m[1] = 2.0f * (x * y + z * w);
  m[2] = 2.0f * (x * z - y * w);
  m[3] = 0.0f;
  m[4] = 2.0f * (x * y - z * w);
  m[5] = 1.0f - 2.0f * (x * x + z * z);
  m[6] = 2.0f * (y * z + x * w);
  m[7] = 0.0f;
  m[8] = 2.0f * (x * z + y * w);
  m[9] = 2.0f * (y * z - x * w);
  m[10] = 1.0f - 2.0f * (x * x + y * y);
  m[11] = 0.0f;
  m[12] = t[0];
  m[13] = t[1];
  m[14] = t[2];
  m[15] = 1.0f;
  return m;
}

void TransformPoint(const Matrix& m, const float p[3], float out[3]) {
  for (int c = 0; c < 3; ++c) {
    out[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
  }
}

void TransformVector(const Matrix& m, const float v[3], float out[3]) {
  for (int c = 0; c < 3; ++c) {
    out[c] = v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c];
  }
}

void Slerp(const float* a, const float* b, float t, float out[4]) {
  float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  float target[4] = {b[0], b[1], b[2], b[3]};
  if (dot < 0.0f) {
    dot = -dot;
    for (float& value : target) {
      value = -value;
    }
  }
  float wa;
  float wb;
  if (dot > 0.9995f) {
    wa = 1.0f - t;
    wb = t;
  } else {
    const float theta = std::acos(dot);
    const float s = std::sin(theta);
    wa = std::sin((1.0f - t) * theta) / s;
    wb = std::sin(t * theta) / s;
  }
  float length = 0.0f;
  for (int k = 0; k < 4; ++k) {
    out[k] = a[k] * wa + target[k] * wb;
    length += out[k] * out[k];
  }
  length = std::sqrt(length);
  if (length > 0.0f) {
    for (int k = 0; k < 4; ++k) {
      out[k] /= length;
    }
  }
}

void BindOffset(const Skeleton& skeleton, uint32_t joint, float out[3]) {
  const uint32_t parent = skeleton.parents[joint];
  for (int k = 0; k < 3; ++k) {
    out[k] = parent < joint
                 ? skeleton.bind[joint][k] - skeleton.bind[parent][k]
                 : skeleton.bind[joint][k] * skeleton.scale[joint][k];
  }
}

uint32_t BodyBit(uint32_t body) { return body ? 1u : 2u; }

bool Wearable(const Entry& entry, uint32_t body) {
  return entry.blob && (entry.BodyMask() & BodyBit(body)) &&
         !IsHatVariant(entry);
}

uint32_t FindBody(const Catalog& catalog, uint32_t body) {
  const uint32_t preferred = body ? kMaleBodyEntry : kFemaleBodyEntry;
  const Entry* entry = catalog.Find(preferred);
  if (entry && entry->kind == kKindBody && entry->blob) {
    return preferred;
  }
  for (const Entry& candidate : catalog.entries()) {
    if (candidate.kind == kKindBody && candidate.blob &&
        (candidate.BodyMask() & BodyBit(body))) {
      return candidate.index;
    }
  }
  return UINT32_MAX;
}

uint32_t FindHead(const Catalog& catalog) {
  const Entry* entry = catalog.Find(kHeadEntry);
  if (entry && entry->kind == kKindHead && entry->blob) {
    return kHeadEntry;
  }
  for (const Entry& candidate : catalog.entries()) {
    if (candidate.kind == kKindHead && candidate.blob) {
      return candidate.index;
    }
  }
  return UINT32_MAX;
}

// The pack names each hairstyle's under-a-hat version outright, so take it
// from there. Matching on the name plus " (Hat)" got nine of the ninety wrong -
// The Captain Cut wears Old Hair (Hat), both Comb Overs share Left Comb Over
// (Hat), both Partings share Parting (Hat) - and it only ever worked in
// English.
uint32_t HatVariantOf(const Catalog& catalog, uint32_t hair) {
  const Entry* entry = catalog.Find(hair);
  if (!entry || entry->substitute == UINT32_MAX) {
    return hair;
  }
  const Entry* worn = catalog.Find(entry->substitute);
  return worn && worn->blob ? worn->index : hair;
}

int32_t FeatureForSlot(uint32_t slot) {
  switch (slot) {
    case kSlotEyes:
      return kFeatureEyes;
    case kSlotEyebrows:
      return kFeatureEyebrows;
    case kSlotMouth:
      return kFeatureMouth;
    case kSlotFacialHair:
      return kFeatureFacialHair;
    case kSlotFacePaint:
      return kFeatureFacePaint;
    case kSlotEyeShadow:
      return kFeatureEyeShadow;
    default:
      return -1;
  }
}

template <size_t N>
uint32_t Pick(const uint32_t (&palette)[N], std::mt19937& rng) {
  return palette[rng() % N];
}

void Sample(const Texture& texture, uint32_t slice, float u, float v,
            bool clamp, float out[4]) {
  const int32_t width = int32_t(texture.width);
  const int32_t height = int32_t(texture.height);
  const uint8_t* base =
      texture.rgba.data() + size_t(slice) * size_t(width) * height * 4;
  const float x = u * float(width) - 0.5f;
  const float y = v * float(height) - 0.5f;
  const float fx = std::floor(x);
  const float fy = std::floor(y);
  const float ax = x - fx;
  const float ay = y - fy;
  const int32_t x0 = int32_t(fx);
  const int32_t y0 = int32_t(fy);
  for (int k = 0; k < 4; ++k) {
    out[k] = 0.0f;
  }
  for (int dy = 0; dy < 2; ++dy) {
    for (int dx = 0; dx < 2; ++dx) {
      int32_t xi = x0 + dx;
      int32_t yi = y0 + dy;
      if (clamp) {
        xi = std::clamp(xi, 0, width - 1);
        yi = std::clamp(yi, 0, height - 1);
      } else {
        xi = ((xi % width) + width) % width;
        yi = ((yi % height) + height) % height;
      }
      const float weight = (dx ? ax : 1.0f - ax) * (dy ? ay : 1.0f - ay);
      const uint8_t* texel = base + (size_t(yi) * width + xi) * 4;
      for (int k = 0; k < 4; ++k) {
        out[k] += weight * float(texel[k]) / 255.0f;
      }
    }
  }
}

void Shade(const Material& material, const float (*uv)[2],
           const float normal[3], const Lighting& light, float out[4]) {
  float color[3] = {material.base[0], material.base[1], material.base[2]};
  float alpha = 1.0f;
  for (uint32_t i = 0; i < kLayerCount; ++i) {
    const uint32_t kind = material.layer[i][0];
    const Texture* texture = material.textures[i];
    if (!kind || !texture) {
      continue;
    }
    const float* coord = uv[std::min(material.layer[i][1], kLayerCount - 1)];
    float s[4];
    Sample(*texture, material.layer[i][2], coord[0], coord[1],
           material.layer[i][3] != 0, s);
    for (int k = 0; k < 3; ++k) {
      switch (kind) {
        case kLayerColor:
          color[k] = s[k];
          break;
        case kLayerMask: {
          const float mixed = material.custom[0][k] * s[0] +
                              material.custom[1][k] * s[1] +
                              material.custom[2][k] * s[2];
          color[k] += (mixed - color[k]) * s[3];
          break;
        }
        case kLayerDecal:
          color[k] += (s[k] - color[k]) * s[3];
          break;
        case kLayerFeature: {
          const float feature =
              std::min(1.0f, material.tint[i][k] * s[0] + s[1] + s[2]);
          color[k] += (feature - color[k]) * s[3];
          break;
        }
        default:
          break;
      }
    }
    if (kind == kLayerColor) {
      alpha = s[3];
    }
  }
  const float diffuse = std::max(
      0.0f, -(normal[0] * light.direction[0] + normal[1] * light.direction[1] +
              normal[2] * light.direction[2]));
  for (int k = 0; k < 3; ++k) {
    out[k] = std::min(1.0f,
                      color[k] * (light.ambient[k] + light.color[k] * diffuse));
  }
  out[3] = alpha;
}

void PutBe32(std::vector<uint8_t>* v, uint32_t x) {
  v->push_back(uint8_t(x >> 24));
  v->push_back(uint8_t(x >> 16));
  v->push_back(uint8_t(x >> 8));
  v->push_back(uint8_t(x));
}

uint32_t Crc32(const uint8_t* p, size_t n) {
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
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

void PngChunk(std::vector<uint8_t>* png, const char* tag,
              const std::vector<uint8_t>& data) {
  PutBe32(png, uint32_t(data.size()));
  const size_t start = png->size();
  png->insert(png->end(), tag, tag + 4);
  png->insert(png->end(), data.begin(), data.end());
  PutBe32(png, Crc32(png->data() + start, png->size() - start));
}

}  // namespace

uint32_t SlotBit(uint32_t slot) { return slot < kSlotCount ? 4u << slot : 0; }

int32_t PrimarySlot(uint32_t kind) {
  const uint32_t bits = kind & kCoverageMask;
  if (!bits || (kind & ~(kCoverageMask | kOutfitBit))) {
    return -1;
  }
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    if (bits & SlotBit(slot)) {
      return int32_t(slot);
    }
  }
  return -1;
}

uint32_t SlotCoverage(uint32_t kind) { return kind & kCoverageMask; }

// Chin, nose and ears are not worn in a slot - they are the three blend shapes
// at manifest + 0xC, indexed by Shape::Type_e.
int32_t BlendShapeSlot(uint32_t kind) {
  switch (kind) {
    case 0x00100000:
      return 0;
    case 0x00080000:
      return 1;
    case 0x00200000:
      return 2;
    default:
      return -1;
  }
}

const char* SlotName(uint32_t slot) {
  return slot < kSlotCount ? kSlotNames[slot] : "";
}

const char* ColorName(uint32_t color) {
  return color < kColorCount ? kColorNames[color] : "";
}

Description::Description() {
  items.fill(kNoItem);
  colors = {0xFFD9A77F, 0xFF3B2618, 0xFFB5655E, 0xFF3D6FA8, 0xFF3B2618,
            0xFF6A4C8C, 0xFF3B2618, 0xFFC08060, 0xFF804030};
  for (auto& slot : custom) {
    slot = {0xFF808080, 0xFF808080, 0xFF808080};
  }
}

std::array<uint8_t, kDescriptionBytes> SerializeDescription(
    const Description& description) {
  std::array<uint8_t, kDescriptionBytes> bytes = {};
  PutLe32(bytes.data(), kDescriptionMagic);
  bytes[4] = description.body;
  bytes[5] = description.height;
  bytes[6] = description.weight;
  PutLe32(bytes.data() + 8, kDescriptionVersion);
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    bytes[kItemsOffset + slot * 2] = uint8_t(description.items[slot]);
    bytes[kItemsOffset + slot * 2 + 1] = uint8_t(description.items[slot] >> 8);
  }
  for (uint32_t color = 0; color < kColorCount; ++color) {
    PutLe32(bytes.data() + kColorsOffset + color * 4,
            description.colors[color]);
  }
  for (uint32_t slot = 0; slot < kClothingSlotCount; ++slot) {
    for (uint32_t k = 0; k < 3; ++k) {
      PutLe32(bytes.data() + kCustomOffset + (slot * 3 + k) * 4,
              description.custom[slot][k]);
    }
  }
  return bytes;
}

bool HasDescriptionMagic(const uint8_t* bytes, size_t size) {
  return bytes && size >= 8 && Le32(bytes) == kDescriptionMagic;
}

bool ParseDescription(const uint8_t* bytes, size_t size, Description* out) {
  if (!HasDescriptionMagic(bytes, size) ||
      size < kCustomOffset + kClothingSlotCount * 12 ||
      Le32(bytes + 8) != kDescriptionVersion) {
    return false;
  }
  Description description;
  description.body = bytes[4] ? 1 : 0;
  description.height = bytes[5];
  description.weight = bytes[6];
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    description.items[slot] =
        uint16_t(bytes[kItemsOffset + slot * 2] |
                 (bytes[kItemsOffset + slot * 2 + 1] << 8));
  }
  for (uint32_t color = 0; color < kColorCount; ++color) {
    description.colors[color] = Le32(bytes + kColorsOffset + color * 4);
  }
  for (uint32_t slot = 0; slot < kClothingSlotCount; ++slot) {
    for (uint32_t k = 0; k < 3; ++k) {
      description.custom[slot][k] =
          Le32(bytes + kCustomOffset + (slot * 3 + k) * 4);
    }
  }
  *out = description;
  return true;
}

float DescriptionHeight(const Description& description) {
  return kMinHeight + kHeightRange * (float(description.height) / 255.0f);
}

float Clip::Length() const {
  return rate > 0.0f && frames ? float(frames) / rate : 0.0f;
}

bool DecodeTexture(const std::vector<uint8_t>& data, Texture* out) {
  Bits b{data.data(), data.size()};
  size_t at = 0;
  std::shared_ptr<const Texture> texture;
  if (!ReadTexture(b, data, &at, &texture) || !texture) {
    return false;
  }
  *out = *texture;
  return true;
}

// STRB record tag 4: Avatars::ShapeOverrides_c. Every offset below was read out
// of xam.xex - ShapeOverrides_c::Read at 0x81975D80 and the chain under
// VertexOverrides_c::Read at 0x8197A710 - and then checked against all 36 blend
// shapes in the shipping pack.
//
// The record is two halves, triangle overrides then vertex overrides, each a
// 32-byte descriptor followed by its own item stream. A blend shape's triangle
// count is zero, so its vertex descriptor sits on record byte 32; a hiding
// template's sits after the triangles. Every bit offset below is relative to
// that descriptor. The vertex header is 658 bits and its item stream follows
// it, starting two bits into its last byte - descriptor byte 82, bit 2.
//
// An item is: vertex, then x, y, z, then four more fields whose widths the
// header carries (in this pack the second and third are always zero bits). The
// three components are a HEXAGONAL CLOSE-PACKED lattice: odd y shifts z by a
// third of a step and odd (y^z) shifts x by half a step, which is what the
// constants 0.3333333 and 0.5 at 0x8163338C are for.
// The triangle half, Avatars::TriangleOverrides_c::Read at 0x8197A078. Its
// descriptor is the record's FIRST 32 bytes and its item stream starts at byte
// 32, bit 0 - no packer context, because a hidden triangle carries no payload:
//
//   +0x00 u32 count    triangles this template hides
//   +0x04 u32 total    the target model's ib_size; xam refuses the record
//                      unless it equals Model_c+0x10, read at header bit 0x80
//   +0x08     16 bytes the target model's asset id, _GUID order (little endian
//                      fields) where the TOC keeps the same id big endian
//   +0x18 u32 bias     added to every index; zero throughout this pack
//   +0x1C u32 width    bits per index; twelve throughout this pack
//
// xam then does, per index (0x8197A1F0):  tri = indices + idx * 6;
// tri[1] = tri[2] = tri[0] - it COLLAPSES the triangle, it does not remove it.
//
// Checked against all 231 templates in the shipping pack: every index lands
// inside the stated total, none repeats, and the vertex descriptor falls
// exactly on byte 32 + ceil(count * width / 8), which is where this leaves off.
bool DecodeHidden(const std::vector<uint8_t>& d, Shape* out,
                  size_t* header_at) {
  *header_at = 32;
  if (d.size() < 32) {
    return false;
  }
  const Bits t{d.data(), d.size()};
  const uint32_t count = t.Get(0, 32);
  const uint32_t total = t.Get(32, 32);
  const uint32_t bias = t.Get(192, 32);
  const uint32_t width = t.Get(224, 32);
  // The GUID's body field, the third little-endian member: 1 male, 2 female.
  out->body = uint32_t(d[14]) | (uint32_t(d[15]) << 8);
  if (!count) {
    return true;
  }
  if (!width || width > 32 || count > (1u << 20) ||
      !t.Holds(256 + uint64_t(count) * width)) {
    return false;
  }
  out->hidden.reserve(count);
  for (uint32_t k = 0; k < count; ++k) {
    const uint32_t index = t.Get(256 + uint64_t(k) * width, width) + bias;
    if (uint64_t(index) * 6 + 6 > total) {
      out->hidden.clear();
      return false;
    }
    out->hidden.push_back(index);
  }
  *header_at = 32 + size_t((uint64_t(count) * width + 7) / 8);
  return true;
}

bool DecodeShapeVertices(const std::vector<uint8_t>& d, size_t header_at,
                         Shape* out) {
  const uint64_t stream_bit = uint64_t(header_at + 82) * 8 + 2;
  // The pack states a vertex as a byte offset into the model's vertex buffer,
  // and the greatest common divisor of every offset in every shape is 52.
  constexpr uint32_t kVertexStride = 52;
  if (d.size() <= header_at + 83) {
    return false;
  }
  const Bits h{d.data() + header_at, d.size() - header_at};
  const uint32_t count = h.Get(0, 32);
  if (!count || count > 4096) {
    return false;
  }
  const uint32_t index_bias = h.Get(192, 32);
  const uint32_t index_width = h.Get(224, 32);
  // The vector packer's context, 0x92 bits at bit 256: a step, an origin and
  // three six-bit component widths.
  constexpr uint32_t kPacker = 256;
  const float step = h.Float(kPacker);
  const float origin[3] = {h.Float(kPacker + 0x20), h.Float(kPacker + 0x40),
                           h.Float(kPacker + 0x60)};
  const uint32_t width[3] = {h.Get(kPacker + 0x80, 6), h.Get(kPacker + 0x86, 6),
                             h.Get(kPacker + 0x8C, 6)};
  struct Field {
    uint32_t bias;
    uint32_t width;
  };
  const Field extra[4] = {
      {h.Get(kPacker + 0x92, 32), h.Get(kPacker + 0xB2, 32)},
      {h.Get(kPacker + 0xD2, 32), h.Get(kPacker + 0xF2, 32)},
      {h.Get(kPacker + 0x112, 32), h.Get(kPacker + 0x132, 32)},
      {h.Get(594, 32), h.Get(626, 32)},
  };
  uint64_t item_bits = index_width;
  for (uint32_t k = 0; k < 3; ++k) {
    if (width[k] > 32) {
      return false;
    }
    item_bits += width[k];
  }
  for (const Field& field : extra) {
    if (field.width > 32) {
      return false;
    }
    item_bits += field.width;
  }
  if (!index_width || index_width > 32 || !item_bits) {
    return false;
  }
  const Bits b{d.data(), d.size()};
  if (!b.Holds(stream_bit + item_bits * count)) {
    return false;
  }
  // Two thirds of a step in z and half a step in x, the HCP layer offsets.
  const float axis[3] = {step * 2.0f, step * 1.7320508f, step * 1.6329932f};
  std::vector<ShapeVertex> vertices;
  vertices.reserve(count);
  for (uint32_t k = 0; k < count; ++k) {
    uint64_t at = stream_bit + item_bits * k;
    const uint32_t offset = b.Get(at, index_width) + index_bias;
    at += index_width;
    uint32_t raw[3] = {};
    for (uint32_t c = 0; c < 3; ++c) {
      raw[c] = b.Get(at, width[c]);
      at += width[c];
    }
    ShapeVertex vertex;
    if (offset % kVertexStride) {
      return false;
    }
    vertex.offset = offset;
    vertex.position[1] = float(raw[1]) * axis[1] + origin[1];
    vertex.position[2] = float(raw[2]) * axis[2] + origin[2];
    if (raw[1] & 1) {
      vertex.position[2] += axis[2] * (1.0f / 3.0f);
    }
    vertex.position[0] = float(raw[0]) * axis[0] + origin[0];
    if ((raw[2] ^ raw[1]) & 1) {
      vertex.position[0] += axis[0] * 0.5f;
    }
    vertex.packed_normal = b.Get(at, extra[0].width) + extra[0].bias;
    vertices.push_back(vertex);
  }
  out->vertices = std::move(vertices);
  return true;
}

bool DecodeShape(const std::vector<uint8_t>& d, Shape* out) {
  Shape shape;
  size_t header_at = 32;
  if (!DecodeHidden(d, &shape, &header_at)) {
    return false;
  }
  // A hiding template's vertex half moves body vertices, and a body's stride
  // is not the head's 52, so that read is allowed to fail: the triangles are
  // the part that stops skin coming through and they stand on their own.
  if (!DecodeShapeVertices(d, header_at, &shape) && shape.hidden.empty()) {
    return false;
  }
  *out = std::move(shape);
  return true;
}

uint32_t HidingTemplateOf(const Catalog& catalog, uint32_t entry_index) {
  const Entry* entry = catalog.Find(entry_index);
  if (!entry || entry->asset_id == std::array<uint8_t, 16>{}) {
    return UINT32_MAX;
  }
  const uint32_t target =
      (uint32_t(entry->asset_id[4]) << 8) | uint32_t(entry->asset_id[5]);
  const Entry* template_entry = catalog.Find(target);
  if (!template_entry || template_entry->kind != kKindHidingTemplate ||
      !template_entry->blob) {
    return UINT32_MAX;
  }
  return target;
}

namespace {

// A triangle's number is its position in the model's one index buffer, so the
// batch that owns it is the one whose run covers it.
bool Collapse(std::vector<uint16_t>& indices, uint32_t first,
              uint32_t triangle) {
  const size_t count = indices.size() / 3;
  if (triangle < first || triangle - first >= count) {
    return false;
  }
  uint16_t* tri = indices.data() + size_t(triangle - first) * 3;
  tri[1] = tri[0];
  tri[2] = tri[0];
  return true;
}

}  // namespace

uint32_t HideTriangles(Model* model, const std::vector<uint32_t>& triangles) {
  uint32_t missed = 0;
  for (const uint32_t triangle : triangles) {
    bool placed = false;
    for (Batch& batch : model->batches) {
      placed |= Collapse(batch.indices, batch.first_triangle, triangle);
    }
    missed += !placed;
  }
  return missed;
}

uint32_t HideTriangles(RawModel* model,
                       const std::vector<uint32_t>& triangles) {
  uint32_t missed = 0;
  for (const uint32_t triangle : triangles) {
    bool placed = false;
    for (RawBatch& batch : model->batches) {
      // Both offsets are into the model's whole gpu buffer, so the index
      // buffer's own base has to come off before this is a triangle number.
      if (batch.ib_offset < model->ib_offset) {
        continue;
      }
      placed |= Collapse(batch.indices,
                         (batch.ib_offset - model->ib_offset) / 6, triangle);
    }
    missed += !placed;
  }
  return missed;
}

bool DecodeModel(const std::vector<uint8_t>& d, Model* out) {
  if (d.size() < 0x30) {
    return false;
  }
  Bits b{d.data(), d.size()};
  const uint32_t batch_count = b.Get(5 * 32, 32);
  const uint32_t texture_count = b.Get(6 * 32, 32);
  // Where the model's one index buffer starts inside its gpu buffer; a batch
  // states its own start the same way, so the difference is a triangle number.
  const uint32_t ib_offset = b.Get(256, 32);
  if (!batch_count || batch_count > 64 || texture_count > 64) {
    return false;
  }
  Model model;
  size_t at = 0x30;
  for (uint32_t index = 0; index < batch_count; ++index) {
    if (at + 0x21 > d.size()) {
      return false;
    }
    const uint64_t bb = uint64_t(at) * 8;
    Batch batch;
    batch.shader = b.Get(bb, 32);
    const uint32_t params = b.Get(bb + 32, 5);
    const uint32_t triangles = b.Get(bb + 37, 32);
    batch.uv_sets = b.Get(bb + 101, 32);
    // The batch's own index-buffer start, six bytes to the triangle. A hiding
    // template numbers triangles across the whole model and nothing else here
    // would say where a batch's run begins.
    const uint32_t batch_ib = b.Get(bb + 229, 32);
    batch.first_triangle =
        batch_ib >= ib_offset ? (batch_ib - ib_offset) / 6 : 0;
    at += 0x21;
    if (!batch.uv_sets || batch.uv_sets > kLayerCount ||
        at + size_t(params) * 0x18 > d.size()) {
      return false;
    }
    for (uint32_t p = 0; p < params; ++p) {
      const uint64_t pb = uint64_t(at) * 8;
      Param param;
      param.type = b.Get(pb, 32);
      param.usage = b.Get(pb + 32, 32);
      for (int k = 0; k < 4; ++k) {
        param.data[k] = b.Get(pb + 64 + 32 * k, 32);
      }
      batch.params.push_back(param);
      at += 0x18;
    }
    size_t used = 0;
    if (!ReadVertices(b, at, batch.uv_sets, &batch.vertices, &used)) {
      return false;
    }
    at += used;
    if (!ReadIndices(b, at, &batch.indices, &used)) {
      return false;
    }
    at += used;
    // Exactly the triangles the batch states, so a triangle's position in here
    // is its position in the model's index buffer. An unusable triangle is
    // COLLAPSED, never dropped: dropping one shifted every triangle after it
    // and a hiding template addresses them by number.
    batch.indices.resize(size_t(triangles) * 3, 0);
    const size_t vertex_count = batch.vertices.size();
    for (size_t t = 0; t < batch.indices.size(); t += 3) {
      if (batch.indices[t] >= vertex_count ||
          batch.indices[t + 1] >= vertex_count ||
          batch.indices[t + 2] >= vertex_count) {
        batch.indices[t] = 0;
        batch.indices[t + 1] = 0;
        batch.indices[t + 2] = 0;
      }
    }
    model.batches.push_back(std::move(batch));
  }
  for (uint32_t index = 0; index < texture_count; ++index) {
    if (at + 8 > d.size()) {
      return false;
    }
    at += 8;
    std::shared_ptr<const Texture> texture;
    if (!ReadTexture(b, d, &at, &texture)) {
      return false;
    }
    model.textures.push_back(texture);
  }
  *out = std::move(model);
  return true;
}

bool DecodeSkeleton(const uint8_t* data, size_t size, Skeleton* out) {
  Bits b{data, size};
  if (!b.Holds(324)) {
    return false;
  }
  const uint32_t count = b.Get(0, 32);
  const Lattice position = ReadLattice(b, 32);
  const Lattice rotation = ReadLattice(b, 178);
  if (!count || count > kMaxJoints || !LatticeValid(position) ||
      !LatticeValid(rotation)) {
    return false;
  }
  const uint64_t per_joint = 8 + LatticeBits(position) + LatticeBits(rotation);
  if (!b.Holds(324 + uint64_t(count) * per_joint)) {
    return false;
  }
  Skeleton skeleton;
  skeleton.count = count;
  uint64_t bit = 324;
  for (uint32_t j = 0; j < count; ++j) {
    skeleton.parents[j] = uint8_t(b.Get(bit, 8));
    bit += 8;
    float unused[3];
    ReadPoint(b, &bit, position, skeleton.bind[j]);
    ReadPoint(b, &bit, rotation, unused);
  }
  *out = skeleton;
  return true;
}

const Skeleton& MainSkeleton() {
  static const Skeleton skeleton = [] {
    Skeleton decoded;
    DecodeSkeleton(kMainSkeletonData, sizeof(kMainSkeletonData), &decoded);
    return decoded;
  }();
  return skeleton;
}

bool DecodeCarryableSkeleton(const std::vector<uint8_t>& data, Skeleton* out) {
  if (data.size() < 12) {
    return false;
  }
  const uint32_t length = Le32(data.data() + 8);
  if (!length || length > data.size()) {
    return false;
  }
  return DecodeSkeleton(data.data() + data.size() - length, length, out);
}

Skeleton::Skeleton() {
  for (uint32_t j = 0; j < kMaxJoints; ++j) {
    for (int k = 0; k < 3; ++k) {
      scale[j][k] = 1.0f;
    }
  }
}

namespace {

struct JointWeight {
  uint8_t joint;
  float scale[3];
};

constexpr float kFatStrength = 0.6f;

constexpr JointWeight kTallWeights[] = {
    {0, {1.1f, 1.1f, 1.1f}},
    {19, {0.9f, 0.9f, 0.9f}},
};

constexpr JointWeight kShortWeights[] = {
    {0, {0.9f, 0.9f, 0.9f}},
    {19, {1.05f, 1.05f, 1.05f}},
};

constexpr JointWeight kFatMaleWeights[] = {
    {4, {1.6f, 1.6f, 2.2f}},  {7, {1.5f, 1.0f, 1.5f}},
    {9, {1.5f, 1.0f, 1.5f}},  {10, {1.8f, 1.0f, 1.9f}},
    {13, {1.5f, 1.0f, 1.5f}}, {17, {1.5f, 1.0f, 1.5f}},
    {18, {1.5f, 1.0f, 1.4f}}, {24, {1.9f, 1.0f, 1.5f}},
    {26, {1.0f, 1.5f, 1.5f}}, {27, {1.0f, 1.5f, 1.5f}},
    {29, {1.0f, 1.5f, 1.5f}}, {30, {1.0f, 1.5f, 1.5f}},
    {32, {1.0f, 1.5f, 1.5f}}, {35, {1.0f, 1.5f, 1.5f}},
};

constexpr JointWeight kFatFemaleWeights[] = {
    {4, {1.5f, 1.5f, 2.0f}},  {7, {1.6f, 1.0f, 1.6f}},
    {9, {1.6f, 1.0f, 1.6f}},  {10, {1.6f, 1.0f, 2.0f}},
    {13, {1.6f, 1.0f, 1.6f}}, {17, {1.6f, 1.0f, 1.6f}},
    {18, {1.6f, 1.0f, 1.6f}}, {24, {2.0f, 1.0f, 1.6f}},
    {26, {1.0f, 1.6f, 1.6f}}, {27, {1.0f, 1.6f, 1.6f}},
    {29, {1.0f, 1.6f, 1.6f}}, {30, {1.0f, 1.6f, 1.6f}},
    {32, {1.0f, 1.6f, 1.6f}}, {35, {1.0f, 1.6f, 1.6f}},
};

constexpr JointWeight kThinMaleWeights[] = {
    {4, {0.84f, 1.0f, 0.92f}},  {7, {0.76f, 1.0f, 0.76f}},
    {9, {0.76f, 1.0f, 0.76f}},  {10, {0.68f, 1.0f, 0.68f}},
    {13, {0.76f, 1.0f, 0.76f}}, {17, {0.76f, 1.0f, 0.76f}},
    {18, {0.92f, 1.0f, 0.84f}}, {24, {0.76f, 1.0f, 0.76f}},
    {26, {1.0f, 0.76f, 0.76f}}, {27, {1.0f, 0.76f, 0.76f}},
    {29, {1.0f, 0.76f, 0.76f}}, {30, {1.0f, 0.76f, 0.76f}},
    {32, {1.0f, 0.76f, 0.76f}}, {35, {1.0f, 0.76f, 0.76f}},
};

constexpr JointWeight kThinFemaleWeights[] = {
    {4, {0.79f, 1.0f, 0.82f}},  {7, {0.82f, 1.0f, 0.82f}},
    {9, {0.82f, 1.0f, 0.82f}},  {10, {0.82f, 1.0f, 0.7f}},
    {13, {0.82f, 1.0f, 0.82f}}, {17, {0.82f, 1.0f, 0.82f}},
    {18, {0.88f, 1.0f, 0.82f}}, {24, {0.82f, 1.0f, 0.82f}},
    {26, {1.0f, 0.82f, 0.82f}}, {27, {1.0f, 0.82f, 0.82f}},
    {29, {1.0f, 0.82f, 0.82f}}, {30, {1.0f, 0.82f, 0.82f}},
    {32, {1.0f, 0.82f, 0.82f}}, {35, {1.0f, 0.82f, 0.82f}},
};

template <size_t N>
void ApplyScaling(Skeleton* skeleton, const JointWeight (&weights)[N],
                  float strength, float amount) {
  amount = std::clamp(amount, 0.0f, 1.0f);
  for (const JointWeight& weight : weights) {
    if (weight.joint >= skeleton->count) {
      continue;
    }
    for (int k = 0; k < 3; ++k) {
      const float target = 1.0f + strength * (weight.scale[k] - 1.0f);
      skeleton->scale[weight.joint][k] *= 1.0f + amount * (target - 1.0f);
    }
  }
}

}  // namespace

Skeleton ScaledSkeleton(const Description& description) {
  Skeleton skeleton = MainSkeleton();
  const float height = float(description.height) / 127.5f - 1.0f;
  const float weight = float(description.weight) / 127.5f - 1.0f;
  if (height >= 0.0f) {
    ApplyScaling(&skeleton, kTallWeights, 1.0f, height);
  } else {
    ApplyScaling(&skeleton, kShortWeights, 1.0f, -height);
  }
  if (description.body) {
    if (weight >= 0.0f) {
      ApplyScaling(&skeleton, kFatMaleWeights, kFatStrength, weight);
    } else {
      ApplyScaling(&skeleton, kThinMaleWeights, 1.0f, -weight);
    }
  } else {
    if (weight > 0.0f) {
      ApplyScaling(&skeleton, kFatFemaleWeights, kFatStrength, weight);
    } else {
      ApplyScaling(&skeleton, kThinFemaleWeights, 1.0f, -weight);
    }
  }
  return skeleton;
}

namespace {

bool DecodeJointSection(const uint8_t* data, size_t size, float rate,
                        Clip* out) {
  Bits b{data, size};
  if (!b.Holds(kClipHeaderBits)) {
    return false;
  }
  const uint32_t frames = b.Get(0, 32);
  const uint32_t joints = b.Get(32, 32);
  if (!frames || frames > 100000 || !joints || joints > kMaxJoints) {
    return false;
  }
  std::vector<Lattice> contexts(size_t(joints) * 3);
  uint64_t bit = 64;
  uint64_t per_frame = 0;
  for (uint32_t s = 0; s < joints; ++s) {
    const uint32_t j = joints - 1 - s;
    for (int k = 0; k < 3; ++k) {
      Lattice& lattice = contexts[size_t(j) * 3 + k];
      lattice = ReadLattice(b, bit);
      if (!LatticeValid(lattice)) {
        return false;
      }
      bit += 146;
      per_frame += LatticeBits(lattice);
    }
  }
  if (!b.Holds(kClipHeaderBits + uint64_t(frames) * per_frame)) {
    return false;
  }
  Clip clip;
  clip.frames = frames;
  clip.joints = joints;
  clip.rate = rate > 0.0f && rate < 1000.0f ? rate : 30.0f;
  clip.keys.resize(size_t(frames) * joints * kKeyFloats);
  for (uint32_t f = 0; f < frames; ++f) {
    uint64_t fb = kClipHeaderBits + uint64_t(f) * per_frame;
    for (uint32_t j = 0; j < joints; ++j) {
      float position[3];
      float rotation[3];
      float scale[3];
      ReadPoint(b, &fb, contexts[size_t(j) * 3 + 0], position);
      ReadPoint(b, &fb, contexts[size_t(j) * 3 + 1], rotation);
      ReadPoint(b, &fb, contexts[size_t(j) * 3 + 2], scale);
      float* key = clip.keys.data() + (size_t(f) * joints + j) * kKeyFloats;
      RotationToQuaternion(rotation, key);
      key[4] = position[0];
      key[5] = position[1];
      key[6] = position[2];
      key[7] = scale[0];
      key[8] = scale[1];
      key[9] = scale[2];
    }
  }
  *out = std::move(clip);
  return true;
}

}  // namespace

bool DecodeClip(const std::vector<uint8_t>& d, Clip* out) {
  if (d.size() < 0x28) {
    return false;
  }
  Bits header{d.data(), d.size()};
  const float rate = header.Float(32);
  const uint32_t section = header.Get(6 * 32, 32);
  if (0x28 + size_t(section) > d.size()) {
    return false;
  }
  return DecodeJointSection(d.data() + 0x28, section, rate, out);
}

bool DecodeCarryableClip(const std::vector<uint8_t>& d, Clip* out) {
  if (d.size() < 0x28) {
    return false;
  }
  Bits header{d.data(), d.size()};
  const float rate = header.Float(32);
  const uint32_t begin = header.Get(6 * 32, 32);
  const uint32_t end = header.Get(8 * 32, 32);
  if (end <= begin || 0x28 + size_t(end) > d.size()) {
    return false;
  }
  return DecodeJointSection(d.data() + 0x28 + begin, end - begin, rate, out);
}

namespace {

constexpr uint32_t kJointContextBytes = 0x2BEC;
constexpr uint32_t kMotionContextOffset = 0x57D8;
constexpr uint32_t kIntegerContextOffset = 0x591C;
constexpr uint32_t kAnimationFieldsOffset = 0x5964;
constexpr uint32_t kContextElementsOffset = 0xC;
constexpr uint32_t kLatticeContextBytes = 0x34;
constexpr uint32_t kLatticeHeaderBits = 146;
constexpr uint32_t kJointElementBytes = 0x9C;
constexpr uint32_t kMotionElementBytes = 0x68;
constexpr uint32_t kIntegerElementBytes = 0xC;
constexpr uint32_t kMotionCount = 3;
constexpr uint32_t kIntegerCount = 5;
constexpr size_t kJointHeaderBytes = 0xF6E;
constexpr size_t kMotionHeaderBytes = 0x76;
constexpr size_t kIntegerHeaderBytes = 0x30;
constexpr uint32_t kFieldWords[9] = {0, 1, 2, 3, 4, 5, 6, 8, 7};
constexpr uint32_t kMirroredFloats[6] = {0x0C, 0x18, 0x38, 0x3C, 0x44, 0x48};

struct Section {
  const uint8_t* data;
  size_t size;
};

void StoreBe32(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

uint32_t WriteLatticeContext(const Bits& b, uint64_t bit, uint8_t* context) {
  const float scale = b.Float(bit);
  const float factors[3] = {kLatticeX, kLatticeY, kLatticeZ};
  uint32_t total = 0;
  StoreBe32(context, b.Get(bit, 32));
  for (uint32_t k = 0; k < 3; ++k) {
    const uint32_t width = b.Get(bit + 128 + 6 * k, 6);
    StoreBe32(context + 0x04 + 4 * k, FloatBits(scale * factors[k]));
    StoreBe32(context + 0x10 + 4 * k, b.Get(bit + 32 + 32 * k, 32));
    StoreBe32(context + 0x28 + 4 * k, width);
    total += width;
  }
  return total;
}

bool WriteLatticeListContext(const Section& section, size_t header_bytes,
                             uint32_t capacity, uint32_t lattices,
                             uint32_t stride, uint8_t* context) {
  if (section.size < header_bytes) {
    return false;
  }
  const Bits b{section.data, header_bytes};
  const uint32_t count = b.Get(32, 32);
  if (count > capacity) {
    return false;
  }
  StoreBe32(context, b.Get(0, 32));
  StoreBe32(context + 4, count);
  uint64_t bit = 64;
  uint32_t total = 0;
  for (uint32_t s = 0; s < count; ++s) {
    uint8_t* element =
        context + kContextElementsOffset + size_t(count - 1 - s) * stride;
    for (uint32_t l = 0; l < lattices; ++l) {
      total += WriteLatticeContext(b, bit, element + l * kLatticeContextBytes);
      bit += kLatticeHeaderBits;
    }
  }
  StoreBe32(context + 8, total);
  return true;
}

bool WriteIntegerContext(const Section& section, uint8_t* context) {
  if (section.size < kIntegerHeaderBytes) {
    return false;
  }
  const Bits b{section.data, kIntegerHeaderBytes};
  const uint32_t count = b.Get(32, 32);
  if (count > kIntegerCount) {
    return false;
  }
  StoreBe32(context, b.Get(0, 32));
  StoreBe32(context + 4, count);
  uint64_t bit = 64;
  uint32_t total = 0;
  for (uint32_t s = 0; s < count; ++s) {
    uint8_t* element = context + kContextElementsOffset +
                       size_t(count - 1 - s) * kIntegerElementBytes;
    const uint32_t width = b.Get(bit + 32, 32);
    StoreBe32(element, width);
    StoreBe32(element + 4, b.Get(bit, 32));
    total += width;
    bit += 64;
  }
  StoreBe32(context + 8, total);
  return true;
}

void MirrorElement(uint8_t* element) {
  for (uint32_t offset : kMirroredFloats) {
    element[offset] ^= 0x80;
  }
}

}  // namespace

bool WriteAnimationObject(const std::vector<uint8_t>& stream, bool mirror,
                          uint8_t* object) {
  if (stream.size() < kAnimationHeaderBytes) {
    return false;
  }
  const Bits header{stream.data(), kAnimationHeaderBytes};
  uint32_t word[10];
  for (uint32_t i = 0; i < 10; ++i) {
    word[i] = header.Get(uint64_t(i) * 32, 32);
  }
  const uint32_t size = word[9];
  if (size > Be32(object + kAnimationSizeOffset) ||
      kAnimationHeaderBytes + size_t(size) > stream.size()) {
    return false;
  }
  const uint32_t bounds[5] = {0, word[6], word[8], word[7], size};
  for (uint32_t i = 0; i < 4; ++i) {
    if (bounds[i] > bounds[i + 1]) {
      return false;
    }
  }
  const uint8_t* payload = stream.data() + kAnimationHeaderBytes;
  auto section = [&](uint32_t i) {
    return Section{payload + bounds[i], size_t(bounds[i + 1] - bounds[i])};
  };
  std::vector<uint8_t> image(object, object + kAnimationObjectBytes);
  uint8_t* out = image.data();
  if (!WriteLatticeListContext(section(0), kJointHeaderBytes, kMaxJoints, 3,
                               kJointElementBytes, out) ||
      !WriteLatticeListContext(section(1), kJointHeaderBytes, kMaxJoints, 3,
                               kJointElementBytes, out + kJointContextBytes) ||
      !WriteLatticeListContext(section(2), kMotionHeaderBytes, kMotionCount, 2,
                               kMotionElementBytes,
                               out + kMotionContextOffset) ||
      !WriteIntegerContext(section(3), out + kIntegerContextOffset)) {
    return false;
  }
  for (uint32_t i = 0; i < 9; ++i) {
    StoreBe32(out + kAnimationFieldsOffset + 4 * i, word[kFieldWords[i]]);
  }
  StoreBe32(out + kAnimationSizeOffset, size);
  if (mirror) {
    for (uint32_t j = 0; j < kMaxJoints; ++j) {
      MirrorElement(out + kContextElementsOffset + j * kJointElementBytes);
      MirrorElement(out + kJointContextBytes + kContextElementsOffset +
                    j * kJointElementBytes);
    }
    for (uint32_t m = 0; m < kMotionCount; ++m) {
      MirrorElement(out + kMotionContextOffset + kContextElementsOffset +
                    m * kMotionElementBytes);
    }
  }
  std::memcpy(object, out, kAnimationObjectBytes);
  return true;
}

int32_t FindAnimation(const Catalog& catalog, const uint8_t* asset_id) {
  std::array<uint8_t, 16> id;
  std::memcpy(id.data(), asset_id, id.size());
  // Only installed content stores its own id there; a pack entry stores the
  // asset that replaces it, so matching those would answer with whatever
  // happens to point at this one.
  if (id != std::array<uint8_t, 16>{}) {
    for (const Entry& entry : catalog.entries()) {
      if (entry.external && entry.asset_id == id) {
        return int32_t(entry.index);
      }
    }
  }
  if (asset_id[0] != 0x00 || asset_id[1] != 0x40 || asset_id[2] != 0x00 ||
      asset_id[3] != 0x00) {
    return -1;
  }
  const uint32_t index = (uint32_t(asset_id[4]) << 8) | asset_id[5];
  return catalog.Find(index) ? int32_t(index) : -1;
}

bool Catalog::Load(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return false;
  }
  const std::streamoff length = file.tellg();
  if (length < 0x30 || length > (256ll << 20)) {
    return false;
  }
  std::vector<uint8_t> data(static_cast<size_t>(length));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(data.data()), length)) {
    return false;
  }
  const uint32_t version = Be32(data.data() + 0x18);
  const uint32_t count = Be32(data.data() + 0x1C);
  const uint32_t locales = version >= 2 ? Be32(data.data() + 0x28) : 13;
  const size_t first = version >= 2 ? 0x30 : 0x28;
  const size_t stride = 8 + 0xB4 + 4 * size_t(locales) + 8;
  if (!locales || locales > 64 || !count || count > 100000 ||
      first + size_t(count) * stride > data.size()) {
    return false;
  }
  std::vector<Entry> entries;
  entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t base = first + size_t(i) * stride;
    const size_t pointers = base + 8 + 0xB4;
    Entry entry;
    entry.index = i;
    entry.kind = Be32(data.data() + base);
    entry.flags = Be32(data.data() + base + 4);
    const uint32_t name_at = Be32(data.data() + pointers);
    const uint32_t blob = Be32(data.data() + pointers + 4 * size_t(locales));
    const uint32_t blob_size =
        Be32(data.data() + pointers + 4 * size_t(locales) + 4);
    if (name_at && name_at < data.size()) {
      entry.name = Utf16BeName(data, name_at);
    }
    // +0x94 into the record's block, not +0x90. Measured over the whole pack:
    // at +0x94 all 405 assets that carry an id end in the eight-byte stock
    // suffix, and their first dword is the kind; at +0x90 not one of them does,
    // because the four bytes before the id are a separate offer number. Reading
    // it four bytes early shifted every id the editor hands out, so nothing the
    // player picked could ever be looked up again.
    std::memcpy(entry.asset_id.data(), data.data() + base + 8 + 0x94,
                entry.asset_id.size());
    // The colour table, at the very front of the same record block: nine slots
    // of three RGB triples, ending well before the asset id at +0x94. Measured
    // over the whole pack, exactly two shapes occur - 36 entries fill one slot
    // and three fill nine, all of them kind 0x08 - and every other entry leaves
    // all 81 bytes zero. A slot's three triples are the colour CHANNELS of one
    // colourway: the Sport Tops state red, green and blue, and the Power Tee
    // repeats one hue across all three.
    std::memcpy(entry.colours.data(), data.data() + base + 8,
                entry.colours.size());
    // And the same region as XAM takes it: one byte earlier and 0x91 long, so
    // the colour layout byte at +0x07 leads it. `first + count * stride` was
    // already bounds checked and 0x07 + 0x91 is well inside the 0x10C stride.
    std::memcpy(entry.record_block.data(), data.data() + base + 7,
                entry.record_block.size());
    for (uint32_t slot = 0; slot < Entry::kColourSlots; ++slot) {
      const uint8_t* cell = entry.colours.data() + slot * 9;
      uint32_t filled = 0;
      for (uint32_t colour = 0; colour < Entry::kColoursPerSlot; ++colour) {
        const uint8_t* rgb = cell + colour * 3;
        if (rgb[0] || rgb[1] || rgb[2]) {
          filled = colour + 1;
        }
      }
      if (filled) {
        entry.colour_count = uint8_t(slot + 1);
        entry.colours_per_slot =
            std::max(entry.colours_per_slot, uint8_t(filled));
      }
    }
    if (!entry.colour_count) {
      entry.colours = {};
    }
    if (blob && blob_size > 0x3C && size_t(blob) + blob_size <= data.size() &&
        std::memcmp(data.data() + blob, "STRB", 4) == 0) {
      entry.blob = blob;
      entry.size = blob_size;
    }
    entries.push_back(std::move(entry));
  }
  // Resolve the substitutions the pack states in that reference. Measured over
  // the shipping pack: every one that points at an entry of its own kind is a
  // hairstyle pointing at its "(Hat)" version - all 88 of them, and nothing
  // else - so this is exactly the set that must not be offered as a choice.
  // The names do not pair up (The Captain Cut wears Old Hair (Hat), and both
  // Comb Overs share one), so matching on " (Hat)" got nine of them wrong.
  for (Entry& entry : entries) {
    if (entry.asset_id == std::array<uint8_t, 16>{}) {
      continue;
    }
    const uint32_t target =
        (uint32_t(entry.asset_id[4]) << 8) | uint32_t(entry.asset_id[5]);
    if (target >= entries.size() || target == entry.index ||
        entries[target].kind != entry.kind) {
      continue;
    }
    entry.substitute = target;
    entries[target].substitute_for = entry.index;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  data_ = std::move(data);
  entries_ = std::move(entries);
  models_.clear();
  features_.clear();
  clips_.clear();
  raw_models_.clear();
  raw_textures_.clear();
  return true;
}

const Entry* Catalog::Find(uint32_t index) const {
  return index < entries_.size() ? &entries_[index] : nullptr;
}

// Installed content only. The pack's own entries carry an id as well, but it
// is shared between an asset's variant rows - measured, eleven entries answer
// to 0100000003550001C1C8F109A19CB2E0 - so it cannot name one of them.
const Entry* Catalog::FindAsset(const uint8_t* asset_id) const {
  for (const Entry& entry : entries_) {
    if (entry.external && std::memcmp(entry.asset_id.data(), asset_id,
                                      entry.asset_id.size()) == 0) {
      return &entry;
    }
  }
  return nullptr;
}

bool Catalog::AddAsset(const std::array<uint8_t, 16>& asset_id,
                       std::string name, std::vector<uint8_t> blob) {
  if (blob.size() <= 0x3C || std::memcmp(blob.data(), "STRB", 4) != 0) {
    return false;
  }
  const uint32_t kind = Be32(asset_id.data());
  if (PrimarySlot(kind) < 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (FindAsset(asset_id.data())) {
    return true;
  }
  uint32_t body = 3;
  size_t at = 0x3C;
  while (at + 12 <= blob.size()) {
    const uint8_t tag = blob[at];
    const uint32_t length = Be32(blob.data() + at + 1);
    const size_t end = at + 12 + length;
    if (!tag || end > blob.size()) {
      break;
    }
    if (tag == 8 && length >= 15 && (blob[at + 13] & 3)) {
      body = blob[at + 13] & 3;
    }
    at = end;
    while (at < blob.size() && (at & 3) && blob[at] == 0) {
      ++at;
    }
  }
  Entry entry;
  entry.index = uint32_t(entries_.size());
  entry.kind = kind;
  entry.flags = body << 24;
  entry.blob = 1;
  entry.size = uint32_t(blob.size());
  entry.name = std::move(name);
  entry.asset_id = asset_id;
  externals_.push_back(std::move(blob));
  entry.external = uint32_t(externals_.size());
  entries_.push_back(std::move(entry));
  return true;
}

bool Catalog::Records(uint32_t index, std::vector<Record>* out) const {
  const Entry* entry = Find(index);
  if (!entry || !entry->blob) {
    return false;
  }
  const uint8_t* b = entry->external ? externals_[entry->external - 1].data()
                                     : data_.data() + entry->blob;
  const size_t size = entry->size;
  size_t at = 0x3C;
  while (at + 12 <= size) {
    const uint8_t tag = b[at];
    const uint32_t length = Be32(b + at + 1);
    const size_t payload = at + 12;
    const size_t end = payload + length;
    if (!tag || end > size) {
      break;
    }
    Record record;
    record.type = tag;
    if (tag == 2 || tag == 3) {
      size_t chunk = payload;
      while (chunk + 12 <= end) {
        const uint32_t compressed = Le32(b + chunk);
        const uint32_t start = Le32(b + chunk + 4);
        const uint32_t expanded = Le32(b + chunk + 8);
        chunk += 12;
        if (!compressed || chunk + compressed > end || expanded > (16u << 20) ||
            size_t(start) + expanded > (64u << 20)) {
          return false;
        }
        std::vector<uint8_t> output;
        if (!Inflate(b + chunk, compressed, expanded, &output)) {
          return false;
        }
        if (record.data.size() < size_t(start) + output.size()) {
          record.data.resize(size_t(start) + output.size());
        }
        std::memcpy(record.data.data() + start, output.data(), output.size());
        chunk += compressed;
      }
    } else {
      record.data.assign(b + payload, b + end);
    }
    out->push_back(std::move(record));
    at = end;
    while (at < size && (at & 3) && b[at] == 0) {
      ++at;
    }
  }
  return !out->empty();
}

std::shared_ptr<const Carryable> Catalog::LoadCarryable(uint32_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = carryables_.find(index);
  if (found != carryables_.end()) {
    return found->second;
  }
  std::shared_ptr<const Carryable> result;
  std::vector<Record> records;
  if (Records(index, &records)) {
    auto carryable = std::make_shared<Carryable>();
    bool has_skeleton = false;
    for (const Record& record : records) {
      if (record.type == 5) {
        has_skeleton =
            DecodeCarryableSkeleton(record.data, &carryable->skeleton);
      } else if (record.type == 1) {
        auto body = std::make_shared<Clip>();
        if (DecodeClip(record.data, body.get())) {
          carryable->body = body;
        }
        auto joints = std::make_shared<Clip>();
        if (DecodeCarryableClip(record.data, joints.get())) {
          carryable->joints = joints;
        }
      }
    }
    if (has_skeleton) {
      result = carryable;
    }
  }
  carryables_[index] = result;
  return result;
}

std::shared_ptr<const Model> Catalog::LoadModel(uint32_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = models_.find(index);
  if (found != models_.end()) {
    return found->second;
  }
  std::shared_ptr<const Model> result;
  std::vector<Record> records;
  if (Records(index, &records)) {
    for (const Record& record : records) {
      if (record.type == 3) {
        auto model = std::make_shared<Model>();
        if (DecodeModel(record.data, model.get())) {
          result = model;
        }
        break;
      }
    }
  }
  models_[index] = result;
  return result;
}

std::shared_ptr<const Shape> Catalog::LoadShape(uint32_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = shapes_.find(index);
  if (found != shapes_.end()) {
    return found->second;
  }
  std::shared_ptr<const Shape> result;
  std::vector<Record> records;
  if (Records(index, &records)) {
    for (const Record& record : records) {
      if (record.type == 4) {
        auto shape = std::make_shared<Shape>();
        if (DecodeShape(record.data, shape.get())) {
          result = shape;
        }
        break;
      }
    }
  }
  shapes_[index] = result;
  return result;
}

std::shared_ptr<const Texture> Catalog::LoadFeature(uint32_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = features_.find(index);
  if (found != features_.end()) {
    return found->second;
  }
  std::shared_ptr<const Texture> result;
  std::vector<Record> records;
  if (Records(index, &records)) {
    for (const Record& record : records) {
      if (record.type == 2) {
        auto texture = std::make_shared<Texture>();
        if (DecodeTexture(record.data, texture.get())) {
          result = texture;
        }
        break;
      }
    }
  }
  features_[index] = result;
  return result;
}

std::shared_ptr<const Clip> Catalog::LoadClip(uint32_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = clips_.find(index);
  if (found != clips_.end()) {
    return found->second;
  }
  std::shared_ptr<const Clip> result;
  std::vector<Record> records;
  if (Records(index, &records)) {
    for (const Record& record : records) {
      if (record.type == 1) {
        auto clip = std::make_shared<Clip>();
        if (DecodeClip(record.data, clip.get())) {
          result = clip;
        }
        break;
      }
    }
  }
  clips_[index] = result;
  return result;
}

bool Catalog::AnimationStream(uint32_t index, std::vector<uint8_t>* out) const {
  std::vector<Record> records;
  if (!Records(index, &records)) {
    return false;
  }
  for (Record& record : records) {
    if (record.type == 1) {
      *out = std::move(record.data);
      return true;
    }
  }
  return false;
}

std::shared_ptr<const RawModel> Catalog::LoadRawModel(uint32_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = raw_models_.find(index);
  if (found != raw_models_.end()) {
    return found->second;
  }
  std::shared_ptr<const RawModel> result;
  std::vector<Record> records;
  if (Records(index, &records)) {
    for (const Record& record : records) {
      if (record.type == 3) {
        auto model = std::make_shared<RawModel>();
        if (DecodeRawModel(record.data, model.get())) {
          result = model;
        }
        break;
      }
    }
  }
  raw_models_[index] = result;
  return result;
}

std::shared_ptr<const RawTexture> Catalog::LoadRawTexture(uint32_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = raw_textures_.find(index);
  if (found != raw_textures_.end()) {
    return found->second;
  }
  std::shared_ptr<const RawTexture> result;
  std::vector<Record> records;
  if (Records(index, &records)) {
    for (const Record& record : records) {
      if (record.type == 2) {
        auto texture = std::make_shared<RawTexture>();
        if (DecodeRawTexture(record.data, texture.get())) {
          result = texture;
        }
        break;
      }
    }
  }
  raw_textures_[index] = result;
  return result;
}

bool DecodeRawTexture(const std::vector<uint8_t>& data, RawTexture* out) {
  Bits b{data.data(), data.size()};
  size_t at = 0;
  return ReadRawTexture(b, data, &at, out);
}

bool DecodeRawModel(const std::vector<uint8_t>& d, RawModel* out) {
  if (d.size() < 0x30) {
    return false;
  }
  Bits b{d.data(), d.size()};
  RawModel model;
  model.cpu_size = b.Get(0, 32);
  model.gpu_size = b.Get(32, 32);
  model.texture_size = b.Get(64, 32);
  model.vb_size = b.Get(96, 32);
  model.ib_size = b.Get(128, 32);
  const uint32_t batch_count = b.Get(160, 32);
  const uint32_t texture_count = b.Get(192, 32);
  model.vb_offset = b.Get(224, 32);
  model.ib_offset = b.Get(256, 32);
  model.batches_offset = b.Get(288, 32);
  model.textures_offset = b.Get(320, 32);
  if (!batch_count || batch_count > 64 || texture_count > 64 ||
      model.cpu_size > (1u << 20) || model.gpu_size > (16u << 20)) {
    return false;
  }
  size_t at = 0x30;
  for (uint32_t index = 0; index < batch_count; ++index) {
    if (at + 0x21 > d.size()) {
      return false;
    }
    const uint64_t bb = uint64_t(at) * 8;
    RawBatch batch;
    batch.shader = b.Get(bb, 32);
    const uint32_t params = b.Get(bb + 32, 5);
    batch.triangles = b.Get(bb + 37, 32);
    batch.uv_sets = b.Get(bb + 101, 32);
    batch.stride = b.Get(bb + 133, 32);
    batch.index_stride = b.Get(bb + 165, 32);
    batch.vb_offset = b.Get(bb + 197, 32);
    batch.ib_offset = b.Get(bb + 229, 32);
    at += 0x21;
    if (!batch.uv_sets || batch.uv_sets > kLayerCount ||
        at + size_t(params) * 0x18 > d.size()) {
      return false;
    }
    for (uint32_t p = 0; p < params; ++p) {
      const uint64_t pb = uint64_t(at) * 8;
      Param param;
      param.type = b.Get(pb, 32);
      param.usage = b.Get(pb + 32, 32);
      for (int k = 0; k < 4; ++k) {
        param.data[k] = b.Get(pb + 64 + 32 * k, 32);
      }
      batch.params.push_back(param);
      at += 0x18;
    }
    size_t used = 0;
    if (!ReadRawVertices(b, at, batch.uv_sets, &batch.vertices, &used)) {
      return false;
    }
    at += used;
    if (!ReadIndices(b, at, &batch.indices, &used)) {
      return false;
    }
    at += used;
    model.batches.push_back(std::move(batch));
  }
  for (uint32_t index = 0; index < texture_count; ++index) {
    if (at + 8 > d.size()) {
      return false;
    }
    const uint64_t tb = uint64_t(at) * 8;
    const uint32_t gpu_offset = b.Get(tb, 32);
    const uint32_t gpu_size = b.Get(tb + 32, 32);
    at += 8;
    RawTexture texture;
    if (!ReadRawTexture(b, d, &at, &texture)) {
      return false;
    }
    texture.gpu_offset = gpu_offset;
    texture.gpu_size = gpu_size;
    model.textures.push_back(std::move(texture));
  }
  *out = std::move(model);
  return true;
}

std::vector<Component> Components(const Catalog& catalog,
                                  const Description& description) {
  std::vector<Component> components;
  const uint32_t body = FindBody(catalog, description.body);
  if (body != UINT32_MAX) {
    components.push_back({body, kKindBody});
  }
  const uint32_t head = FindHead(catalog);
  if (head != UINT32_MAX) {
    components.push_back({head, kKindHead});
  }
  const bool hat = description.items[kSlotHat] != kNoItem;
  for (uint32_t slot = 0; slot < kClothingSlotCount; ++slot) {
    const uint16_t item = description.items[slot];
    const Entry* entry = item == kNoItem ? nullptr : catalog.Find(item);
    if (!entry || PrimarySlot(entry->kind) != int32_t(slot)) {
      continue;
    }
    const uint32_t chosen =
        slot == kSlotHair && hat ? HatVariantOf(catalog, item) : item;
    components.push_back({chosen, SlotCoverage(entry->kind) & 0x1FFCu});
  }
  return components;
}

int32_t FeatureEntry(const Description& description, uint32_t feature) {
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    if (FeatureForSlot(slot) == int32_t(feature)) {
      const uint16_t item = description.items[slot];
      return item == kNoItem ? -1 : int32_t(item);
    }
  }
  return -1;
}

int32_t HeadFeatureForUsage(uint32_t usage) {
  for (const HeadLayer& layer : kHeadLayers) {
    if (layer.usage == usage) {
      return int32_t(layer.feature);
    }
  }
  return -1;
}

std::filesystem::path CatalogPath(const std::filesystem::path& content_root) {
  return content_root / "0000000000000000" / "FFFE07DF" / "00008000" /
         "FFFE07DF00000002" / "AvatarAssetPack.toc";
}

Catalog* SharedCatalog(const std::filesystem::path& content_root) {
  static std::mutex mutex;
  static std::unique_ptr<Catalog> catalog;
  static std::chrono::steady_clock::time_point last_attempt;
  static bool attempted = false;
  std::lock_guard<std::mutex> lock(mutex);
  if (catalog) {
    return catalog.get();
  }
  const auto now = std::chrono::steady_clock::now();
  if (attempted && now - last_attempt < std::chrono::seconds(5)) {
    return nullptr;
  }
  attempted = true;
  last_attempt = now;
  auto loaded = std::make_unique<Catalog>();
  if (!loaded->Load(CatalogPath(content_root))) {
    return nullptr;
  }
  catalog = std::move(loaded);
  return catalog.get();
}

bool IsHatVariant(const Entry& entry) {
  static const char kSuffix[] = "(Hat)";
  const size_t length = sizeof(kSuffix) - 1;
  return entry.name.size() >= length &&
         entry.name.compare(entry.name.size() - length, length, kSuffix) == 0;
}

std::vector<uint32_t> ItemsForSlot(const Catalog& catalog, uint32_t slot,
                                   uint32_t body) {
  std::vector<uint32_t> items;
  for (const Entry& entry : catalog.entries()) {
    // A hat version of a hairstyle is not a hairstyle you can pick; it goes on
    // by itself when a hat does.
    if (PrimarySlot(entry.kind) == int32_t(slot) && !entry.is_substitute() &&
        Wearable(entry, body)) {
      items.push_back(entry.index);
    }
  }
  return items;
}

void PlaceItem(const Catalog& catalog, Description* description, uint32_t slot,
               uint16_t entry) {
  if (slot >= kSlotCount) {
    return;
  }
  if (entry == kNoItem) {
    description->items[slot] = kNoItem;
    return;
  }
  const Entry* placed = catalog.Find(entry);
  if (!placed) {
    return;
  }
  const uint32_t coverage = SlotCoverage(placed->kind) | SlotBit(slot);
  for (uint32_t other = 0; other < kSlotCount; ++other) {
    if (other == slot || description->items[other] == kNoItem) {
      continue;
    }
    const Entry* existing = catalog.Find(description->items[other]);
    const uint32_t existing_coverage =
        (existing ? SlotCoverage(existing->kind) : 0) | SlotBit(other);
    if (existing_coverage & coverage) {
      description->items[other] = kNoItem;
    }
  }
  description->items[slot] = entry;
}

Description RandomDescription(const Catalog& catalog, std::mt19937& rng,
                              int32_t body) {
  Description description;
  description.body =
      uint8_t(body == 0 || body == 1 ? body : int32_t(rng() & 1));
  description.height = uint8_t(rng() & 0xFF);
  description.weight = uint8_t(rng() & 0xFF);
  const bool male = description.body != 0;
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  const auto covered = [&](uint32_t slot) {
    for (uint32_t other = 0; other < kSlotCount; ++other) {
      if (description.items[other] == kNoItem) {
        continue;
      }
      const Entry* entry = catalog.Find(description.items[other]);
      if (entry && (SlotCoverage(entry->kind) & SlotBit(slot))) {
        return true;
      }
    }
    return false;
  };
  const auto pick = [&](uint32_t slot, float chance) {
    if (covered(slot) || unit(rng) >= chance) {
      return;
    }
    const std::vector<uint32_t> items =
        ItemsForSlot(catalog, slot, description.body);
    if (!items.empty()) {
      PlaceItem(catalog, &description, slot,
                uint16_t(items[rng() % items.size()]));
    }
  };
  pick(kSlotShirt, 1.0f);
  pick(kSlotTrousers, 1.0f);
  pick(kSlotShoes, 1.0f);
  pick(kSlotHair, 0.97f);
  pick(kSlotHat, 0.2f);
  pick(kSlotGlasses, 0.2f);
  pick(kSlotWristwear, 0.15f);
  pick(kSlotEarrings, male ? 0.1f : 0.4f);
  pick(kSlotRing, 0.1f);
  pick(kSlotGloves, 0.05f);
  pick(kSlotEyes, 1.0f);
  pick(kSlotEyebrows, 1.0f);
  pick(kSlotMouth, 1.0f);
  pick(kSlotFacialHair, male ? 0.35f : 0.0f);
  pick(kSlotEyeShadow, male ? 0.0f : 0.4f);
  pick(kSlotFacePaint, 0.03f);
  description.colors[kColorSkin] = Pick(kSkinPalette, rng);
  description.colors[kColorHair] = Pick(kHairPalette, rng);
  description.colors[kColorLips] = Pick(kLipPalette, rng);
  description.colors[kColorIris] = Pick(kIrisPalette, rng);
  description.colors[kColorEyebrow] = description.colors[kColorHair];
  description.colors[kColorFacialHair] = description.colors[kColorHair];
  description.colors[kColorEyeShadow] = Pick(kShadowPalette, rng);
  for (auto& slot : description.custom) {
    for (auto& color : slot) {
      color = Pick(kClothingPalette, rng);
    }
  }
  return description;
}

namespace {

constexpr size_t kManifestWeightOffset = 0x4;
constexpr size_t kManifestHeightOffset = 0x8;
constexpr size_t kManifestBlendOffset = 0x0C;
constexpr uint32_t kManifestBlendCount = 3;
constexpr size_t kManifestFaceOffset = 0x3C;
constexpr uint32_t kManifestFaceCount = 6;
constexpr size_t kManifestColorsOffset = 0xFC;
constexpr size_t kManifestBodyOffset = 0x120;
constexpr size_t kManifestHeadOffset = 0x140;
constexpr size_t kManifestComponentsOffset = 0x160;
constexpr uint32_t kManifestComponentCount = 13;
constexpr size_t kManifestDefaultsOffset = 0x300;
constexpr uint32_t kManifestDefaultCount = 4;
constexpr size_t kManifestEntryBytes = 0x20;
constexpr size_t kManifestEntryMaskOffset = 0x10;
constexpr size_t kManifestXuidOffset = 0x380;
constexpr uint8_t kStockAssetSuffix[8] = {0xC1, 0xC8, 0xF1, 0x09,
                                          0xA1, 0x9C, 0xB2, 0xE0};
// Avatars::Manifest::Version0::Shape::Type_e, read out of xam.xex: the writer
// at 0x8196C500 stores at manifest + 0xC + (type << 4), and its caller passes
// type 1 with kind 0x80000 and type 2 with 0x200000, leaving 0x100000 for type
// 0.
//
// XAvatarMetadataGetBlendShapeIDs lists its out-parameters chin, ear, nose,
// which is NOT this order - the asset pack settles it: every one of the 18
// assets of kind 0x80000 is named "... Nose" and all 9 of kind 0x200000 are
// named "... Ears".
enum ShapeType : uint32_t { kShapeChin, kShapeNose, kShapeEars };
enum TextureType : uint32_t {
  kTextureMouth,
  kTextureEyes,
  kTextureEyebrows,
  kTextureFacialHair,
  kTextureEyeShadow,
  kTextureFacePaint,
};
constexpr uint32_t kShapeKinds[kManifestBlendCount] = {0x100000, 0x80000,
                                                       0x200000};
constexpr uint16_t kMaleShapeDefaults[kManifestBlendCount] = {0x031D, 0x032B,
                                                              0x0337};
constexpr uint16_t kFemaleShapeDefaults[kManifestBlendCount] = {0x0321, 0x0327,
                                                                0x033A};
constexpr uint32_t kTextureKinds[kManifestFaceCount] = {
    0x8000, 0x2000, 0x4000, 0x10000, 0x40000, 0x20000};
constexpr uint32_t kRequiredTextureCount = kTextureEyebrows + 1;
constexpr uint16_t kMaleTextureDefaults[kRequiredTextureCount] = {
    0x02EB, 0x02AC, 0x0267};
constexpr uint16_t kFemaleTextureDefaults[kRequiredTextureCount] = {
    0x02EC, 0x0292, 0x0262};
constexpr uint32_t kManifestDefaultMasks[kManifestDefaultCount] = {0x20, 0x10,
                                                                   0x08, 0x04};
constexpr uint16_t kManifestMaleDefaults[kManifestDefaultCount] = {
    0x0031, 0x0090, 0x0048, 0x01C1};
constexpr uint16_t kManifestFemaleDefaults[kManifestDefaultCount] = {
    0x00FC, 0x015A, 0x012C, 0x0221};

uint16_t ManifestBe16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

void ManifestPut16(uint8_t* p, uint16_t value) {
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
}

void ManifestPut32(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

uint8_t ManifestAmount(uint32_t bits);

uint32_t ManifestAmountBits(uint8_t value, uint32_t original) {
  if (original && ManifestAmount(original) == value) {
    return original;
  }
  const float amount = float(value) / 127.5f - 1.0f;
  uint32_t bits;
  std::memcpy(&bits, &amount, sizeof(bits));
  return bits;
}

int32_t ManifestAssetEntry(const Catalog& catalog, const uint8_t* id);
bool NullAsset(const uint8_t* id);

// NOTHING goes here.
//
// The claim this replaces - that xam's caller puts a float 1.0f at +0x10 - is
// wrong, and it was never checked. `SetReplacementTexture` (0x8196C550) is six
// instructions: `memcpy(this[0x3EC] + 0x3C + type * 0x20, src, 0x20)`. It
// copies whatever it is handed and writes no float anywhere, so the only thing
// that can say what a real entry holds is a real entry. +0x10 of a manifest
// entry is the XAVATAR_COMPONENT_INFO ComponentMask - a WORD - and the real
// console leaves it, and the whole sixteen bytes after it, ZERO on every face
// texture entry. Checked against a genuine console profile (E00013258D7953EE,
// manifest at +0x1C5218):
//
//   0 id 0000800002EA0003C1C8F109A19CB2E0 mask 0000 tail 00000000...
//   1 id 00002000029E0003C1C8F109A19CB2E0 mask 0000 tail 00000000...
//   2 id 00004000026D0003C1C8F109A19CB2E0 mask 0000 tail 00000000...
//
// This used to write float 1.0 there as a "texture weight", so the editor read
// a ComponentMask of 0x3F80 for the mouth, the eyes and the eyebrows. That is
// not a component bit at all, and the editor keys its component collection by
// that value - a bad key drops the entry and aborts the rest of the insert
// loop, which leaves the collection empty. An empty collection is exactly what
// makes the six guarded Colour tiles ghost, because their enable is
// sub_920E1ED8 -> the scene's vtbl+0x30 walk over items that were never added.
// Skin and Eye Shadow are built from the base class with no such check, which
// is why those two open regardless.
void PutTextureWeight(uint8_t* out) { (void)out; }

void PutStockAsset(uint8_t* out, uint32_t kind, uint16_t index, uint16_t body) {
  ManifestPut32(out, kind);
  ManifestPut16(out + 4, index);
  ManifestPut16(out + 6, body);
  std::memcpy(out + 8, kStockAssetSuffix, sizeof(kStockAssetSuffix));
}

bool PutStockEntry(const Catalog* catalog, uint8_t* out, uint16_t index) {
  const Entry* entry = catalog ? catalog->Find(index) : nullptr;
  if (!entry || !entry->blob) {
    return false;
  }
  if (entry->external) {
    std::memcpy(out, entry->asset_id.data(), entry->asset_id.size());
    return true;
  }
  const uint32_t body = entry->BodyMask() & 3;
  PutStockAsset(out, entry->kind, index, uint16_t(body ? body : 3));
  return true;
}

uint8_t ManifestAmount(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  if (!std::isfinite(value)) {
    return 128;
  }
  return uint8_t(
      std::lround(std::clamp((value + 1.0f) * 127.5f, 0.0f, 255.0f)));
}

bool NullAsset(const uint8_t* id) {
  for (int i = 0; i < 16; ++i) {
    if (id[i]) {
      return false;
    }
  }
  return true;
}

int32_t ManifestAssetEntry(const Catalog& catalog, const uint8_t* id) {
  if (std::memcmp(id + 8, kStockAssetSuffix, sizeof(kStockAssetSuffix)) != 0) {
    const Entry* external = catalog.FindAsset(id);
    return external ? int32_t(external->index) : -1;
  }
  const uint32_t index = ManifestBe16(id + 4);
  const Entry* entry = catalog.Find(index);
  if (!entry || !entry->blob ||
      (entry->kind & ~kOutfitBit) != (Be32(id) & ~kOutfitBit)) {
    return -1;
  }
  return int32_t(index);
}

}  // namespace

// The one id an item is known by, whoever asks - and the id has to name this
// entry and no other, because everything downstream turns it back into an
// index. Only installed content owns an id that does that; the pack's own id
// is shared between variant rows and two thirds of its entries have none at
// all, so for those the id is built from the entry's kind and index, which is
// exactly what ManifestAssetEntry decodes.
//
// Handing out the pack's bytes instead is what made a chosen item vanish: for
// an entry with no id it was sixteen zeroes, which the manifest writer reads
// as "take it off", and for the rest it was an id no lookup could resolve.
std::array<uint8_t, 16> ManifestAssetId(const Entry& entry) {
  if (entry.external && !NullAsset(entry.asset_id.data())) {
    return entry.asset_id;
  }
  std::array<uint8_t, 16> id = {};
  const uint32_t body = entry.BodyMask() & 3;
  PutStockAsset(id.data(), entry.kind, uint16_t(entry.index),
                uint16_t(body ? body : 3));
  return id;
}

bool HasManifestLayout(const uint8_t* bytes, size_t size) {
  if (!bytes || size < kManifestBytes || HasDescriptionMagic(bytes, size)) {
    return false;
  }
  return ManifestBe16(bytes + kManifestBodyOffset + kManifestEntryMaskOffset) ==
             kKindBody &&
         ManifestBe16(bytes + kManifestHeadOffset + kManifestEntryMaskOffset) ==
             kKindHead;
}

bool ParseManifest(const Catalog* catalog, const uint8_t* bytes, size_t size,
                   Description* out) {
  if (!HasManifestLayout(bytes, size)) {
    return false;
  }
  Description description;
  description.body = ManifestBe16(bytes + kManifestBodyOffset + 6) == 2 ? 0 : 1;
  description.weight_bits = Be32(bytes + kManifestWeightOffset);
  description.height_bits = Be32(bytes + kManifestHeightOffset);
  description.weight = ManifestAmount(description.weight_bits);
  description.height = ManifestAmount(description.height_bits);
  for (uint32_t color = 0; color < kColorCount; ++color) {
    description.colors[color] = Be32(bytes + kManifestColorsOffset + color * 4);
  }
  for (uint32_t i = 0; i < kManifestBlendCount; ++i) {
    std::memcpy(description.blend_shapes[i].data(),
                bytes + kManifestBlendOffset + i * 16, 16);
  }
  if (catalog) {
    const auto covered = [&](uint32_t slot) {
      for (uint32_t other = 0; other < kSlotCount; ++other) {
        if (description.items[other] == kNoItem) {
          continue;
        }
        const Entry* entry = catalog->Find(description.items[other]);
        const uint32_t coverage =
            (entry ? SlotCoverage(entry->kind) : 0) | SlotBit(other);
        if (coverage & SlotBit(slot)) {
          return true;
        }
      }
      return false;
    };
    const auto place = [&](const uint8_t* record, bool only_if_uncovered) {
      if (NullAsset(record)) {
        return true;
      }
      const int32_t index = ManifestAssetEntry(*catalog, record);
      if (index < 0) {
        return false;
      }
      const int32_t slot = PrimarySlot(catalog->Find(uint32_t(index))->kind);
      if (slot < 0 || (only_if_uncovered && covered(uint32_t(slot)))) {
        return true;
      }
      description.items[slot] = uint16_t(index);
      return true;
    };
    for (uint32_t i = 0; i < kManifestFaceCount; ++i) {
      place(bytes + kManifestFaceOffset + i * kManifestEntryBytes, false);
    }
    for (uint32_t i = 0; i < kManifestComponentCount; ++i) {
      const uint8_t* record =
          bytes + kManifestComponentsOffset + i * kManifestEntryBytes;
      if (NullAsset(record)) {
        break;
      }
      place(record, false);
    }
    for (uint32_t i = 0; i < kManifestDefaultCount; ++i) {
      place(bytes + kManifestDefaultsOffset + i * kManifestEntryBytes, true);
    }
  }
  for (uint32_t i = 0; i < kManifestComponentCount; ++i) {
    const uint8_t* record =
        bytes + kManifestComponentsOffset + i * kManifestEntryBytes;
    if (NullAsset(record)) {
      break;
    }
    std::array<uint8_t, 32> raw;
    std::memcpy(raw.data(), record, raw.size());
    description.components.push_back(raw);
  }
  for (uint32_t i = 0; i < kManifestDefaultCount; ++i) {
    std::memcpy(description.required[i].data(),
                bytes + kManifestDefaultsOffset + i * kManifestEntryBytes,
                kManifestEntryBytes);
  }
  *out = description;
  return true;
}

std::array<uint8_t, kManifestBytes> SerializeManifest(
    const Catalog* catalog, const Description& description, uint64_t xuid) {
  std::array<uint8_t, kManifestBytes> manifest = {};
  uint8_t* bytes = manifest.data();
  const bool male = description.body != 0;
  ManifestPut32(
      bytes + kManifestWeightOffset,
      ManifestAmountBits(description.weight, description.weight_bits));
  ManifestPut32(
      bytes + kManifestHeightOffset,
      ManifestAmountBits(description.height, description.height_bits));
  for (uint32_t i = 0; i < kManifestBlendCount; ++i) {
    uint8_t* out = bytes + kManifestBlendOffset + i * 16;
    if (!NullAsset(description.blend_shapes[i].data())) {
      std::memcpy(out, description.blend_shapes[i].data(), 16);
      continue;
    }
    PutStockAsset(out, kShapeKinds[i],
                  male ? kMaleShapeDefaults[i] : kFemaleShapeDefaults[i], 3);
  }
  for (uint32_t i = 0; i < kManifestFaceCount; ++i) {
    uint8_t* out = bytes + kManifestFaceOffset + i * kManifestEntryBytes;
    const int32_t slot = PrimarySlot(kTextureKinds[i]);
    if (!(slot >= 0 && description.items[slot] != kNoItem &&
          PutStockEntry(catalog, out, description.items[slot]))) {
      if (i < kRequiredTextureCount) {
        PutStockAsset(
            out, kTextureKinds[i],
            male ? kMaleTextureDefaults[i] : kFemaleTextureDefaults[i], 3);
      }
    }
    PutTextureWeight(out);
  }
  for (uint32_t color = 0; color < kColorCount; ++color) {
    ManifestPut32(bytes + kManifestColorsOffset + color * 4,
                  description.colors[color]);
  }
  uint8_t* body = bytes + kManifestBodyOffset;
  PutStockAsset(body, kKindBody,
                uint16_t(male ? kMaleBodyEntry : kFemaleBodyEntry),
                male ? 1 : 2);
  ManifestPut16(body + kManifestEntryMaskOffset, uint16_t(kKindBody));
  uint8_t* head = bytes + kManifestHeadOffset;
  PutStockAsset(head, kKindHead, uint16_t(kHeadEntry), 3);
  ManifestPut16(head + kManifestEntryMaskOffset, uint16_t(kKindHead));
  uint32_t count = 0;
  std::array<bool, kSlotCount> written = {};
  const auto emit = [&](const uint8_t* record) {
    std::memcpy(bytes + kManifestComponentsOffset + count * kManifestEntryBytes,
                record, kManifestEntryBytes);
    ++count;
  };
  for (const auto& raw : description.components) {
    if (count >= kManifestComponentCount) {
      break;
    }
    const int32_t index =
        catalog ? ManifestAssetEntry(*catalog, raw.data()) : -1;
    const int32_t slot = index >= 0
                             ? PrimarySlot(catalog->Find(uint32_t(index))->kind)
                             : PrimarySlot(Be32(raw.data()));
    if (slot < 0 || uint32_t(slot) >= kClothingSlotCount || written[slot]) {
      continue;
    }
    const uint16_t item = description.items[slot];
    if (index >= 0 ? item == uint16_t(index) : item == kNoItem) {
      emit(raw.data());
      written[slot] = true;
    }
  }
  for (uint32_t slot = 0;
       slot < kClothingSlotCount && count < kManifestComponentCount; ++slot) {
    const uint16_t item = description.items[slot];
    uint8_t* record =
        bytes + kManifestComponentsOffset + count * kManifestEntryBytes;
    if (written[slot] || item == kNoItem ||
        !PutStockEntry(catalog, record, item)) {
      continue;
    }
    ManifestPut16(record + kManifestEntryMaskOffset,
                  uint16_t(catalog->Find(item)->kind));
    ++count;
  }
  for (uint32_t i = 0; i < kManifestDefaultCount; ++i) {
    const uint32_t mask = kManifestDefaultMasks[i];
    uint8_t* record = bytes + kManifestDefaultsOffset + i * kManifestEntryBytes;
    int32_t equipped = -1;
    for (uint32_t slot = 0; catalog && slot < kClothingSlotCount; ++slot) {
      const Entry* entry = description.items[slot] == kNoItem
                               ? nullptr
                               : catalog->Find(description.items[slot]);
      if (entry && entry->kind == mask) {
        equipped = description.items[slot];
        break;
      }
    }
    const auto& previous = description.required[i];
    const int32_t previous_index =
        catalog && !NullAsset(previous.data())
            ? ManifestAssetEntry(*catalog, previous.data())
            : -1;
    const bool previous_fits =
        !NullAsset(previous.data()) &&
        ManifestBe16(previous.data() + kManifestEntryMaskOffset) == mask &&
        (previous.data()[7] & (male ? 1 : 2));
    if (previous_fits && (equipped < 0 || equipped == previous_index)) {
      std::memcpy(record, previous.data(), kManifestEntryBytes);
      continue;
    }
    const uint16_t pick = equipped >= 0 ? uint16_t(equipped)
                                        : (male ? kManifestMaleDefaults[i]
                                                : kManifestFemaleDefaults[i]);
    if (!PutStockEntry(catalog, record, pick)) {
      PutStockAsset(record, mask, pick, mask == 0x04 ? 3 : (male ? 1 : 2));
    }
    ManifestPut16(record + kManifestEntryMaskOffset, uint16_t(mask));
  }
  ManifestPut32(bytes + kManifestXuidOffset, uint32_t(xuid >> 32));
  ManifestPut32(bytes + kManifestXuidOffset + 4, uint32_t(xuid));
  return manifest;
}

bool ParseAnyDescription(const Catalog* catalog, const uint8_t* bytes,
                         size_t size, Description* out) {
  return ParseDescription(bytes, size, out) ||
         ParseManifest(catalog, bytes, size, out);
}

Description DescriptionFromBytes(const Catalog* catalog, const uint8_t* bytes,
                                 size_t size) {
  Description description;
  if (ParseAnyDescription(catalog, bytes, size, &description)) {
    return description;
  }
  uint32_t seed = 2166136261u;
  for (size_t i = 0; bytes && i < size; ++i) {
    seed = (seed ^ bytes[i]) * 16777619u;
  }
  std::mt19937 rng(seed);
  const bool magic = HasDescriptionMagic(bytes, size);
  const int32_t body = magic ? (bytes[4] ? 1 : 0) : -1;
  if (catalog) {
    description = RandomDescription(*catalog, rng, body);
  } else if (body >= 0) {
    description.body = uint8_t(body);
  }
  if (magic) {
    description.height = bytes[5];
  }
  return description;
}

int32_t PresetClip(uint32_t preset) {
  return preset < sizeof(kPresetClips) / sizeof(kPresetClips[0])
             ? kPresetClips[preset]
             : -1;
}

Matrix Identity() {
  Matrix m = {};
  m[0] = m[5] = m[10] = m[15] = 1.0f;
  return m;
}

Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out[r * 4 + c] = a[r * 4 + 0] * b[0 + c] + a[r * 4 + 1] * b[4 + c] +
                       a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    }
  }
  return out;
}

void BindPose(const Skeleton&, Matrix* local) {
  for (uint32_t j = 0; j < kBoneCount; ++j) {
    local[j] = Identity();
  }
}

void SamplePose(const Clip& clip, const Skeleton& skeleton, float seconds,
                Matrix* local) {
  BindPose(skeleton, local);
  const float length = clip.Length();
  if (length <= 0.0f || !clip.frames) {
    return;
  }
  float time = std::fmod(seconds, length);
  if (time < 0.0f) {
    time += length;
  }
  const float frame = time * clip.rate;
  const uint32_t f0 = std::min(uint32_t(frame), clip.frames - 1);
  const uint32_t f1 = (f0 + 1) % clip.frames;
  const float t = frame - float(f0);
  const uint32_t joints =
      std::min(std::min(clip.joints, skeleton.count), kBoneCount);
  for (uint32_t j = 0; j < joints; ++j) {
    const float* a =
        clip.keys.data() + (size_t(f0) * clip.joints + j) * kKeyFloats;
    const float* b =
        clip.keys.data() + (size_t(f1) * clip.joints + j) * kKeyFloats;
    float q[4];
    Slerp(a, b, t, q);
    float translation[3];
    Matrix scale = Identity();
    for (int k = 0; k < 3; ++k) {
      translation[k] = a[4 + k] + (b[4 + k] - a[4 + k]) * t;
      scale[k * 5] = a[7 + k] + (b[7 + k] - a[7 + k]) * t;
    }
    local[j] = Multiply(scale, FromQuaternion(q, translation));
  }
}

void SkinMatrices(const Skeleton& skeleton, const Matrix* local, Matrix* skin) {
  Matrix world[kMaxJoints];
  for (uint32_t j = 0; j < kMaxJoints; ++j) {
    skin[j] = Identity();
    world[j] = Identity();
  }
  const uint32_t count = std::min(skeleton.count, kBoneCount);
  for (uint32_t j = 0; j < count; ++j) {
    const uint32_t parent = skeleton.parents[j];
    float offset[3];
    BindOffset(skeleton, j, offset);
    Matrix scale = Identity();
    scale[0] = skeleton.scale[j][0];
    scale[5] = skeleton.scale[j][1];
    scale[10] = skeleton.scale[j][2];
    const Matrix posed = Multiply(
        scale,
        Multiply(local[j], Translation(offset[0], offset[1], offset[2])));
    world[j] = parent < j ? Multiply(posed, world[parent]) : posed;
    skin[j] = Multiply(Translation(-skeleton.bind[j][0], -skeleton.bind[j][1],
                                   -skeleton.bind[j][2]),
                       world[j]);
  }
}

void SampleCarryable(const Carryable& carryable, float seconds, Matrix* local) {
  if (carryable.joints) {
    SamplePose(*carryable.joints, carryable.skeleton, seconds, local);
  } else {
    BindPose(carryable.skeleton, local);
  }
}

// The shapes a description names, in manifest order. Empty when it wears none.
std::vector<std::shared_ptr<const Shape>> HeadShapes(
    Catalog& catalog, const Description& description) {
  std::vector<std::shared_ptr<const Shape>> shapes;
  for (uint32_t i = 0; i < kManifestBlendCount; ++i) {
    if (NullAsset(description.blend_shapes[i].data())) {
      continue;
    }
    const int32_t entry =
        ManifestAssetEntry(catalog, description.blend_shapes[i].data());
    if (entry < 0) {
      continue;
    }
    if (auto shape = catalog.LoadShape(uint32_t(entry))) {
      shapes.push_back(std::move(shape));
    }
  }
  return shapes;
}

// A shape states ABSOLUTE positions for the vertices it moves, keyed by their
// BYTE OFFSET in the vertex buffer - so the batch that owns the offset decides
// the stride. The offsets come in two runs 324 vertices apart: measured, those
// are the left and the right side of the head, mirrored exactly across x = 0 -
// Large Chin moves v341 to (-0.09642, 1.20610, 0.05333) and v17 to
// (+0.09641, 1.20610, 0.05333), with v641 on the midline. Both runs are this
// head's, so both are applied.
template <typename Batches, typename Place>
uint32_t PlaceShapeVertices(const Shape& shape, const Batches& layout,
                            uint32_t vb_offset, const Place& place) {
  uint32_t missed = 0;
  for (const ShapeVertex& moved : shape.vertices) {
    bool placed = false;
    for (size_t index = 0; index < layout.size(); ++index) {
      const RawBatch& batch = layout[index];
      // A batch that does not state a stride uses the fixed fields plus one
      // dword per UV set: 28 + 4 * uv_sets, which is the 52 the blend shapes
      // are keyed on when all six sets are present.
      const uint32_t stride =
          batch.stride ? batch.stride : 28 + 4 * batch.uv_sets;
      // A batch states where it sits in the whole GPU buffer, which also holds
      // the indices and the textures. A shape counts from the start of the
      // VERTEX buffer - Model_c::field_24 in xam, the pointer the offset is
      // added to - so the model's own vb_offset comes off first.
      const uint32_t base =
          batch.vb_offset >= vb_offset ? batch.vb_offset - vb_offset : 0;
      if (!stride || moved.offset < base) {
        continue;
      }
      const uint32_t at = moved.offset - base;
      if (at % stride || at / stride >= batch.vertices.size()) {
        continue;
      }
      place(index, at / stride, moved);
      placed = true;
      break;
    }
    missed += placed ? 0 : 1;
  }
  return missed;
}

bool ReshapeRawHead(Catalog& catalog, const Description& description,
                    RawModel* head) {
  const auto shapes = HeadShapes(catalog, description);
  if (shapes.empty() || !head) {
    return false;
  }
  for (const auto& shape : shapes) {
    const uint32_t missed = PlaceShapeVertices(
        *shape, head->batches, head->vb_offset,
        [&](size_t batch, uint32_t vertex, const ShapeVertex& moved) {
          RawVertex& out = head->batches[batch].vertices[vertex];
          out.position[0] = moved.position[0];
          out.position[1] = moved.position[1];
          out.position[2] = moved.position[2];
        });
    // A tripwire: every offset a shape names should land in one of the head's
    // batches. Silent when it does; when it is not, say what the head's own
    // layout is, because that is the only thing that can explain it.
    if (missed) {
      std::string layout;
      for (const RawBatch& batch : head->batches) {
        layout +=
            fmt::format(" [vb {:X} stride {} uv {} n {}]", batch.vb_offset,
                        batch.stride, batch.uv_sets, batch.vertices.size());
      }
      XELOGW(
          "avatar: a blend shape names {} of {} vertices this head has not; "
          "head vb {:X} size {}, batches{}",
          missed, shape->vertices.size(), head->vb_offset, head->vb_size,
          layout);
    }
  }
  return true;
}

// The same deformation on the model our own renderer draws. The decoded model
// and the raw one come out of the same record in the same order, so the raw
// one's batch layout is what resolves an offset for both.
void ReshapeHead(Catalog& catalog, const Description& description,
                 Scene* scene) {
  Part* head = nullptr;
  for (Part& part : scene->parts) {
    if (part.kind == kKindHead) {
      head = &part;
      break;
    }
  }
  if (!head || !head->model) {
    return;
  }
  const auto shapes = HeadShapes(catalog, description);
  if (shapes.empty()) {
    return;
  }
  auto layout = catalog.LoadRawModel(head->entry);
  if (!layout) {
    return;
  }
  auto reshaped = std::make_shared<Model>(*head->model);
  for (const auto& shape : shapes) {
    PlaceShapeVertices(
        *shape, layout->batches, layout->vb_offset,
        [&](size_t batch, uint32_t vertex, const ShapeVertex& moved) {
          if (batch >= reshaped->batches.size() ||
              vertex >= reshaped->batches[batch].vertices.size()) {
            return;
          }
          Vertex& out = reshaped->batches[batch].vertices[vertex];
          out.position[0] = moved.position[0];
          out.position[1] = moved.position[1];
          out.position[2] = moved.position[2];
        });
  }
  head->model = reshaped;
}

// Collect the hiding templates of everything the avatar has on and collapse
// the body triangles they name. This is what stops skin coming through
// clothing: every template in the pack targets the body (kind 2), male or
// female, and nothing else.
void HideCoveredBody(Catalog& catalog, const Description& description,
                     Scene* scene) {
  Part* body = nullptr;
  for (Part& part : scene->parts) {
    if (part.kind == kKindBody) {
      body = &part;
      break;
    }
  }
  if (!body || !body->model) {
    return;
  }
  std::vector<uint32_t> hidden;
  for (const Part& part : scene->parts) {
    const uint32_t index = HidingTemplateOf(catalog, part.entry);
    if (index == UINT32_MAX) {
      continue;
    }
    auto shape = catalog.LoadShape(index);
    // A template is authored against one body, so refuse the other one's -
    // its triangle numbers mean something else entirely there. The template
    // states the body the way BodyMask does, 1 male and 2 female, not the way
    // Description does.
    if (!shape || shape->hidden.empty() ||
        shape->body != BodyBit(description.body)) {
      continue;
    }
    hidden.insert(hidden.end(), shape->hidden.begin(), shape->hidden.end());
  }
  if (hidden.empty()) {
    return;
  }
  auto edited = std::make_shared<Model>(*body->model);
  const uint32_t missed = HideTriangles(edited.get(), hidden);
  if (missed) {
    XELOGW(
        "avatar: {} of {} hidden body triangles fell outside every batch - "
        "the batch index-buffer offsets do not line up with the template",
        missed, hidden.size());
  }
  body->model = std::move(edited);
}

Scene BuildScene(Catalog& catalog, const Description& description) {
  Scene scene;
  scene.body = description.body;
  scene.skeleton = ScaledSkeleton(description);
  for (uint32_t color = 0; color < kColorCount; ++color) {
    ColorToFloat(description.colors[color], scene.colors[color]);
  }
  const auto add = [&](uint32_t entry, uint32_t kind,
                       const std::array<uint32_t, 3>& custom) {
    if (entry == UINT32_MAX) {
      return;
    }
    auto model = catalog.LoadModel(entry);
    if (!model) {
      return;
    }
    Part part;
    part.model = model;
    part.entry = entry;
    part.kind = kind;
    for (int k = 0; k < 3; ++k) {
      ColorToFloat(custom[k], part.custom[k]);
    }
    scene.parts.push_back(std::move(part));
  };
  const uint32_t skin = description.colors[kColorSkin];
  const uint32_t hair = description.colors[kColorHair];
  add(FindBody(catalog, description.body), kKindBody, {skin, skin, skin});
  add(FindHead(catalog), kKindHead, {skin, skin, skin});
  ReshapeHead(catalog, description, &scene);
  const bool hat = description.items[kSlotHat] != kNoItem;
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    const uint16_t item = description.items[slot];
    const Entry* entry = item == kNoItem ? nullptr : catalog.Find(item);
    if (!entry || PrimarySlot(entry->kind) != int32_t(slot)) {
      continue;
    }
    const int32_t feature = FeatureForSlot(slot);
    if (feature >= 0) {
      scene.features[feature] = catalog.LoadFeature(item);
      scene.feature_entries[feature] = item;
      continue;
    }
    if (slot == kSlotHair) {
      add(hat ? HatVariantOf(catalog, item) : item, entry->kind,
          {hair, hair, hair});
    } else {
      add(item, entry->kind, description.custom[slot]);
      if (slot == kSlotCarryable && !scene.parts.empty() &&
          scene.parts.back().entry == item) {
        scene.carryable = catalog.LoadCarryable(item);
        scene.parts.back().carried = scene.carryable != nullptr;
      }
    }
  }
  HideCoveredBody(catalog, description, &scene);
  return scene;
}

Material BuildMaterial(const Scene& scene, const Part& part, const Batch& batch,
                       const Expression& expression) {
  Material material;
  const bool head = part.kind == kKindHead;
  for (int k = 0; k < 4; ++k) {
    material.base[k] = head ? scene.colors[kColorSkin][k] : 1.0f;
    for (int c = 0; c < 3; ++c) {
      material.custom[c][k] = part.custom[c][k];
    }
  }
  const uint32_t expressions[5] = {expression.mouth, expression.left_eyebrow,
                                   expression.right_eyebrow,
                                   expression.left_eye, expression.right_eye};
  uint32_t layer = 0;
  for (const Param& param : batch.params) {
    if (param.type != kParamTexture || layer >= kLayerCount) {
      continue;
    }
    const uint32_t index = param.data[0] & 0xFFFF;
    const uint32_t uv = (param.data[0] >> 16) & 0xFFFF;
    const Texture* texture = nullptr;
    uint64_t id = 0;
    uint32_t kind = kLayerNone;
    uint32_t slice = 0;
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (!head) {
      kind = param.usage == 1   ? kLayerColor
             : param.usage == 2 ? kLayerMask
             : param.usage == 3 ? kLayerDecal
                                : kLayerNone;
      if (index < part.model->textures.size()) {
        texture = part.model->textures[index].get();
      }
      id = (uint64_t(part.entry) << 16) | index;
    } else {
      for (const HeadLayer& candidate : kHeadLayers) {
        if (candidate.usage != param.usage) {
          continue;
        }
        kind = candidate.kind;
        texture = scene.features[candidate.feature].get();
        id =
            (uint64_t(scene.feature_entries[candidate.feature]) << 16) | 0xFFFF;
        if (candidate.color >= 0) {
          std::memcpy(tint, scene.colors[candidate.color], sizeof(tint));
        }
        if (candidate.expression >= 0) {
          slice = expressions[candidate.expression];
        }
        break;
      }
    }
    if (kind == kLayerNone || !texture || !texture->slices) {
      continue;
    }
    material.layer[layer][0] = kind;
    material.layer[layer][1] = std::min(uv, batch.uv_sets - 1);
    material.layer[layer][2] = std::min(slice, texture->slices - 1);
    material.layer[layer][3] = param.data[1] == 0 ? 1 : 0;
    std::memcpy(material.tint[layer], tint, sizeof(tint));
    material.textures[layer] = texture;
    material.texture_ids[layer] = id;
    ++layer;
  }
  return material;
}

Lighting DefaultLighting() {
  Lighting light;
  const float direction[3] = {0.3f, -1.0f, -0.5f};
  const float length =
      std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                direction[2] * direction[2]);
  for (int k = 0; k < 3; ++k) {
    light.direction[k] = direction[k] / length;
    light.color[k] = 0.6f;
    light.ambient[k] = 0.45f;
  }
  return light;
}

void UnpackNormal(uint32_t packed, float out[3]) { DecodeNormal(packed, out); }

void SkinBatch(const Batch& batch, const Matrix* skin,
               std::vector<GpuVertex>* out, float inset) {
  out->resize(batch.vertices.size());
  for (size_t i = 0; i < batch.vertices.size(); ++i) {
    const Vertex& vertex = batch.vertices[i];
    GpuVertex& result = (*out)[i];
    float position[3] = {};
    float normal[3] = {};
    for (int k = 0; k < 4; ++k) {
      const float weight = vertex.weights[k];
      if (weight <= 0.0f) {
        continue;
      }
      const uint32_t bone = vertex.bones[k] < kMaxJoints ? vertex.bones[k] : 0;
      float p[3];
      float n[3];
      TransformPoint(skin[bone], vertex.position, p);
      TransformVector(skin[bone], vertex.normal, n);
      for (int c = 0; c < 3; ++c) {
        position[c] += weight * p[c];
        normal[c] += weight * n[c];
      }
    }
    const float length = std::sqrt(
        normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    for (int c = 0; c < 3; ++c) {
      const float unit = length > 0.0f ? normal[c] / length : 0.0f;
      const float facing = c == 1 ? 1.0f : -1.0f;
      result.normal[c] = unit * facing;
      result.position[c] = (position[c] - unit * inset) * facing;
    }
    std::memcpy(result.uv, vertex.uv, sizeof(result.uv));
  }
}

void FillMaterialConstants(const Material& material, GpuConstants* out) {
  std::memcpy(out->base, material.base, sizeof(out->base));
  std::memcpy(out->custom, material.custom, sizeof(out->custom));
  std::memcpy(out->tint, material.tint, sizeof(out->tint));
  std::memcpy(out->layer, material.layer, sizeof(out->layer));
}

void RenderPreview(const Scene& scene, const Matrix* local,
                   const Matrix* carried_local, const Expression& expression,
                   uint32_t width, uint32_t height, float yaw,
                   std::vector<uint8_t>* rgba) {
  rgba->assign(size_t(width) * height * 4, 0);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    (*rgba)[i * 4 + 0] = 38;
    (*rgba)[i * 4 + 1] = 41;
    (*rgba)[i * 4 + 2] = 48;
    (*rgba)[i * 4 + 3] = 255;
  }
  if (!width || !height) {
    return;
  }
  Matrix skin[kMaxJoints];
  SkinMatrices(scene.skeleton, local, skin);
  Matrix carry_skin[kMaxJoints];
  if (scene.carryable) {
    Matrix carry_local[kMaxJoints];
    if (carried_local) {
      std::copy(carried_local, carried_local + kMaxJoints, carry_local);
    } else {
      BindPose(scene.carryable->skeleton, carry_local);
    }
    SkinMatrices(scene.carryable->skeleton, carry_local, carry_skin);
  }
  const Lighting light = DefaultLighting();
  const float facing = yaw + 3.14159265f;
  const float cosine = std::cos(facing);
  const float sine = std::sin(facing);
  struct Prepared {
    const Batch* batch;
    Material material;
    std::vector<GpuVertex> vertices;
  };
  std::vector<Prepared> prepared;
  float low[2] = {1e9f, 1e9f};
  float high[2] = {-1e9f, -1e9f};
  for (const Part& part : scene.parts) {
    for (const Batch& batch : part.model->batches) {
      Prepared entry;
      entry.batch = &batch;
      entry.material = BuildMaterial(scene, part, batch, expression);
      SkinBatch(batch, part.carried ? carry_skin : skin, &entry.vertices);
      for (GpuVertex& vertex : entry.vertices) {
        const float x = vertex.position[0];
        const float z = vertex.position[2];
        vertex.position[0] = x * cosine + z * sine;
        vertex.position[2] = -x * sine + z * cosine;
        const float nx = vertex.normal[0];
        const float nz = vertex.normal[2];
        vertex.normal[0] = nx * cosine + nz * sine;
        vertex.normal[2] = -nx * sine + nz * cosine;
        low[0] = std::min(low[0], vertex.position[0]);
        high[0] = std::max(high[0], vertex.position[0]);
        low[1] = std::min(low[1], vertex.position[1]);
        high[1] = std::max(high[1], vertex.position[1]);
      }
      prepared.push_back(std::move(entry));
    }
  }
  if (prepared.empty() || high[0] <= low[0] || high[1] <= low[1]) {
    return;
  }
  const float scale = std::min(float(width) / ((high[0] - low[0]) * 1.15f),
                               float(height) / ((high[1] - low[1]) * 1.08f));
  const float center_x = (low[0] + high[0]) * 0.5f;
  const float center_y = (low[1] + high[1]) * 0.5f;
  std::vector<float> depth(size_t(width) * height, -1e30f);
  for (const Prepared& entry : prepared) {
    const auto& vertices = entry.vertices;
    const auto& indices = entry.batch->indices;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
      const GpuVertex* v[3] = {&vertices[indices[t]], &vertices[indices[t + 1]],
                               &vertices[indices[t + 2]]};
      float sx[3];
      float sy[3];
      for (int k = 0; k < 3; ++k) {
        sx[k] = float(width) * 0.5f + (v[k]->position[0] - center_x) * scale;
        sy[k] = float(height) * 0.5f - (v[k]->position[1] - center_y) * scale;
      }
      const float area =
          (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
      if (std::fabs(area) < 1e-9f) {
        continue;
      }
      const int32_t min_x =
          std::max(0, int32_t(std::floor(std::min({sx[0], sx[1], sx[2]}))));
      const int32_t max_x =
          std::min(int32_t(width) - 1,
                   int32_t(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
      const int32_t min_y =
          std::max(0, int32_t(std::floor(std::min({sy[0], sy[1], sy[2]}))));
      const int32_t max_y =
          std::min(int32_t(height) - 1,
                   int32_t(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
      for (int32_t y = min_y; y <= max_y; ++y) {
        for (int32_t x = min_x; x <= max_x; ++x) {
          const float px = float(x) + 0.5f;
          const float py = float(y) + 0.5f;
          const float w0 =
              ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) /
              area;
          const float w1 =
              ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) /
              area;
          const float w2 = 1.0f - w0 - w1;
          if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
            continue;
          }
          const float z = w0 * v[0]->position[2] + w1 * v[1]->position[2] +
                          w2 * v[2]->position[2];
          const size_t pixel = size_t(y) * width + x;
          if (z <= depth[pixel]) {
            continue;
          }
          float uv[kLayerCount][2];
          for (uint32_t s = 0; s < kLayerCount; ++s) {
            for (int c = 0; c < 2; ++c) {
              uv[s][c] = w0 * v[0]->uv[s][c] + w1 * v[1]->uv[s][c] +
                         w2 * v[2]->uv[s][c];
            }
          }
          float normal[3];
          float length = 0.0f;
          for (int c = 0; c < 3; ++c) {
            normal[c] = w0 * v[0]->normal[c] + w1 * v[1]->normal[c] +
                        w2 * v[2]->normal[c];
            length += normal[c] * normal[c];
          }
          length = std::sqrt(length);
          for (int c = 0; c < 3; ++c) {
            normal[c] = length > 0.0f ? normal[c] / length : 0.0f;
          }
          float color[4];
          Shade(entry.material, uv, normal, light, color);
          if (color[3] < 0.5f) {
            continue;
          }
          depth[pixel] = z;
          for (int c = 0; c < 3; ++c) {
            (*rgba)[pixel * 4 + c] = uint8_t(color[c] * 255.0f + 0.5f);
          }
          (*rgba)[pixel * 4 + 3] = 255;
        }
      }
    }
  }
}

std::vector<uint8_t> EncodePng(uint32_t width, uint32_t height,
                               const std::vector<uint8_t>& rgba) {
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> header;
  PutBe32(&header, width);
  PutBe32(&header, height);
  header.insert(header.end(), {8, 6, 0, 0, 0});
  PngChunk(&png, "IHDR", header);
  std::vector<uint8_t> raw;
  raw.reserve((size_t(width) * 4 + 1) * height);
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + size_t(y) * width * 4,
               rgba.begin() + size_t(y + 1) * width * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1;
  uint32_t b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  size_t at = 0;
  do {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    const bool last = at + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
  } while (at < raw.size());
  PutBe32(&z, (b << 16) | a);
  PngChunk(&png, "IDAT", z);
  PngChunk(&png, "IEND", {});
  return png;
}

// ---- mip generation --------------------------------------------------------
//
// XamAvatarGenerateMipMaps has to fill the mip chain of every texture in the
// asset buffer, in that texture's OWN format - xam's sub_8196B1E8 walks the
// models at stride 0x34 and their textures at 0x2C and does exactly that. So a
// DXT level has to come back out as DXT, which needs an encoder as well as the
// decoder the pack loader already has.

uint32_t TextureSliceBytes(uint32_t format, uint32_t width, uint32_t height,
                           uint32_t* out_pitch) {
  const uint32_t kind = format & 0x3F;
  uint32_t block = 1;
  uint32_t bytes = 4;
  if (kind == 0x12) {
    block = 4;
    bytes = 8;
  } else if (kind == 0x13 || kind == 0x14) {
    block = 4;
    bytes = 16;
  } else if (kind != 0x06) {
    return 0;
  }
  const uint32_t wide = (std::max(width, 1u) + block - 1) / block;
  const uint32_t high = (std::max(height, 1u) + block - 1) / block;
  const uint32_t pitch = wide * bytes;
  if (out_pitch) {
    *out_pitch = pitch;
  }
  return pitch * high;
}

bool DecodeTextureSlice(const uint8_t* data, size_t size, uint32_t format,
                        uint32_t width, uint32_t height, uint32_t pitch,
                        uint8_t* rgba) {
  return DecodeSlice(data, size, format, width, height, pitch, rgba);
}

namespace {

// One 4x4 block, endpoints from the bounding box of its colours. Good enough
// for a mip level and entirely deterministic, which matters more here than
// squeezing the last bit of quality out of the fit.
void EncodeColorBlock(const uint8_t pixels[16][4], bool opaque, uint8_t* out) {
  uint8_t low[3] = {255, 255, 255};
  uint8_t high[3] = {0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    for (int k = 0; k < 3; ++k) {
      low[k] = std::min(low[k], pixels[i][k]);
      high[k] = std::max(high[k], pixels[i][k]);
    }
  }
  const auto pack = [](const uint8_t* rgb) -> uint16_t {
    return uint16_t(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) |
                    (rgb[2] >> 3));
  };
  uint16_t c0 = pack(high);
  uint16_t c1 = pack(low);
  // c0 > c1 selects the four-colour block; DXT1 uses the other ordering to mean
  // "one index is transparent", which a mip of an opaque surface must not say.
  if (opaque && c0 <= c1) {
    if (c1 == 0xFFFF) {
      c0 = c1;
      c1 = 0;
    } else {
      c0 = uint16_t(c1 + 1);
    }
  }
  uint8_t ends[4][3];
  const auto unpack = [](uint16_t c, uint8_t* rgb) {
    rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    rgb[2] = uint8_t((c & 31) * 255 / 31);
  };
  unpack(c0, ends[0]);
  unpack(c1, ends[1]);
  for (int k = 0; k < 3; ++k) {
    ends[2][k] = uint8_t((2 * ends[0][k] + ends[1][k]) / 3);
    ends[3][k] = uint8_t((ends[0][k] + 2 * ends[1][k]) / 3);
  }
  uint32_t indices = 0;
  for (int i = 0; i < 16; ++i) {
    uint32_t best = 0;
    int32_t best_error = INT32_MAX;
    for (uint32_t e = 0; e < 4; ++e) {
      int32_t error = 0;
      for (int k = 0; k < 3; ++k) {
        const int32_t d = int32_t(pixels[i][k]) - int32_t(ends[e][k]);
        error += d * d;
      }
      if (error < best_error) {
        best_error = error;
        best = e;
      }
    }
    indices |= best << (2 * i);
  }
  out[0] = uint8_t(c0);
  out[1] = uint8_t(c0 >> 8);
  out[2] = uint8_t(c1);
  out[3] = uint8_t(c1 >> 8);
  for (int k = 0; k < 4; ++k) {
    out[4 + k] = uint8_t(indices >> (8 * k));
  }
}

void EncodeAlphaBlock(const uint8_t pixels[16][4], uint8_t* out) {
  uint8_t low = 255;
  uint8_t high = 0;
  for (int i = 0; i < 16; ++i) {
    low = std::min(low, pixels[i][3]);
    high = std::max(high, pixels[i][3]);
  }
  out[0] = high;
  out[1] = low;
  uint64_t indices = 0;
  for (int i = 0; i < 16; ++i) {
    uint32_t best = 0;
    int32_t best_error = INT32_MAX;
    for (uint32_t e = 0; e < 8; ++e) {
      int32_t value;
      if (e == 0) {
        value = high;
      } else if (e == 1) {
        value = low;
      } else if (high > low) {
        value = (int32_t(high) * (8 - e) + int32_t(low) * (int32_t(e) - 1)) / 7;
      } else {
        value = low;
      }
      const int32_t error = std::abs(int32_t(pixels[i][3]) - value);
      if (error < best_error) {
        best_error = error;
        best = e;
      }
    }
    indices |= uint64_t(best) << (3 * i);
  }
  for (int k = 0; k < 6; ++k) {
    out[2 + k] = uint8_t(indices >> (8 * k));
  }
}

}  // namespace

bool EncodeTextureSlice(const uint8_t* rgba, uint32_t format, uint32_t width,
                        uint32_t height, uint32_t pitch, uint8_t* out,
                        size_t size) {
  const uint32_t kind = format & 0x3F;
  if (kind == 0x06) {
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const size_t d = size_t(y) * pitch + size_t(x) * 4;
        if (d + 4 > size) {
          return false;
        }
        const uint8_t* s = rgba + (size_t(y) * width + x) * 4;
        // The same ARGB order DecodeSlice reads back.
        out[d + 0] = s[3];
        out[d + 1] = s[0];
        out[d + 2] = s[1];
        out[d + 3] = s[2];
      }
    }
    return true;
  }
  const bool dxt1 = kind == 0x12;
  const bool dxt3 = kind == 0x13;
  const bool dxt5 = kind == 0x14;
  if (!dxt1 && !dxt3 && !dxt5) {
    return false;
  }
  const uint32_t block = dxt1 ? 8 : 16;
  const uint32_t blocks_wide = (width + 3) / 4;
  const uint32_t blocks_high = (height + 3) / 4;
  for (uint32_t by = 0; by < blocks_high; ++by) {
    for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
      uint8_t pixels[16][4] = {};
      for (int i = 0; i < 16; ++i) {
        const uint32_t x = std::min(bx * 4 + (i & 3), width - 1);
        const uint32_t y = std::min(by * 4 + uint32_t(i >> 2), height - 1);
        std::memcpy(pixels[i], rgba + (size_t(y) * width + x) * 4, 4);
      }
      uint8_t packed[16] = {};
      if (dxt1) {
        EncodeColorBlock(pixels, true, packed);
      } else {
        EncodeColorBlock(pixels, true, packed + 8);
        if (dxt5) {
          EncodeAlphaBlock(pixels, packed);
        } else {
          for (int i = 0; i < 16; i += 2) {
            packed[i / 2] =
                uint8_t((pixels[i][3] >> 4) | ((pixels[i + 1][3] >> 4) << 4));
          }
        }
      }
      // Stored byte-swapped in halfwords, exactly as DecodeSlice un-swaps it.
      for (uint32_t k = 0; k + 1 < block; k += 2) {
        std::swap(packed[k], packed[k + 1]);
      }
      const size_t d = size_t(by) * pitch + size_t(bx) * block;
      if (d + block > size) {
        return false;
      }
      std::memcpy(out + d, packed, block);
    }
  }
  return true;
}

}  // namespace avatar
}  // namespace xna
}  // namespace kernel
}  // namespace xe
