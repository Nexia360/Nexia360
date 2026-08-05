/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "xenia/apu/sdl/voice_chat.h"
#include "xenia/base/byte_order.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

constexpr uint32_t kVoiceObjectSize = 0x2000;

static void ResetVoiceCodecs();

dword_result_t XamVoiceIsActiveProcess_entry() { return 1; }
DECLARE_XAM_EXPORT1(XamVoiceIsActiveProcess, kNone, kImplemented);

dword_result_t XamVoiceCreate_entry(dword_t user_index,
                                    dword_t max_attached_packets,
                                    lpdword_t out_voice_ptr, dword_t a4,
                                    dword_t a5, dword_t a6) {
  if (!out_voice_ptr) {
    return X_E_INVALIDARG;
  }
  uint32_t voice_ptr =
      kernel_state()->memory()->SystemHeapAlloc(kVoiceObjectSize);
  if (!voice_ptr) {
    *out_voice_ptr = 0;
    return X_E_INVALIDARG;
  }
  std::memset(kernel_state()->memory()->TranslateVirtual<uint8_t*>(voice_ptr),
              0, kVoiceObjectSize);
  *out_voice_ptr = voice_ptr;
  ResetVoiceCodecs();
  apu::sdl::VoiceChat::Get().AddRef();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVoiceCreate, kNone, kImplemented);

