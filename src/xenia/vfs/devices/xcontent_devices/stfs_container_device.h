/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_XCONTENT_DEVICES_STFS_CONTAINER_DEVICE_H_
#define XENIA_VFS_DEVICES_XCONTENT_DEVICES_STFS_CONTAINER_DEVICE_H_

#include <memory>
#include <unordered_map>
#include <vector>

#include "xenia/kernel/util/xex2_info.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/devices/xcontent_devices/stfs_container_entry.h"

namespace xe {
namespace vfs {

// https://free60.org/System-Software/Formats/STFS/

class StfsContainerDevice : public XContentContainerDevice {
 public:
  StfsContainerDevice(const std::string_view mount_path,
                      StfsVolumeDescriptor* descriptor,
                      std::unique_ptr<MappedMemory> data);
  ~StfsContainerDevice() override;

  void set_allow_nested_mount(bool allow) { allow_nested_mount_ = allow; }

  bool is_read_only() const override {
    return descriptor_->flags.bits.read_only_format;
  }
  bool is_io_queued() const override { return true; }

  uint32_t component_name_max_length() const override { return 40; }

  uint32_t total_allocation_units() const override {
    return descriptor_->total_block_count;
  }
  uint32_t available_allocation_units() const override {
    if (!is_read_only()) {
      return kBlocksPerHashLevel[2] -
             (descriptor_->total_block_count - descriptor_->free_block_count);
    }
    return 0;
  }

 private:
  static const uint8_t kBlocksHashLevelAmount = 3;
  const uint32_t kBlocksPerHashLevel[kBlocksHashLevelAmount] = {170, 28900,
                                                                4913000};
  const uint32_t kEndOfChain = 0xFFFFFF;
  const uint32_t kEntriesPerDirectoryBlock =
      kBlockSize / sizeof(StfsDirectoryEntry);

  Result LoadHostFiles(const size_t data_file_count) override;

  Result Read() override;
  std::unique_ptr<StfsContainerEntry> ReadEntry(
      Entry* parent, const StfsDirectoryEntry* dir_entry);

  size_t BlockToOffset(uint64_t block_index) const;
  uint32_t BlockToHashBlockNumber(uint32_t block_index,
                                  uint32_t hash_level) const;
  size_t BlockToHashBlockOffset(uint32_t block_index,
                                uint32_t hash_level) const;
  const uint8_t GetAmountOfHashLevelsToCheck(uint32_t total_block_count) const;

  const StfsHashEntry* GetBlockHash(uint32_t block_index);
  void UpdateCachedHashTable(uint32_t block_index, uint8_t hash_level,
                             uint32_t& secondary_table_offset);
  void UpdateCachedHashTables(uint32_t block_index,
                              uint8_t highest_hash_level_to_update,
                              uint32_t& secondary_table_offset);

  // Copies a whole entry out of this package by walking its block list.
  static bool ReadEntryBytes(StfsContainerEntry* entry,
                             std::vector<uint8_t>& out);
  // If the package carries `nxeart` but no tile image of its own, mounts
  // nxeart and grafts the artwork inside it into the root under a name the
  // dashboard will ask for.
  void MountNestedNxeArt();
  // Adds a synthetic file to the root whose contents are `bytes`. The buffer
  // and its mapping are owned by this device, so the entry stays valid for as
  // long as the package is mounted.
  void InjectMemoryEntry(const std::string_view name,
                         std::vector<uint8_t> bytes, const Entry* timestamps);

  uint8_t blocks_per_hash_table_;
  uint32_t block_step_[2];

  std::unordered_map<size_t, StfsHashTable> cached_hash_tables_;

  std::unique_ptr<MappedMemory> data_;
  StfsVolumeDescriptor* descriptor_;

  // False on a nested device, so a package inside a package cannot recurse.
  bool allow_nested_mount_ = true;
  // Storage for entries grafted in by MountNestedNxeArt. Both vectors must
  // outlive the entries that point into them.
  std::vector<std::unique_ptr<std::vector<uint8_t>>> injected_blobs_;
  std::vector<std::unique_ptr<MappedMemory>> injected_maps_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_XCONTENT_DEVICES_STFS_CONTAINER_DEVICE_H_
