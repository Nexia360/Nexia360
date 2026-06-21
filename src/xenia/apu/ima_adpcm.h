/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_IMA_ADPCM_H_
#define XENIA_APU_IMA_ADPCM_H_

#include <cstdint>
#include <vector>

// Narrowband IMA ADPCM (4-bit, mono). Used for chat voice: deliberately low
// quality (8 kHz mono, ~16 kbit/s) to match Xbox Live voice. Two samples pack
// into one byte; low nibble first. State carries between calls for streaming.

namespace xe {
namespace apu {
namespace ima_adpcm {

constexpr uint32_t kSampleRate = 16000;  // XHV_PCM_SAMPLE_RATE

struct State {
  int32_t predictor = 0;  // last decoded sample, [-32768, 32767]
  int32_t index = 0;      // step table index, [0, 88]
};

inline constexpr int16_t kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,
    21,    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,
    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,
    173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,
    494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,
    1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,
    4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};

inline constexpr int8_t kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                           -1, -1, -1, -1, 2, 4, 6, 8};

inline int32_t Clamp(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

inline uint8_t EncodeSample(State& s, int16_t sample) {
  int32_t step = kStepTable[s.index];
  int32_t diff = sample - s.predictor;
  uint8_t code = 0;
  if (diff < 0) {
    code = 8;
    diff = -diff;
  }
  int32_t vpdiff = step >> 3;
  if (diff >= step) {
    code |= 4;
    diff -= step;
    vpdiff += step;
  }
  step >>= 1;
  if (diff >= step) {
    code |= 2;
    diff -= step;
    vpdiff += step;
  }
  step >>= 1;
  if (diff >= step) {
    code |= 1;
    vpdiff += step;
  }
  s.predictor = Clamp(s.predictor + ((code & 8) ? -vpdiff : vpdiff), -32768,
                      32767);
  s.index = Clamp(s.index + kIndexTable[code], 0, 88);
  return code;
}

inline int16_t DecodeSample(State& s, uint8_t code) {
  int32_t step = kStepTable[s.index];
  int32_t vpdiff = step >> 3;
  if (code & 4) vpdiff += step;
  if (code & 2) vpdiff += step >> 1;
  if (code & 1) vpdiff += step >> 2;
  s.predictor = Clamp(s.predictor + ((code & 8) ? -vpdiff : vpdiff), -32768,
                      32767);
  s.index = Clamp(s.index + kIndexTable[code], 0, 88);
  return static_cast<int16_t>(s.predictor);
}

// Encodes mono PCM16 to packed 4-bit ADPCM. Appends to out.
inline void Encode(State& s, const int16_t* pcm, size_t count,
                   std::vector<uint8_t>& out) {
  for (size_t i = 0; i < count; i += 2) {
    uint8_t lo = EncodeSample(s, pcm[i]);
    uint8_t hi = (i + 1 < count) ? EncodeSample(s, pcm[i + 1]) : 0;
    out.push_back(static_cast<uint8_t>(lo | (hi << 4)));
  }
}

// Decodes packed 4-bit ADPCM to mono PCM16. Appends to out.
inline void Decode(State& s, const uint8_t* data, size_t bytes,
                   std::vector<int16_t>& out) {
  for (size_t i = 0; i < bytes; ++i) {
    out.push_back(DecodeSample(s, data[i] & 0x0F));
    out.push_back(DecodeSample(s, (data[i] >> 4) & 0x0F));
  }
}

}  // namespace ima_adpcm
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_IMA_ADPCM_H_