dword_result_t XamVoiceClose_entry(lpunknown_t voice_ptr) {
  if (voice_ptr) {
    kernel_state()->memory()->SystemHeapFree(voice_ptr.guest_address());
    apu::sdl::VoiceChat::Get().Release();
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVoiceClose, kNone, kImplemented);

dword_result_t XamVoiceHeadsetPresent_entry(lpunknown_t voice_ptr) {
  return apu::sdl::VoiceChat::Get().enabled() ? 1 : 0;
}
DECLARE_XAM_EXPORT1(XamVoiceHeadsetPresent, kNone, kImplemented);

class G726Codec {
 public:
  G726Codec() { Reset(); }

  void Reset() {
    yl_ = 0;
    yu_ = 0;
    dms_ = 0;
    dml_ = 0;
    ap_ = 0;
    td_ = 0;
    for (int i = 0; i < 2; ++i) {
      a_[i] = 0;
      pk_[i] = 0;
      sr_[i] = 0;
    }
    for (int i = 0; i < 6; ++i) {
      b_[i] = 0;
      dq_[i] = 0;
    }
  }

  void Decode(const uint8_t* in, size_t byte_count, int16_t* out) {
    for (size_t i = 0; i < byte_count; ++i) {
      out[i * 2 + 0] = DecodeCode(in[i] & 0x0F);
      out[i * 2 + 1] = DecodeCode((in[i] >> 4) & 0x0F);
    }
  }

  void Encode(const int16_t* pcm, size_t byte_count, uint8_t* out) {
    for (size_t i = 0; i < byte_count; ++i) {
      int lo = EncodeSample(pcm[i * 2 + 0]);
      int hi = EncodeSample(pcm[i * 2 + 1]);
      out[i] = static_cast<uint8_t>((lo & 0x0F) | ((hi & 0x0F) << 4));
    }
  }

 private:
  int EncodeSample(int sl) {
    if (sl > 7880) {
      sl = 7880;
    } else if (sl < -7880) {
      sl = -7880;
    }
    int16_t sezi = static_cast<int16_t>(PredictorZero());
    int16_t sez = static_cast<int16_t>(sezi >> 1);
    int16_t sei = static_cast<int16_t>(sezi + PredictorPole());
    int16_t se = static_cast<int16_t>(sei >> 1);
    int16_t d = static_cast<int16_t>(sl - se);
    int16_t y = static_cast<int16_t>(StepSize());
    int i = Quantize(d, y);
    int16_t mag = static_cast<int16_t>(Reconstruct(kDqlnTab[i], y));
    bool neg = (i & 8) != 0;
    int16_t sr = static_cast<int16_t>(neg ? (se - mag) : (se + mag));
    int16_t dqsez = static_cast<int16_t>(neg ? (sez - mag) : (sez + mag));
    Update(y, kWiTab[i] << 5, kFiTab[i], mag, neg, sr, dqsez);
    return i;
  }
  static int Quantize(int d, int y) {
    int dqm = std::abs(d);
    int ex = Quan(dqm >> 1, kPower2, 15);
    int mant = ((dqm << 7) >> ex) & 0x7F;
    int dl = (ex << 7) + mant;
    int dln = dl - (y >> 2);
    int i = (dl == 0) ? 0 : Quan(dln, kQTab, 7);
    if (d < 0) {
      return 15 - i;
    } else if (i == 0) {
      return 15;
    } else {
      return i;
    }
  }
  static const int16_t kQTab[7];
  static int Quan(int val, const int16_t* table, int size) {
    int i = 0;
    for (; i < size; ++i) {
      if (val < table[i]) {
        break;
      }
    }
    return i;
  }

  static int Fmult(int an, int srn) {
    int16_t am = static_cast<int16_t>((an > 0) ? an : ((-an) & 0x1FFF));
    int16_t ae = static_cast<int16_t>(Quan(am, kPower2, 15) - 6);
    int16_t amt = static_cast<int16_t>(
        (am == 0) ? 32 : ((ae >= 0) ? (am >> ae) : (am << -ae)));
    int16_t we = static_cast<int16_t>(ae + ((srn >> 6) & 0xF) - 13);
    int16_t wm = static_cast<int16_t>((amt * (srn & 0x3F) + 0x30) >> 4);
    int16_t r =
        static_cast<int16_t>((we >= 0) ? ((wm << we) & 0x7FFF) : (wm >> -we));
    return ((an ^ srn) < 0) ? -r : r;
  }

  int PredictorZero() const {
    int z = Fmult(b_[0] >> 2, dq_[0]);
    for (int i = 1; i < 6; ++i) {
      z += Fmult(b_[i] >> 2, dq_[i]);
    }
    return z;
  }

  int PredictorPole() const {
    return Fmult(a_[1] >> 2, sr_[1]) + Fmult(a_[0] >> 2, sr_[0]);
  }

  int StepSize() const {
    if (ap_ >= 256) {
      return yu_;
    }
    int y = static_cast<int>(yl_ >> 6);
    int dif = yu_ - y;
    int al = ap_ >> 2;
    if (dif > 0) {
      y += (dif * al) >> 6;
    } else if (dif < 0) {
      y += (dif * al + 0x3F) >> 6;
    }
    return y;
  }

  static int Reconstruct(int dqln, int y) {
    int dql = (uint16_t)dqln + (y >> 2);
    int dqt = 128 + (dql & 0x7F);
    int dex = ((dql & 0x780) >> 7) - 7;
    int mag = (dex >= 0) ? (int16_t)(dqt << dex) : (dqt >> -dex);
    if ((int16_t)dql < 0) {
      mag = 0;
    }
    return (int16_t)mag;
  }

  int16_t DecodeCode(int code) {
    int16_t sezi = static_cast<int16_t>(PredictorZero());
    int16_t sez = static_cast<int16_t>(sezi >> 1);
    int16_t sei = static_cast<int16_t>(sezi + PredictorPole());
    int16_t se = static_cast<int16_t>(sei >> 1);
    int16_t y = static_cast<int16_t>(StepSize());
    int16_t mag = static_cast<int16_t>(Reconstruct(kDqlnTab[code], y));
    bool neg = (code & 8) != 0;
    int16_t sr = static_cast<int16_t>(neg ? (se - mag) : (se + mag));
    int16_t dqsez = static_cast<int16_t>(neg ? (sez - mag) : (sez + mag));
    Update(y, kWiTab[code] << 5, kFiTab[code], mag, neg, sr, dqsez);
    int o = sr;
    if (o > 8191) {
      o = 8191;
    } else if (o < -8192) {
      o = -8192;
    }
    return static_cast<int16_t>(static_cast<int16_t>(o) << 2);
  }

  void Update(int y, int wi, int fi, int mag_in, bool neg, int sr, int dqsez) {
    int16_t mag = static_cast<int16_t>(mag_in);
    const int off = neg ? -0x400 : 0;
    int16_t pk0 = static_cast<int16_t>((dqsez < 0) ? 1 : 0);
    int16_t yi = static_cast<int16_t>(yl_ >> 15);
    int16_t yf = static_cast<int16_t>((yl_ >> 10) & 0x1F);
    int16_t thr1 = static_cast<int16_t>((32 + yf) << yi);
    int16_t thr2 = static_cast<int16_t>((yi > 9) ? (31 << 10) : thr1);
    int16_t dqthr = static_cast<int16_t>((thr2 + (thr2 >> 1)) >> 1);
    int tr = (!td_ || mag <= dqthr) ? 0 : 1;

    yu_ = static_cast<int16_t>(y + ((wi - y) >> 5));
    if (yu_ < 544) {
      yu_ = 544;
    } else if (yu_ > 5120) {
      yu_ = 5120;
    }
    yl_ += yu_ + ((-yl_) >> 6);

    int16_t a2p = 0;
    if (tr == 1) {
      a_[0] = 0;
      a_[1] = 0;
      for (int i = 0; i < 6; ++i) {
        b_[i] = 0;
      }
    } else {
      int16_t pks1 = static_cast<int16_t>(pk0 ^ pk_[0]);
      a2p = static_cast<int16_t>(a_[1] - (a_[1] >> 7));
      if (dqsez != 0) {
        int16_t fa1 = static_cast<int16_t>(pks1 ? a_[0] : -a_[0]);
        if (fa1 < -8191) {
          a2p = static_cast<int16_t>(a2p - 0x100);
        } else if (fa1 > 8191) {
          a2p = static_cast<int16_t>(a2p + 0xFF);
        } else {
          a2p = static_cast<int16_t>(a2p + (fa1 >> 5));
        }
        if (pk0 ^ pk_[1]) {
          if (a2p <= -12160) {
            a2p = -12288;
          } else if (a2p >= 12416) {
            a2p = 12288;
          } else {
            a2p = static_cast<int16_t>(a2p - 0x80);
          }
        } else {
          if (a2p <= -12416) {
            a2p = -12288;
          } else if (a2p >= 12160) {
            a2p = 12288;
          } else {
            a2p = static_cast<int16_t>(a2p + 0x80);
          }
        }
      }
      a_[1] = a2p;
      a_[0] = static_cast<int16_t>(a_[0] - (a_[0] >> 8));
      if (dqsez != 0) {
        a_[0] = static_cast<int16_t>(a_[0] + (pks1 == 0 ? 192 : -192));
      }
      int16_t a1ul = static_cast<int16_t>(15360 - a2p);
      if (a_[0] < -a1ul) {
        a_[0] = static_cast<int16_t>(-a1ul);
      } else if (a_[0] > a1ul) {
        a_[0] = a1ul;
      }
      for (int i = 0; i < 6; ++i) {
        b_[i] = static_cast<int16_t>(b_[i] - (b_[i] >> 8));
        if (mag) {
          b_[i] = static_cast<int16_t>(
              b_[i] + ((static_cast<int16_t>(dq_[i] ^ off) >= 0) ? 128 : -128));
        }
      }
    }

    for (int i = 5; i > 0; --i) {
      dq_[i] = dq_[i - 1];
    }
    if (mag == 0) {
      dq_[0] = static_cast<int16_t>(0x60 + off);
    } else {
      int16_t exp = static_cast<int16_t>(Quan(mag, kPower2, 15));
      dq_[0] =
          static_cast<int16_t>((exp << 6) + (((mag << 6) >> exp) | 0x20) + off);
    }
    sr_[1] = sr_[0];
    if (sr == 0) {
      sr_[0] = 0x60;
    } else if (sr > 0) {
      int16_t exp = static_cast<int16_t>(Quan(sr, kPower2, 15));
      sr_[0] = static_cast<int16_t>((exp << 6) + ((sr << 6) >> exp));
    } else if (sr > -32768) {
      int16_t smag = static_cast<int16_t>(-sr);
      int16_t exp = static_cast<int16_t>(Quan(smag, kPower2, 15));
      sr_[0] = static_cast<int16_t>((exp << 6) + ((smag << 6) >> exp) - 0x400);
    } else {
      sr_[0] = static_cast<int16_t>(0xFC60);
    }
    pk_[1] = pk_[0];
    pk_[0] = pk0;
    td_ = (tr == 1) ? 0 : (a2p < -11776 ? 1 : 0);

    dms_ = static_cast<int16_t>(dms_ + (((fi << 9) - dms_) >> 5));
    dml_ = static_cast<int16_t>(dml_ + (((fi << 11) - dml_) >> 7));
    if (tr == 1) {
      ap_ = 256;
    } else if (y < 1536 || td_ == 1 ||
               std::abs((dms_ << 2) - dml_) >= (dml_ >> 3)) {
      ap_ = static_cast<int16_t>(ap_ + ((0x200 - ap_) >> 4));
    } else {
      ap_ = static_cast<int16_t>(ap_ + ((-ap_) >> 4));
    }
  }

  static const int16_t kDqlnTab[16];
  static const int16_t kWiTab[16];
  static const int16_t kFiTab[16];
  static const int16_t kPower2[15];

  int32_t yl_;
  int16_t yu_, dms_, dml_, ap_;
  int16_t a_[2], b_[6], pk_[2], dq_[6], sr_[2];
  int16_t td_;
};

const int16_t G726Codec::kDqlnTab[16] = {-2048, 4,   135, 213,  273, 323,
                                         373,   425, 425, 373,  323, 273,
                                         213,   135, 4,   -2048};
const int16_t G726Codec::kWiTab[16] = {-12,  18,  41,  64,  112, 198, 355, 1122,
                                       1122, 355, 198, 112, 64,  41,  18,  -12};
const int16_t G726Codec::kFiTab[16] = {0, 0, 0, 1, 1, 1, 3, 7,
                                       7, 3, 1, 1, 1, 0, 0, 0};
const int16_t G726Codec::kQTab[7] = {-124, 80, 178, 246, 300, 349, 400};
const int16_t G726Codec::kPower2[15] = {1,     2,     4,      8,      0x10,
                                        0x20,  0x40,  0x80,   0x100,  0x200,
                                        0x400, 0x800, 0x1000, 0x2000, 0x4000};

static G726Codec g_playback_codec;
static G726Codec g_capture_codec;
static std::mutex g_codec_mutex;

constexpr uint64_t kCaptureRateHz = 16000;
static std::deque<uint32_t> g_pending_capture;
static std::chrono::steady_clock::time_point g_capture_due{};

static void ResetVoiceCodecs() {
  std::lock_guard<std::mutex> lk(g_codec_mutex);
  g_playback_codec.Reset();
  g_capture_codec.Reset();
}

constexpr uint32_t kPacketStatus = 0x00;
constexpr uint32_t kPacketDone = 0x04;
constexpr uint32_t kPacketBuffer = 0x08;
constexpr uint32_t kPacketLength = 0x0C;
constexpr uint32_t kPacketDevice = 0x10;
constexpr uint32_t kPacketFormat = 0x14;

constexpr uint32_t kHeadsetDeviceClass = 0x70000000;

constexpr uint32_t kMaxPacketBytes = 0x4000;

static uint32_t ReadField(Memory* memory, uint32_t packet, uint32_t off) {
  return xe::load_and_swap<uint32_t>(
      memory->TranslateVirtual<uint8_t*>(packet + off));
}

static void WriteField(Memory* memory, uint32_t packet, uint32_t off,
                       uint32_t value) {
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(packet + off),
                               value);
}

