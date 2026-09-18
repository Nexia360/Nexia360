/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_editor/component_collection.h"

#include <string.h>

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

namespace {

// sub_920BCB48, the collection's vtable slot +0x00
bool CategoryOffersAnEmptyChoice(uint32_t category) {
  return (category >= 0x09 && category <= 0x0B) ||
         (category >= 0x10 && category <= 0x16);
}

// sub_920BCB78, the collection's vtable slot +0x04
const xe::be<uint16_t>* EmptyChoiceNameForCategory(uint32_t category) {
  // Categories 9..0x16 each name their empty tile with one of the caption ids
  // 0xCC..0xD5, resolved through the editor's localised string table. Nothing
  // in this port hosts that table, so they all read as the same blank caption
  // the original falls back to for the categories outside that range.
  (void)category;
  return EmptyWideString();
}

}  // namespace

// sub_92216A58
void ComponentCollection::SeedEmptyChoicePerCategory() {
  const uint8_t body_type =
      body_kind() != 0 ? uint8_t(kBodyTypeSecond) : uint8_t(kBodyTypeFirst);
  for (uint32_t category = 0; category < kComponentBucketCount; ++category) {
    if (!CategoryOffersAnEmptyChoice(category)) {
      continue;
    }
    AssetRecordFields fields = {};
    fields.asset_id = EmptyAssetId();
    fields.category = category;
    fields.type_mask =
        uint16_t(CategoryToTypeMask(category) & kCategoryKeyMask);
    fields.body_type_primary = body_type;
    fields.display_name = EmptyChoiceNameForCategory(category);
    EmplaceRecord(fields);
  }
}

// the collection's vtable slot +0x08
void ComponentCollection::AnnounceBuildFinished() {
  // The original signals its owner here. Ours polls IsReady instead, so what
  // the slot is worth is the shape of the finished collection.
  for (uint32_t category = 0; category < kComponentBucketCount; ++category) {
    const uint32_t count = BucketEntryCount(category);
    if (count) {
      XELOGW("avatar_editor: category {:02X} holds {} components", category,
             count);
    }
  }
}

// sub_92217E28
void ComponentCollection::Build(uint32_t user_index, uint32_t body_kind) {
  {
    std::lock_guard<std::recursive_mutex> lock(CollectionLock());
    Reset();
    Store32(kUserIndexAt, user_index);
    Store32(kBodyKindAt, body_kind);

    // The seed runs whether or not a cancel is pending; only the stages below
    // it are gated.
    SeedEmptyChoicePerCategory();

    if (!CancelRequested()) {
      EnumerateAvatarAssets();
    }
    if (!CancelRequested()) {
      FileRecordsIntoBuckets();
    }
    if (!CancelRequested()) {
      LinkVariantsToPackages();
    }
    if (!CancelRequested()) {
      AnnounceBuildFinished();
    }
    if (!CancelRequested()) {
      Store8(kReadyAt, 1);
      XELOGW("avatar_editor: component collection is ready, {} records",
             record_count());
    }
  }
  ClearCancel();
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
