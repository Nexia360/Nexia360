/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_EDITOR_ASSET_TILE_H_
#define XENIA_KERNEL_XAM_AVATAR_EDITOR_ASSET_TILE_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "xenia/kernel/xam/avatar_editor/avatar_editor.h"
#include "xenia/kernel/xam/avatar_editor/component_collection.h"
#include "xenia/kernel/xam/avatar_editor/editor_asset_record.h"
#include "xenia/kernel/xam/avatar_editor/navigation.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

constexpr uint32_t kAssetTileBytes = 0x6A0;
constexpr uint32_t kSwatchTileBytes = 0x6F0;
constexpr uint32_t kMultiColourTileBytes = 0x700;

constexpr uint32_t kAssetTileVtable = 0x920097F0;
constexpr uint32_t kAssetTileSecondaryVtable = 0x920097EC;
constexpr uint32_t kAvatarItemVtable = 0x92008FA8;
constexpr uint32_t kColourButtonTileVtable = 0x920098D8;
constexpr uint32_t kSwatchTileVtable = 0x92009958;
constexpr uint32_t kMultiColourTileVtable = 0x920099D8;

// The subclasses store their extra state immediately past the base. The
// colour-button subclass writes at +0x6A0, which is one dword past the 0x6A0
// allocation the factory hands out for it - measured on both sides, and not yet
// reconciled, so it gets a dword of its own here rather than the first dword of
// whatever the heap put next.
constexpr uint32_t kColourButtonStateOffset = 0x6A0;
constexpr uint32_t kColourButtonTileBytes = kColourButtonStateOffset + 4;
constexpr uint32_t kSwatchStateOffset = 0x6A0;
constexpr uint32_t kSwatchVectorDataOffset = 0x6B0;
constexpr uint32_t kSwatchVectorDataBytes = 0x40;
constexpr uint32_t kMultiColourRecordOffset = 0x6A0;
constexpr uint32_t kMultiColourFlagOffset = 0x6F8;

constexpr uint32_t kTileUiStateDefault = 0;
constexpr uint32_t kTileUiStateRefused = 2;

// Tile objects come out of a bump heap of ours. It sits between the last of the
// application's globals (the sign-in watcher, 0x9428B380) and the framework
// object at 0x943F0000 - NOT over the scene object heap at 0x94410000, whose
// own header is a cursor at its first dword and would be read and written as
// this one. Its cursor is likewise the first dword of the range, so a freshly
// committed editor range starts empty.
constexpr uint32_t kTileHeapBase = 0x94290000;
constexpr uint32_t kTileHeapSize = 0x00160000;
constexpr uint32_t kTileHeapCursorOffset = 0x00;
constexpr uint32_t kTileManifestScratchOffset = 0x10;
constexpr uint32_t kTileHeapFirstObjectOffset = 0x400;

// The manifest edit each screen on the stack left behind, at
// kManifestHistoryAddress: a presence byte then the manifest itself, one entry
// per navigation command.
constexpr uint32_t kPendingEditStride = kManifestBytes + 1;
constexpr uint32_t kPendingEditTableOffset = 0x49;
constexpr uint32_t kPendingEditSlotCount = 0x54;

// The four other regions the manifest keeps items in, byte for byte the ones
// xna_avatar_format.cc reads and writes. An item's 32-bit kind says which one
// it belongs to, and only what is left over is a component at +0x160.
constexpr uint32_t kManifestBlendOffset = 0x0C;
constexpr uint32_t kManifestBlendBytes = 0x10;
constexpr uint32_t kManifestBlendCount = 3;
constexpr uint32_t kManifestFaceOffset = 0x3C;
constexpr uint32_t kManifestFaceCount = 6;
constexpr uint32_t kManifestRequiredCount = 4;

// Chin, nose, ears - Avatars::Manifest::Version0::Shape::Type_e, whose writer
// in xam.xex stores at manifest + 0xC + (type << 4) and is called with type 1
// for 0x80000 and type 2 for 0x200000. The pack names every 0x80000 asset a
// nose and every 0x200000 one a pair of ears, whatever order the SDK's
// XAvatarMetadataGetBlendShapeIDs lists its parameters in. Blend shapes carry
// an asset id and nothing else.
constexpr uint32_t kBlendShapeKinds[kManifestBlendCount] = {0x100000, 0x80000,
                                                            0x200000};
