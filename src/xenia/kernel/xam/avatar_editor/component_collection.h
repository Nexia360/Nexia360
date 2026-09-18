/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_EDITOR_COMPONENT_COLLECTION_H_
#define XENIA_KERNEL_XAM_AVATAR_EDITOR_COMPONENT_COLLECTION_H_

#include <stddef.h>
#include <stdint.h>

#include <mutex>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xam/avatar_editor/avatar_editor.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

// The original reads its empty asset id from sixteen zero bytes at 0x92017EF8
// and its fallback caption from an empty wide string at 0x92053090. Both live
// in the image's .rdata, which this port never maps - the session commits only
// the editor's own working range - so both are host constants here.
constexpr uint32_t kComponentBucketCount = 25;
constexpr uint32_t kCategoryFallback = 23;
constexpr uint32_t kCategoryRetryBit = 0x00800000;
constexpr uint32_t kCategoryKeyMask = 0x1FFF;

constexpr uint32_t kRecordCapacity = 0x10000;
constexpr uint32_t kRecordBytes = 0x198;
constexpr uint32_t kPackageCapacity = 0x1000;
constexpr uint32_t kPackageBytes = 0x8C;
constexpr uint32_t kVariantCapacity = 0x4000;
constexpr uint32_t kVariantBytes = 0x11C;
constexpr uint32_t kBucketBytes = 12;
constexpr uint32_t kRecordColourCount = 56;

// Displacements from kComponentCollectionAddress. The pools come first and the
// flags every other section reads come last, ~31 MB in.
constexpr uint32_t kRecordsAt = 0x0000004;
constexpr uint32_t kRecordCountAt = 0x1980004;
constexpr uint32_t kPackagesAt = 0x1980008;
constexpr uint32_t kPackageIndexAt = 0x1A0C008;
constexpr uint32_t kPackageCountAt = 0x1A10008;
constexpr uint32_t kVariantsAt = 0x1A1000C;
constexpr uint32_t kVariantCountAt = 0x1E8000C;
constexpr uint32_t kVariantArenaAt = 0x1E80010;
constexpr uint32_t kVariantArenaUsedAt = 0x1E90010;
constexpr uint32_t kBucketArenaAt = 0x1E90014;
constexpr uint32_t kBucketArenaUsedAt = 0x1ED0014;
constexpr uint32_t kBucketsAt = 0x1ED0018;
constexpr uint32_t kUserIndexAt = 0x1ED0144;
constexpr uint32_t kBodyKindAt = 0x1ED0148;
constexpr uint32_t kReadyAt = 0x1ED014C;
constexpr uint32_t kCancelAt = 0x1ED014D;
constexpr uint32_t kBuildLockAt = 0x1ED0150;
constexpr uint32_t kBuildThreadAt = 0x1ED0170;
constexpr uint32_t kTeardownLockAt = 0x1ED0174;
constexpr uint32_t kField_190At = 0x1ED0190;
constexpr uint32_t kTeardownHandleAt = 0x1ED01AC;

static_assert(kRecordsAt + kRecordCapacity * kRecordBytes == kRecordCountAt,
              "the record pool ends where its count begins");
static_assert(kPackagesAt + kPackageCapacity * kPackageBytes == kPackageIndexAt,
              "the package pool ends where its index begins");
static_assert(kPackageIndexAt + kPackageCapacity * 4 == kPackageCountAt,
              "the package index ends where its count begins");
static_assert(kVariantsAt + kVariantCapacity * kVariantBytes == kVariantCountAt,
              "the variant pool ends where its count begins");
static_assert(kVariantArenaAt + kVariantCapacity * 4 == kVariantArenaUsedAt,
              "the variant arena ends where its cursor begins");
static_assert(kBucketArenaAt + kRecordCapacity * 4 == kBucketArenaUsedAt,
              "the bucket arena ends where its cursor begins");
static_assert(kBucketsAt + kComponentBucketCount * kBucketBytes == kUserIndexAt,
              "the buckets end where the build parameters begin");

#pragma pack(push, 1)

struct GuestComponentBucket {
  xe::be<uint32_t> count;
  xe::be<uint32_t> items;
  xe::be<uint32_t> field_8;
};
static_assert(sizeof(GuestComponentBucket) == kBucketBytes, "");

