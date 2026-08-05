/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_state.h"

#include <string>
#include <vector>

#include "third_party/rapidjson/include/rapidjson/document.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/emulator.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/util/friends_util.h"
#include "xenia/kernel/util/net_utils.h"
#include "xenia/kernel/xam/online_schema.h"
#include "xenia/kernel/xnet.h"

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace kernel {
namespace xam {

XamState::XamState(Emulator* emulator, KernelState* kernel_state)
    : kernel_state_(kernel_state) {
  app_manager_ = std::make_unique<AppManager>();

  auto content_root = emulator->content_root();
  if (!content_root.empty()) {
    content_root = std::filesystem::absolute(content_root);
  }
  content_manager_ =
      std::make_unique<ContentManager>(kernel_state, content_root);

  user_tracker_ = std::make_unique<UserTracker>();
  profile_manager_ =
      std::make_unique<ProfileManager>(kernel_state, user_tracker_.get());
  // Opened before FriendsManager, which takes a pointer to it. Lives in the
  // Library folder alongside the per-title update folders, not in the content
  // tree - it is host state spanning every profile and title. Open() creates
  // the folder if it is not there yet.
  friends_db_ = std::make_unique<FriendsDB>();
  if (!content_root.empty()) {
    friends_db_->Open(content_root.parent_path() / "Library" /
                      "friends.sqlite");
  }

  friends_manager_ = std::make_unique<FriendsManager>(
      kernel_state, profile_manager_.get(), friends_db_.get());
  achievement_manager_ = std::make_unique<AchievementManager>();
  presence_manager_ = std::make_unique<PresenceManager>(
      kernel_state, profile_manager_.get(), friends_manager_.get());

  LoadOnlineFriends();
  LoadOnlineSchema();
  LoadLanguageLocaleFallback();
  LoadIptvServiceName();

  AppManager::RegisterApps(kernel_state, app_manager_.get());
}

// Total guest region for the online schema. Generous fixed size so a
// build-matched schema fetched from the hub (or a future, larger one) fits in
// place without moving the address the dashboard already read.
static constexpr uint32_t kOnlineSchemaRegionSize = 0x40000;  // 256 KB

void XamState::LoadOnlineSchema() {
  constexpr uint32_t schema_data_address = 0x80E00000;

  if (kernel_state_->memory()
          ->LookupHeap(0x80000000)
          ->AllocFixed(schema_data_address, kOnlineSchemaRegionSize, 0x1000,
                       kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite)) {
    online_schema_data_address = schema_data_address;
    // The embedded blob is the fallback so there is always a valid schema even
    // when the hub is unreachable; it is replaced lazily by the build-matched
    // schema on the first XamGetOnlineSchema (EnsureOnlineSchema).
    WriteOnlineSchemaBlob(OnlineSchemaData_v6_5, sizeof(OnlineSchemaData_v6_5));
  }
}

void XamState::WriteOnlineSchemaBlob(const uint8_t* blob, uint32_t size) {
  if (!online_schema_data_address) {
    return;
  }
  auto* schema_ptr =
      kernel_state_->memory()->TranslateVirtual<XONLINE_SCHEMA_DATA*>(
          online_schema_data_address);
  std::memcpy(schema_ptr + 1, blob, size);
  schema_ptr->schema_ptr = kernel_state_->memory()->HostToGuestVirtual(
      std::to_address(schema_ptr + 1));
  schema_ptr->schema_size = size;
}

// The retail dashboard version-checks the online schema (XamGetOnlineSchema)
// against its own embedded copy; a ToolVersion mismatch forces an
// ordinal->index remap that produces wrong task indices (empty task URLs -> the
// "Get User Info" retry loop). We fetch the schema matching the running dash
// BUILD from the hub so the versions align. Runs once; any failure keeps the
// embedded fallback so a missing/older hub never breaks boot.
void XamState::EnsureOnlineSchema() {
  if (online_schema_fetched_ || !online_schema_data_address) {
    return;
  }
  online_schema_fetched_ = true;

  // XEX version string is "major.minor.build.qfe"; we want the build.
  const std::string& version = kernel_state_->emulator()->title_version();
  uint32_t build = 0;
  const size_t p1 = version.find('.');
  const size_t p2 = p1 == std::string::npos ? p1 : version.find('.', p1 + 1);
  if (p2 != std::string::npos) {
    const size_t p3 = version.find('.', p2 + 1);
    const std::string build_str =
        version.substr(p2 + 1, p3 == std::string::npos ? p3 : p3 - (p2 + 1));
    build = xe::string_util::from_string<uint32_t>(build_str);
  }
  if (!build) {
    return;
  }

  // XStorageDownload/Get expect a full URL (no api_address prepend), so build
  // it from the configured hub (GetApiAddress() carries the trailing slash).
  const std::string url =
      XLiveAPI::GetApiAddress() + fmt::format("Dash/schema/{}", build);
  const std::vector<uint8_t> data =
      kernel_state_->GetXboxLiveAPI()->XStorageDownload(url);
  if (data.empty()) {
    XELOGW("Online schema fetch for build {} returned no data", build);
    return;
  }
  if (BuildOnlineSchemaFromJson(std::string(data.begin(), data.end()))) {
    XELOGI("Online schema for build {} loaded from hub", build);
  }
}

bool XamState::BuildOnlineSchemaFromJson(const std::string& json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("header") ||
      !doc.HasMember("entries") || !doc.HasMember("urls")) {
    XELOGE("Online schema JSON invalid");
    return false;
  }

  auto as_u32 = [](const rapidjson::Value& v) -> uint32_t {
    if (v.IsUint()) {
      return v.GetUint();
    }
    if (v.IsInt()) {
      return static_cast<uint32_t>(v.GetInt());
    }
    if (v.IsUint64()) {
      return static_cast<uint32_t>(v.GetUint64());
    }
    if (v.IsInt64()) {
      return static_cast<uint32_t>(v.GetInt64());
    }
    return 0;
  };
  auto field = [&](const rapidjson::Value& obj, const char* k) -> uint32_t {
    return obj.HasMember(k) ? as_u32(obj[k]) : 0;
  };

  std::vector<uint8_t> blob;
  blob.reserve(kOnlineSchemaRegionSize);
  auto put16 = [&](uint32_t v) {
    blob.push_back(static_cast<uint8_t>(v >> 8));
    blob.push_back(static_cast<uint8_t>(v));
  };
  auto put32 = [&](uint32_t v) {
    blob.push_back(static_cast<uint8_t>(v >> 24));
    blob.push_back(static_cast<uint8_t>(v >> 16));
    blob.push_back(static_cast<uint8_t>(v >> 8));
    blob.push_back(static_cast<uint8_t>(v));
  };

  // SCHEMA_HEADER (0x2C), big-endian, in struct field order.
  const auto& h = doc["header"];
  put16(field(h, "SchemaVersionMajor"));
  put16(field(h, "SchemaVersionMinor"));
  put32(field(h, "ToolVersion"));
  put32(field(h, "Flags"));
  put32(field(h, "CompressedSize"));
  put32(field(h, "UncompressedSize"));
  put32(field(h, "ConstantsTableOffset"));
  put16(field(h, "ConstantsTableSize"));
  put16(field(h, "ConstantSize"));
  put32(field(h, "UrlTableOffset"));
  put16(field(h, "UrlTableSize"));
  put16(field(h, "UrlTableDataSize"));
  put16(field(h, "HeaderSize"));
  put16(field(h, "ExtensionDataSize"));
  put16(field(h, "SchemaTableEntries"));
  put16(field(h, "SchemaTableEntrySize"));

  // ORDINAL_TO_INDEX[] : [[ordinal, index], ...]
  if (doc.HasMember("ordinalToIndex")) {
    for (const auto& pair : doc["ordinalToIndex"].GetArray()) {
      put16(as_u32(pair[0]));
      put16(as_u32(pair[1]));
    }
  }
  // SCHEMA_TABLE_ENTRY[]
  for (const auto& e : doc["entries"].GetArray()) {
    put16(field(e, "reqSize"));
    put16(field(e, "respSize"));
    put32(field(e, "reqOff"));
    put32(field(e, "respOff"));
    put32(field(e, "maxReqAgg"));
    put32(field(e, "maxRespAgg"));
    put16(field(e, "svcIdIdx"));
    put16(field(e, "urlIdx"));
  }
  // schema-data descriptor blob (hex).
  if (doc.HasMember("schemaData_hex")) {
    const char* hex = doc["schemaData_hex"].GetString();
    const size_t hex_len = doc["schemaData_hex"].GetStringLength();
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
      }
      return 0;
    };
    for (size_t i = 0; i + 1 < hex_len; i += 2) {
      blob.push_back(
          static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
  }
  // constants (u32[])
  if (doc.HasMember("constants")) {
    for (const auto& c : doc["constants"].GetArray()) {
      put32(as_u32(c));
    }
  }
  // URL offsets + data, rebuilt from the ordered url string list.
  {
    std::vector<uint16_t> offsets;
    std::vector<uint8_t> data;
    for (const auto& u : doc["urls"].GetArray()) {
      offsets.push_back(static_cast<uint16_t>(data.size()));
      const char* s = u.GetString();
      data.insert(data.end(), s, s + u.GetStringLength());
      data.push_back(0);
    }
    for (uint16_t o : offsets) {
      put16(o);
    }
    blob.insert(blob.end(), data.begin(), data.end());
  }

  if (blob.size() + sizeof(XONLINE_SCHEMA_DATA) > kOnlineSchemaRegionSize) {
    XELOGE("Online schema too large ({} bytes)", blob.size());
    return false;
  }
  WriteOnlineSchemaBlob(blob.data(), static_cast<uint32_t>(blob.size()));
  return true;
}

void XamState::LoadLanguageLocaleFallback() {
  const std::array<std::u16string, 18> locale_data = {
      u"",      u"",      u"ja-JP", u"de-DE", u"fr-FR",  u"es-ES",
      u"it-IT", u"ko-KR", u"zh-TW", u"pt-BR", u"zh-CHS", u"pl-PL",
      u"ru-RU", u"sv-SE", u"tr-TR", u"nb-NO", u"nl-NL",  u"zh-CHS"};

  constexpr uint32_t array_start = 0x80D00000;

  if (kernel_state_->memory()
          ->LookupHeap(0x80000000)
          ->AllocFixed(array_start, 0xC8, 0x1000, kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite)) {
    char16_t* ptr =
        kernel_state_->memory()->TranslateVirtual<char16_t*>(array_start);

    for (size_t i = 1; i < locale_data.size(); i++) {
      language_fallback_address_[i] =
          kernel_state_->memory()->HostToGuestVirtual(ptr);
      ptr += xe::string_util::copy_and_swap_truncating(
                 ptr, locale_data.at(i), locale_data.at(i).size() + 1) +
             1;
    }
  }
}

void XamState::LoadIptvServiceName() {
  constexpr uint32_t address = 0x80D10000;

  if (kernel_state_->memory()
          ->LookupHeap(0x80000000)
          ->AllocFixed(address, 0x78, 0x1000, kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite)) {
    iptv_name_address_ = address;
  }
}

void XamState::LoadOnlineFriends() {
  for (uint32_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    const auto profile = GetUserProfile(user_index);

    if (profile) {
      friends_manager_->AddFriends(
          profile->xuid(),
          LoadProfileFriends(profile->xuid(), friends_db_.get()));
    }
  }
}

UserProfile* XamState::GetUserProfile(uint32_t user_index) const {
  if (user_index >= XUserMaxUserCount && user_index < XUserIndexLatest) {
    return nullptr;
  }

  return profile_manager_->GetProfile(static_cast<uint8_t>(user_index));
}

UserProfile* XamState::GetUserProfile(uint64_t xuid) const {
  if (IsOnlineXUID(xuid)) {
    assert_always();
    XELOGI("{}: Using online XUID {:016X}", __func__, xuid);
  }

  return profile_manager_->GetProfile(xuid);
}

UserProfile* XamState::GetUserProfileLive(uint64_t xuid) const {
  return profile_manager_->GetProfileLive(xuid);
}

UserProfile* XamState::GetUserProfileAny(uint64_t xuid) const {
  return profile_manager_->GetProfileAny(xuid);
}

uint8_t XamState::GetUserIndexAssignedToProfileFromXUID(uint64_t xuid) const {
  const uint8_t user_index =
      profile_manager_->GetUserIndexAssignedToProfile(xuid);

  if (user_index != XUserIndexAny) {
    return user_index;
  }

  return profile_manager_->GetUserIndexAssignedToLiveProfile(xuid);
}

bool XamState::IsUserSignedIn(uint32_t user_index) const {
  return profile_manager_->GetProfile(static_cast<uint8_t>(user_index)) !=
         nullptr;
}

bool XamState::IsUserSignedIn(uint64_t xuid) const {
  return GetUserProfileAny(xuid) != nullptr;
}

void XamState::LoadSpaInfo(const SpaInfo* info) {
  if (!info) {
    return;
  }
  // Check if we have loaded SpaInfo already. If yes then check currently loaded
  // version.
  if (spa_info_) {
    // Trying to load spa with lower version, for whatever reason.
    if (*info <= *spa_info_) {
      return;
    }
  }

  spa_info_ = std::make_unique<SpaInfo>(*info);
  spa_info_->Load();
  user_tracker_->UpdateSpaInfo(spa_info_.get());
}

void XamState::StartPeriodicMaintenance() const {
  for (uint32_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    const auto profile = GetUserProfile(user_index);

    if (profile) {
      user_tracker()->StartPeriodicMaintenance(profile->xuid());
    }
  }
}

void XamState::StopPeriodicMaintenance() const {
  for (uint32_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    const auto profile = GetUserProfile(user_index);

    if (profile) {
      user_tracker()->StopPeriodicMaintenance(profile->xuid());
    }
  }
}

void XamState::SetContentRegisterCallback(uint32_t callback) {
  content_register_callback = callback;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
