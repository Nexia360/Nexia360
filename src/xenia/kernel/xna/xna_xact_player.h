/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_XACT_PLAYER_H_
#define XENIA_KERNEL_XNA_XNA_XACT_PLAYER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace xe {
namespace kernel {
namespace xna {

enum : uint32_t {
  kXnaAudioUnknown = 0,
  kXnaAudioPcm = 1,
  kXnaAudioXma = 2,
  kXnaAudioWma = 3,
  kXnaAudioAdpcm = 4,
};

struct XnaAudioFormat {
  uint32_t codec = kXnaAudioUnknown;
  uint32_t channels = 0;
  uint32_t sample_rate = 0;
  uint32_t block_align = 0;
  uint32_t bits_per_sample = 0;
  uint32_t avg_bytes_per_second = 0;
  bool big_endian = false;
};

using XnaSamples = std::shared_ptr<const std::vector<float>>;

enum : uint32_t {
  kXnaVoiceCue = 1,
  kXnaVoiceSound = 2,
  kXnaVoiceMusic = 3,
};

constexpr uint64_t XnaVoiceOwner(uint32_t kind, uint32_t id) {
  return (uint64_t(kind) << 32) | id;
}

struct XnaVoiceParams {
  uint32_t category = UINT32_MAX;
  float volume = 1.0f;
  float pan = 0.0f;
  float pitch = 0.0f;
  bool loop = false;
  uint32_t loop_begin = 0;
  uint32_t loop_length = 0;
  bool paused = false;
};

enum class XnaVoiceState { kNone, kPlaying, kPaused };

XnaSamples XnaDecodeAudio(const XnaAudioFormat& format, const uint8_t* data,
                          uint32_t size, bool cache = true);
XnaSamples XnaDecodeMediaFile(const std::vector<uint8_t>& file,
                              uint32_t* sample_rate);

bool XnaVoiceStart(uint64_t owner, XnaSamples samples, uint32_t sample_rate,
                   const XnaVoiceParams& params);
void XnaVoiceStartAsync(uint64_t owner,
                        std::function<XnaSamples(uint32_t*)> decode,
                        const XnaVoiceParams& params);
bool XnaVoiceQueue(uint64_t owner, XnaSamples samples, uint32_t sample_rate,
                   const XnaVoiceParams& params);
uint32_t XnaVoicePending(uint64_t owner);
void XnaVoiceStop(uint64_t owner);
void XnaVoiceDetach(uint64_t owner);
void XnaVoiceSetPaused(uint64_t owner, bool paused);
void XnaVoiceSetVolume(uint64_t owner, float volume);
void XnaVoiceSetPan(uint64_t owner, float pan);
void XnaVoiceSetPitch(uint64_t owner, float pitch);
void XnaVoiceSetLoop(uint64_t owner, bool loop);
XnaVoiceState XnaVoiceQuery(uint64_t owner, double* seconds);
void XnaVoiceStopCategory(uint32_t category);
void XnaVoicePauseCategory(uint32_t category, bool paused);
void XnaVoiceSetCategoryVolume(uint32_t category, float volume);
void XnaAudioSetMasterVolume(float volume);

// Decodes and starts a wave, owned by `cue`. Returns false if it could not be
// decoded, which is a real failure worth reporting rather than silence the
// title thinks is sound.
bool XactPlayWave(uint32_t cue, uint32_t category, const uint8_t* data,
                  uint32_t size, const XnaAudioFormat& format, float volume);

// Whether that cue still has samples left to play.
//
// A cue has to STOP BY ITSELF when its wave runs out. Nothing pushes a
// notification back - XNA polls Cue.IsPlaying, which is one bit of GetState -
// so the state has to be answered from what the mixer is actually doing. Left
// to a flag set at Play time, a cue reads as playing forever and a title
// waiting for a sound to finish waits for good.
bool XactIsCuePlaying(uint32_t cue);

// Silences one cue immediately - what Cue.Stop means.
void XactStopCue(uint32_t cue);

// Releasing a handle is not a stop: the voice plays out but is no longer
// reachable by the cue id, so a reused id cannot inherit or silence it.
void XactDetachCue(uint32_t cue);

// Stops only the voices in that category.
void XactStopCategory(uint32_t category);

void XactStopAll();

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_XACT_PLAYER_H_
