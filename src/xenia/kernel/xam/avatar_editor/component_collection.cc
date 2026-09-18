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

#include <algorithm>

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

namespace {

// Walk order is meaning: the lookup answers with the first row that fits.
const CategoryRule kCategoryRules[] = {
    {0x00000001, 0x00000000, 0x01}, {0x00000002, 0x00000000, 0x02},
    {0x00080000, 0x00000000, 0x04}, {0x00100000, 0x00000000, 0x03},
    {0x00200000, 0x00000000, 0x05}, {0x00002000, 0x00000000, 0x07},
    {0x00004000, 0x00000000, 0x08}, {0x00008000, 0x00000000, 0x06},
    {0x00010000, 0x00000000, 0x09}, {0x00040000, 0x00000000, 0x0A},
    {0x00020000, 0x00000000, 0x0B}, {0x00000004, 0x00000000, 0x0C},
    {0x00000008, 0x00000200, 0x0D}, {0x00000010, 0x00000000, 0x0E},
    {0x00000020, 0x00000000, 0x0F}, {0x00000040, 0x00800504, 0x10},
    {0x00000080, 0x00000A00, 0x11}, {0x00000100, 0x00000000, 0x12},
    {0x00000200, 0x00000000, 0x13}, {0x00000400, 0x00000000, 0x14},
    {0x00000800, 0x00000000, 0x15}, {0x00001000, 0x00000000, 0x16},
};

void CopyWideName(xe::be<uint16_t>* destination, uint32_t capacity,
                  const xe::be<uint16_t>* source) {
  uint32_t i = 0;
  if (source) {
    while (i + 1 < capacity && source[i] != 0) {
      destination[i] = source[i];
      ++i;
    }
  }
  destination[i] = 0;
}

int CompareWideNames(const xe::be<uint16_t>* left,
                     const xe::be<uint16_t>* right) {
  for (uint32_t i = 0;; ++i) {
    const uint16_t a = left[i];
    const uint16_t b = right[i];
    if (a != b) {
      return a < b ? -1 : 1;
    }
    if (!a) {
      return 0;
    }
  }
}

}  // namespace

const uint8_t* EmptyAssetId() {
  static const uint8_t kEmptyAssetId[kAssetIdBytes] = {};
  return kEmptyAssetId;
}

const xe::be<uint16_t>* EmptyWideString() {
  static const xe::be<uint16_t> kEmptyWideString[1] = {};
  return kEmptyWideString;
}

std::recursive_mutex& CollectionLock() {
  static std::recursive_mutex lock;
  return lock;
}

// sub_922169E0
uint32_t ComponentCollection::CategoryForTypeMask(uint32_t type_mask) {
  for (const CategoryRule& rule : kCategoryRules) {
    if (type_mask & ~(rule.required | rule.optional)) {
      continue;
    }
    if ((type_mask & rule.required) == rule.required) {
      return rule.category;
    }
  }

  // The strict pass above refuses a mask carrying ANY bit a rule does not
  // name, which a whole asset kind almost always does - and the kind is what
  // has to be passed, or chin, nose and ears arrive with no category at all.
  // So a second pass files it under the first rule whose own bits it carries,
  // keeping the table's order as the priority it already is. Without this
  // every multi-bit kind ended up in the fallback bucket or nowhere: in the
  // last run categories 1 to 5 - which are 0x1, 0x2, chin, nose and ears -
  // held nothing, and twenty-three collected the outfits.
  for (const CategoryRule& rule : kCategoryRules) {
    if (rule.required && (type_mask & rule.required) == rule.required) {
      return rule.category;
    }
  }

  if (type_mask && !(type_mask & 0xFFFFF003 & ~kCategoryRetryBit)) {
    return kCategoryFallback;
  }
  return 0;
}

// sub_92216988
uint32_t ComponentCollection::CategoryToTypeMask(uint32_t category) {
  for (const CategoryRule& rule : kCategoryRules) {
    if (rule.category == category) {
      return rule.required;
    }
  }
  return category == kCategoryFallback ? kCategoryRetryBit : 0;
}

// sub_92216938
bool ComponentCollection::IsReady() const {
  std::unique_lock<std::recursive_mutex> lock(CollectionLock(),
                                              std::try_to_lock);
  // A build in flight owns the lock, and the original answers "not ready"
  // rather than waiting for it.
  if (!lock.owns_lock()) {
    return false;
  }
  return Load8(kReadyAt) != 0;
}

bool ComponentCollection::CancelRequested() const {
  return Load8(kCancelAt) != 0;
}

void ComponentCollection::RequestCancel() { Store8(kCancelAt, 1); }