// Mouth, eyes, eyebrows, facial hair, eye shadow, face paint, in the order the
// face region stores them.
constexpr uint32_t kFaceTextureKinds[kManifestFaceCount] = {
    0x8000, 0x2000, 0x4000, 0x10000, 0x40000, 0x20000};
// Shoes, trousers, shirt, hair - the WORD table at 0x816331A0 in xam.xex, in
// this order, and the same four XAVATAR_COMPONENT_MASK bits. +0x300 is the
// PREVIOUS one per slot (ManifestReader::GetPreviousRequiredComponentInfo
// reads manifest + ((i + 0x18) << 5)); EquipRequiredComponents puts it back
// when a slot falls empty. A required item is worn among the components like
// anything else.
constexpr uint32_t kRequiredSlotKinds[kManifestRequiredCount] = {0x20, 0x10,
                                                                 0x08, 0x04};

// The slots an item covers. An outfit carries several of them plus the bit at
// 0x00800000, so one press can take three components off. A component's own
// mask is a WORD - XAVATAR_COMPONENT_MASK_ALL is 0x1FFF - so only the low bits
// of this ever match one; the rest belong to the face textures and the blend
// shapes, which are not components.
constexpr uint32_t kAssetKindCoverageMask = 0x0007FFFC;

// The swatch subclass is what the factory builds while one of these screens is
// on the stack.
constexpr uint32_t kCommandSwatchScreenFirst = 0x21;
constexpr uint32_t kCommandSwatchScreenLast = 0x23;

#pragma pack(push, 1)
struct AvatarComponentInfo {
  uint8_t asset_id[kAssetIdBytes];  // +0x00
  xe::be<uint16_t> type;            // +0x10
  xe::be<uint16_t> padding_012;     // +0x12
  uint8_t colours[12];              // +0x14
};

struct AssetTileFields {
  xe::be<uint32_t> widget_vtable;         // +0x000
  uint8_t unknown_004[0x1E4];             // +0x004
  uint8_t selected_tick_badge;            // +0x1E8
  uint8_t new_badge;                      // +0x1E9
  uint8_t purchased_badge;                // +0x1EA
  uint8_t multi_colour_badge;             // +0x1EB
  uint8_t unknown_1ec[8];                 // +0x1EC
  xe::be<uint32_t> award_info;            // +0x1F4
  uint8_t unknown_1f8[8];                 // +0x1F8
  xe::be<uint32_t> secondary_vtable;      // +0x200
  uint8_t unknown_204[0x450];             // +0x204
  xe::be<uint32_t> ui_state;              // +0x654
  uint8_t unknown_658[8];                 // +0x658
  xe::be<uint32_t> activate_command;      // +0x660
  xe::be<uint32_t> display_name;          // +0x664
  xe::be<uint32_t> unknown_668;           // +0x668
  xe::be<uint32_t> unknown_66c;           // +0x66C
  xe::be<uint32_t> avatar_item_vtable;    // +0x670
  xe::be<uint32_t> colour_channel_count;  // +0x674
  AvatarComponentInfo component_info;     // +0x678
  uint8_t incompatible_asset_version;     // +0x698
  uint8_t padding_699[3];                 // +0x699
  // The component info's type is XAM's 16-bit field, and the kinds above it -
  // chin, ears, nose, facial hair, eye shadow, face paint - do not survive it.
  // The whole kind goes here, in the padding the original leaves alone.
  xe::be<uint32_t> asset_kind;  // +0x69C
};
#pragma pack(pop)

static_assert(sizeof(AvatarComponentInfo) == 0x20,
              "SetComponentInfo takes a 0x20-byte _XAVATAR_COMPONENT_INFO");
