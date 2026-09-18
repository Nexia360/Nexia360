/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_EDITOR_AVATAR_EDITOR_H_
#define XENIA_KERNEL_XAM_AVATAR_EDITOR_AVATAR_EDITOR_H_

#include <stddef.h>
#include <stdint.h>

#include "xenia/base/byte_order.h"
#include "xenia/memory.h"

// AvatarEditor.xex (title 0x584D07D1), hand translated into Nexia as a XAM
// extension. The code is ours; the data is the console's, in guest memory at
// the addresses and in the layouts the image used.
//
// This header holds only what every section shares. Each section owns its own
// header and sources: navigation, scenes, the component collection, tiles, and
// the application.

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

constexpr uint32_t kNavigationAddress = 0x92328AB4;
constexpr uint32_t kSceneRegistryAddress = 0x92331BAC;
constexpr uint32_t kComponentCollectionAddress = 0x92331D3C;
constexpr uint32_t kApplicationAddress = 0x92327340;

// The avatar manifest, in XAM's shape - the same buffer
// XamAvatarGetMetadataForAssets fills.
constexpr uint32_t kManifestBytes = 0x3E8;
constexpr uint32_t kManifestComponentsOffset = 0x160;
constexpr uint32_t kManifestComponentCount = 13;
constexpr uint32_t kManifestDefaultsOffset = 0x300;
constexpr uint32_t kManifestEntryBytes = 0x20;
constexpr uint32_t kManifestTypeMaskOffset = 0x10;
constexpr uint32_t kAssetIdBytes = 16;

// A big-endian view of the console's memory. Data only, by design: there is no
// way to call guest code through this.
class Guest {
 public:
  explicit Guest(Memory* memory) : memory_(memory) {}

  template <typename T>
  T* At(uint32_t address) const {
    return address ? memory_->TranslateVirtual<T*>(address) : nullptr;
  }

  uint8_t Load8(uint32_t address) const {
    return *memory_->TranslateVirtual(address);
  }
  void Store8(uint32_t address, uint8_t value) const {
    *memory_->TranslateVirtual(address) = value;
  }
  uint16_t Load16(uint32_t address) const {
    return xe::load_and_swap<uint16_t>(memory_->TranslateVirtual(address));
  }
  void Store16(uint32_t address, uint16_t value) const {
    xe::store_and_swap<uint16_t>(memory_->TranslateVirtual(address), value);
  }
  uint32_t Load32(uint32_t address) const {
    return xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(address));
  }
  void Store32(uint32_t address, uint32_t value) const {
    xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(address), value);
  }

  Memory* memory() const { return memory_; }

 private:
  Memory* memory_;
};

// A piece of the editor with a name and a call site here but no body yet. The
// action comes first, the original's address second.
void NotTranslated(const char* action, const char* origin);

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_EDITOR_AVATAR_EDITOR_H_