void ComponentCollection::ClearCancel() { Store8(kCancelAt, 0); }

uint32_t ComponentCollection::user_index() const {
  return Load32(kUserIndexAt);
}

uint32_t ComponentCollection::body_kind() const { return Load32(kBodyKindAt); }

// sub_92217058
void ComponentCollection::Reset() {
  Store8(kReadyAt, 0);
  uint8_t* base = guest_.At<uint8_t>(address_);
  if (!base) {
    return;
  }
  memset(base + kRecordsAt, 0, kRecordCountAt + 4 - kRecordsAt);
  memset(base + kPackagesAt, 0, kPackageCountAt + 4 - kPackagesAt);
  memset(base + kVariantsAt, 0, kVariantCountAt + 4 - kVariantsAt);
  memset(base + kVariantArenaAt, 0, kVariantArenaUsedAt + 4 - kVariantArenaAt);
  memset(base + kBucketArenaAt, 0, kBucketArenaUsedAt + 4 - kBucketArenaAt);
  memset(base + kBucketsAt, 0, kComponentBucketCount * kBucketBytes);
}

bool ComponentCollection::AssetIdIsEmpty(const uint8_t* asset_id) const {
  return memcmp(asset_id, EmptyAssetId(), kAssetIdBytes) == 0;
}

// Every count below is a guest dword, so every one of them is clamped to the
// pool it indexes before anything walks that far.
uint32_t ComponentCollection::record_count() const {
  return std::min(Load32(kRecordCountAt), kRecordCapacity);
}

uint32_t ComponentCollection::RecordAddress(uint32_t index) const {
  return address_ + kRecordsAt + index * kRecordBytes;
}

GuestAssetRecord* ComponentCollection::RecordAt(uint32_t index) const {
  return index < kRecordCapacity
             ? guest_.At<GuestAssetRecord>(RecordAddress(index))
             : nullptr;
}

bool ComponentCollection::HoldsAssetId(const uint8_t* asset_id) const {
  const uint32_t count = record_count();
  for (uint32_t i = 0; i < count; ++i) {
    const GuestAssetRecord* record = RecordAt(i);
    if (record && memcmp(record->asset_id, asset_id, kAssetIdBytes) == 0) {
      return true;
    }
  }
  return false;
}

// sub_92218428 and sub_922182E8
GuestAssetRecord* ComponentCollection::EmplaceRecord(
    const AssetRecordFields& fields) {
  const uint32_t count = record_count();
  if (count >= kRecordCapacity) {
    return nullptr;
  }
  GuestAssetRecord* record = RecordAt(count);
  if (!record) {
    return nullptr;
  }
  Store32(kRecordCountAt, count + 1);

  memcpy(record->asset_id, fields.asset_id, kAssetIdBytes);
  if (fields.field_10) {
    memcpy(record->field_10, fields.field_10, kAssetIdBytes);
  } else {
    memset(record->field_10, 0, kAssetIdBytes);
  }
  record->sort_key = fields.sort_key;
  record->category = fields.category;
  record->type_mask = fields.type_mask;
  record->body_type_primary = fields.body_type_primary;
  record->padding_2b = 0;

  const uint8_t body_source =
      (fields.body_type_primary == 1 || fields.body_type_primary == 2)
          ? fields.body_type_primary
          : fields.body_type_fallback;
  record->body_selector = body_source == 1 ? 0 : (body_source == 2 ? 2 : 1);

  record->field_30 = fields.field_30;
  record->field_31 = fields.field_31;
  record->field_32 = fields.field_32;
  record->field_33 = fields.field_33;
  record->variant = fields.variant;

  if (fields.colours) {
    for (uint32_t i = 0; i < kRecordColourCount; ++i) {
      record->colours[i] = fields.colours[i];
    }
  } else {
    memset(record->colours, 0, sizeof(record->colours));
  }
  CopyWideName(record->display_name, 0x40, fields.display_name);

  GuestVariant* variant = guest_.At<GuestVariant>(fields.variant);
  if (variant) {
    variant->record = RecordAddress(count);
  }
  return record;
}

uint32_t ComponentCollection::variant_count() const {
  return std::min(Load32(kVariantCountAt), kVariantCapacity);
}

GuestVariant* ComponentCollection::VariantAt(uint32_t index) const {
  return index < kVariantCapacity
             ? guest_.At<GuestVariant>(address_ + kVariantsAt +
                                       index * kVariantBytes)
             : nullptr;
}