static void PlaybackPacket(Memory* memory, uint32_t packet) {
  const uint32_t buffer = ReadField(memory, packet, kPacketBuffer);
  const uint32_t length = ReadField(memory, packet, kPacketLength);
  const uint32_t format = ReadField(memory, packet, kPacketFormat);
  if (!buffer || !length || length > kMaxPacketBytes) {
    return;
  }
  const auto* in = memory->TranslateVirtual<uint8_t*>(buffer);
  std::vector<int16_t> pcm;
  if (format == 1) {
    pcm.resize(length * 2);
    std::lock_guard<std::mutex> lk(g_codec_mutex);
    g_playback_codec.Decode(in, length, pcm.data());
  } else {
    pcm.resize(length / 2);
    for (size_t i = 0; i < pcm.size(); ++i) {
      pcm[i] = static_cast<int16_t>((in[i * 2] << 8) | in[i * 2 + 1]);
    }
  }
  std::vector<int16_t> up(pcm.size() * 2);
  for (size_t i = 0; i < pcm.size(); ++i) {
    const int16_t cur = pcm[i];
    const int16_t nxt = (i + 1 < pcm.size()) ? pcm[i + 1] : cur;
    up[i * 2 + 0] = cur;
    up[i * 2 + 1] = static_cast<int16_t>((cur + nxt) / 2);
  }
  apu::sdl::VoiceChat::Get().PlayPcm(up.data(), up.size());
}

