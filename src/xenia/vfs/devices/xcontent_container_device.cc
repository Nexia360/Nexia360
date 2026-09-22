/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/base/logging.h"
#include "xenia/base/utf8.h"
#include "xenia/vfs/devices/xcontent_devices/stfs_container_device.h"
#include "xenia/vfs/devices/xcontent_devices/svod_container_device.h"

namespace xe {
namespace vfs {

std::unique_ptr<XContentContainerDevice>
XContentContainerDevice::CreateContentDevice(
    const std::string_view mount_path, const std::filesystem::path& host_path) {
  if (!std::filesystem::exists(host_path)) {
    XELOGE("Path to XContent container does not exist: {}", host_path);
    return nullptr;
  }

  if (std::filesystem::is_directory(host_path)) {
    return nullptr;
  }

  FILE* host_file = xe::filesystem::OpenFile(host_path, "rb");
  if (!host_file) {
    XELOGE("Error opening XContent file.");
    return nullptr;
  }

  const uint64_t package_size = std::filesystem::file_size(host_path);
  if (package_size < sizeof(XContentContainerHeader)) {
    return nullptr;
  }

  const auto header = XContentContainerDevice::ReadContainerHeader(host_file);
  if (header == nullptr) {
    return nullptr;
  }

  fclose(host_file);

  if (!header->content_header.is_magic_valid()) {
    return nullptr;
  }

  switch (header->content_metadata.volume_type) {
    case XContentVolumeType::kStfs:
      return std::make_unique<StfsContainerDevice>(mount_path, host_path);
      break;
    case XContentVolumeType::kSvod:
      return std::make_unique<SvodContainerDevice>(mount_path, host_path);
      break;
    default:
      break;
  }

  return nullptr;
}

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path, const std::filesystem::path& host_path)
    : Device(mount_path),
      name_("XContent"),
      host_path_(host_path),
      files_total_size_(0),
      header_(std::make_unique<XContentContainerHeader>()) {}

XContentContainerDevice::~XContentContainerDevice() {}

bool XContentContainerDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    XELOGE("Path to XContent container does not exist: {}", host_path_);
    return false;
  }

  if (std::filesystem::is_directory(host_path_)) {
    return false;
  }

  XELOGI("Loading XContent header file: {}", host_path_);
  auto header_file = xe::filesystem::OpenFile(host_path_, "rb");
  if (!header_file) {
    XELOGE("Error opening XContent header file.");
    return false;
  }

  auto header_loading_result = ReadHeaderAndVerify(header_file);
  if (header_loading_result != Result::kSuccess) {
    XELOGE("Error reading XContent header: {}",
           static_cast<int32_t>(header_loading_result));
    fclose(header_file);
    return false;
  }

  SetupContainer();

  if (LoadHostFiles() != Result::kSuccess) {
    XELOGE("Error loading XContent host files.");
    return false;
  }

  return Read() == Result::kSuccess;
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(
    const std::filesystem::path& file_path) {
  if (!std::filesystem::exists(file_path)) {
    return {};
  }

  if (std::filesystem::file_size(file_path) < sizeof(XContentContainerHeader)) {
    return {};
  }

  auto header_file = xe::filesystem::OpenFile(file_path, "rb");
  if (!header_file) {
    return {};
  }

  // This used to return without closing, leaking a handle on the container
  // every time. The CreateContentDevice overload above fcloses its own file;
  // this one is called for every item of every content enumeration.
  auto header = ReadContainerHeader(header_file);
  fclose(header_file);
  return header;
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(FILE* host_file) {
  std::unique_ptr<XContentContainerHeader> header =
      std::make_unique<XContentContainerHeader>();

  // Read header & check signature
  if (fread(header.get(), sizeof(XContentContainerHeader), 1, host_file) != 1) {
    return nullptr;
  }
  return header;
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

kernel::xam::XCONTENT_AGGREGATE_DATA XContentContainerDevice::content_header()
    const {
  kernel::xam::XCONTENT_AGGREGATE_DATA data;

  std::memset(&data, 0, sizeof(kernel::xam::XCONTENT_AGGREGATE_DATA));

  data.device_id = 1;
  data.title_id = header_->content_metadata.execution_info.title_id;
  data.content_type = header_->content_metadata.content_type;

  auto name = header_->content_metadata.display_name(XLanguage::kEnglish);
  if (name.empty()) {
    // Find first filled language and use it. It might be incorrect, but meh
    // until stfs support is done.
    for (uint8_t i = 0; i < header_->content_metadata.kNumLanguagesV2; i++) {
      name = header_->content_metadata.display_name((XLanguage)i);
      if (!name.empty()) {
        break;
      }
    }
  }

  data.set_display_name(name);

  return data;
}

XContentContainerDevice::Result XContentContainerDevice::ReadHeaderAndVerify(
    FILE* header_file) {
  files_total_size_ = std::filesystem::file_size(host_path_);
  if (files_total_size_ < sizeof(XContentContainerHeader)) {
    return Result::kTooSmall;
  }

  auto header = ReadContainerHeader(header_file);
  if (header == nullptr) {
    return Result::kReadError;
  }

  if (!header->content_header.is_magic_valid()) {
    // Unexpected format.
    return Result::kFileMismatch;
  }

  header_ = std::move(header);
  return Result::kSuccess;
}

}  // namespace vfs
}  // namespace xe
