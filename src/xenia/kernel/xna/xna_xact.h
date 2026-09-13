/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_XACT_H_
#define XENIA_KERNEL_XNA_XNA_XACT_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace xe {
namespace kernel {
namespace xna {

// XACT, as the banks a title actually ships describe it.
//
// Nexia already decodes XMA and already has an audio driver - what it has never
// had is the layer above them, the one that turns PlayCue("CoinsDropping") into
// a wave. That layer is these banks, and nothing here decodes or mixes: it
// resolves a cue to a wave entry and hands the bytes on.
//
// EVERYTHING IS BIG-ENDIAN. These are Xbox 360 builds - the signatures appear
// as WBND/SDBK byte-swapped to DNBW/KBDS - so the PC layout documented for XACT
// applies field for field but not byte for byte. Every offset below was checked
// against the real files before any of it was written: the wave data segment
// ends exactly at the end of the file, and the cue name blob ends exactly at
// the end of the sound bank.

// One wave in a wave bank, as its metadata entry describes it.
struct XactWaveEntry {
  // WAVEBANKMINIWAVEFORMAT, unpacked from the low bits up: tag:2, channels:3,
  // samples/sec:18, block align:8, bits/sample:1.
  uint32_t format_tag = 0;  // 0 PCM, 1 XMA, 2 ADPCM, 3 WMA
  uint32_t channels = 0;
  uint32_t sample_rate = 0;
  uint32_t block_align = 0;
  uint32_t bits_per_sample = 0;

  uint32_t PlaybackRate() const {
    if (format_tag != 1) {
      return sample_rate;
    }
    static constexpr uint32_t kXmaRates[] = {24000, 32000, 44100, 48000};
    uint32_t best = 48000;
    uint32_t best_distance = UINT32_MAX;
    for (uint32_t rate : kXmaRates) {
      const uint32_t distance =
          rate > sample_rate ? rate - sample_rate : sample_rate - rate;
      if (distance < best_distance) {
        best = rate;
        best_distance = distance;
      }
    }
    return best;
  }
  // Where the samples are, relative to the start of the wave data segment.
  uint32_t play_offset = 0;
  uint32_t play_length = 0;
};

class XactWaveBank {
 public:
  bool Load(const std::string& path);

  const std::string& name() const { return name_; }
  size_t entry_count() const { return entries_.size(); }
  const XactWaveEntry* entry(uint32_t index) const {
    return index < entries_.size() ? &entries_[index] : nullptr;
  }
  // The samples for an entry, or null. Points into the loaded file.
  const uint8_t* EntryData(uint32_t index, uint32_t* size_out) const;

 private:
  std::string name_;
  std::vector<uint8_t> file_;
  std::vector<XactWaveEntry> entries_;
  uint32_t wave_data_offset_ = 0;
  uint32_t wave_data_length_ = 0;
};

struct XactWaveRef {
  uint32_t wave_bank = 0;
  uint32_t wave_entry = 0;
  uint16_t category = 0;
};

// What a cue resolves to.
struct XactCue {
  std::string name;
  bool resolved = false;
  uint32_t wave_bank = 0;
  uint32_t wave_entry = 0;
  // The XGS category this cue plays in, from its sound record. Stopping a
  // category has to stop only the cues that are IN it, so this has to travel
  // with the cue all the way to the mixer.
  uint16_t category = 0;
  std::vector<XactWaveRef> variations;
  uint32_t variation_mode = 0;
};

// The category table out of the global settings file (.xgs).
//
// CATEGORY IDS COME FROM HERE, NOT FROM THE ORDER THE TITLE ASKS FOR THEM.
// Handing out indices as names were first seen produced a numbering unrelated
// to the one the sound records use, so "stop Music" arrived as a number that
// meant a different category entirely - and stopping everything to be safe
// silenced every sound in the game. Arcadecraft's table is Global, Default,
// Music, AmbientLoops, CustomMachines, and its cues reference 1, 2 and 4.
class XactGlobalSettings {
 public:
  bool Load(const std::string& path);

  // The real index for a category name, or UINT32_MAX when the settings file
  // does not name it.
  uint32_t FindCategory(const std::string& name) const;
  const std::vector<std::string>& category_names() const { return categories_; }

 private:
  std::vector<std::string> categories_;
};

class XactSoundBank {
 public:
  bool Load(const std::string& path);

  const std::string& name() const { return name_; }
  size_t cue_count() const { return cues_.size(); }
  const XactCue* cue(uint32_t index) const {
    return index < cues_.size() ? &cues_[index] : nullptr;
  }
  // The cue index for a name, or UINT32_MAX.
  uint32_t FindCue(const std::string& name) const;
  const std::string& wave_bank_name(uint32_t index) const {
    static const std::string kEmpty;
    return index < wave_bank_names_.size() ? wave_bank_names_[index] : kEmpty;
  }

 private:
  std::string name_;
  std::vector<uint8_t> file_;
  std::vector<XactCue> cues_;
  std::vector<std::string> wave_bank_names_;
  std::unordered_map<std::string, uint32_t> by_name_;
};

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_XACT_H_
