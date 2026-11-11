/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/title_update_downloader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

// clang-format off
// We want to include platform.h first to define NOMINMAX to prevent window.h
// from defining the macros.
#include "xenia/base/platform.h"
#include "third_party/libcurl/include/curl/curl.h"
// clang-format on

#include "third_party/rapidjson/include/rapidjson/document.h"

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/util/net_utils.h"
#include "xenia/kernel/util/xex2_info.h"

DECLARE_string(api_address);

namespace xe {
namespace kernel {
namespace util {

// The catalogue this pulls from. Kept here rather than in the UI: the user is
// offered "Download Title Updates", not a particular site.
static constexpr const char* kCatalogueHost = "https://xboxunity.net";

// The hub publishes, per title id, {title version -> TU name}. The catalogue
// knows neither: its "version" is the build number alone, and for Minecraft
// build 80 is TU75. This list is the only place the TU number comes from.
static constexpr const char* kHubVersionList = "versions.json";
static constexpr long kHubVersionTimeoutSeconds = 10;

// Media ids and hashes come back in whatever case the catalogue stored them.
static bool EqualsNoCase(const std::string& a, const std::string& b) {
  return a.size() == b.size() &&
         std::equal(a.cbegin(), a.cend(), b.cbegin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

// Offset of the content hash inside an STFS header. The catalogue publishes
// this same value as the update's "hash", so it identifies the package.
static constexpr size_t kStfsContentHashOffset = 0x32C;
static constexpr size_t kStfsContentHashSize = 20;

// A downloaded package is accepted only when it is an STFS container carrying
// the content hash that was advertised, at the advertised length.
static bool VerifyPackage(const std::filesystem::path& path,
                          const RemoteTitleUpdate& update) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");

  if (!file) {
    return false;
  }

  uint8_t header[kStfsContentHashOffset + kStfsContentHashSize] = {};
  const size_t read = fread(header, 1, sizeof(header), file);

  fseek(file, 0, SEEK_END);
  const uint64_t file_size = static_cast<uint64_t>(ftell(file));
  fclose(file);

  if (read != sizeof(header)) {
    XELOGE("Title update is too small to be a package");
    return false;
  }

  const bool is_stfs = !std::memcmp(header, "LIVE", 4) ||
                       !std::memcmp(header, "PIRS", 4) ||
                       !std::memcmp(header, "CON ", 4);

  if (!is_stfs) {
    XELOGE("Title update is not an STFS package");
    return false;
  }

  std::string content_hash;

  for (size_t i = 0; i < kStfsContentHashSize; ++i) {
    content_hash += fmt::format("{:02X}", header[kStfsContentHashOffset + i]);
  }

  if (!update.hash.empty() && !EqualsNoCase(content_hash, update.hash)) {
    XELOGE("Title update content hash mismatch: expected {} got {}",
           update.hash, content_hash);
    return false;
  }

  // Size is listed in KiB; the transfer itself is in bytes.
  const uint64_t expected = update.listed_size * 1024ull;

  if (expected && file_size != expected) {
    XELOGE("Title update is {} bytes, expected {}", file_size, expected);
    return false;
  }

  return true;
}

static size_t WriteToStringCallback(void* data, size_t size, size_t count,
                                    void* userp) {
  const size_t bytes = size * count;
  static_cast<std::string*>(userp)->append(static_cast<char*>(data), bytes);
  return bytes;
}

struct DownloadState {
  FILE* file = nullptr;
  uint64_t received = 0;
  uint64_t total = 0;
  const std::function<void(uint64_t, uint64_t)>* progress = nullptr;
  const std::atomic<bool>* cancel = nullptr;
  bool write_failed = false;
};

static size_t WriteToFileCallback(void* data, size_t size, size_t count,
                                  void* userp) {
  auto* state = static_cast<DownloadState*>(userp);
  const size_t bytes = size * count;

  if (fwrite(data, 1, bytes, state->file) != bytes) {
    state->write_failed = true;
    return 0;
  }

  state->received += bytes;

  if (state->progress) {
    (*state->progress)(state->received, state->total);
  }

  return bytes;
}

static int DownloadProgressCallback(void* userp, curl_off_t total_download,
                                    curl_off_t now_download, curl_off_t,
                                    curl_off_t) {
  auto* state = static_cast<DownloadState*>(userp);

  if (total_download > 0) {
    state->total = static_cast<uint64_t>(total_download);
  }

  // Non-zero aborts the transfer.
  return (state->cancel && state->cancel->load()) ? 1 : 0;
}

std::string TitleUpdateDownloader::ReadMediaId(
    const std::filesystem::path& xex_path) {
  FILE* file = xe::filesystem::OpenFile(xex_path, "rb");

  if (!file) {
    return "";
  }

  // The optional header table sits at the front of the file; the execution
  // info it points at is well inside the first few KiB in practice.
  std::vector<uint8_t> head(0x4000);
  const size_t read = fread(head.data(), 1, head.size(), file);
  fclose(file);

  if (read < 0x20 || std::memcmp(head.data(), "XEX2", 4) != 0) {
    return "";
  }

  auto be32 = [&head](size_t offset) -> uint32_t {
    return (static_cast<uint32_t>(head[offset]) << 24) |
           (static_cast<uint32_t>(head[offset + 1]) << 16) |
           (static_cast<uint32_t>(head[offset + 2]) << 8) |
           static_cast<uint32_t>(head[offset + 3]);
  };

  const uint32_t count = be32(0x18);

  for (uint32_t i = 0; i < count; ++i) {
    const size_t entry = 0x20 + i * 8;

    if (entry + 8 > read) {
      break;
    }

    const uint32_t key = be32(entry);
    const uint32_t value = be32(entry + 4);

    if (key != XEX_HEADER_EXECUTION_INFO) {
      continue;
    }

    // media_id is the first field of xex2_opt_execution_info.
    if (value + sizeof(xex2_opt_execution_info) > read) {
      return "";
    }

    return fmt::format("{:08X}", be32(value));
  }

  return "";
}

std::string TitleUpdateDownloader::BuildListUrl(uint32_t title_id) {
  return fmt::format("{}/Resources/Lib/TitleUpdateInfo.php?titleid={:08X}",
                     kCatalogueHost, title_id);
}

std::string TitleUpdateDownloader::BuildDownloadUrl(uint32_t update_id) {
  return fmt::format("{}/Resources/Lib/TitleUpdate.php?tuid={}", kCatalogueHost,
                     update_id);
}

// title id (upper hex) -> {title version -> TU name}, fetched from the hub
// once per run. Missing or unreachable leaves it empty and every update keeps
// its catalogue name.
static const std::map<std::string, std::map<std::string, std::string>>&
HubVersionNames() {
  static std::map<std::string, std::map<std::string, std::string>> names;
  static std::once_flag once;

  std::call_once(once, []() {
    std::string base = cvars::api_address;

    if (base.empty()) {
      return;
    }

    if (base.back() != '/') {
      base.push_back('/');
    }

    CURL* curl = curl_easy_init();

    if (!curl) {
      return;
    }

    const std::string url = base + kHubVersionList;
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, NexiaUserAgent());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kHubVersionTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    const CURLcode result = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK || status != 200) {
      XELOGW("Hub title update names unavailable: curl {} http {}",
             static_cast<int>(result), status);
      return;
    }

    rapidjson::Document doc;
    doc.Parse(response.c_str());

    if (doc.HasParseError() || !doc.IsObject()) {
      XELOGW("Hub title update names are not an object");
      return;
    }

    for (auto title = doc.MemberBegin(); title != doc.MemberEnd(); ++title) {
      if (!title->name.IsString() || !title->value.IsObject()) {
        continue;
      }

      std::string id = title->name.GetString();

      for (char& c : id) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }

      auto& by_version = names[id];

      for (auto entry = title->value.MemberBegin();
           entry != title->value.MemberEnd(); ++entry) {
        if (entry->name.IsString() && entry->value.IsString()) {
          by_version[entry->name.GetString()] = entry->value.GetString();
        }
      }
    }

    XELOGI("Hub lists title update names for {} titles", names.size());
  });