GuestVariant* ComponentCollection::EmplaceVariant(uint32_t* out_address) {
  const uint32_t count = variant_count();
  if (count >= kVariantCapacity) {
    return nullptr;
  }
  const uint32_t variant_address =
      address_ + kVariantsAt + count * kVariantBytes;
  GuestVariant* variant = guest_.At<GuestVariant>(variant_address);
  if (!variant) {
    return nullptr;
  }
  Store32(kVariantCountAt, count + 1);
  memset(variant, 0, sizeof(*variant));
  *out_address = variant_address;
  return variant;
}

uint32_t ComponentCollection::package_count() const {
  return std::min(Load32(kPackageCountAt), kPackageCapacity);
}

GuestPackage* ComponentCollection::PackageAt(uint32_t index) const {
  std::lock_guard<std::recursive_mutex> lock(CollectionLock());
  if (index >= package_count()) {
    return nullptr;
  }
  return guest_.At<GuestPackage>(
      guest_.Load32(address_ + kPackageIndexAt + index * 4));
}

uint32_t ComponentCollection::FindPackageAddressById(uint32_t id) const {
  const uint32_t count = package_count();
  for (uint32_t i = count; i-- > 0;) {
    const uint32_t package_address =
        guest_.Load32(address_ + kPackageIndexAt + i * 4);
    const GuestPackage* package = guest_.At<GuestPackage>(package_address);
    if (package && package->id == id) {
      return package_address;
    }
  }
  return 0;
}

GuestPackage* ComponentCollection::EmplacePackage(uint32_t* out_address) {
  const uint32_t count = package_count();
  if (count >= kPackageCapacity) {
    return nullptr;
  }
  const uint32_t package_address =
      address_ + kPackagesAt + count * kPackageBytes;
  GuestPackage* package = guest_.At<GuestPackage>(package_address);
  if (!package) {
    return nullptr;
  }
  guest_.Store32(address_ + kPackageIndexAt + count * 4, package_address);
  Store32(kPackageCountAt, count + 1);
  memset(package, 0, sizeof(*package));
  *out_address = package_address;
  return package;
}

// sub_922173E8
uint32_t ComponentCollection::PackageMemberCount(
    const GuestPackage* package) const {
  return package ? uint32_t(package->member_count) : 0;
}

// sub_922173F0
GuestVariant* ComponentCollection::PackageMemberAt(const GuestPackage* package,
                                                   uint32_t index) const {
  if (!package || index >= uint32_t(package->member_count)) {
    return nullptr;
  }
  const uint32_t members = package->members;
  if (!members) {
    return nullptr;
  }
  return guest_.At<GuestVariant>(guest_.Load32(members + index * 4));
}

GuestComponentBucket* ComponentCollection::BucketAt(uint32_t category) const {
  return category < kComponentBucketCount
             ? guest_.At<GuestComponentBucket>(address_ + kBucketsAt +
                                               category * kBucketBytes)
             : nullptr;
}

// sub_92217138
uint32_t ComponentCollection::BucketEntryCount(uint32_t category) const {
  std::lock_guard<std::recursive_mutex> lock(CollectionLock());
  const GuestComponentBucket* bucket = BucketAt(category);
  return bucket ? uint32_t(bucket->count) : 0;
}

GuestAssetRecord* ComponentCollection::BucketEntryAt(uint32_t category,
                                                     uint32_t index) const {
  std::lock_guard<std::recursive_mutex> lock(CollectionLock());
  const GuestComponentBucket* bucket = BucketAt(category);
  if (!bucket || index >= uint32_t(bucket->count) || !bucket->items) {
    return nullptr;
  }
  return guest_.At<GuestAssetRecord>(
      guest_.Load32(uint32_t(bucket->items) + index * 4));
}

// sub_92217188
int ComponentCollection::BucketFind(uint32_t category,
                                    const uint8_t* asset_id) const {
  std::lock_guard<std::recursive_mutex> lock(CollectionLock());
  const GuestComponentBucket* bucket = BucketAt(category);
  if (!bucket || !bucket->count || !bucket->items) {
    return -1;
  }
  const uint32_t count = bucket->count;
  const uint32_t items = bucket->items;
  for (uint32_t i = 0; i < count; ++i) {
    const GuestAssetRecord* record =
        guest_.At<GuestAssetRecord>(guest_.Load32(items + i * 4));
    if (record && memcmp(record->asset_id, asset_id, kAssetIdBytes) == 0) {
      return int(i);
    }
  }
  return -1;
}

// sub_922182A0
void ComponentCollection::StoreInBucket(uint32_t index,
                                        GuestComponentBucket* bucket,
                                        uint32_t record_address) {
  if (!bucket || !bucket->items || index >= uint32_t(bucket->count) ||
      !record_address) {
    return;
  }
  guest_.Store32(uint32_t(bucket->items) + index * 4, record_address);
  const GuestAssetRecord* record = guest_.At<GuestAssetRecord>(record_address);
  if (record && record->field_31) {
    bucket->field_8 = uint32_t(bucket->field_8) + 1;
  }
}

