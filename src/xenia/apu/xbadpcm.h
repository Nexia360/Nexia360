/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Xbox 360 headset codec ("XBADPCM"). Reverse-engineered from xhv2.lib's
// g726adpcm.obj (debug symbols + tables): it is ITU-T G.726 at 32 kbps (the
// Sun/G.721 reference, exact tables), 4 bits/sample, codes packed low-nibble
// first. Decode output is sr<<1 (the MS variant; the textbook LINEAR path uses
// sr<<2). Decode = g726adpcm mode 1, encode = mode 0. State persists across
// calls (the predictor adapts continuously) and is only cleared by Reset.

#ifndef XENIA_APU_XBADPCM_H_
#define XENIA_APU_XBADPCM_H_

#include <cstdint>

namespace xe {
namespace apu {
namespace xbadpcm {

// G.726 predictor/scale state. Persists across calls; one instance per stream
// direction (one decoder, one encoder).
struct State {
  int32_t yl;
  int16_t yu, dms, dml, ap;
  int16_t a[2], b[6], pk[2], dq[6], sr[2];
  int8_t td;
};

void Reset(State* s);

// Decode in_bytes of ADPCM (2 codes/byte, low nibble first) into 2*in_bytes
// 16-bit samples. Returns the sample count written.
int Decode(State* s, const uint8_t* in, int in_bytes, int16_t* out);

// Encode in_samples of 16-bit PCM into in_samples/2 bytes (2 codes/byte, low
// nibble first). Returns the byte count written.
int Encode(State* s, const int16_t* in, int in_samples, uint8_t* out);

}  // namespace xbadpcm
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_XBADPCM_H_