  return names;
}

// The build number out of a version string. An Xbox version is
// major.minor.BUILD.qfe, and the catalogue publishes the build on its own, so
// that component is what lines the two lists up.
static std::string BuildComponent(const std::string& version) {
  const size_t second = version.find('.', version.find('.') + 1);

  if (second == std::string::npos) {
    return "";
  }

  const size_t third = version.find('.', second + 1);

  return version.substr(second + 1, third == std::string::npos
                                        ? std::string::npos
                                        : third - second - 1);
}

static void NameFromHub(uint32_t title_id, RemoteTitleUpdate* update) {
  const auto& all = HubVersionNames();
  const auto title = all.find(fmt::format("{:08X}", title_id));

  if (title == all.cend() || update->version.empty()) {
    return;
  }

  const auto& by_version = title->second;

  // A catalogue that lists the full version needs no translation.
  const auto exact = by_version.find(update->version);

  if (exact != by_version.cend()) {
    update->title_version = exact->first;
    update->tu_name = exact->second;
    return;
  }

  for (const auto& [version, name] : by_version) {
    if (BuildComponent(version) == update->version) {
      update->title_version = version;
      update->tu_name = name;
      return;
    }
  }
}

std::vector<RemoteTitleUpdate> TitleUpdateDownloader::List(
    uint32_t title_id, const std::string& media_id) {
  std::vector<RemoteTitleUpdate> updates;

  if (!title_id || media_id.empty()) {
    return updates;
  }

  CURL* curl = curl_easy_init();

  if (!curl) {
    return updates;
  }

  const std::string url = BuildListUrl(title_id);

  std::string response;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, NexiaUserAgent());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToStringCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  const CURLcode result = curl_easy_perform(curl);

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK || status != 200) {
    XELOGE("Title update catalogue unreachable: curl {} http {}",
           static_cast<int>(result), status);
    return updates;
  }

  rapidjson::Document doc;
  doc.Parse(response.c_str());

  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("MediaIDS") ||
      !doc["MediaIDS"].IsArray()) {
    return updates;
  }

  // STRICT match: only the group whose media id is this copy of the game.
  // Anything published under a different media id is for a different release
  // and must not be offered.
  for (const auto& group : doc["MediaIDS"].GetArray()) {
    if (!group.IsObject() || !group.HasMember("MediaID") ||
        !group["MediaID"].IsString()) {
      continue;
    }

    const std::string group_media = group["MediaID"].GetString();

    if (!EqualsNoCase(group_media, media_id)) {
      continue;
    }

    if (!group.HasMember("Updates") || !group["Updates"].IsArray()) {
      continue;
    }

    for (const auto& entry : group["Updates"].GetArray()) {
      if (!entry.IsObject()) {
        continue;
      }

      RemoteTitleUpdate update = {};
      update.media_id = group_media;

      if (entry.HasMember("TitleUpdateID") &&
          entry["TitleUpdateID"].IsString()) {
        update.id = string_util::from_string<uint32_t>(
            entry["TitleUpdateID"].GetString());
      }

      if (entry.HasMember("Version") && entry["Version"].IsString()) {
        update.version = entry["Version"].GetString();
      }

      if (entry.HasMember("BaseVersion") && entry["BaseVersion"].IsString()) {
        update.base_version = entry["BaseVersion"].GetString();
      }

      if (entry.HasMember("Name") && entry["Name"].IsString()) {
        update.name = entry["Name"].GetString();
      }

      if (entry.HasMember("UploadDate") && entry["UploadDate"].IsString()) {
        update.upload_date = entry["UploadDate"].GetString();
      }

      if (entry.HasMember("hash") && entry["hash"].IsString()) {
        update.hash = entry["hash"].GetString();
      }

      if (entry.HasMember("Size") && entry["Size"].IsString()) {
        update.listed_size =
            string_util::from_string<uint64_t>(entry["Size"].GetString());
      }

      if (update.id) {
        updates.push_back(update);
      }
    }
  }

  // Newest version first. Versions are plain integers in the catalogue.
  std::sort(updates.begin(), updates.end(),
            [](const RemoteTitleUpdate& a, const RemoteTitleUpdate& b) {
              return string_util::from_string<uint32_t>(a.version) >
                     string_util::from_string<uint32_t>(b.version);
            });

  for (RemoteTitleUpdate& update : updates) {
    NameFromHub(title_id, &update);
  }

  return updates;
}

