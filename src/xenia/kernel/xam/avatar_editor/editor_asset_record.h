/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_ASSET_RECORD_H_
#define XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_ASSET_RECORD_H_

#include <stddef.h>
#include <stdint.h>

#include "xenia/kernel/xam/avatar_editor/avatar_editor.h"
#include "xenia/kernel/xam/avatar_editor/component_collection.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

// Two different records describe an asset, and every accessor below belongs to
// the second one:
//
//   XamAvatarEnumAssets fills the XAM enumeration record, 0x2C4 bytes, declared
//   in component_collection.h as GuestEnumeratedAsset. Its only reader is the
//   editor's enumerator.
//
//   The enumerator turns each of those into a 0x198-byte record - the
//   collection's GuestAssetRecord - and everything else in the editor, the
//   buckets, the grid and the tiles, reads only that one.
//
// The collection owns the layout. This header names the fields the tiles care
// about and leaves the storage where it is.
using EditorAssetRecord = GuestAssetRecord;

constexpr uint32_t kEditorColoursPerGroup = 3;

// The enumerator memcpy's one 0xE0-byte block over +0x38..+0x117, and the last
// two dwords of that block are the two counts rather than colours - so they
// land inside the record's colour array and are addressed as its last two
// slots.
constexpr uint32_t kEditorColourGroupCountSlot = 54;
constexpr uint32_t kEditorColourChannelCountSlot = 55;
constexpr uint32_t kEditorColourGroupCapacity =
    kEditorColourGroupCountSlot / kEditorColoursPerGroup;

constexpr uint32_t kEditorAssetRecordNameOffset = 0x118;
constexpr uint32_t kEditorAssetRecordNameLength = 0x40;

// Byte 6's low nibble classifies the asset; the enumerator deduplicates on
// kind 2 and the tile factory picks a subclass from kinds 1 and 2.
constexpr uint32_t kAssetKindByte = 6;
constexpr uint8_t kAssetKindMultiPart = 1;
constexpr uint8_t kAssetKindDeduplicated = 2;

static_assert(offsetof(EditorAssetRecord, colours) +
                      kEditorColourGroupCountSlot * sizeof(uint32_t) ==
                  0x110,
              "the colour group count is the block's penultimate dword");
static_assert(offsetof(EditorAssetRecord, colours) +
                      kEditorColourChannelCountSlot * sizeof(uint32_t) ==
                  0x114,
              "the colour channel count is the block's last dword");
static_assert(offsetof(EditorAssetRecord, display_name) ==
                  kEditorAssetRecordNameOffset,
              "");

inline int32_t EditorColourGroupCount(const EditorAssetRecord* record) {
  return record
             ? int32_t(uint32_t(record->colours[kEditorColourGroupCountSlot]))
             : 0;
}

// 1, 2 or 3: how many customisable colour channels the item exposes, and
// therefore which of the Grid1x1{One,Two,Three}ColourButton widgets draws it.
inline uint32_t EditorColourChannelCount(const EditorAssetRecord* record) {
  return record ? uint32_t(record->colours[kEditorColourChannelCountSlot]) : 0;
}

inline const xe::be<uint32_t>* EditorColourGroupAt(
    const EditorAssetRecord* record, int32_t group_index) {
  if (!record || group_index < 0 ||
      uint32_t(group_index) >= kEditorColourGroupCapacity) {
    return nullptr;
  }
  return &record->colours[uint32_t(group_index) * kEditorColoursPerGroup];
}

inline bool EditorRecordShowsNewBadge(const EditorAssetRecord* record) {
  return record && record->field_31 != 0;
}

inline bool EditorRecordIsPurchased(const EditorAssetRecord* record) {
  return record && record->field_32 != 0;
}

// Set only for a purchased asset whose compatibility byte in the XAM
// enumeration record does not list the running avatar spec. Measured: the tile
// then refuses to write the item into the manifest and reports a UI state of 2.
// The spec reading of the byte itself is inferred.
inline bool EditorRecordIsIncompatible(const EditorAssetRecord* record) {
  return record && record->field_33 != 0;
}

// The variant carries the package and description an awarded item is announced
// with, and the tile shows the award badge whenever there is one.
inline uint32_t EditorRecordAwardInfo(const EditorAssetRecord* record) {
  return record ? uint32_t(record->variant) : 0;
}

inline bool EditorRecordIsMultiPart(const EditorAssetRecord* record) {
  return record && record->field_30 != 0;
}

inline uint8_t EditorRecordAssetKind(const EditorAssetRecord* record) {
  return record ? uint8_t(record->asset_id[kAssetKindByte] & 0xF) : 0;
}

inline uint32_t EditorAssetRecordNameAddress(uint32_t record_address) {
  return record_address ? record_address + kEditorAssetRecordNameOffset : 0;
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_ASSET_RECORD_H_
