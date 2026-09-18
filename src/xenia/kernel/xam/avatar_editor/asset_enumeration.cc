/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string.h>

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/avatar_editor/component_collection.h"

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XamAvatarBeginEnumAssets_entry(
    dword_t user_index, dword_t batch_size, dword_t kind_mask,
    dword_t body_type, dword_t unk5, pointer_t<XAM_OVERLAPPED> overlapped_ptr);
dword_result_t XamAvatarEnumAssets_entry(
    lpvoid_t buffer_ptr, lpdword_t count_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr);
dword_result_t XamAvatarEndEnumAssets_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr);

namespace avatar_editor {

namespace {

shim::TypedPointerParam<XAM_OVERLAPPED> NoOverlapped() {
  return shim::TypedPointerParam<XAM_OVERLAPPED>(
      static_cast<XAM_OVERLAPPED*>(nullptr));
}

// A parameter built here carries only a host pointer, and XamAvatarEnumAssets
// writes its records through the GUEST address instead - which stays zero
// unless the base class is assigned to directly.
shim::PointerParam GuestBuffer(void* host_pointer, uint32_t guest_address) {
  shim::PointerParam buffer(host_pointer);
  static_cast<shim::ParamBase<uint32_t>&>(buffer) = guest_address;
  return buffer;
}

uint32_t AssetKind(const uint8_t* asset_id) { return asset_id[6] & 0xF; }

// sub_922D5900
uint32_t AssetSortKey(const GuestEnumeratedAsset& asset) {
  const uint32_t key = asset.unknown_0c0;
  return key ? key : 0xFFFFFFFF;
}

// sub_922177DC, the tail of sub_92217420
uint8_t PackageAvailabilityBit(const GuestEnumeratedAsset& asset) {
  if (!(uint32_t(asset.flags) & kEnumerationFlagRecordField_32)) {
    return 0;
  }
  // The original derives this mask from the avatar library's state word at
  // 0x945BFDAC, which XAvatarInitialize sets to 4 and XAvatarShutdown clears.
  // Anything from 3 up gives 1; the library is up whenever an asset can be
  // enumerated at all.
  const uint8_t mask = 1;
  if (asset.unknown_2c0 & mask) {
    return 0;
  }
  return (asset.unknown_2c0 & ~mask) ? uint8_t(0) : uint8_t(1);
}

// sub_920DA1D0 with strings 6 and 5, which a variant borrows when its own name
// is withheld. Their table is not hosted here, and our producer never sets the
// flag that asks for them.
const xe::be<uint16_t>* GenericVariantName() { return EmptyWideString(); }

const xe::be<uint16_t>* GenericVariantDescription() {
  return EmptyWideString();
}

void PackColourGroups(const GuestEnumeratedAsset& asset,
                      xe::be<uint32_t>* colours) {
  memset(colours, 0, kRecordColourCount * sizeof(colours[0]));
  const uint32_t group_count = asset.colour_layout >> 4;
  const uint32_t colours_per_group = asset.colour_layout & 0xF;
  if (colours_per_group > 3) {
    return;
  }
  uint32_t index = 0;
  const uint8_t* source = &asset.colour_bytes[0][0];
  for (uint32_t group = 0; group < group_count; ++group) {
    for (uint32_t step = 0; step < 3; ++step, ++index) {
      if (index >= 9 || index >= kRecordColourCount) {
        return;
      }
      colours[index] = (uint32_t(source[index * 3 + 0]) << 16) |
                       (uint32_t(source[index * 3 + 1]) << 8) |
                       uint32_t(source[index * 3 + 2]);
    }
  }
}

uint8_t RecordField_30(const GuestEnumeratedAsset& asset) {
  const uint32_t group_count = asset.colour_layout >> 4;
  if (group_count > 1) {
    return 1;
  }
  return (!group_count &&
          (uint32_t(asset.flags) & kEnumerationFlagRecordField_30))
             ? uint8_t(1)
             : uint8_t(0);
}

GuestVariant* AdmitVariant(ComponentCollection& collection,
                           const GuestEnumeratedAsset& asset,
                           uint32_t* out_address) {
  GuestVariant* variant = collection.EmplaceVariant(out_address);
  if (!variant) {
    return nullptr;
  }
  const uint32_t flags = asset.flags;

  const xe::be<uint16_t>* name = asset.name;
  const xe::be<uint16_t>* description = nullptr;
  if (flags & kEnumerationFlagGenericName) {
    name = GenericVariantName();
    description = GenericVariantDescription();
  }
  for (uint32_t i = 0; i + 1 < 0x21; ++i) {
    variant->name[i] = name ? uint16_t(name[i]) : uint16_t(0);
    if (!variant->name[i]) {
      break;
    }
  }
  for (uint32_t i = 0; i + 1 < 0x64; ++i) {
    variant->description[i] =
        description ? uint16_t(description[i]) : uint16_t(0);
    if (!variant->description[i]) {
      break;
    }
  }
  variant->field_10A = (flags & kEnumerationFlagVariantOffered) ? 1 : 0;
  variant->field_114 = asset.unknown_2b8;
  variant->field_118 = asset.unknown_2bc;

  uint32_t package_address =
      collection.FindPackageAddressById(asset.package_id);
  if (!package_address) {
    GuestPackage* package = collection.EmplacePackage(&package_address);
    if (package) {
      package->id = asset.package_id;
      for (uint32_t i = 0; i + 1 < 0x40; ++i) {
        package->name[i] = asset.package_name[i];
        if (!package->name[i]) {
          break;
        }
      }
    }
  }
  variant->package = package_address;
  return variant;
}

bool AdmitEnumeratedAsset(ComponentCollection& collection,
                          const GuestEnumeratedAsset& asset) {
  if (collection.record_count() >= kRecordCapacity) {
    return false;
  }
  const uint32_t flags = asset.flags;
  if (!(flags & kEnumerationFlagUsable)) {
    return true;
  }
  if (AssetKind(asset.asset_id) == kDeduplicatedAssetKind &&
      collection.HoldsAssetId(asset.asset_id)) {
    return true;
  }

  xe::be<uint32_t> colours[kRecordColourCount];
  PackColourGroups(asset, colours);

  uint32_t variant_address = 0;
  if (flags & kEnumerationFlagHasVariant) {
    if (!AdmitVariant(collection, asset, &variant_address)) {
      return true;
    }
  }

  AssetRecordFields fields = {};
  fields.asset_id = asset.asset_id;
  fields.sort_key = AssetSortKey(asset);
  // The category takes the WHOLE type mask; only the record's own copy of it is
  // cut to 13 bits.
  fields.category =
      ComponentCollection::CategoryForTypeMask(uint32_t(asset.type_mask));
  fields.type_mask = uint16_t(uint32_t(asset.type_mask) & kCategoryKeyMask);
  fields.body_type_primary = asset.body_type_primary;
  fields.body_type_fallback = asset.body_type_fallback;
  fields.field_30 = RecordField_30(asset);
  fields.field_32 = (flags & kEnumerationFlagRecordField_32) ? 1 : 0;
  fields.field_33 = PackageAvailabilityBit(asset);
  fields.variant = variant_address;
  fields.colours = colours;
  fields.display_name = asset.name;
  collection.EmplaceRecord(fields);
  return true;
}

}  // namespace

// sub_92217420
void ComponentCollection::EnumerateAvatarAssets() {
  Memory* memory = guest_.memory();
  const uint32_t buffer_address =
      memory ? memory->SystemHeapAlloc(kEnumerationBatchBytes) : 0;
  GuestEnumeratedAsset* records =
      guest_.At<GuestEnumeratedAsset>(buffer_address);
  if (!records) {
    XELOGE("avatar_editor: no guest memory for the {} byte enumeration batch",
           kEnumerationBatchBytes);
    if (buffer_address) {
      memory->SystemHeapFree(buffer_address);
    }
    return;
  }

  const uint32_t body_type =
      body_kind() != 0 ? kBodyTypeSecond : kBodyTypeFirst;
  XamAvatarBeginEnumAssets_entry(
      shim::ParamBase<uint32_t>(user_index()),
      shim::ParamBase<uint32_t>(kEnumerationBatchRecords),
      shim::ParamBase<uint32_t>(kEnumerationKindMask),
      shim::ParamBase<uint32_t>(body_type), shim::ParamBase<uint32_t>(1),
      NoOverlapped());

  for (;;) {
    if (CancelRequested()) {
      break;
    }
    xe::be<uint32_t> returned = kEnumerationBatchRecords;
    memset(records, 0, kEnumerationBatchBytes);
    const uint32_t result =
        XamAvatarEnumAssets_entry(GuestBuffer(records, buffer_address),
                                  shim::PrimitivePointerParam<uint32_t>(
                                      reinterpret_cast<uint32_t*>(&returned)),
                                  NoOverlapped());
    if (int32_t(result) < 0 || !uint32_t(returned)) {
      break;
    }
    bool keep_going = true;
    for (uint32_t i = 0; i < uint32_t(returned) && keep_going; ++i) {
      keep_going = AdmitEnumeratedAsset(*this, records[i]);
    }
    if (!keep_going) {
      break;
    }
  }

  XamAvatarEndEnumAssets_entry(NoOverlapped());
  memory->SystemHeapFree(buffer_address);
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