bool TitleUpdateDownloader::Download(
    const RemoteTitleUpdate& update, const std::filesystem::path& dest_path,
    const std::function<void(uint64_t, uint64_t)>& progress,
    const std::atomic<bool>* cancel) {
  if (!update.id) {
    return false;
  }

  if (!DownloadUrl(BuildDownloadUrl(update.id), dest_path, progress, cancel)) {
    return false;
  }

  // Verify before anyone installs it.
  //
  // The catalogue's listed hash is NOT a checksum of the download - it is the
  // package's own content hash, stored inside the STFS header at 0x32C, which
  // is how the console identifies the update. Checking it against a hash of
  // the file would reject every download. So verification is:
  //
  //   1. the package is an STFS container (LIVE / PIRS / CON )
  //   2. the header carries the exact content hash the catalogue advertised
  //   3. the length matches what was advertised - Size is listed in KiB
  //
  // Together those say "this is the package that was offered, complete".
  if (!VerifyPackage(dest_path, update)) {
    std::error_code ec;
    std::filesystem::remove(dest_path, ec);
    return false;
  }

  return true;
}

bool TitleUpdateDownloader::DownloadUrl(
    const std::string& url, const std::filesystem::path& dest_path,
    const std::function<void(uint64_t, uint64_t)>& progress,
    const std::atomic<bool>* cancel) {
  CURL* curl = curl_easy_init();

  if (!curl) {
    return false;
  }

  DownloadState state = {};
  state.progress = progress ? &progress : nullptr;
  state.cancel = cancel;
  state.file = xe::filesystem::OpenFile(dest_path, "wb");

  if (!state.file) {
    curl_easy_cleanup(curl);
    XELOGE("Cannot write download to {}", xe::path_to_utf8(dest_path));
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, NexiaUserAgent());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFileCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, DownloadProgressCallback);

  const CURLcode result = curl_easy_perform(curl);

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);

  fclose(state.file);

  const bool cancelled = cancel && cancel->load();

  if (result != CURLE_OK || status != 200 || state.write_failed || cancelled) {
    std::error_code ec;
    std::filesystem::remove(dest_path, ec);

    if (!cancelled) {
      XELOGE("Download of {} failed: curl {} http {}", url,
             static_cast<int>(result), status);
    }

    return false;
  }

  return true;
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