static_assert(sizeof(AssetTileFields) == kAssetTileBytes, "");
static_assert(offsetof(AssetTileFields, selected_tick_badge) == 0x1E8, "");
static_assert(offsetof(AssetTileFields, new_badge) == 0x1E9, "");
static_assert(offsetof(AssetTileFields, purchased_badge) == 0x1EA, "");
static_assert(offsetof(AssetTileFields, multi_colour_badge) == 0x1EB, "");
static_assert(offsetof(AssetTileFields, award_info) == 0x1F4, "");
static_assert(offsetof(AssetTileFields, secondary_vtable) == 0x200, "");
static_assert(offsetof(AssetTileFields, ui_state) == 0x654, "");
static_assert(offsetof(AssetTileFields, activate_command) == 0x660, "");
static_assert(offsetof(AssetTileFields, display_name) == 0x664, "");
static_assert(offsetof(AssetTileFields, avatar_item_vtable) == 0x670, "");
static_assert(offsetof(AssetTileFields, colour_channel_count) == 0x674, "");
static_assert(offsetof(AssetTileFields, component_info) == 0x678, "");
static_assert(offsetof(AssetTileFields, component_info) +
                      offsetof(AvatarComponentInfo, type) ==
                  0x688,
              "");
static_assert(offsetof(AssetTileFields, component_info) +
                      offsetof(AvatarComponentInfo, colours) ==
                  0x68C,
              "");
static_assert(offsetof(AssetTileFields, incompatible_asset_version) == 0x698,
              "");
static_assert(offsetof(AssetTileFields, asset_kind) == 0x69C, "");
static_assert(kManifestComponentsOffset +
                      kManifestComponentCount * sizeof(AvatarComponentInfo) ==
                  kManifestDefaultsOffset,
              "the manifest's components end where its defaults begin");
static_assert(kManifestBlendOffset +
                      kManifestBlendCount * kManifestBlendBytes ==
                  kManifestFaceOffset,
              "the blend shapes end where the face textures begin");
static_assert(kManifestFaceOffset + kManifestFaceCount * kManifestEntryBytes ==
                  0xFC,
              "the face textures end where the colours begin");
static_assert(kManifestDefaultsOffset +
                      kManifestRequiredCount * kManifestEntryBytes ==
                  0x380,
              "the required slots end where the xuid begins");
static_assert(kTileHeapBase >= 0x94290000 &&
                  kTileHeapBase + kTileHeapSize <= 0x943F0000,
              "the tile heap stops short of the framework object and of the "
              "scene object heap above it");

enum class TileVariant {
  kBase,
  kColourButton,
  kSwatch,
  kMultiColour,
};

enum class TileBadgeImage : uint32_t {
  kSelectedTick = 2,
  kPurchased = 3,
  kNew = 4,
  kAwarded = 5,
  kMoreColours = 6,
  kLoadFailed = 7,
};

uint32_t TileBytesForVariant(TileVariant variant);
const char16_t* BadgeImageName(TileBadgeImage badge);
const char16_t* ColourButtonClassName(uint32_t colour_channel_count);

void ConstructTileBase(const Guest& guest, uint32_t tile_address,
                       uint32_t activate_command, uint32_t display_name_address,
                       uint8_t new_badge, uint32_t award_info);
void ConstructTileBaseWithoutCommand(const Guest& guest, uint32_t tile_address,
                                     uint32_t display_name_address,
                                     uint8_t new_badge, uint32_t award_info);

uint32_t AllocateFromTileHeap(const Guest& guest, uint32_t bytes);
void ResetTileHeap(const Guest& guest);

// Which region of the manifest an item of this kind belongs in, or -1 when it
// is not that kind of item.
int32_t ManifestBlendSlotForKind(uint32_t asset_kind);
int32_t ManifestFaceSlotForKind(uint32_t asset_kind);
int32_t ManifestRequiredSlotForKind(uint32_t asset_kind);

// The one writer: Avatars::ManifestReaderWriter::SetComponentInfo and
// RemoveComponents, routed by kind. An empty asset id takes the item off.
void WriteManifestItem(const Guest& guest, uint32_t manifest_address,
                       const AvatarComponentInfo& info, uint32_t asset_kind);

// The whole asset record's kind, rebuilt from the category the collection filed
// it under - the record's own copy of the mask is cut to thirteen bits.
uint32_t AssetKindOfRecord(const EditorAssetRecord* record);

// Which of the record's colour groups the avatar is wearing for this asset, or
// -1 when the asset is not worn at all.
int32_t WornColourGroupIndex(const Guest& guest, uint32_t manifest_address,
                             const EditorAssetRecord* record,
                             const uint8_t* asset_id);
