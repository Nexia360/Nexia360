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
#include <cctype>
#include <cstdio>
#include <utility>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/prettywriter.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/base/xxhash.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/util/xex2_info.h"

namespace xe {
namespace kernel {
namespace util {

static constexpr uint32_t kXex2Magic = 0x58455832;  // 'XEX2'
static const char* kContentXuid = "0000000000000000";
static const char* kInstallerType = "000B0000";
static const char* kManifestName = "title_updates.json";
static const char* kHeaderDirName = "Headers";
// The update payload lives in its own subfolder so a game whose update
// contains a folder called "Content" cannot collide with the per-update
// save/DLC area that sits alongside it.
static const char* kUpdateDirName = "UPDATE";

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

std::filesystem::path TitleUpdateManager::update_dir(
    uint32_t title_id, const std::string& id) const {
  return library_root(title_id) / id / kUpdateDirName;
}

std::filesystem::path TitleUpdateManager::content_update_dir(
    uint32_t title_id) const {
  return content_root_ / kContentXuid / fmt::format("{:08X}", title_id) /
         kInstallerType;
}

std::filesystem::path TitleUpdateManager::content_header_dir(
    uint32_t title_id) const {
  return content_root_ / kContentXuid / fmt::format("{:08X}", title_id) /
         kHeaderDirName / kInstallerType;
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

std::string TitleUpdateManager::ComputeUpdateHash(
    const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    return "";
  }

  // Collect every regular file keyed by its relative path, then sort, so the
  // digest does not depend on directory iteration order.
  std::vector<std::pair<std::string, std::filesystem::path>> files;
  for (const auto& f : std::filesystem::recursive_directory_iterator(
           dir, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (ec) {
      return "";
    }
    if (!f.is_regular_file(ec)) {
      continue;
    }
    auto rel = std::filesystem::relative(f.path(), dir, ec);
    if (ec) {
      return "";
    }
    std::string key = xe::path_to_utf8(rel);
    // Normalize separators and case - these come off FAT/STFS.
    std::replace(key.begin(), key.end(), '\\', '/');
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    files.emplace_back(std::move(key), f.path());
  }

  if (files.empty()) {
    return "";
  }

  std::sort(files.begin(), files.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  XXH3_state_t state;
  XXH3_128bits_reset(&state);

  std::vector<uint8_t> buffer(64 * 1024);
  for (const auto& [key, path] : files) {
    XXH3_128bits_update(&state, key.data(), key.size());

    FILE* f = xe::filesystem::OpenFile(path, "rb");
    if (!f) {
      return "";
    }
    while (true) {
      size_t read = fread(buffer.data(), 1, buffer.size(), f);
      if (read) {
        XXH3_128bits_update(&state, buffer.data(), read);
      }
      if (read < buffer.size()) {
        break;
      }
    }
    fclose(f);
  }

  XXH128_hash_t digest = XXH3_128bits_digest(&state);
  return fmt::format("{:016x}{:016x}", digest.high64, digest.low64);
}

std::string TitleUpdateManager::FindByHash(
    const std::string& hash,
    const std::vector<TitleUpdateEntry>& entries) const {
  if (hash.empty()) {
    return "";
  }
  for (const auto& e : entries) {
    if (e.hash == hash) {
      return e.id;
    }
  }
  return "";
}

void TitleUpdateManager::EnsureHashes(uint32_t title_id) {
  std::string active;
  std::vector<TitleUpdateEntry> entries;
  if (!LoadManifest(title_id, active, entries)) {
    return;
  }

  bool changed = false;
  for (auto& e : entries) {
    if (!e.hash.empty()) {
      continue;
    }
    e.hash = ComputeUpdateHash(update_dir(title_id, e.id));
    if (!e.hash.empty()) {
      changed = true;
      XELOGD("TitleUpdateManager: hashed existing update {:08X}/{} -> {}",
             title_id, e.id, e.hash);
    }
  }

  if (changed) {
    SaveManifest(title_id, active, entries);
  }
}

std::filesystem::path TitleUpdateManager::GetContentRoot(uint32_t title_id,
                                                         const std::string& id,
                                                         uint64_t xuid) const {
  if (id.empty()) {
    return {};
  }
  return library_root(title_id) / id / "Content" / fmt::format("{:016X}", xuid);
}

std::filesystem::path TitleUpdateManager::GetActiveContentRoot(
    uint32_t title_id, uint64_t xuid) const {
  std::string active = GetActive(title_id);

  // "None" is an overlay of its own, not a fall-through to the global tree.
  if (active.empty()) {
    active = kNoTitleUpdateId;
  }

  return GetContentRoot(title_id, active, xuid);
}

void TitleUpdateManager::MigrateGlobalContentToNoTu(uint32_t title_id) {
  std::error_code ec;
  const auto title_str = fmt::format("{:08X}", title_id);

  if (!std::filesystem::exists(content_root_, ec)) {
    return;
  }

  for (const auto& xuid_dir : std::filesystem::directory_iterator(
           content_root_,
           std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (!xuid_dir.is_directory(ec)) {
      continue;
    }

    const auto src = xuid_dir.path() / title_str;
    if (!std::filesystem::exists(src, ec)) {
      continue;
    }

    const auto xuid_name = xe::path_to_utf8(xuid_dir.path().filename());
    const auto dst =
        library_root(title_id) / kNoTitleUpdateId / "Content" / xuid_name;

    // Installers stay in the global tree - that is where update packages are
    // installed to and where ImportFromContent reads them from. That applies
    // to their headers too, which live one level down in Headers/000B0000,
    // so Headers must be descended into rather than moved wholesale.
    auto move_type = [&](const std::filesystem::path& src_type,
                         const std::filesystem::path& dst_type,
                         const std::string& label) {
      if (std::filesystem::exists(dst_type, ec)) {
        return;  // already migrated
      }

      std::filesystem::create_directories(dst_type.parent_path(), ec);
      std::filesystem::rename(src_type, dst_type, ec);
      if (ec) {
        ec.clear();
        std::filesystem::copy(src_type, dst_type,
                              std::filesystem::copy_options::recursive, ec);
        if (!ec) {
          std::filesystem::remove_all(src_type, ec);
        }
      }

      XELOGD("TitleUpdateManager: migrated {:08X} {}/{} into {}", title_id,
             xuid_name, label, kNoTitleUpdateId);
    };

    for (const auto& type_dir : std::filesystem::directory_iterator(
             src, std::filesystem::directory_options::skip_permission_denied,
             ec)) {
      const auto type_name = xe::path_to_utf8(type_dir.path().filename());

      if (type_name == kInstallerType) {
        continue;
      }

      if (type_name == kHeaderDirName) {
        // Move each header type individually, leaving the installer headers
        // where the content manager still expects to find them.
        for (const auto& hdr_type : std::filesystem::directory_iterator(
                 type_dir.path(),
                 std::filesystem::directory_options::skip_permission_denied,
                 ec)) {
          const auto hdr_name = xe::path_to_utf8(hdr_type.path().filename());
          if (hdr_name == kInstallerType) {
            continue;
          }
          move_type(hdr_type.path(), dst / kHeaderDirName / hdr_name,
                    std::string(kHeaderDirName) + "/" + hdr_name);
        }
        continue;
      }

      move_type(type_dir.path(), dst / type_name, type_name);
    }
  }
}

std::vector<std::string> TitleUpdateManager::ImportNoTuContent(
    uint32_t title_id, const std::string& target_id, uint64_t xuid,
    bool dry_run) {
  std::vector<std::string> conflicts;

  const auto src = GetContentRoot(title_id, kNoTitleUpdateId, xuid);
  const auto dst = GetContentRoot(title_id, target_id, xuid);

  std::error_code ec;
  if (src.empty() || dst.empty() || !std::filesystem::exists(src, ec)) {
    return conflicts;
  }

  for (const auto& f : std::filesystem::recursive_directory_iterator(
           src, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (!f.is_regular_file(ec)) {
      continue;
    }

    auto rel = std::filesystem::relative(f.path(), src, ec);
    if (ec) {
      continue;
    }

    const auto target = dst / rel;
    if (std::filesystem::exists(target, ec)) {
      conflicts.push_back(xe::path_to_utf8(rel));
    }

    if (!dry_run) {
      std::filesystem::create_directories(target.parent_path(), ec);
      std::filesystem::copy_file(
          f.path(), target, std::filesystem::copy_options::overwrite_existing,
          ec);
      if (ec) {
        XELOGE("TitleUpdateManager: failed to import {} ({})",
               xe::path_to_utf8(rel), ec.message());
        ec.clear();
      }
    }
  }

  return conflicts;
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
      if (u.HasMember("hash") && u["hash"].IsString()) {
        e.hash = u["hash"].GetString();
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
    o.AddMember("hash", rapidjson::Value(e.hash.c_str(), al), al);
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
  auto path = update_dir(title_id, active);
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

  // Anything sitting in the global content tree predates title-update
  // management, so it belongs to the "None" overlay. Runs once per title -
  // afterwards each selection owns its own content and switching is only a
  // pointer change.
  MigrateGlobalContentToNoTu(title_id);

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

  // Hash the staged update before it moves, and make sure everything
  // already in the library has a hash to compare against.
  EnsureHashes(title_id);

  std::string active;
  std::vector<TitleUpdateEntry> entries;
  LoadManifest(title_id, active, entries);

  const std::string incoming_hash = ComputeUpdateHash(src_dir);
  const std::string duplicate_of = FindByHash(incoming_hash, entries);

  if (!duplicate_of.empty()) {
    XELOGD(
        "TitleUpdateManager: {:08X} update {} is identical to installed "
        "{} ({}), discarding",
        title_id, source_dirname, duplicate_of, incoming_hash);
    std::error_code dup_ec;
    std::filesystem::remove_all(src_dir, dup_ec);
    std::filesystem::remove(src_header, dup_ec);
    if (auto_activate) {
      SetActive(title_id, duplicate_of);
    }
    return duplicate_of;
  }

  std::string id = MakeUniqueId(title_id, version, entries);

  std::error_code ec;
  std::filesystem::create_directories(library_root(title_id), ec);
  auto lib_dir = update_dir(title_id, id);
  std::filesystem::remove_all(lib_dir, ec);
  std::filesystem::create_directories(lib_dir.parent_path(), ec);
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
  entry.hash = incoming_hash;
  entries.push_back(entry);

  SaveManifest(title_id, active, entries);

  if (auto_activate) {
    SetActive(title_id, id);
  }
  return id;
}

// Older libraries stored the update payload directly at <Library>/<id>/.
// Move it down into <id>/UPDATE so the per-update Content folder cannot be
// confused with an update that ships its own "Content" directory.
void TitleUpdateManager::MigrateLegacyLibraryLayout(uint32_t title_id) {
  std::string active;
  std::vector<TitleUpdateEntry> entries;
  if (!LoadManifest(title_id, active, entries)) {
    return;
  }

  std::error_code ec;
  for (const auto& e : entries) {
    const auto id_dir = library_root(title_id) / e.id;
    const auto payload = update_dir(title_id, e.id);

    if (std::filesystem::exists(payload, ec) ||
        !std::filesystem::exists(id_dir, ec)) {
      continue;  // already migrated, or nothing there
    }

    // Everything except the per-update Content folder is payload.
    std::vector<std::filesystem::path> payload_entries;
    for (const auto& item : std::filesystem::directory_iterator(
             id_dir, std::filesystem::directory_options::skip_permission_denied,
             ec)) {
      if (xe::path_to_utf8(item.path().filename()) == "Content") {
        continue;
      }
      payload_entries.push_back(item.path());
    }

    if (payload_entries.empty()) {
      continue;
    }

    std::filesystem::create_directories(payload, ec);
    for (const auto& item : payload_entries) {
      const auto dst = payload / item.filename();
      std::filesystem::rename(item, dst, ec);
      if (ec) {
        ec.clear();
        std::filesystem::copy(item, dst,
                              std::filesystem::copy_options::recursive, ec);
        if (!ec) {
          std::filesystem::remove_all(item, ec);
        }
      }
    }

    XELOGD("TitleUpdateManager: moved {:08X}/{} payload into {}", title_id,
           e.id, kUpdateDirName);
  }
}

void TitleUpdateManager::MigrateLegacy(uint32_t title_id) {
  auto content_updates = content_update_dir(title_id);
  std::error_code ec;
  if (!std::filesystem::exists(content_updates)) {
    return;
  }

  std::string active;
  std::vector<TitleUpdateEntry> entries;
  LoadManifest(title_id, active, entries);

  // Collect real (non-link) directories that aren't already tracked.
  std::vector<std::string> to_import;
  for (const auto& entry : std::filesystem::directory_iterator(
           content_updates,
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

  // Convert any library still holding its payload at <Library>/<id>/ over
  // to the <id>/UPDATE layout. Walks the library, not the content tree, so
  // titles whose content folder is long gone are converted too.
  auto library = device_root() / "Library";
  if (!std::filesystem::exists(library, ec)) {
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(
           library, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (!entry.is_directory(ec)) {
      continue;
    }
    uint32_t title_id = 0;
    if (!ParseHexTitleId(xe::path_to_utf8(entry.path().filename()), title_id)) {
      continue;
    }
    MigrateLegacyLibraryLayout(title_id);
  }
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
