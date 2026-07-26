/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/title_update_manager.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/prettywriter.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/util/xex2_info.h"

namespace xe {
namespace kernel {
namespace util {

static constexpr uint32_t kXex2Magic = 0x58455832;  // 'XEX2'
static const char* kContentXuid = "0000000000000000";
static const char* kInstallerType = "000B0000";
static const char* kManifestName = "title_updates.json";

TitleUpdateManager::TitleUpdateManager(
    const std::filesystem::path& content_root)
    : content_root_(content_root) {}

std::filesystem::path TitleUpdateManager::device_root() const {
  return content_root_.parent_path();
}

std::filesystem::path TitleUpdateManager::library_root(
    uint32_t title_id) const {
  return device_root() / "Library" / fmt::format("{:08X}", title_id);
}

std::filesystem::path TitleUpdateManager::manifest_path(
    uint32_t title_id) const {
  return library_root(title_id) / kManifestName;
}

std::filesystem::path TitleUpdateManager::content_update_dir(
    uint32_t title_id) const {
  return content_root_ / kContentXuid / fmt::format("{:08X}", title_id) /
         kInstallerType;
}

std::filesystem::path TitleUpdateManager::content_header_dir(
    uint32_t title_id) const {
  return content_root_ / kContentXuid / fmt::format("{:08X}", title_id) /
         "Headers" / kInstallerType;
}

static bool ParseHexTitleId(const std::string& name, uint32_t& out) {
  if (name.size() != 8) {
    return false;
  }
  uint32_t value = 0;
  for (char c : name) {
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= static_cast<uint32_t>(c - '0');
    } else if (c >= 'A' && c <= 'F') {
      value |= static_cast<uint32_t>(c - 'A' + 10);
    } else if (c >= 'a' && c <= 'f') {
      value |= static_cast<uint32_t>(c - 'a' + 10);
    } else {
      return false;
    }
  }
  out = value;
  return true;
}

std::string TitleUpdateManager::SanitizeId(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|') {
      out.push_back('_');
    } else {
      out.push_back(c);
    }
  }
  out = std::string(xe::string_util::trim(out));
  if (out.empty()) {
    out = "update";
  }
  return out;
}

bool TitleUpdateManager::ReadXexpVersion(const std::filesystem::path& xexp,
                                         std::string& out_version,
                                         uint32_t& out_value) {
  FILE* f = xe::filesystem::OpenFile(xexp, "rb");
  if (!f) {
    return false;
  }
  uint8_t prefix[sizeof(xex2_header)] = {};
  if (fread(prefix, 1, sizeof(prefix), f) != sizeof(prefix)) {
    fclose(f);
    return false;
  }
  const auto* prefix_header = reinterpret_cast<const xex2_header*>(prefix);
  if (prefix_header->magic != kXex2Magic) {
    fclose(f);
    return false;
  }
  uint32_t header_size = prefix_header->header_size;
  if (header_size < sizeof(xex2_header) || header_size > 0x100000) {
    fclose(f);
    return false;
  }
  std::vector<uint8_t> buffer(header_size);
  std::fseek(f, 0, SEEK_SET);
  size_t got = fread(buffer.data(), 1, header_size, f);
  fclose(f);
  if (got < sizeof(xex2_header)) {
    return false;
  }
  const auto* header = reinterpret_cast<const xex2_header*>(buffer.data());
  xex2_opt_execution_info* exec = nullptr;
  if (!cpu::XexModule::GetOptHeader(header, XEX_HEADER_EXECUTION_INFO, &exec) ||
      !exec) {
    return false;
  }
  xex2_version version = exec->version();
  out_value = version.value;
  out_version = fmt::format("{}.{}.{}.{}", uint32_t(version.major),
                            uint32_t(version.minor), uint32_t(version.build),
                            uint32_t(version.qfe));
  return true;
}

bool TitleUpdateManager::LoadManifest(
    uint32_t title_id, std::string& active,
    std::vector<TitleUpdateEntry>& entries) const {
  FILE* f = xe::filesystem::OpenFile(manifest_path(title_id), "rb");
  if (!f) {
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    fclose(f);
    return false;
  }
  std::string text(static_cast<size_t>(size), '\0');
  size_t read = fread(text.data(), 1, text.size(), f);
  fclose(f);
  text.resize(read);

  rapidjson::Document doc;
  doc.Parse(text.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return false;
  }
  if (doc.HasMember("active") && doc["active"].IsString()) {
    active = doc["active"].GetString();
  }
  if (doc.HasMember("updates") && doc["updates"].IsArray()) {
    for (const auto& u : doc["updates"].GetArray()) {
      if (!u.IsObject()) {
        continue;
      }
      TitleUpdateEntry e;
      if (u.HasMember("id") && u["id"].IsString()) {
        e.id = u["id"].GetString();
      }
      if (u.HasMember("name") && u["name"].IsString()) {
        e.name = u["name"].GetString();
      }
      if (u.HasMember("version") && u["version"].IsString()) {
        e.version = u["version"].GetString();
      }
      if (u.HasMember("version_value") && u["version_value"].IsUint()) {
        e.version_value = u["version_value"].GetUint();
      }
      if (u.HasMember("size_bytes") && u["size_bytes"].IsUint64()) {
        e.size_bytes = u["size_bytes"].GetUint64();
      }
      if (u.HasMember("source_file") && u["source_file"].IsString()) {
        e.source_file = u["source_file"].GetString();
      }
      if (!e.id.empty()) {
        entries.push_back(std::move(e));
      }
    }
  }
  return true;
}