struct GuestAssetRecord {
  uint8_t asset_id[kAssetIdBytes];
  uint8_t field_10[kAssetIdBytes];
  xe::be<uint32_t> sort_key;
  xe::be<uint32_t> category;
  xe::be<uint16_t> type_mask;
  uint8_t body_type_primary;
  uint8_t padding_2b;
  xe::be<uint32_t> body_selector;
  uint8_t field_30;
  uint8_t field_31;
  uint8_t field_32;
  uint8_t field_33;
  xe::be<uint32_t> variant;
  xe::be<uint32_t> colours[kRecordColourCount];
  xe::be<uint16_t> display_name[0x40];
};
static_assert(sizeof(GuestAssetRecord) == kRecordBytes, "");
static_assert(offsetof(GuestAssetRecord, sort_key) == 0x20, "");
static_assert(offsetof(GuestAssetRecord, category) == 0x24, "");
static_assert(offsetof(GuestAssetRecord, type_mask) == 0x28, "");
static_assert(offsetof(GuestAssetRecord, body_selector) == 0x2C, "");
static_assert(offsetof(GuestAssetRecord, variant) == 0x34, "");
static_assert(offsetof(GuestAssetRecord, colours) == 0x38, "");
static_assert(offsetof(GuestAssetRecord, display_name) == 0x118, "");

struct GuestVariant {
  xe::be<uint16_t> name[0x21];
  xe::be<uint16_t> description[0x64];
  uint8_t field_10A;
  uint8_t padding_10b;
  xe::be<uint32_t> package;
  xe::be<uint32_t> record;
  xe::be<uint32_t> field_114;
  xe::be<uint32_t> field_118;
};
static_assert(sizeof(GuestVariant) == kVariantBytes, "");
static_assert(offsetof(GuestVariant, description) == 0x42, "");
static_assert(offsetof(GuestVariant, field_10A) == 0x10A, "");
static_assert(offsetof(GuestVariant, package) == 0x10C, "");
static_assert(offsetof(GuestVariant, record) == 0x110, "");

struct GuestPackage {
  xe::be<uint32_t> id;
  xe::be<uint16_t> name[0x40];
  xe::be<uint32_t> member_count;
  xe::be<uint32_t> members;
};
static_assert(sizeof(GuestPackage) == kPackageBytes, "");
static_assert(offsetof(GuestPackage, member_count) == 0x84, "");
static_assert(offsetof(GuestPackage, members) == 0x88, "");

// One record of the buffer XamAvatarEnumAssets fills, in the layout
// xam_avatar.cc writes it.
struct GuestEnumeratedAsset {
  uint8_t asset_id[kAssetIdBytes];
  xe::be<uint32_t> type_mask;
  uint8_t body_type_primary;
  uint8_t body_type_fallback;
  uint8_t unknown_016[2];
  xe::be<uint32_t> flags;
  uint8_t colour_layout;
  uint8_t colour_bytes[9][3];
  uint8_t unknown_038[0x84];
  uint8_t unknown_0bc[4];
  xe::be<uint32_t> unknown_0c0;
  xe::be<uint32_t> unknown_0c4;
  xe::be<uint32_t> unknown_0c8;
  xe::be<uint32_t> unknown_0cc;
  uint8_t unknown_0d0[0x14];
  xe::be<uint16_t> name[0x40];
  uint8_t unknown_164[0x2C];
  uint8_t unknown_190[0xA4];
  xe::be<uint32_t> package_id;
  xe::be<uint16_t> package_name[0x40];
  xe::be<uint32_t> unknown_2b8;
  xe::be<uint32_t> unknown_2bc;
  uint8_t unknown_2c0;
  uint8_t padding_2c1[3];
};
static_assert(sizeof(GuestEnumeratedAsset) == 0x2C4, "");
static_assert(offsetof(GuestEnumeratedAsset, type_mask) == 0x10, "");
static_assert(offsetof(GuestEnumeratedAsset, flags) == 0x18, "");
static_assert(offsetof(GuestEnumeratedAsset, colour_layout) == 0x1C, "");
static_assert(offsetof(GuestEnumeratedAsset, name) == 0xE4, "");
static_assert(offsetof(GuestEnumeratedAsset, package_id) == 0x234, "");
static_assert(offsetof(GuestEnumeratedAsset, package_name) == 0x238, "");

#pragma pack(pop)

constexpr uint32_t kEnumerationBatchRecords = 0x32;
constexpr uint32_t kEnumerationBatchBytes =
    kEnumerationBatchRecords * uint32_t(sizeof(GuestEnumeratedAsset));
static_assert(kEnumerationBatchBytes == 0x8A48, "");