static void CapturePacket(Memory* memory, uint32_t packet) {
  const uint32_t format = ReadField(memory, packet, kPacketFormat);
  uint32_t length = ReadField(memory, packet, kPacketLength);
  const uint32_t buffer = ReadField(memory, packet, kPacketBuffer);
  if (!buffer || !length || length > kMaxPacketBytes) {
    return;
  }
  auto* out = memory->TranslateVirtual<uint8_t*>(buffer);

  if (format == 1) {
    const size_t samples = static_cast<size_t>(length) * 2;
    std::vector<int16_t> pcm(samples, 0);
    apu::sdl::VoiceChat::Get().ReadCapturePcm(pcm.data(), samples);
    for (size_t i = 0; i < samples; ++i) {
      pcm[i] = static_cast<int16_t>(pcm[i] >> 2);
    }
    std::lock_guard<std::mutex> lk(g_codec_mutex);
    g_capture_codec.Encode(pcm.data(), length, out);
    WriteField(memory, packet, kPacketDevice, kHeadsetDeviceClass);
    WriteField(memory, packet, kPacketDone, length);
    return;
  }

  if (ReadField(memory, packet, kPacketDone) == 0 && format == 2) {
    WriteField(memory, packet, kPacketFormat, 0);
    WriteField(memory, packet, kPacketDevice, kHeadsetDeviceClass);
    length <<= 2;
    WriteField(memory, packet, kPacketLength, length);
    if (length > kMaxPacketBytes) {
      return;
    }
  }
  const size_t samples = length / 2;
  std::vector<int16_t> pcm(samples, 0);
  apu::sdl::VoiceChat::Get().ReadCapturePcm(pcm.data(), samples);
  for (size_t i = 0; i < samples; ++i) {
    out[i * 2 + 0] = static_cast<uint8_t>(pcm[i] >> 8);
    out[i * 2 + 1] = static_cast<uint8_t>(pcm[i] & 0xFF);
  }
  WriteField(memory, packet, kPacketDone, length);
}

