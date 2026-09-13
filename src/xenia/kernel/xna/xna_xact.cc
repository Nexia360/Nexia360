/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_xact.h"

#include <cstdio>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/kernel/xna/xna_launcher.h"

namespace xe {
namespace kernel {
namespace xna {

namespace {

uint16_t Read16(const std::vector<uint8_t>& d, size_t at) {
  if (at + 2 > d.size()) {
    return 0;
  }
  return static_cast<uint16_t>((d[at] << 8) | d[at + 1]);
}

uint32_t Read32(const std::vector<uint8_t>& d, size_t at) {
  if (at + 4 > d.size()) {
    return 0;
  }
  return (static_cast<uint32_t>(d[at]) << 24) |
         (static_cast<uint32_t>(d[at + 1]) << 16) |
         (static_cast<uint32_t>(d[at + 2]) << 8) |
         static_cast<uint32_t>(d[at + 3]);
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
  if (XnaReadTitleFile(path, out)) {
    return true;
  }
  XELOGW("[xna] XACT: no \"{}\" in the package", path);
  return false;
}

std::string ReadFixedName(const std::vector<uint8_t>& d, size_t at,
                          size_t max) {
  std::string name;
  for (size_t i = 0; i < max && at + i < d.size(); ++i) {
    const char c = static_cast<char>(d[at + i]);
    if (!c) {
      break;
    }
    name.push_back(c);
  }
  return name;
}

// WAVEBANKHEADER, then five segments: BANKDATA, ENTRYMETADATA, SEEKTABLES,
// ENTRYNAMES, ENTRYWAVEDATA.
constexpr uint32_t kWaveBankSegments = 5;
constexpr uint32_t kSegmentBankData = 0;
constexpr uint32_t kSegmentEntryMetadata = 1;
constexpr uint32_t kSegmentEntryWaveData = 4;

}  // namespace

bool XactWaveBank::Load(const std::string& path) {
  if (!ReadFile(path, &file_)) {
    XELOGE("[xna] XACT could not read the wave bank {}", path);
    return false;
  }
  // 'WBND' byte-swapped, which is what a 360 build carries.
  if (file_.size() < 52 || std::memcmp(file_.data(), "DNBW", 4) != 0) {
    XELOGE("[xna] {} is not an Xbox 360 wave bank", path);
    return false;
  }
  const uint32_t version = Read32(file_, 4);

  uint32_t offsets[kWaveBankSegments] = {};
  uint32_t lengths[kWaveBankSegments] = {};
  for (uint32_t i = 0; i < kWaveBankSegments; ++i) {
    offsets[i] = Read32(file_, 12 + i * 8);
    lengths[i] = Read32(file_, 12 + i * 8 + 4);
  }

  const uint32_t data = offsets[kSegmentBankData];
  const uint32_t entry_count = Read32(file_, data + 4);
  name_ = ReadFixedName(file_, data + 8, 64);
  const uint32_t metadata_size = Read32(file_, data + 72);

  wave_data_offset_ = offsets[kSegmentEntryWaveData];
  wave_data_length_ = lengths[kSegmentEntryWaveData];
  if (wave_data_offset_ + wave_data_length_ > file_.size()) {
    XELOGE("[xna] wave bank {} says its samples run past the end of the file",
           path);
    return false;
  }

  const uint32_t metadata = offsets[kSegmentEntryMetadata];
  entries_.reserve(entry_count);
  for (uint32_t i = 0; i < entry_count; ++i) {
    const size_t at = metadata + static_cast<size_t>(i) * metadata_size;
    if (at + 16 > file_.size()) {
      break;
    }
    const uint32_t format = Read32(file_, at + 4);
    XactWaveEntry entry;
    entry.format_tag = format & 0x3;
    entry.channels = (format >> 2) & 0x7;
    entry.sample_rate = (format >> 5) & 0x3FFFF;
    entry.block_align = (format >> 23) & 0xFF;
    entry.bits_per_sample = (format >> 31) & 0x1;
    entry.play_offset = Read32(file_, at + 8);
    entry.play_length = Read32(file_, at + 12);
    entries_.push_back(entry);
  }

  uint32_t xma_entries = 0;
  for (const auto& entry : entries_) {
    xma_entries += entry.format_tag == 1 ? 1 : 0;
  }
  XELOGI(
      "[xna] XACT wave bank \"{}\" (version {}): {} entries ({} XMA), {} "
      "byte(s) of samples",
      name_, version, entries_.size(), xma_entries, wave_data_length_);
  return !entries_.empty();
}

const uint8_t* XactWaveBank::EntryData(uint32_t index,
                                       uint32_t* size_out) const {
  const XactWaveEntry* found = entry(index);
  if (!found) {
    return nullptr;
  }
  const uint64_t at =
      static_cast<uint64_t>(wave_data_offset_) + found->play_offset;
  if (at + found->play_length > file_.size()) {
    return nullptr;
  }
  if (size_out) {
    *size_out = found->play_length;
  }
  return file_.data() + at;
}

bool XactGlobalSettings::Load(const std::string& path) {
  std::vector<uint8_t> file;
  if (!ReadFile(path, &file)) {
    XELOGE("[xna] XACT could not read the global settings {}", path);
    return false;
  }
  // 'XGSF' byte-swapped, the same way the other two banks are stored.
  if (file.size() < 0x50 || std::memcmp(file.data(), "FSGX", 4) != 0) {
    XELOGE("[xna] {} is not an Xbox 360 global settings file", path);
    return false;
  }

  // Header shape confirmed against the real file: the counts are unaligned
  // 16-bit fields from 0x13, and the segment offsets are unaligned 32-bit
  // fields from 0x21. Entry 6 is the category NAME blob - a run of NUL
  // terminated names in index order.
  const uint32_t count = Read16(file, 0x13);
  const uint32_t names_offset = Read32(file, 0x21 + 6 * 4);
  if (!count || names_offset >= file.size()) {
    return false;
  }

  size_t at = names_offset;
  while (categories_.size() < count && at < file.size()) {
    size_t stop = at;
    while (stop < file.size() && file[stop]) {
      ++stop;
    }
    categories_.emplace_back(reinterpret_cast<const char*>(file.data() + at),
                             stop - at);
    at = stop + 1;
  }

  std::string listed;
  for (size_t i = 0; i < categories_.size(); ++i) {
    listed += (i ? ", " : "") + std::to_string(i) + " " + categories_[i];
  }
  XELOGI("[xna] XACT categories: {}", listed);
  return !categories_.empty();
}

uint32_t XactGlobalSettings::FindCategory(const std::string& name) const {
  for (size_t i = 0; i < categories_.size(); ++i) {
    if (categories_[i] == name) {
      return static_cast<uint32_t>(i);
    }
  }
  return UINT32_MAX;
}

bool XactSoundBank::Load(const std::string& path) {
  if (!ReadFile(path, &file_)) {
    XELOGE("[xna] XACT could not read the sound bank {}", path);
    return false;
  }
  // 'SDBK' byte-swapped.
  if (file_.size() < 0x50 || std::memcmp(file_.data(), "KBDS", 4) != 0) {
    XELOGE("[xna] {} is not an Xbox 360 sound bank", path);
    return false;
  }

  const uint32_t simple_cues = Read16(file_, 0x13);
  const uint32_t complex_cues = Read16(file_, 0x15);
  const uint32_t total_cues = Read16(file_, 0x19);
  const uint32_t names_length = Read32(file_, 0x1E);
  const uint32_t simple_offset = Read32(file_, 0x22);
  const uint32_t names_offset = Read32(file_, 0x2A);

  constexpr uint32_t kWaveBankCountOffset = 0x1B;
  constexpr uint32_t kWaveBankNamesOffset = 0x3A;
  constexpr uint32_t kWaveBankNameLength = 64;
  const uint32_t wave_bank_count = file_[kWaveBankCountOffset];
  const uint32_t wave_bank_names = Read32(file_, kWaveBankNamesOffset);
  for (uint32_t i = 0; i < wave_bank_count; ++i) {
    const size_t at =
        wave_bank_names + static_cast<size_t>(i) * kWaveBankNameLength;
    if (!wave_bank_names || at + kWaveBankNameLength > file_.size()) {
      break;
    }
    wave_bank_names_.push_back(ReadFixedName(file_, at, kWaveBankNameLength));
  }

  // The name blob is in cue order - the 6-byte lookup table before it ends
  // exactly where the blob begins, one entry per cue - so the names can be
  // walked straight through without reproducing XACT's name hash.
  if (names_offset && names_length &&
      names_offset + names_length <= file_.size()) {
    size_t at = names_offset;
    const size_t end = names_offset + names_length;
    while (at < end && cues_.size() < total_cues) {
      size_t stop = at;
      while (stop < end && file_[stop]) {
        ++stop;
      }
      XactCue cue;
      cue.name.assign(reinterpret_cast<const char*>(file_.data() + at),
                      stop - at);
      if (!cue.name.empty()) {
        by_name_[cue.name] = static_cast<uint32_t>(cues_.size());
        cues_.push_back(std::move(cue));
      }
      at = stop + 1;
    }
  }

  // A simple cue is a flags byte and the offset of its sound. The wave a sound
  // plays lives at +9 (entry, 16-bit big-endian) and +11 (bank) of the record
  // that carries it - but WHICH record depends on the sound's own flags byte.
  //
  // A simple sound (flags bit 0 clear) holds the wave inline, so that record is
  // the sound itself. A COMPLEX sound (flags bit 0 set) holds a track instead,
  // whose 32-bit code at +11 is the absolute offset of the event that plays the
  // wave - and the same two fields sit there. Reading the sound's own +10/+11
  // for a complex sound reads two bytes of the track header: that is what gave
  // MUSIC_Insert Coin "entry 180 in Wave Bank" (it is really entry 1 in the
  // Music Wave Bank) so the menu music never played while the flags-0 cues did.
  constexpr uint32_t kSimpleCueStride = 5;
  constexpr uint32_t kSoundCategoryOffset = 1;
  constexpr uint8_t kSoundFlagComplex = 0x01;
  constexpr uint32_t kSoundTrackCodeOffset = 11;
  constexpr uint32_t kWaveEntryOffset = 9;
  constexpr uint32_t kWaveBankOffset = 11;
  const auto resolve_sound = [&](uint32_t sound, XactWaveRef* out) {
    if (!sound || sound + 12 > file_.size()) {
      return false;
    }
    // The category is a big-endian 16-bit field right after the flags byte.
    // Checked against the real bank before it was written: across all 106 cues
    // it only ever reads 1, 2 or 4, which are exactly Default, Music and
    // CustomMachines in the settings file's table.
    out->category = Read16(file_, sound + kSoundCategoryOffset);
    uint32_t wave_record = sound;
    if (file_[sound] & kSoundFlagComplex) {
      wave_record = Read32(file_, sound + kSoundTrackCodeOffset);
    }
    if (!wave_record || wave_record + 12 > file_.size()) {
      return false;
    }
    out->wave_entry = Read16(file_, wave_record + kWaveEntryOffset);
    out->wave_bank = file_[wave_record + kWaveBankOffset];
    return true;
  };
  const auto finish = [](XactCue& cue) {
    if (cue.variations.empty()) {
      return false;
    }
    cue.resolved = true;
    cue.wave_bank = cue.variations[0].wave_bank;
    cue.wave_entry = cue.variations[0].wave_entry;
    cue.category = cue.variations[0].category;
    return true;
  };

  uint32_t resolved = 0;
  for (uint32_t i = 0; i < simple_cues && i < cues_.size(); ++i) {
    const size_t at = simple_offset + static_cast<size_t>(i) * kSimpleCueStride;
    if (at + kSimpleCueStride > file_.size()) {
      break;
    }
    XactWaveRef ref;
    if (resolve_sound(Read32(file_, at + 1), &ref)) {
      cues_[i].variations.push_back(ref);
      resolved += finish(cues_[i]) ? 1 : 0;
    }
  }

  constexpr uint32_t kComplexCueOffset = 0x26;
  constexpr uint32_t kComplexCueStride = 15;
  constexpr uint8_t kComplexCueSingleSound = 0x04;
  constexpr uint16_t kDefaultCategory = 1;
  const uint32_t complex_offset = Read32(file_, kComplexCueOffset);
  for (uint32_t i = 0; i < complex_cues && simple_cues + i < cues_.size();
       ++i) {
    const size_t at =
        complex_offset + static_cast<size_t>(i) * kComplexCueStride;
    if (!complex_offset || at + kComplexCueStride > file_.size()) {
      break;
    }
    XactCue& cue = cues_[simple_cues + i];
    const uint32_t target = Read32(file_, at + 1);
    if (file_[at] & kComplexCueSingleSound) {
      XactWaveRef ref;
      if (resolve_sound(target, &ref)) {
        cue.variations.push_back(ref);
      }
    } else if (target && target + 8 <= file_.size()) {
      const uint32_t flags = Read16(file_, target);
      const uint32_t count = Read16(file_, target + 2);
      const uint32_t type = (flags >> 3) & 0x7;
      cue.variation_mode = flags & 0x7;
      size_t entry = size_t(target) + 8;
      for (uint32_t j = 0; j < count; ++j) {
        XactWaveRef ref;
        ref.category = kDefaultCategory;
        bool ok = false;
        if (type == 0 && entry + 5 <= file_.size()) {
          ref.wave_entry = Read16(file_, entry);
          ref.wave_bank = file_[entry + 2];
          ok = true;
          entry += 5;
        } else if (type == 1 && entry + 6 <= file_.size()) {
          ok = resolve_sound(Read32(file_, entry), &ref);
          entry += 6;
        } else if (type == 3 && entry + 16 <= file_.size()) {
          ok = resolve_sound(Read32(file_, entry), &ref);
          entry += 16;
        } else if (type == 4 && entry + 4 <= file_.size()) {
          const uint32_t wave = Read32(file_, entry);
          ref.wave_entry = wave & 0xFFFF;
          ref.wave_bank = (wave >> 16) & 0xFF;
          ok = true;
          entry += 4;
        } else {
          XELOGW("[xna] XACT cue \"{}\": variation type {} is not read",
                 cue.name, type);
          break;
        }
        if (ok) {
          cue.variations.push_back(ref);
        }
      }
    }
    resolved += finish(cue) ? 1 : 0;
  }

  name_ = path;
  XELOGI(
      "[xna] XACT sound bank: {} cue(s) ({} simple, {} complex), {} resolved "
      "to a wave",
      cues_.size(), simple_cues, complex_cues, resolved);
  return !cues_.empty();
}

uint32_t XactSoundBank::FindCue(const std::string& name) const {
  auto found = by_name_.find(name);
  return found != by_name_.end() ? found->second : UINT32_MAX;
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
