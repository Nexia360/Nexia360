/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/xbadpcm.h"

#include <cstdlib>

namespace xe {
namespace apu {
namespace xbadpcm {

// ITU-T / Sun G.726 (G.721) 32 kbps tables (byte-exact from g726adpcm.obj).
static const int16_t kQtab[7] = {-124, 80, 178, 246, 300, 349, 400};
static const int16_t kDqln[16] = {-2048, 4,   135, 213, 273, 323, 373, 425,
                                  425,   373, 323, 273, 213, 135, 4,   -2048};
static const int16_t kWi[16] = {-12,  18,   41,  64,  112, 198, 355, 1122,
                                1122, 355,  198, 112, 64,  41,  18,  -12};
static const int16_t kFi[16] = {0, 0, 0, 1, 1, 1, 3, 7, 7, 3, 1, 1, 1, 0, 0, 0};
static const int16_t kPow2[15] = {1,     2,     4,     8,    0x10,
                                  0x20,  0x40,  0x80,  0x100, 0x200,
                                  0x400, 0x800, 0x1000, 0x2000, 0x4000};

static int Quan(int val, const int16_t* table, int size) {
  int i;
  for (i = 0; i < size; i++)
    if (val < *table++) break;
  return i;
}

static int Fmult(int an, int srn) {
  int16_t anmag = (an > 0) ? an : ((-an) & 0x1FFF);
  int16_t anexp = Quan(anmag, kPow2, 15) - 6;
  int16_t anmant =
      (anmag == 0) ? 32 : ((anexp >= 0) ? (anmag >> anexp) : (anmag << -anexp));
  int16_t wanexp = anexp + ((srn >> 6) & 0xF) - 13;
  int16_t wanmant = (anmant * (srn & 0x3F) + 0x30) >> 4;
  int16_t r = (wanexp >= 0) ? ((wanmant << wanexp) & 0x7FFF) : (wanmant >> -wanexp);
  return ((an ^ srn) < 0) ? -r : r;
}

static int PredictZero(const State* s) {
  int z = Fmult(s->b[0] >> 2, s->dq[0]);
  for (int i = 1; i < 6; i++) z += Fmult(s->b[i] >> 2, s->dq[i]);
  return z;
}
static int PredictPole(const State* s) {
  return Fmult(s->a[1] >> 2, s->sr[1]) + Fmult(s->a[0] >> 2, s->sr[0]);
}
static int StepSize(const State* s) {
  if (s->ap >= 256) return s->yu;
  int y = s->yl >> 6, dif = s->yu - y, al = s->ap >> 2;
  if (dif > 0)
    y += (dif * al) >> 6;
  else if (dif < 0)
    y += (dif * al + 0x3F) >> 6;
  return y;
}
static int Reconstruct(int sign, int dqln, int y) {
  int16_t dql = dqln + (y >> 2);
  if (dql < 0) return sign ? -0x8000 : 0;
  int16_t dex = (dql >> 7) & 15;
  int16_t dqt = 128 + (dql & 127);
  int16_t dq = (dqt << 7) >> (14 - dex);
  return sign ? (dq - 0x8000) : dq;
}
static int Quantize(int d, int y) {
  int16_t dqm = std::abs(d);
  int16_t exp = Quan(dqm >> 1, kPow2, 15);
  int16_t mant = ((dqm << 7) >> exp) & 0x7F;
  int16_t dl = (exp << 7) + mant;
  int16_t dln = dl - (y >> 2);
  int i = Quan(dln, kQtab, 7);
  if (d < 0)
    return 15 - i;
  else if (i == 0)
    return 15;
  return i;
}

static void Update(int y, int wi, int fi, int dq, int sr, int dqsez, State* s) {
  int16_t mag, exp, a2p = 0, a1ul, pks1, fa1;
  int8_t tr;
  int16_t ylint, thr2, dqthr, ylfrac, pk0;
  pk0 = (dqsez < 0) ? 1 : 0;
  mag = dq & 0x7FFF;
  ylint = s->yl >> 15;
  ylfrac = (s->yl >> 10) & 0x1F;
  thr2 = (ylint > 9) ? (31 << 10) : ((32 + ylfrac) << ylint);
  dqthr = (thr2 + (thr2 >> 1)) >> 1;
  if (!s->td)
    tr = 0;
  else if (mag <= dqthr)
    tr = 0;
  else
    tr = 1;

  s->yu = y + ((wi - y) >> 5);
  if (s->yu < 544) s->yu = 544;
  else if (s->yu > 5120) s->yu = 5120;
  s->yl += s->yu + ((-s->yl) >> 6);

  if (tr == 1) {
    s->a[0] = s->a[1] = 0;
    for (int c = 0; c < 6; c++) s->b[c] = 0;
  } else {
    pks1 = pk0 ^ s->pk[0];
    a2p = s->a[1] - (s->a[1] >> 7);
    if (dqsez != 0) {
      fa1 = pks1 ? s->a[0] : -s->a[0];
      if (fa1 < -8191)
        a2p -= 0x100;
      else if (fa1 > 8191)
        a2p += 0xFF;
      else
        a2p += fa1 >> 5;
      if (pk0 ^ s->pk[1]) {
        if (a2p <= -12160) a2p = -12288;
        else if (a2p >= 12416) a2p = 12288;
        else a2p -= 0x80;
      } else {
        if (a2p <= -12416) a2p = -12288;
        else if (a2p >= 12160) a2p = 12288;
        else a2p += 0x80;
      }
    }
    s->a[1] = a2p;
    s->a[0] -= s->a[0] >> 8;
    if (dqsez != 0) s->a[0] += (pks1 == 0) ? 192 : -192;
    a1ul = 15360 - a2p;
    if (s->a[0] < -a1ul) s->a[0] = -a1ul;
    else if (s->a[0] > a1ul) s->a[0] = a1ul;
    for (int c = 0; c < 6; c++) {
      s->b[c] -= s->b[c] >> 8;
      if (dq & 0x7FFF) s->b[c] += ((dq ^ s->dq[c]) >= 0) ? 128 : -128;
    }
  }
  for (int c = 5; c > 0; c--) s->dq[c] = s->dq[c - 1];
  if (mag == 0) {
    s->dq[0] = (dq >= 0) ? 0x20 : (int16_t)0xFC20;
  } else {
    exp = Quan(mag, kPow2, 15);
    s->dq[0] = (dq >= 0) ? (exp << 6) + ((mag << 6) >> exp)
                         : (exp << 6) + ((mag << 6) >> exp) - 0x400;
  }
  s->sr[1] = s->sr[0];
  if (sr == 0) {
    s->sr[0] = 0x20;
  } else if (sr > 0) {
    exp = Quan(sr, kPow2, 15);
    s->sr[0] = (exp << 6) + ((sr << 6) >> exp);
  } else if (sr > -32768) {
    mag = -sr;
    exp = Quan(mag, kPow2, 15);
    s->sr[0] = (exp << 6) + ((mag << 6) >> exp) - 0x400;
  } else {
    s->sr[0] = (int16_t)0xFC20;
  }
  s->pk[1] = s->pk[0];
  s->pk[0] = pk0;
  s->td = (tr == 1) ? 0 : (a2p < -11776 ? 1 : 0);
  s->dms += (fi - s->dms) >> 5;
  s->dml += ((fi << 2) - s->dml) >> 7;
  if (tr == 1)
    s->ap = 256;
  else if (y < 1536)
    s->ap += (0x200 - s->ap) >> 4;
  else if (s->td == 1)
    s->ap += (0x200 - s->ap) >> 4;
  else if (std::abs((s->dms << 2) - s->dml) >= (s->dml >> 3))
    s->ap += (0x200 - s->ap) >> 4;
  else
    s->ap += (-s->ap) >> 4;
}

static int DecodeCode(int i, State* s) {
  i &= 0xF;
  int sezi = PredictZero(s), sez = sezi >> 1;
  int se = (sezi + PredictPole(s)) >> 1;
  int y = StepSize(s);
  int dq = Reconstruct(i & 8, kDqln[i], y);
  int sr = (dq < 0) ? (se - (dq & 0x3FFF)) : (se + dq);
  Update(y, kWi[i] << 5, kFi[i], dq, sr, sr - se + sez, s);
  return sr;  // 14-bit reconstructed signal
}

static int EncodeSample(int sl, State* s) {
  int sezi = PredictZero(s), sez = sezi >> 1;
  int se = (sezi + PredictPole(s)) >> 1;
  int d = sl - se;
  int y = StepSize(s);
  int i = Quantize(d, y);
  int dq = Reconstruct(i & 8, kDqln[i], y);
  int sr = (dq < 0) ? (se - (dq & 0x3FFF)) : (se + dq);
  Update(y, kWi[i] << 5, kFi[i], dq, sr, sr - se + sez, s);
  return i;
}

void Reset(State* s) {
  s->yl = 34816;
  s->yu = 544;
  s->dms = s->dml = s->ap = 0;
  for (int i = 0; i < 2; i++) {
    s->a[i] = 0;
    s->pk[i] = 0;
    s->sr[i] = 32;
  }
  for (int i = 0; i < 6; i++) {
    s->b[i] = 0;
    s->dq[i] = 32;
  }
  s->td = 0;
}

int Decode(State* s, const uint8_t* in, int in_bytes, int16_t* out) {
  int n = 0;
  for (int b = 0; b < in_bytes; b++) {
    int byte = in[b];
    out[n++] = static_cast<int16_t>(DecodeCode(byte & 0xF, s) << 1);
    out[n++] = static_cast<int16_t>(DecodeCode((byte >> 4) & 0xF, s) << 1);
  }
  return n;
}

int Encode(State* s, const int16_t* in, int in_samples, uint8_t* out) {
  int nb = 0;
  for (int i = 0; i + 1 < in_samples; i += 2) {
    int lo = EncodeSample(in[i] >> 1, s) & 0xF;
    int hi = EncodeSample(in[i + 1] >> 1, s) & 0xF;
    out[nb++] = static_cast<uint8_t>(lo | (hi << 4));
  }
  return nb;
}

}  // namespace xbadpcm
}  // namespace apu
}  // namespace xe