static void ServicePendingCapture(Memory* memory) {
  const auto now = std::chrono::steady_clock::now();
  while (!g_pending_capture.empty()) {
    if (g_pending_capture.size() < 64 &&
        g_capture_due != std::chrono::steady_clock::time_point{} &&
        now < g_capture_due) {
      break;
    }
    const uint32_t packet = g_pending_capture.front();
    const uint32_t len = ReadField(memory, packet, kPacketLength);
    const uint32_t fmt = ReadField(memory, packet, kPacketFormat);
    const uint32_t frame_samples = (fmt == 1) ? len * 2 : len / 2;
    g_pending_capture.pop_front();
    CapturePacket(memory, packet);
    WriteField(memory, packet, kPacketStatus, X_ERROR_SUCCESS);
    const auto span = std::chrono::microseconds(
        frame_samples ? (frame_samples * 1000000ull / kCaptureRateHz) : 0);
    const auto base =
        (g_capture_due == std::chrono::steady_clock::time_point{} ||
         now - g_capture_due > std::chrono::milliseconds(200))
            ? now
            : g_capture_due;
    g_capture_due = base + span;
  }
}

dword_result_t XamVoiceSubmitPacket_entry(lpunknown_t voice_ptr,
                                          dword_t packet_count,
                                          lpdword_t buffer_ptr) {
  const uint32_t packet = buffer_ptr.guest_address();
  if (!packet) {
    return X_E_INVALIDARG;
  }
  auto* memory = kernel_state()->memory();
  ServicePendingCapture(memory);

  if (uint32_t(packet_count) == 0) {
    PlaybackPacket(memory, packet);
    WriteField(memory, packet, kPacketDone,
               ReadField(memory, packet, kPacketLength));
    WriteField(memory, packet, kPacketStatus, X_ERROR_SUCCESS);
  } else {
    WriteField(memory, packet, kPacketStatus, 0x103);
    g_pending_capture.push_back(packet);
    ServicePendingCapture(memory);
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVoiceSubmitPacket, kNone, kImplemented);

dword_result_t XamVoiceGetMicArrayStatus_entry() { return 0; }
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayStatus, kNone, kStub);

dword_result_t XamVoiceGetBatteryStatus_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceGetBatteryStatus, kNone, kStub);

dword_result_t XamVoiceSetAudioCaptureRoutine_entry(lpunknown_t voice_ptr,
                                                    lpdword_t desc_ptr) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVoiceSetAudioCaptureRoutine, kNone, kStub);

dword_result_t XamVoiceGetDirectionalData_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceGetDirectionalData, kNone, kStub);

dword_result_t XamVoiceSetMicArrayIdleUsers_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceSetMicArrayIdleUsers, kNone, kStub);

dword_result_t XamVoiceMuteMicArray_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceMuteMicArray, kNone, kStub);

dword_result_t XamVoiceGetMicArrayUnderrunStatus_entry() {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayUnderrunStatus, kNone, kStub);

dword_result_t XamVoiceDisableMicArray_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceDisableMicArray, kNone, kStub);

dword_result_t XamVoiceSetMicArrayBeamAngle_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceSetMicArrayBeamAngle, kNone, kStub);

dword_result_t XamVoiceRecordUserPrivileges_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceRecordUserPrivileges, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Voice);