bool TitleUpdateManager::SaveManifest(
    uint32_t title_id, const std::string& active,
    const std::vector<TitleUpdateEntry>& entries) const {
  rapidjson::Document doc;
  doc.SetObject();
  auto& al = doc.GetAllocator();
  doc.AddMember("active", rapidjson::Value(active.c_str(), al), al);
  rapidjson::Value arr(rapidjson::kArrayType);
  for (const auto& e : entries) {
    rapidjson::Value o(rapidjson::kObjectType);
    o.AddMember("id", rapidjson::Value(e.id.c_str(), al), al);
    o.AddMember("name", rapidjson::Value(e.name.c_str(), al), al);
    o.AddMember("version", rapidjson::Value(e.version.c_str(), al), al);
    o.AddMember("version_value", e.version_value, al);
    o.AddMember("size_bytes", e.size_bytes, al);
    o.AddMember("source_file", rapidjson::Value(e.source_file.c_str(), al), al);
    arr.PushBack(o, al);
  }
  doc.AddMember("updates", arr, al);

  rapidjson::StringBuffer sb;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
  doc.Accept(writer);

  std::error_code ec;
  std::filesystem::create_directories(library_root(title_id), ec);
  FILE* f = xe::filesystem::OpenFile(manifest_path(title_id), "wb");
  if (!f) {
    return false;
  }
  fwrite(sb.GetString(), 1, sb.GetSize(), f);
  fclose(f);
  return true;
}

std::vector<TitleUpdateEntry> TitleUpdateManager::List(
    uint32_t title_id) const {
  std::string active;
  std::vector<TitleUpdateEntry> entries;
  LoadManifest(title_id, active, entries);
  return entries;
}

std::string TitleUpdateManager::GetActive(uint32_t title_id) const {
  std::string active;
  std::vector<TitleUpdateEntry> entries;
  LoadManifest(title_id, active, entries);
  return active;
}

std::filesystem::path TitleUpdateManager::GetActiveLibraryPath(
    uint32_t title_id) const {
  std::string active = GetActive(title_id);
  if (active.empty()) {
    return {};
  }
  auto path = library_root(title_id) / active;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return {};
  }
  return path;
}

std::string TitleUpdateManager::MakeUniqueId(
    uint32_t title_id, const std::string& preferred,
    const std::vector<TitleUpdateEntry>& entries) const {
  std::string base = SanitizeId(preferred);
  std::string candidate = base;
  uint32_t suffix = 2;
  auto taken = [&](const std::string& id) {
    if (std::filesystem::exists(library_root(title_id) / id)) {
      return true;
    }
    return std::any_of(entries.begin(), entries.end(),
                       [&](const TitleUpdateEntry& e) { return e.id == id; });
  };
  while (taken(candidate)) {
    candidate = fmt::format("{}-{}", base, suffix++);
  }
  return candidate;
}

void TitleUpdateManager::DeactivateLink(uint32_t title_id,
                                        const std::string& id) {
  if (id.empty()) {
    return;
  }
  std::error_code ec;
  auto link = content_update_dir(title_id) / id;
  if (!xe::filesystem::RemoveDirectoryJunction(link)) {
    // Not a junction (copy fallback path) - remove the real directory.
    std::filesystem::remove_all(link, ec);
  }
  std::filesystem::remove(content_header_dir(title_id) / (id + ".header"), ec);
}

bool TitleUpdateManager::SetActive(uint32_t title_id, const std::string& id) {
  std::string active;
  std::vector<TitleUpdateEntry> entries;
  LoadManifest(title_id, active, entries);

  bool exists =
      std::any_of(entries.begin(), entries.end(),
                  [&](const TitleUpdateEntry& e) { return e.id == id; });
  if (!id.empty() && !exists) {
    return false;
  }

  // The active update is read directly from the library at load time, so there
  // is nothing to copy or link. Just clean up any content-tree links left by
  // older builds and record the choice.
  for (const auto& e : entries) {
    DeactivateLink(title_id, e.id);
  }

  return SaveManifest(title_id, id, entries);
}