constexpr uint32_t kEnumerationKindMask = 0x01FFFFFF;
constexpr uint32_t kEnumerationFlagUsable = 0x1;
constexpr uint32_t kEnumerationFlagHasVariant = 0x100;
constexpr uint32_t kEnumerationFlagVariantOffered = 0x200;
constexpr uint32_t kEnumerationFlagRecordField_32 = 0x400;
constexpr uint32_t kEnumerationFlagGenericName = 0x800;
constexpr uint32_t kEnumerationFlagRecordField_30 = 0x8;
constexpr uint32_t kDeduplicatedAssetKind = 2;

constexpr uint32_t kBodyTypeFirst = 1;
constexpr uint32_t kBodyTypeSecond = 2;

struct CategoryRule {
  uint32_t required;
  uint32_t optional;
  uint32_t category;
};

// Everything a record is constructed from, in the order the original's
// allocator forwards them.
struct AssetRecordFields {
  const uint8_t* asset_id;
  const uint8_t* field_10;
  uint32_t sort_key;
  uint32_t category;
  uint16_t type_mask;
  uint8_t body_type_primary;
  uint8_t body_type_fallback;
  uint8_t field_30;
  uint8_t field_31;
  uint8_t field_32;
  uint8_t field_33;
  uint32_t variant;
  const xe::be<uint32_t>* colours;
  const xe::be<uint16_t>* display_name;
};

std::recursive_mutex& CollectionLock();

class ComponentCollection {
 public:
  explicit ComponentCollection(const Guest& guest,
                               uint32_t address = kComponentCollectionAddress)
      : guest_(guest), address_(address) {}

  bool IsReady() const;
  bool CancelRequested() const;
  void RequestCancel();
  void ClearCancel();

  uint32_t user_index() const;
  uint32_t body_kind() const;

  void Build(uint32_t user_index, uint32_t body_kind);
  void Reset();
  void SeedEmptyChoicePerCategory();
  void EnumerateAvatarAssets();
  void FileRecordsIntoBuckets();
  void LinkVariantsToPackages();
  void AnnounceBuildFinished();

  uint32_t BucketEntryCount(uint32_t category) const;
  int BucketFind(uint32_t category, const uint8_t* asset_id) const;
  GuestAssetRecord* BucketEntryAt(uint32_t category, uint32_t index) const;

  uint32_t record_count() const;
  GuestAssetRecord* RecordAt(uint32_t index) const;
  uint32_t RecordAddress(uint32_t index) const;
  GuestAssetRecord* EmplaceRecord(const AssetRecordFields& fields);
  bool HoldsAssetId(const uint8_t* asset_id) const;

  uint32_t package_count() const;
  GuestPackage* PackageAt(uint32_t index) const;
  uint32_t FindPackageAddressById(uint32_t id) const;
  GuestPackage* EmplacePackage(uint32_t* out_address);
  uint32_t PackageMemberCount(const GuestPackage* package) const;
  GuestVariant* PackageMemberAt(const GuestPackage* package,
                                uint32_t index) const;

  uint32_t variant_count() const;
  GuestVariant* VariantAt(uint32_t index) const;
  GuestVariant* EmplaceVariant(uint32_t* out_address);

  bool AssetIdIsEmpty(const uint8_t* asset_id) const;
  const Guest& guest() const { return guest_; }
  uint32_t address() const { return address_; }

  static uint32_t CategoryForTypeMask(uint32_t type_mask);
  static uint32_t CategoryToTypeMask(uint32_t category);

 private:
  GuestComponentBucket* BucketAt(uint32_t category) const;
  void StoreInBucket(uint32_t index, GuestComponentBucket* bucket,
                     uint32_t record_address);
  void SortBucket(GuestComponentBucket* bucket) const;
  bool RecordSortsBefore(uint32_t left_address, uint32_t right_address) const;

  uint32_t Load32(uint32_t at) const { return guest_.Load32(address_ + at); }
  void Store32(uint32_t at, uint32_t value) const {
    guest_.Store32(address_ + at, value);
  }
  uint8_t Load8(uint32_t at) const { return guest_.Load8(address_ + at); }
  void Store8(uint32_t at, uint8_t value) const {
    guest_.Store8(address_ + at, value);
  }

  Guest guest_;
  uint32_t address_;
};

// The two .rdata constants the build reads, as host data.
const uint8_t* EmptyAssetId();
const xe::be<uint16_t>* EmptyWideString();

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_EDITOR_COMPONENT_COLLECTION_H_