// asset_kind is what decides where the empty choice looks; a caller that only
// has the sixteen-bit type gets the component array answer it always got.
bool ManifestWearsAsset(const Guest& guest, uint32_t manifest_address,
                        const uint8_t* asset_id, uint16_t component_type,
                        uint32_t asset_kind = 0);

// One item in the creator grid. The object lives in guest memory; this is a
// view over it and holds nothing of its own.
class AssetTile {
 public:
  AssetTile(const Guest& guest, uint32_t address)
      : guest_(guest), address_(address) {}

  uint32_t address() const { return address_; }
  bool valid() const { return address_ != 0; }
  AssetTileFields* fields() const {
    return guest_.At<AssetTileFields>(address_);
  }

  static TileVariant ChooseVariant(const Guest& guest,
                                   uint32_t asset_record_address);
  static AssetTile CreateForGridEntry(const Guest& guest,
                                      uint32_t asset_record_address,
                                      uint32_t activate_command,
                                      int32_t colour_group_index);

  static AssetTile ConstructBase(const Guest& guest, uint32_t tile_address,
                                 uint32_t activate_command,
                                 uint32_t asset_record_address,
                                 int32_t colour_group_index);
  static AssetTile ConstructColourButton(const Guest& guest,
                                         uint32_t tile_address,
                                         uint32_t activate_command,
                                         uint32_t asset_record_address,
                                         int32_t colour_group_index);
  static AssetTile ConstructSwatch(const Guest& guest, uint32_t tile_address,
                                   uint32_t activate_command,
                                   uint32_t asset_record_address,
                                   int32_t colour_group_index);
  static AssetTile ConstructMultiColour(const Guest& guest,
                                        uint32_t tile_address,
                                        uint32_t activate_command,
                                        uint32_t asset_record_address,
                                        int32_t colour_group_index);

  void SetNewBadge(uint8_t shown);
  void SetPurchasedBadge(uint8_t shown);
  void SetMultiColourBadge(uint8_t shown);
  void SetSelectedTickBadge(uint8_t shown);
  void SetAwardInfo(uint32_t display_item_address);

  uint32_t ActivateCommand() const;
  uint32_t ColourChannelCount() const;
  uint32_t DisplayNameAddress() const;
  std::u16string DisplayName() const;
  uint32_t ComponentInfoAddress() const;
  const uint8_t* AssetId() const;
  uint16_t ComponentType() const;
  uint32_t AssetKind() const;
  const char16_t* WidgetClassName() const;
  bool RefusesManifestWrite() const;
  bool IsWorn() const;
  uint32_t UiState() const;

  void Activate();
  void CommitSelection();
  void WriteItemToManifest(uint32_t manifest_address);

 private:
  void InstallVtables();
  void BuildComponentInfo(uint32_t asset_record_address,
                          int32_t colour_group_index);
  void UnwindNavigationToOwningScene();

  Guest guest_;
  uint32_t address_;
};

// What the renderer needs to draw one tile, with no guest layout in it.
struct TileInfo {
  uint32_t address = 0;
  std::u16string display_name;
  std::string display_name_utf8;
  uint32_t colour_channel_count = 0;
  uint32_t activate_command = 0;
  bool is_worn = false;
  bool shows_new_badge = false;
  bool shows_purchased_badge = false;
  bool shows_awarded_badge = false;
  bool shows_multi_colour_badge = false;
  bool refuses_manifest_write = false;
  const char16_t* widget_class_name = nullptr;
};

// The tiles of one creator-grid category, built from the collection's bucket
// for that category. Construction allocates and constructs every tile; nothing
// past that touches the collection.
class CategoryTiles {
 public:
  CategoryTiles(const Guest& guest, uint32_t category,
                uint32_t activate_command);

  uint32_t category() const { return category_; }
  size_t count() const { return tiles_.size(); }

  AssetTile TileAt(size_t index) const;
  TileInfo InfoAt(size_t index) const;

  void ActivateAt(size_t index);

 private:
  Guest guest_;
  uint32_t category_;
  std::vector<uint32_t> tiles_;
};

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_EDITOR_ASSET_TILE_H_
