/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_container_device.h"

#include <cstdio>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/utf8.h"

namespace xe {
namespace vfs {

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path, const size_t data_file_count)
    : Device(mount_path),
      name_("XContent"),
      data_file_count_(data_file_count),
      files_total_size_(0) {}

XContentContainerDevice::~XContentContainerDevice() {}

std::unique_ptr<kernel::xam::XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(
    const std::filesystem::path& file_path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file_path, ec) ||
      std::filesystem::file_size(file_path, ec) <
          sizeof(kernel::xam::XContentContainerHeader)) {
    return nullptr;
  }
  FILE* file = xe::filesystem::OpenFile(file_path, "rb");
  if (!file) {
    return nullptr;
  }
  auto header = std::make_unique<kernel::xam::XContentContainerHeader>();
  const bool read = fread(header.get(), sizeof(*header), 1, file) == 1;
  fclose(file);
  return read ? std::move(header) : nullptr;
}

bool XContentContainerDevice::Initialize() {
  if (LoadHostFiles(data_file_count_) != Result::kSuccess) {
    XELOGE("Error loading XContent host files.");
    return false;
  }

  if (Read() != Result::kSuccess) {
    return false;
  }

  return true;
}

static const char* const kTileImageAliases[] = {
    "game.png",
    "gametile.png",
    "game_tile.png",
    "nxetile.png",
};

static bool IsTileImageName(const std::string_view name) {
  const std::string lower = xe::utf8::lower_ascii(name);
  for (const char* alias : kTileImageAliases) {
    if (lower == alias) {
      return true;
    }
  }
  return false;
}

Entry* ResolveTileImageAlias(Entry* parent, const std::string_view requested) {
  if (!parent || !IsTileImageName(requested)) {
    return nullptr;
  }
  for (const char* alias : kTileImageAliases) {
    Entry* entry = parent->GetChild(alias);
    if (entry) {
      XELOGW("XContentContainer: '{}' not in package, serving '{}' instead",
             requested, alias);
      return entry;
    }
  }
  return nullptr;
}

Entry* XContentContainerDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("StfsContainerDevice::ResolvePath({})", path);
  Entry* entry = root_entry_->ResolvePath(path);
  if (entry) {
    return entry;
  }

  // NtCreateFile resolves the parent and calls GetChild itself, so the tile
  // fallback mostly happens in VirtualFileSystem::OpenFile. This covers the
  // callers that do pass a whole path, such as NtQueryFullAttributesFile.
  const size_t separator = path.find_last_of("\\/");
  const std::string_view leaf =
      separator == std::string_view::npos ? path : path.substr(separator + 1);
  Entry* parent = separator == std::string_view::npos
                      ? root_entry_.get()
                      : root_entry_->ResolvePath(path.substr(0, separator));
  return ResolveTileImageAlias(parent, leaf);
}

void XContentContainerDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

}  // namespace vfs
}  // namespace xe
