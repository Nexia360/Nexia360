/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_devices/stfs_container_device.h"

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/vfs/devices/xcontent_devices/stfs_container_entry.h"

namespace xe {
namespace vfs {

StfsContainerDevice::StfsContainerDevice(const std::string_view mount_path,
                                         const std::filesystem::path& host_path)
    : XContentContainerDevice(mount_path, host_path),
      blocks_per_hash_table_(1),
      block_step_{0, 0} {
  SetName("STFS");
}

StfsContainerDevice::~StfsContainerDevice() {}

bool StfsContainerDevice::Initialize() {
  if (backing_data_.empty()) {
    return XContentContainerDevice::Initialize();
  }

  // Memory-backed: the package is a blob we already hold (see
  // SetBackingData), so there is no host file to open or stat.
  if (backing_data_.size() < sizeof(XContentContainerHeader)) {
    return false;
  }

  auto header = std::make_unique<XContentContainerHeader>();
  std::memcpy(header.get(), backing_data_.data(),
              sizeof(XContentContainerHeader));
  if (!header->content_header.is_magic_valid()) {
    return false;
  }
  header_ = std::move(header);
  SetFilesSize(backing_data_.size());

  SetupContainer();

  if (LoadHostFiles() != Result::kSuccess) {
    return false;
  }

  return Read() == Result::kSuccess;
}

void StfsContainerDevice::SetupContainer() {
  // Additional part specific to STFS container.
  const XContentContainerHeader* header = GetContainerHeader();
  blocks_per_hash_table_ = header->is_package_readonly() ? 1 : 2;

  block_step_[0] = kBlocksPerHashLevel[0] + blocks_per_hash_table_;
  block_step_[1] = kBlocksPerHashLevel[1] +
                   ((kBlocksPerHashLevel[0] + 1) * blocks_per_hash_table_);
}

XContentContainerDevice::Result StfsContainerDevice::LoadHostFiles() {
  const XContentContainerHeader* header = GetContainerHeader();

  if (header->content_metadata.data_file_count > 0) {
    XELOGW("STFS container is not a single file. Loading might fail!");
  }

  if (!backing_data_.empty()) {
    // Non-owning view over our own buffer; backing_data_ outlives data_.
    data_ = std::unique_ptr<MappedMemory>(
        new MappedMemory(backing_data_.data(), backing_data_.size()));
    return Result::kSuccess;
  }

  data_ = MappedMemory::Open(host_path_, MappedMemory::Mode::kRead);
  if (!data_) {
    return Result::kOutOfMemory;
  }

  return Result::kSuccess;
}

StfsContainerDevice::Result StfsContainerDevice::Read() {
  auto root_entry = new StfsContainerEntry(this, nullptr, "", data_.get());
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  std::vector<StfsContainerEntry*> all_entries;

  // Load all listings.
  StfsDirectoryBlock directory;

  auto& descriptor =
      GetContainerHeader()->content_metadata.volume_descriptor.stfs;
  uint32_t table_block_index = descriptor.file_table_block_number();
  size_t n = 0;
  for (n = 0; n < descriptor.file_table_block_count; n++) {
    const size_t offset = BlockToOffset(table_block_index);
    directory = *reinterpret_cast<StfsDirectoryBlock*>(data_->data() + offset);

    for (size_t m = 0; m < kEntriesPerDirectoryBlock; m++) {
      const StfsDirectoryEntry& dir_entry = directory.entries[m];

      if (dir_entry.name[0] == 0) {
        // Done.
        break;
      }

      StfsContainerEntry* parent_entry =
          dir_entry.directory_index == 0xFFFF
              ? root_entry
              : all_entries[dir_entry.directory_index];

      std::unique_ptr<StfsContainerEntry> entry =
          ReadEntry(parent_entry, &dir_entry);
      all_entries.push_back(entry.get());
      parent_entry->children_.emplace_back(std::move(entry));
    }

    const StfsHashEntry* block_hash = GetBlockHash(table_block_index);
    table_block_index = block_hash->level0_next_block();
    if (table_block_index == kEndOfChain) {
      break;
    }
  }

  if (n + 1 != descriptor.file_table_block_count) {
    XELOGW("STFS read {} file table blocks, but STFS headers expected {}!",
           n + 1, descriptor.file_table_block_count);
    assert_always();
  }

  if (allow_nested_mount_) {
    MountNestedNxeArt();
  }

  return Result::kSuccess;
}

bool StfsContainerDevice::ReadEntryBytes(StfsContainerEntry* entry,
                                         std::vector<uint8_t>& out) {
  if (!entry || !entry->data()) {
    return false;
  }

  const uint8_t* base = entry->data()->data();
  const size_t mapped_size = entry->data()->size();

  out.resize(entry->size());
  size_t written = 0;
  for (const auto& record : entry->block_list()) {
    if (written + record.length > out.size() ||
        record.offset + record.length > mapped_size) {
      return false;
    }
    std::memcpy(out.data() + written, base + record.offset, record.length);
    written += record.length;
  }

  return written == out.size();
}

void StfsContainerDevice::InjectMemoryEntry(const std::string_view name,
                                            std::vector<uint8_t> bytes,
                                            const Entry* timestamps) {
  auto blob = std::make_unique<std::vector<uint8_t>>(std::move(bytes));
  auto map = std::unique_ptr<MappedMemory>(
      new MappedMemory(blob->data(), blob->size()));

  auto* root = static_cast<StfsContainerEntry*>(root_entry_.get());
  auto entry = StfsContainerEntry::Create(this, root, name, map.get());
  entry->attributes_ = kFileAttributeNormal | kFileAttributeReadOnly;
  entry->size_ = blob->size();
  entry->allocation_size_ = xe::round_up(blob->size(), kBlockSize);
  entry->data_offset_ = 0;
  entry->data_size_ = blob->size();
  // The whole file is one contiguous run at offset 0 of its own mapping, so
  // StfsContainerFile::Read lands exactly on the bytes.
  entry->block_list_.push_back({0, 0, blob->size()});
  if (timestamps) {
    entry->create_timestamp_ = timestamps->create_timestamp();
    entry->write_timestamp_ = timestamps->write_timestamp();
    entry->access_timestamp_ = timestamps->access_timestamp();
  }

  root->children_.emplace_back(std::move(entry));
  injected_maps_.emplace_back(std::move(map));
  injected_blobs_.emplace_back(std::move(blob));
}

void StfsContainerDevice::MountNestedNxeArt() {
  if (!root_entry_) {
    return;
  }

  // Only step in when the package has no tile image the dashboard can open.
  // A package that ships game.png is already fine.
  static const char* const kTileNames[] = {"game.png", "gametile.png",
                                           "game_tile.png", "nxetile.png"};
  for (const char* tile_name : kTileNames) {
    if (root_entry_->GetChild(tile_name)) {
      return;
    }
  }

  auto* nxeart = root_entry_->GetChild("nxeart");
  if (!nxeart || (nxeart->attributes() & kFileAttributeDirectory)) {
    return;
  }

  std::vector<uint8_t> package;
  if (!ReadEntryBytes(static_cast<StfsContainerEntry*>(nxeart), package)) {
    XELOGW("XContentContainer: could not read nxeart out of {}", host_path_);
    return;
  }

  if (package.size() < sizeof(XContentContainerHeader) ||
      (std::memcmp(package.data(), "LIVE", 4) != 0 &&
       std::memcmp(package.data(), "CON ", 4) != 0 &&
       std::memcmp(package.data(), "PIRS", 4) != 0)) {
    // Some titles really do ship a plain image called nxeart. Leave it be.
    return;
  }

  auto nested = std::make_unique<StfsContainerDevice>("", host_path_);
  nested->set_allow_nested_mount(false);
  nested->SetBackingData(std::move(package));
  if (!nested->Initialize() || !nested->root_entry_) {
    XELOGW("XContentContainer: nxeart in {} did not mount", host_path_);
    return;
  }

  // nxeslot.jpg is the slot/tile art; nxebg.jpg is the full-screen
  // background, which is not what a tile wants.
  static const char* const kNestedTileNames[] = {"nxeslot.jpg", "nxeslot.png",
                                                 "nxetile.jpg", "nxetile.png"};
  for (const char* nested_name : kNestedTileNames) {
    auto* art = nested->root_entry_->GetChild(nested_name);
    if (!art || (art->attributes() & kFileAttributeDirectory)) {
      continue;
    }

    std::vector<uint8_t> bytes;
    if (!ReadEntryBytes(static_cast<StfsContainerEntry*>(art), bytes)) {
      continue;
    }

    // Named game.png because that is what ArcadeInfo.xml asks for; the
    // dashboard's image loader goes by content, not by extension.
    InjectMemoryEntry("game.png", std::move(bytes), nxeart);
    XELOGI("XContentContainer: serving nxeart\\{} as game.png ({} bytes)",
           nested_name, art->size());
    return;
  }

  XELOGW("XContentContainer: nxeart in {} holds no tile image", host_path_);
}

std::unique_ptr<StfsContainerEntry> StfsContainerDevice::ReadEntry(
    Entry* parent, const StfsDirectoryEntry* dir_entry) {
  // Filename is stored as Windows-1252, convert it to UTF-8.
  std::string ansi_name(reinterpret_cast<const char*>(dir_entry->name),
                        dir_entry->flags.name_length & 0x3F);
  std::string name = xe::win1252_to_utf8(ansi_name);
  // Fallback to normal name if for whatever reason conversion from 1252 code
  // page failed.
  if (name.empty()) {
    name = ansi_name;
  }

  auto entry = StfsContainerEntry::Create(this, parent, name, data_.get());

  if (dir_entry->flags.directory) {
    entry->attributes_ = kFileAttributeDirectory;
  } else {
    entry->attributes_ = kFileAttributeNormal | kFileAttributeReadOnly;
    entry->data_offset_ = BlockToOffset(dir_entry->start_block_number());
    entry->data_size_ = dir_entry->length;
  }
  entry->size_ = dir_entry->length;
  entry->allocation_size_ = xe::round_up(dir_entry->length, kBlockSize);

  entry->create_timestamp_ =
      decode_fat_timestamp(dir_entry->create_date, dir_entry->create_time);
  entry->write_timestamp_ =
      decode_fat_timestamp(dir_entry->modified_date, dir_entry->modified_time);
  entry->access_timestamp_ = entry->write_timestamp_;

  // Fill in all block records.
  // It's easier to do this now and just look them up later, at the cost
  // of some memory. Nasty chain walk.
  // TODO(benvanik): optimize if flags.contiguous is set.
  if (entry->attributes() & X_FILE_ATTRIBUTE_NORMAL) {
    uint32_t block_index = dir_entry->start_block_number();
    size_t remaining_size = dir_entry->length;
    while (remaining_size && block_index != kEndOfChain) {
      size_t block_size =
          std::min(static_cast<size_t>(kBlockSize), remaining_size);
      size_t offset = BlockToOffset(block_index);
      entry->block_list_.push_back({0, offset, block_size});
      remaining_size -= block_size;
      auto block_hash = GetBlockHash(block_index);
      block_index = block_hash->level0_next_block();
    }

    if (remaining_size) {
      // Loop above must have exited prematurely, bad hash tables?
      XELOGW(
          "STFS file {} only found {} bytes for file, expected {} ({} "
          "bytes missing)",
          name, dir_entry->length.get() - remaining_size,
          dir_entry->length.get(), remaining_size);
      assert_always();
    }

    // Check that the number of blocks retrieved from hash entries matches
    // the block count read from the file entry
    if (entry->block_list_.size() != dir_entry->allocated_data_blocks()) {
      XELOGW(
          "STFS failed to read correct block-chain for entry {}, read {} "
          "blocks, expected {}",
          entry->name_, entry->block_list_.size(),
          dir_entry->allocated_data_blocks());
      assert_always();
    }
  }

  return entry;
}

size_t StfsContainerDevice::BlockToOffset(uint64_t block_index) const {
  // For every level there is a hash table
  // Level 0: hash table of next 170 blocks
  // Level 1: hash table of next 170 hash tables
  // Level 2: hash table of next 170 level 1 hash tables
  // And so on...
  uint64_t block = block_index;
  for (uint32_t i = 0; i < kBlocksHashLevelAmount; i++) {
    const uint32_t level_base = kBlocksPerHashLevel[i];
    block += ((block_index + level_base) / level_base) * blocks_per_hash_table_;
    if (block_index < level_base) {
      break;
    }
  }

  return xe::round_up(GetContainerHeader()->content_header.header_size,
                      kBlockSize) +
         (block << 12);
}

uint32_t StfsContainerDevice::BlockToHashBlockNumber(
    uint32_t block_index, uint32_t hash_level) const {
  if (hash_level == 2) {
    return block_step_[1];
  }

  if (block_index < kBlocksPerHashLevel[hash_level]) {
    return hash_level == 0 ? 0 : block_step_[hash_level - 1];
  }

  uint32_t block =
      (block_index / kBlocksPerHashLevel[hash_level]) * block_step_[hash_level];

  if (hash_level == 0) {
    block +=
        ((block_index / kBlocksPerHashLevel[1]) + 1) * blocks_per_hash_table_;

    if (block_index < kBlocksPerHashLevel[1]) {
      return block;
    }
  }

  return block + blocks_per_hash_table_;
}

size_t StfsContainerDevice::BlockToHashBlockOffset(uint32_t block_index,
                                                   uint32_t hash_level) const {
  const uint64_t block = BlockToHashBlockNumber(block_index, hash_level);
  return xe::round_up(header_->content_header.header_size, kBlockSize) +
         (block << 12);
}

const uint8_t StfsContainerDevice::GetAmountOfHashLevelsToCheck(
    uint32_t total_block_count) const {
  for (uint8_t level = 0; level < kBlocksHashLevelAmount; level++) {
    if (total_block_count < kBlocksPerHashLevel[level]) {
      return level;
    }
  }
  XELOGE("GetAmountOfHashLevelsToCheck - Invalid total_block_count: {}",
         total_block_count);
  return 0;
}

void StfsContainerDevice::UpdateCachedHashTable(
    uint32_t block_index, uint8_t hash_level,
    uint32_t& secondary_table_offset) {
  const size_t hash_offset = BlockToHashBlockOffset(block_index, hash_level);
  // Do nothing. It's already there.
  if (!cached_hash_tables_.count(hash_offset)) {
    cached_hash_tables_[hash_offset] = *reinterpret_cast<StfsHashTable*>(
        data_->data() + hash_offset + secondary_table_offset);
  }

  uint32_t record = block_index % kBlocksPerHashLevel[0];
  if (hash_level >= 1) {
    record = (block_index / kBlocksPerHashLevel[hash_level - 1]) %
             kBlocksPerHashLevel[0];
  }
  const StfsHashEntry* record_data =
      &cached_hash_tables_[hash_offset].entries[record];
  secondary_table_offset = record_data->levelN_active_index() ? kBlockSize : 0;
}

void StfsContainerDevice::UpdateCachedHashTables(
    uint32_t block_index, uint8_t highest_hash_level_to_update,
    uint32_t& secondary_table_offset) {
  for (int8_t level = highest_hash_level_to_update; level >= 0; level--) {
    UpdateCachedHashTable(block_index, level, secondary_table_offset);
  }
}

const StfsHashEntry* StfsContainerDevice::GetBlockHash(uint32_t block_index) {
  const StfsVolumeDescriptor& descriptor =
      header_->content_metadata.volume_descriptor.stfs;

  // Offset for selecting the secondary hash block, in packages that have them
  uint32_t secondary_table_offset =
      descriptor.flags.bits.root_active_index ? kBlockSize : 0;

  uint8_t hash_levels_to_process =
      GetAmountOfHashLevelsToCheck(descriptor.total_block_count);

  if (header_->is_package_readonly()) {
    secondary_table_offset = 0;
    // Because we have read only package we only need to check first hash level.
    hash_levels_to_process = 0;
  }

  UpdateCachedHashTables(block_index, hash_levels_to_process,
                         secondary_table_offset);

  const size_t hash_offset = BlockToHashBlockOffset(block_index, 0);
  const uint32_t record = block_index % kBlocksPerHashLevel[0];
  return &cached_hash_tables_[hash_offset].entries[record];
}

const uint8_t StfsContainerDevice::GetBlocksPerHashTableFromContainerHeader()
    const {
  const XContentContainerHeader* header = GetContainerHeader();
  if (!header) {
    XELOGE(
        "VFS: SetBlocksPerHashTableBasedOnContainerHeader - Missing "
        "Container "
        "Header!");
    return 0;
  }

  if (header->is_package_readonly()) {
    return 1;
  }

  return 2;
}

}  // namespace vfs
}  // namespace xe
