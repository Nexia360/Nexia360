/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_editor/asset_tile.h"

#include <string.h>

#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/kernel/xam/avatar_editor/editor_app.h"
#include "xenia/kernel/xam/avatar_editor/editor_session.h"
#include "xenia/kernel/xam/avatar_editor/scene.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

static_assert(kTileHeapBase + kTileHeapSize <= kObjectHeapBase ||
                  kTileHeapBase >= kObjectHeapBase + kObjectHeapSize,
              "the tile heap and the scene object heap would share a cursor");

namespace {

bool AssetIdIsZero(const uint8_t* asset_id) {
  for (uint32_t i = 0; i < kAssetIdBytes; ++i) {
    if (asset_id[i]) {
      return false;
    }
  }
  return true;
}

uint8_t* ManifestEntryAt(uint8_t* manifest, uint32_t region_offset,
                         uint32_t count, uint32_t index) {
  return index < count ? manifest + region_offset + index * kManifestEntryBytes
                       : nullptr;
}

const AvatarComponentInfo* ManifestComponentAt(const Guest& guest,
                                               uint32_t manifest_address,
                                               uint32_t index) {
  if (!manifest_address || index >= kManifestComponentCount) {
    return nullptr;
  }
  return guest.At<AvatarComponentInfo>(
      manifest_address + kManifestComponentsOffset +
      index * uint32_t(sizeof(AvatarComponentInfo)));
}

uint16_t ManifestEntryType(const uint8_t* entry) {
  return uint16_t((entry[kManifestTypeMaskOffset] << 8) |
                  entry[kManifestTypeMaskOffset + 1]);
}

// The kind a record carries is the category's own bit OR'd with the record's
// thirteen-bit remnant, so it is rarely equal to the bare kind these tables
// hold. Matching on equality alone sent every face texture past its own slot
// and into the component array, where it overwrote what the avatar was
// wearing. Carrying the table's bits is what makes it that kind.
int32_t IndexOfKind(const uint32_t* kinds, uint32_t count, uint32_t kind) {
  for (uint32_t i = 0; i < count; ++i) {
    if (kinds[i] == kind) {
      return int32_t(i);
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (kinds[i] && (kind & kinds[i]) == kinds[i]) {
      return int32_t(i);
    }
  }
  return -1;
}

// sub_9213C420: every component the new item covers stops being worn. The array
// is closed by a zero asset id, so what is left has to be packed back down.
void RemoveManifestComponentsCovering(uint8_t* manifest, uint32_t asset_kind) {
  const uint32_t coverage = asset_kind & kAssetKindCoverageMask;
  if (!coverage) {
    return;
  }
  uint32_t kept = 0;
  for (uint32_t i = 0; i < kManifestComponentCount; ++i) {
    uint8_t* entry =
        manifest + kManifestComponentsOffset + i * kManifestEntryBytes;
    if (AssetIdIsZero(entry) ||
        (uint32_t(ManifestEntryType(entry)) & coverage)) {
      continue;
    }
    uint8_t* keep =
        manifest + kManifestComponentsOffset + kept * kManifestEntryBytes;
    if (keep != entry) {
      memcpy(keep, entry, kManifestEntryBytes);
    }
    ++kept;
  }
  memset(manifest + kManifestComponentsOffset + kept * kManifestEntryBytes, 0,
         (kManifestComponentCount - kept) * kManifestEntryBytes);
}

// sub_9213BFE0: thirteen is XAM's limit, and past it the item is simply not
// worn.
void AppendManifestComponent(uint8_t* manifest,
                             const AvatarComponentInfo& info) {
  for (uint32_t i = 0; i < kManifestComponentCount; ++i) {
    uint8_t* entry =
        manifest + kManifestComponentsOffset + i * kManifestEntryBytes;
    if (!AssetIdIsZero(entry)) {
      continue;
    }
    memcpy(entry, &info, sizeof(info));
    return;
  }
}

bool ManifestRegionHoldsAssetId(const uint8_t* manifest, uint32_t region_offset,
                                uint32_t count, uint32_t stride,
                                const uint8_t* asset_id) {
  for (uint32_t i = 0; i < count; ++i) {
    if (memcmp(manifest + region_offset + i * stride, asset_id,
               kAssetIdBytes) == 0) {
      return true;
    }
  }
  return false;
}

// sub_920C7730 and sub_920C78A8, over the table at kManifestHistoryAddress. The
// key is the screen's command, which is what sub_920C7D00 files an edit under.
uint32_t PendingEditForCommand(const Guest& guest, uint32_t command) {
  if (command >= kPendingEditSlotCount) {
    return 0;
  }
  const uint32_t entry = kManifestHistoryAddress + kPendingEditTableOffset +
                         command * kPendingEditStride;
  return guest.Load8(entry) ? entry + 1 : 0;
}

EditorApp* CurrentApp() {
  EditorSession* session = EditorSession::Current();
  return session ? &session->app() : nullptr;
}

}  // namespace

int32_t ManifestBlendSlotForKind(uint32_t asset_kind) {
  return IndexOfKind(kBlendShapeKinds, kManifestBlendCount, asset_kind);
}

int32_t ManifestFaceSlotForKind(uint32_t asset_kind) {
  return IndexOfKind(kFaceTextureKinds, kManifestFaceCount, asset_kind);
}

int32_t ManifestRequiredSlotForKind(uint32_t asset_kind) {
  return IndexOfKind(kRequiredSlotKinds, kManifestRequiredCount, asset_kind);
}

uint32_t AssetKindOfRecord(const EditorAssetRecord* record) {
  if (!record) {
    return 0;
  }
  return ComponentCollection::CategoryToTypeMask(uint32_t(record->category)) |
         uint32_t(uint16_t(record->type_mask));
}

// sub_920DEF08, with Avatars::ManifestReaderWriter spelled out
void WriteManifestItem(const Guest& guest, uint32_t manifest_address,
                       const AvatarComponentInfo& info, uint32_t asset_kind) {
  uint8_t* manifest = guest.At<uint8_t>(manifest_address);
  if (!manifest) {
    return;
  }
  const bool take_it_off = AssetIdIsZero(info.asset_id);

  const int32_t blend = ManifestBlendSlotForKind(asset_kind);
  if (blend >= 0) {
    memcpy(
        manifest + kManifestBlendOffset + uint32_t(blend) * kManifestBlendBytes,
        info.asset_id, kAssetIdBytes);
    return;
  }

  const int32_t face = ManifestFaceSlotForKind(asset_kind);
  if (face >= 0) {
    uint8_t* entry = ManifestEntryAt(manifest, kManifestFaceOffset,
                                     kManifestFaceCount, uint32_t(face));
    if (entry) {
      memset(entry, 0, kManifestEntryBytes);
      if (!take_it_off) {
        memcpy(entry, info.asset_id, kAssetIdBytes);
        // A face slot holds a ReplacementTexture_c, not a component: xam's
        // Avatars::ManifestReaderWriter::SetReplacementTexture copies an asset
        // id and a float 1.0f at +0x10, which is where a component keeps its
        // mask instead.
        const float one = 1.0f;
        uint32_t bits;
        memcpy(&bits, &one, sizeof(bits));
        entry[kManifestTypeMaskOffset + 0] = uint8_t(bits >> 24);
        entry[kManifestTypeMaskOffset + 1] = uint8_t(bits >> 16);
        entry[kManifestTypeMaskOffset + 2] = uint8_t(bits >> 8);
        entry[kManifestTypeMaskOffset + 3] = uint8_t(bits);
      }
    }
    RemoveManifestComponentsCovering(manifest, asset_kind);
    return;
  }

  RemoveManifestComponentsCovering(manifest, asset_kind);
  if (take_it_off) {
    return;
  }

  // Avatars::ManifestReaderWriter::SetComponentInfo (xam.xex 0x8196D1B0) files
  // a required item - shoes, trousers, shirt or hair, the four masks in the
  // table at 0x816331A0 - into the backup at +0x300 AND THEN wears it like
  // anything else. +0x300 is GetPreviousRequiredComponentInfo's region: what
  // EquipRequiredComponents puts back when a slot is emptied, not where the
  // item is worn. Returning here left the component array with no hair, no
  // shirt, no trousers and no shoes, so everything that reads the worn set -
  // GetCombinedComponentMask, and every title through XamAvatarGetAssets - saw
  // them come off the moment one was chosen.
  const int32_t required = ManifestRequiredSlotForKind(asset_kind);
  if (required >= 0) {
    uint8_t* entry =
        ManifestEntryAt(manifest, kManifestDefaultsOffset,
                        kManifestRequiredCount, uint32_t(required));
    if (entry) {
      memcpy(entry, &info, sizeof(info));
    }
  }

  // A component with no type matches the manifest's unused slots, so writing
  // one does not add an item - it overwrites whatever the avatar was already
  // wearing. An item that reaches here with a zero type has a kind this
  // routing did not recognise, and refusing it loses that one item instead of
  // the profile.
  if (uint16_t(info.type) == 0) {
    XELOGW("avatar_editor: refusing a component with no type, kind {:08X}",
           asset_kind);
    return;
  }
  AppendManifestComponent(manifest, info);
}

uint32_t TileBytesForVariant(TileVariant variant) {
  switch (variant) {
    case TileVariant::kSwatch:
      return kSwatchTileBytes;
    case TileVariant::kMultiColour:
      return kMultiColourTileBytes;
    case TileVariant::kColourButton:
      return kColourButtonTileBytes;
    case TileVariant::kBase:
    default:
      return kAssetTileBytes;
  }
}

// sub_920DC528
const char16_t* BadgeImageName(TileBadgeImage badge) {
  switch (badge) {
    case TileBadgeImage::kSelectedTick:
      return u"item_selected_tick.png";
    case TileBadgeImage::kPurchased:
      return u"item_purchased.png";
    case TileBadgeImage::kNew:
      return u"newitem.png";
    case TileBadgeImage::kAwarded:
      return u"awardeditem.png";
    case TileBadgeImage::kMoreColours:
      return u"more_colours.png";
    case TileBadgeImage::kLoadFailed:
      return u"loadfailed.png";
    default:
      return u"";
  }
}

// sub_920DF4F8, the switch at 0x920DF580
const char16_t* ColourButtonClassName(uint32_t colour_channel_count) {
  switch (colour_channel_count) {
    case 1:
      return u"Grid1x1OneColourButton";
    case 2:
      return u"Grid1x1TwoColourButton";
    case 3:
      return u"Grid1x1ThreeColourButton";
    default:
      return nullptr;
  }
}

// sub_920FC228, retargeted onto a heap of ours
uint32_t AllocateFromTileHeap(const Guest& guest, uint32_t bytes) {
  const uint32_t aligned = (bytes + 0xF) & ~uint32_t(0xF);
  if (!aligned || aligned > kTileHeapSize - kTileHeapFirstObjectOffset) {
    return 0;
  }
  uint32_t cursor = guest.Load32(kTileHeapBase + kTileHeapCursorOffset);
  if (cursor < kTileHeapFirstObjectOffset || cursor > kTileHeapSize ||
      cursor + aligned > kTileHeapSize) {
    // Nothing frees a tile, so the heap wraps rather than refusing: a grid
    // never holds more than a few hundred, and what is overwritten belongs to
    // screens the stack has already left.
    cursor = kTileHeapFirstObjectOffset;
  }
  guest.Store32(kTileHeapBase + kTileHeapCursorOffset, cursor + aligned);
  const uint32_t address = kTileHeapBase + cursor;
  memset(guest.At<uint8_t>(address), 0, aligned);
  return address;
}

void ResetTileHeap(const Guest& guest) {
  guest.Store32(kTileHeapBase + kTileHeapCursorOffset,
                kTileHeapFirstObjectOffset);
}

// sub_920DF378
int32_t WornColourGroupIndex(const Guest& guest, uint32_t manifest_address,
                             const EditorAssetRecord* record,
                             const uint8_t* asset_id) {
  const int32_t group_count = EditorColourGroupCount(record);
  const uint8_t* manifest = guest.At<uint8_t>(manifest_address);
  if (!manifest) {
    return -1;
  }
  // The components and the four required slots are the only regions that keep
  // colours; a blend shape or a face texture is an asset id on its own.
  for (uint32_t i = 0; i < kManifestComponentCount + kManifestRequiredCount;
       ++i) {
    const uint8_t* entry =
        i < kManifestComponentCount
            ? manifest + kManifestComponentsOffset + i * kManifestEntryBytes
            : manifest + kManifestDefaultsOffset +
                  (i - kManifestComponentCount) * kManifestEntryBytes;
    const auto* worn = reinterpret_cast<const AvatarComponentInfo*>(entry);
    if (memcmp(worn->asset_id, asset_id, kAssetIdBytes) != 0) {
      continue;
    }
    for (int32_t group = 0; group < group_count; ++group) {
      const xe::be<uint32_t>* colours = EditorColourGroupAt(record, group);
      if (colours &&
          memcmp(worn->colours, colours, sizeof(worn->colours)) == 0) {
        return group;
      }
    }
    return 0;
  }
  return -1;
}

bool ManifestWearsAsset(const Guest& guest, uint32_t manifest_address,
                        const uint8_t* asset_id, uint16_t component_type,
                        uint32_t asset_kind) {
  const uint8_t* manifest = guest.At<uint8_t>(manifest_address);
  if (!manifest) {
    return false;
  }

  if (!AssetIdIsZero(asset_id)) {
    return ManifestRegionHoldsAssetId(manifest, kManifestBlendOffset,
                                      kManifestBlendCount, kManifestBlendBytes,
                                      asset_id) ||
           ManifestRegionHoldsAssetId(manifest, kManifestFaceOffset,
                                      kManifestFaceCount, kManifestEntryBytes,
                                      asset_id) ||
           ManifestRegionHoldsAssetId(manifest, kManifestComponentsOffset,
                                      kManifestComponentCount,
                                      kManifestEntryBytes, asset_id) ||
           ManifestRegionHoldsAssetId(manifest, kManifestDefaultsOffset,
                                      kManifestRequiredCount,
                                      kManifestEntryBytes, asset_id);
  }

  const int32_t face = ManifestFaceSlotForKind(asset_kind);
  if (face >= 0) {
    return AssetIdIsZero(manifest + kManifestFaceOffset +
                         uint32_t(face) * kManifestEntryBytes);
  }

  for (uint32_t i = 0; i < kManifestComponentCount; ++i) {
    const AvatarComponentInfo* worn =
        ManifestComponentAt(guest, manifest_address, i);
    if (!worn || AssetIdIsZero(worn->asset_id)) {
      continue;
    }
    if (uint16_t(worn->type) == component_type) {
      return false;
    }
  }
  return true;
}

// sub_920DBE70
void ConstructTileBase(const Guest& guest, uint32_t tile_address,
                       uint32_t activate_command, uint32_t display_name_address,
                       uint8_t new_badge, uint32_t award_info) {
  AssetTile tile(guest, tile_address);
  auto* fields = tile.fields();
  fields->activate_command = activate_command;
  fields->display_name = display_name_address;
  tile.SetNewBadge(new_badge);
  tile.SetAwardInfo(award_info);
}

// sub_920DBB58
void ConstructTileBaseWithoutCommand(const Guest& guest, uint32_t tile_address,
                                     uint32_t display_name_address,
                                     uint8_t new_badge, uint32_t award_info) {
  ConstructTileBase(guest, tile_address, 0, display_name_address, new_badge,
                    award_info);
}

// sub_920BFD68, the size selection
TileVariant AssetTile::ChooseVariant(const Guest& guest,
                                     uint32_t asset_record_address) {
  const auto* record = guest.At<EditorAssetRecord>(asset_record_address);
  const uint8_t kind = EditorRecordAssetKind(record);
  if (EditorColourGroupCount(record) > 1) {
    return TileVariant::kMultiColour;
  }
  if ((kind == kAssetKindMultiPart || kind == kAssetKindDeduplicated) &&
      EditorRecordIsMultiPart(record)) {
    return TileVariant::kMultiColour;
  }
  Navigation navigation(guest.memory());
  const uint32_t command = navigation.CurrentCommand();
  if (command >= kCommandSwatchScreenFirst &&
      command <= kCommandSwatchScreenLast) {
    return TileVariant::kSwatch;
  }
  return TileVariant::kBase;
}

// sub_920BFD68
AssetTile AssetTile::CreateForGridEntry(const Guest& guest,
                                        uint32_t asset_record_address,
                                        uint32_t activate_command,
                                        int32_t colour_group_index) {
  const TileVariant variant = ChooseVariant(guest, asset_record_address);
  const uint32_t tile_address =
      AllocateFromTileHeap(guest, TileBytesForVariant(variant));
  if (!tile_address) {
    return AssetTile(guest, 0);
  }

  AssetTile tile(guest, 0);
  switch (variant) {
    case TileVariant::kSwatch:
      tile = ConstructSwatch(guest, tile_address, activate_command,
                             asset_record_address, colour_group_index);
      break;
    case TileVariant::kMultiColour:
      tile = ConstructMultiColour(guest, tile_address, activate_command,
                                  asset_record_address, colour_group_index);
      tile.SetMultiColourBadge(1);
      break;
    case TileVariant::kColourButton:
      tile = ConstructColourButton(guest, tile_address, activate_command,
                                   asset_record_address, colour_group_index);
      break;
    case TileVariant::kBase:
    default:
      tile = ConstructBase(guest, tile_address, activate_command,
                           asset_record_address, colour_group_index);
      break;
  }

  const auto* record = guest.At<EditorAssetRecord>(asset_record_address);
  Navigation navigation(guest.memory());
  if (EditorRecordAssetKind(record) == kAssetKindDeduplicated &&
      navigation.StackContains(kCommandCloset) &&
      EditorRecordIsPurchased(record)) {
    tile.SetPurchasedBadge(1);
  }
  return tile;
}

// sub_920DF128
AssetTile AssetTile::ConstructBase(const Guest& guest, uint32_t tile_address,
                                   uint32_t activate_command,
                                   uint32_t asset_record_address,
                                   int32_t colour_group_index) {
  AssetTile tile(guest, tile_address);
  const auto* record = guest.At<EditorAssetRecord>(asset_record_address);
  ConstructTileBase(guest, tile_address, activate_command,
                    EditorAssetRecordNameAddress(asset_record_address),
                    EditorRecordShowsNewBadge(record) ? 1 : 0,
                    EditorRecordAwardInfo(record));
  tile.InstallVtables();
  tile.BuildComponentInfo(asset_record_address, colour_group_index);

  auto* fields = tile.fields();
  fields->colour_channel_count = EditorColourChannelCount(record);
  fields->incompatible_asset_version =
      EditorRecordIsIncompatible(record) ? 1 : 0;
  return tile;
}

// sub_920DF4F8
AssetTile AssetTile::ConstructColourButton(const Guest& guest,
                                           uint32_t tile_address,
                                           uint32_t activate_command,
                                           uint32_t asset_record_address,
                                           int32_t colour_group_index) {
  AssetTile tile = ConstructBase(guest, tile_address, activate_command,
                                 asset_record_address, colour_group_index);
  const auto* record = guest.At<EditorAssetRecord>(asset_record_address);
  const int32_t worn_group =
      record ? WornColourGroupIndex(guest, kLiveManifestAddress, record,
                                    record->asset_id)
             : -1;
  guest.Store32(tile_address, kColourButtonTileVtable);
  guest.Store32(tile_address + kColourButtonStateOffset,
                worn_group >= 0 ? uint32_t(worn_group) : 1);
  return tile;
}

// sub_920DF9D8
AssetTile AssetTile::ConstructSwatch(const Guest& guest, uint32_t tile_address,
                                     uint32_t activate_command,
                                     uint32_t asset_record_address,
                                     int32_t colour_group_index) {
  AssetTile tile = ConstructBase(guest, tile_address, activate_command,
                                 asset_record_address, colour_group_index);
  guest.Store32(tile_address, kSwatchTileVtable);
  guest.Store32(tile_address + kSwatchStateOffset, 0);
  memset(guest.At<uint8_t>(tile_address + kSwatchVectorDataOffset), 0,
         kSwatchVectorDataBytes);
  return tile;
}

// sub_920DFCB0
AssetTile AssetTile::ConstructMultiColour(const Guest& guest,
                                          uint32_t tile_address,
                                          uint32_t activate_command,
                                          uint32_t asset_record_address,
                                          int32_t colour_group_index) {
  AssetTile tile = ConstructBase(guest, tile_address, activate_command,
                                 asset_record_address, colour_group_index);
  const auto* record = guest.At<EditorAssetRecord>(asset_record_address);
  guest.Store32(tile_address, kMultiColourTileVtable);
  guest.Store32(tile_address + kMultiColourRecordOffset, asset_record_address);
  guest.Store32(tile_address + kMultiColourFlagOffset,
                EditorColourGroupCount(record) == 0 ? 1 : 0);
  return tile;
}

// sub_920DF128, the three vtable stores
void AssetTile::InstallVtables() {
  auto* fields = this->fields();
  fields->widget_vtable = kAssetTileVtable;
  fields->secondary_vtable = kAssetTileSecondaryVtable;
  fields->avatar_item_vtable = kAvatarItemVtable;
}

// sub_920DF128, the block at 0x920DF1C0
void AssetTile::BuildComponentInfo(uint32_t asset_record_address,
                                   int32_t colour_group_index) {
  auto* info = &fields()->component_info;
  const auto* record = guest_.At<EditorAssetRecord>(asset_record_address);
  if (!record) {
    memset(info, 0, sizeof(*info));
    fields()->asset_kind = 0;
    return;
  }
  memcpy(info->asset_id, record->asset_id, kAssetIdBytes);
  const uint32_t kind = AssetKindOfRecord(record);
  fields()->asset_kind = kind;
  info->type = uint16_t(kind);
  info->padding_012 = 0;
  const xe::be<uint32_t>* colours =
      EditorColourGroupCount(record) > 0
          ? EditorColourGroupAt(record, colour_group_index)
          : nullptr;
  if (colours) {
    memcpy(info->colours, colours, sizeof(info->colours));
  } else {
    memset(info->colours, 0, sizeof(info->colours));
  }
}

// sub_920DC508
void AssetTile::SetNewBadge(uint8_t shown) { fields()->new_badge = shown; }

// sub_920DC510
void AssetTile::SetPurchasedBadge(uint8_t shown) {
  fields()->purchased_badge = shown;
}

// sub_920DC518
void AssetTile::SetMultiColourBadge(uint8_t shown) {
  fields()->multi_colour_badge = shown;
}

void AssetTile::SetSelectedTickBadge(uint8_t shown) {
  fields()->selected_tick_badge = shown;
}

// sub_920DC520
void AssetTile::SetAwardInfo(uint32_t display_item_address) {
  fields()->award_info = display_item_address;
}

uint32_t AssetTile::ActivateCommand() const {
  return fields()->activate_command;
}

uint32_t AssetTile::ColourChannelCount() const {
  return fields()->colour_channel_count;
}

uint32_t AssetTile::DisplayNameAddress() const {
  return fields()->display_name;
}

std::u16string AssetTile::DisplayName() const {
  const auto* name = guest_.At<xe::be<uint16_t>>(DisplayNameAddress());
  if (!name) {
    return std::u16string();
  }
  std::u16string text;
  for (uint32_t i = 0; i < kEditorAssetRecordNameLength && name[i]; ++i) {
    text.push_back(char16_t(uint16_t(name[i])));
  }
  return text;
}

uint32_t AssetTile::ComponentInfoAddress() const {
  return address_ + uint32_t(offsetof(AssetTileFields, component_info));
}

const uint8_t* AssetTile::AssetId() const {
  return fields()->component_info.asset_id;
}

uint16_t AssetTile::ComponentType() const {
  return uint16_t(fields()->component_info.type);
}

uint32_t AssetTile::AssetKind() const { return fields()->asset_kind; }

const char16_t* AssetTile::WidgetClassName() const {
  return ColourButtonClassName(ColourChannelCount());
}

bool AssetTile::RefusesManifestWrite() const {
  return fields()->incompatible_asset_version != 0;
}

bool AssetTile::IsWorn() const {
  return ManifestWearsAsset(guest_, kLiveManifestAddress, AssetId(),
                            ComponentType(), AssetKind());
}

// sub_920DEE80
uint32_t AssetTile::UiState() const {
  const uint32_t state = fields()->ui_state;
  if (RefusesManifestWrite() && state == kTileUiStateDefault) {
    return kTileUiStateRefused;
  }
  return state;
}

// sub_920DEF08
void AssetTile::WriteItemToManifest(uint32_t manifest_address) {
  if (RefusesManifestWrite()) {
    return;
  }
  // By value: the write compacts the manifest, and for the live manifest the
  // tile's own info is not what moves.
  const AvatarComponentInfo info = fields()->component_info;
  WriteManifestItem(guest_, manifest_address, info, AssetKind());
}

// sub_920DF098, including the manifest it hands back through the item interface
void AssetTile::CommitSelection() {
  WriteItemToManifest(kLiveManifestAddress);
  if (EditorApp* app = CurrentApp()) {
    app->PushManifestToRenderer(kLiveManifestAddress, false);
  }
}

// sub_920DBCA0, the loop at 0x920DBD10. Every screen still on the stack that
// recorded an edit gets this tile's item written into its recorded manifest and
// is then popped; the manifest the press carries forward is the live one.
void AssetTile::UnwindNavigationToOwningScene() {
  Navigation navigation(guest_.memory());
  int32_t depth = navigation.state()->depth;
  if (depth >= int32_t(kScreenStackLimit)) {
    depth = int32_t(kScreenStackLimit) - 1;
  }
  for (int32_t level = 0; level <= depth; ++level) {
    const uint32_t pending =
        PendingEditForCommand(guest_, navigation.CommandAt(level));
    if (!pending) {
      continue;
    }
    WriteItemToManifest(pending);
    if (navigation.state()->depth <= 0) {
      break;
    }
    navigation.PopScreen(1);
  }
}

// sub_920DBCA0 - the A press inside the creator grid
void AssetTile::Activate() {
  CommitSelection();

  Navigation navigation(guest_.memory());
  const uint32_t command = ActivateCommand();
  if (!command) {
    navigation.PopUntilAScreenStaysUp();
    return;
  }

  const uint32_t snapshot = kTileHeapBase + kTileManifestScratchOffset;
  memcpy(guest_.At<uint8_t>(snapshot), guest_.At<uint8_t>(kLiveManifestAddress),
         kManifestBytes);

  UnwindNavigationToOwningScene();

  if (EditorApp* app = CurrentApp()) {
    app->AdoptManifest(snapshot);
    app->PushManifestToRenderer(snapshot, false);
  }
  navigation.PushCommand(command, nullptr, nullptr, -1, -1);
}

CategoryTiles::CategoryTiles(const Guest& guest, uint32_t category,
                             uint32_t activate_command)
    : guest_(guest), category_(category) {
  ComponentCollection collection(guest);
  const uint32_t count = collection.BucketEntryCount(category);
  tiles_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const GuestAssetRecord* record = collection.BucketEntryAt(category, i);
    if (!record) {
      continue;
    }
    const uint32_t record_address =
        guest.memory()->HostToGuestVirtual(static_cast<const void*>(record));
    // Every tile in the grid stands for the asset's first colour group; the
    // other groups are reached through the swatch screen the tile opens.
    AssetTile tile = AssetTile::CreateForGridEntry(guest, record_address,
                                                   activate_command, 0);
    if (!tile.valid()) {
      break;
    }
    tiles_.push_back(tile.address());
  }
}

AssetTile CategoryTiles::TileAt(size_t index) const {
  return AssetTile(guest_, index < tiles_.size() ? tiles_[index] : 0);
}

TileInfo CategoryTiles::InfoAt(size_t index) const {
  TileInfo info;
  AssetTile tile = TileAt(index);
  if (!tile.valid()) {
    return info;
  }
  const auto* fields = tile.fields();
  info.address = tile.address();
  info.display_name = tile.DisplayName();
  info.display_name_utf8 = xe::to_utf8(info.display_name);
  info.colour_channel_count = tile.ColourChannelCount();
  info.activate_command = tile.ActivateCommand();
  info.is_worn = tile.IsWorn();
  info.shows_new_badge = fields->new_badge != 0;
  info.shows_purchased_badge = fields->purchased_badge != 0;
  info.shows_awarded_badge = uint32_t(fields->award_info) != 0;
  info.shows_multi_colour_badge = fields->multi_colour_badge != 0;
  info.refuses_manifest_write = tile.RefusesManifestWrite();
  info.widget_class_name = tile.WidgetClassName();
  return info;
}

void CategoryTiles::ActivateAt(size_t index) {
  AssetTile tile = TileAt(index);
  if (tile.valid()) {
    tile.Activate();
  }
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