// sub_922184C0
bool ComponentCollection::RecordSortsBefore(uint32_t left_address,
                                            uint32_t right_address) const {
  const GuestAssetRecord* left = guest_.At<GuestAssetRecord>(left_address);
  const GuestAssetRecord* right = guest_.At<GuestAssetRecord>(right_address);
  if (!left || !right) {
    return false;
  }
  const bool left_is_empty = AssetIdIsEmpty(left->asset_id);
  const bool right_is_empty = AssetIdIsEmpty(right->asset_id);
  if (left_is_empty != right_is_empty) {
    return left_is_empty;
  }
  if (left->sort_key != right->sort_key) {
    return uint32_t(left->sort_key) < uint32_t(right->sort_key);
  }
  return CompareWideNames(left->display_name, right->display_name) < 0;
}

// sub_92218598
void ComponentCollection::SortBucket(GuestComponentBucket* bucket) const {
  if (!bucket) {
    return;
  }
  const uint32_t count = bucket->count;
  const uint32_t items = bucket->items;
  if (!count || !items) {
    return;
  }
  xe::be<uint32_t>* entries = guest_.At<xe::be<uint32_t>>(items);
  if (!entries) {
    return;
  }
  std::sort(
      entries, entries + count,
      [this](const xe::be<uint32_t>& left, const xe::be<uint32_t>& right) {
        return RecordSortsBefore(uint32_t(left), uint32_t(right));
      });
}

// sub_92216B70
void ComponentCollection::FileRecordsIntoBuckets() {
  for (uint32_t pass = 0; pass < 2; ++pass) {
    if (CancelRequested()) {
      return;
    }
    uint32_t filed[kComponentBucketCount] = {};
    const uint32_t count = record_count();
    for (uint32_t i = 0; i < count; ++i) {
      GuestAssetRecord* record = RecordAt(i);
      if (!record) {
        continue;
      }
      const GuestVariant* variant =
          guest_.At<GuestVariant>(uint32_t(record->variant));
      if (variant && (!variant->field_10A || !record->field_32)) {
        continue;
      }
      const uint32_t category = record->category;
      if (category >= kComponentBucketCount) {
        continue;
      }
      if (pass == 1) {
        StoreInBucket(filed[category], BucketAt(category), RecordAddress(i));
      }
      ++filed[category];
    }

    if (pass == 0) {
      for (uint32_t category = 0; category < kComponentBucketCount;
           ++category) {
        uint32_t wanted = filed[category];
        if (!wanted) {
          continue;
        }
        const uint32_t used = Load32(kBucketArenaUsedAt);
        if (wanted > kRecordCapacity - used) {
          wanted = kRecordCapacity - used;
        }
        GuestComponentBucket* bucket = BucketAt(category);
        if (!bucket) {
          continue;
        }
        bucket->count = wanted;
        bucket->items = address_ + kBucketArenaAt + used * 4;
        Store32(kBucketArenaUsedAt, used + wanted);
      }
    } else {
      for (uint32_t category = 0; category < kComponentBucketCount;
           ++category) {
        SortBucket(BucketAt(category));
      }
    }
  }
}

// sub_92216CF8
void ComponentCollection::LinkVariantsToPackages() {
  const uint32_t packages = package_count();
  const uint32_t variants = variant_count();
  for (uint32_t p = 0; p < packages; ++p) {
    if (CancelRequested()) {
      return;
    }
    const uint32_t package_address = address_ + kPackagesAt + p * kPackageBytes;
    GuestPackage* package = guest_.At<GuestPackage>(package_address);
    if (!package) {
      continue;
    }
    // Written before anything is gathered, so an empty package still points at
    // the arena's high-water mark with no members.
    package->members =
        address_ + kVariantArenaAt + Load32(kVariantArenaUsedAt) * 4;
    for (uint32_t v = 0; v < variants; ++v) {
      const uint32_t variant_address =
          address_ + kVariantsAt + v * kVariantBytes;
      const GuestVariant* variant = guest_.At<GuestVariant>(variant_address);
      if (!variant || uint32_t(variant->package) != package_address) {
        continue;
      }
      const uint32_t used = Load32(kVariantArenaUsedAt);
      if (used >= kVariantCapacity) {
        continue;
      }
      guest_.Store32(address_ + kVariantArenaAt + used * 4, variant_address);
      package->member_count = uint32_t(package->member_count) + 1;
      Store32(kVariantArenaUsedAt, used + 1);
    }
  }
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