bool TitleUpdateManager::Rename(uint32_t title_id, const std::string& id,
                                const std::string& new_name) {
  std::string active;
  std::vector<TitleUpdateEntry> entries;
  if (!LoadManifest(title_id, active, entries)) {
    return false;
  }
  bool found = false;
  for (auto& e : entries) {
    if (e.id == id) {
      e.name = new_name;
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }
  return SaveManifest(title_id, active, entries);
}

bool TitleUpdateManager::Remove(uint32_t title_id, const std::string& id) {
  std::string active;
  std::vector<TitleUpdateEntry> entries;
  if (!LoadManifest(title_id, active, entries)) {
    return false;
  }
  DeactivateLink(title_id, id);

  std::error_code ec;
  std::filesystem::remove_all(library_root(title_id) / id, ec);
  std::filesystem::remove(library_root(title_id) / (id + ".header"), ec);

  entries.erase(
      std::remove_if(entries.begin(), entries.end(),
                     [&](const TitleUpdateEntry& e) { return e.id == id; }),
      entries.end());
  if (active == id) {
    active.clear();
  }
  return SaveManifest(title_id, active, entries);
}

std::string TitleUpdateManager::ImportFromContent(
    uint32_t title_id, const std::string& source_dirname, bool auto_activate) {
  auto src_dir = content_update_dir(title_id) / source_dirname;
  auto src_header = content_header_dir(title_id) / (source_dirname + ".header");
  if (!std::filesystem::exists(src_dir)) {
    return "";
  }

  std::string version = source_dirname;
  uint32_t version_value = 0;
  std::string parsed_version;
  uint32_t parsed_value = 0;
  if (ReadXexpVersion(src_dir / "default.xexp", parsed_version, parsed_value)) {
    version = parsed_version;
    version_value = parsed_value;
  }

  std::string active;
  std::vector<TitleUpdateEntry> entries;
  LoadManifest(title_id, active, entries);

  std::string id = MakeUniqueId(title_id, version, entries);

  std::error_code ec;
  std::filesystem::create_directories(library_root(title_id), ec);
  auto lib_dir = library_root(title_id) / id;
  std::filesystem::remove_all(lib_dir, ec);
  std::filesystem::rename(src_dir, lib_dir, ec);
  if (ec) {
    // Cross-volume or busy - fall back to copy + delete.
    ec.clear();
    std::filesystem::copy(src_dir, lib_dir,
                          std::filesystem::copy_options::recursive, ec);
    if (ec) {
      XELOGE("TitleUpdateManager: failed to import {} ({})", source_dirname,
             ec.message());
      return "";
    }
    std::filesystem::remove_all(src_dir, ec);
  }

  if (std::filesystem::exists(src_header)) {
    std::filesystem::rename(src_header,
                            library_root(title_id) / (id + ".header"), ec);
    if (ec) {
      ec.clear();
      std::filesystem::copy_file(
          src_header, library_root(title_id) / (id + ".header"),
          std::filesystem::copy_options::overwrite_existing, ec);
      std::filesystem::remove(src_header, ec);
    }
  }

  uint64_t size_bytes = 0;
  for (const auto& f : std::filesystem::recursive_directory_iterator(
           lib_dir, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (f.is_regular_file(ec)) {
      size_bytes += f.file_size(ec);
    }
  }

  TitleUpdateEntry entry;
  entry.id = id;
  entry.name = version;
  entry.version = version;
  entry.version_value = version_value;
  entry.size_bytes = size_bytes;
  entry.source_file = source_dirname;
  entries.push_back(entry);

  SaveManifest(title_id, active, entries);

  if (auto_activate) {
    SetActive(title_id, id);
  }
  return id;
}

void TitleUpdateManager::MigrateLegacy(uint32_t title_id) {
  auto update_dir = content_update_dir(title_id);
  std::error_code ec;
  if (!std::filesystem::exists(update_dir)) {
    return;
  }

  std::string active;
  std::vector<TitleUpdateEntry> entries;
  LoadManifest(title_id, active, entries);

  // Collect real (non-link) directories that aren't already tracked.
  std::vector<std::string> to_import;
  for (const auto& entry : std::filesystem::directory_iterator(
           update_dir,
           std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_directory(ec)) {
      continue;
    }
    // Skip junctions/symlinks - those are our active links.
    if (std::filesystem::is_symlink(entry.symlink_status(ec))) {
      continue;
    }
    std::string name = xe::path_to_utf8(entry.path().filename());
    bool tracked = std::any_of(
        entries.begin(), entries.end(),
        [&](const TitleUpdateEntry& e) { return e.source_file == name; });
    if (!tracked) {
      to_import.push_back(name);
    }
  }

  bool had_active = !active.empty();
  for (const auto& name : to_import) {
    // Activate the first migrated update only if nothing is active yet.
    std::string imported = ImportFromContent(title_id, name, !had_active);
    if (!imported.empty() && !had_active) {
      had_active = true;
    }
  }
}

void TitleUpdateManager::MigrateAllLegacy() {
  auto base = content_root_ / kContentXuid;
  std::error_code ec;
  if (!std::filesystem::exists(base, ec)) {
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(
           base, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (!entry.is_directory(ec)) {
      continue;
    }
    uint32_t title_id = 0;
    if (!ParseHexTitleId(xe::path_to_utf8(entry.path().filename()), title_id)) {
      continue;
    }
    if (std::filesystem::exists(content_update_dir(title_id), ec)) {
      MigrateLegacy(title_id);
    }
  }
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
