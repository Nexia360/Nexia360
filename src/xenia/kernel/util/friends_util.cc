/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/rapidcsv/src/rapidcsv.h"

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/util/friends_db.h"
#include "xenia/kernel/util/friends_util.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xnet.h"

DEFINE_string(friends_xuids, "", "Comma delimited list of XUIDs. (Max 100)",
              "Live");

namespace xe {
namespace kernel {

std::vector<std::string> ParseDelimitedList(std::string_view csv,
                                            uint32_t count) {
  std::vector<std::string> parsed_list;

  std::stringstream sstream(csv.data());

  rapidcsv::Document delimiter(
      sstream, rapidcsv::LabelParams(-1, -1),
      rapidcsv::SeparatorParams(',', true), rapidcsv::ConverterParams(),
      rapidcsv::LineReaderParams(true /* pSkipCommentLines */,
                                 '#' /* pCommentPrefix */,
                                 true /* pSkipEmptyLines */));

  if (!delimiter.GetRowCount()) {
    return parsed_list;
  }

  parsed_list = delimiter.GetRow<std::string>(0);

  parsed_list.erase(std::remove_if(parsed_list.begin(), parsed_list.end(),
                                   [](const std::string& element) {
                                     return element.empty();
                                   }),
                    parsed_list.end());

  if (count != 0 && parsed_list.size() > count) {
    parsed_list.resize(count);
  }

  return parsed_list;
}

std::string BuildCSVFromVector(std::vector<std::string>& data, uint32_t count) {
  rapidcsv::Document doc(
      "", rapidcsv::LabelParams(-1, -1), rapidcsv::SeparatorParams(',', true),
      rapidcsv::ConverterParams(),
      rapidcsv::LineReaderParams(true /* pSkipCommentLines */,
                                 '#' /* pCommentPrefix */,
                                 true /* pSkipEmptyLines */));

  std::ostringstream csv;

  if (count != 0 && data.size() > count) {
    data.resize(count);
  }

  doc.InsertRow(0, data);
  doc.Save(csv);

  return xe::string_util::trim(csv.str());
}

std::set<uint64_t> ParseFriendsXUIDs() {
  const auto& xuids = cvars::friends_xuids;

  const std::vector<std::string> friends_xuids =
      ParseDelimitedList(xuids, X_ONLINE_MAX_FRIENDS);

  std::set<uint64_t> xuids_parsed;

  uint32_t index = 0;
  for (const auto& friend_xuid : friends_xuids) {
    const uint64_t xuid = string_util::from_string<uint64_t>(
        xe::string_util::trim(friend_xuid), true);

    if (xuid == 0) {
      XELOGI("{}: Skip adding invalid friend XUID!", __func__);
      continue;
    }

    if (index == 0 && xuid <= X_ONLINE_MAX_FRIENDS) {
      kernel_state()->GetXboxLiveAPI()->SetDummyFriendsCount(
          static_cast<uint32_t>(xuid));

      index++;
      continue;
    }

    xuids_parsed.insert(xuid);

    index++;
  }

  return xuids_parsed;
}

std::set<uint64_t> LoadProfileFriends(uint64_t owner_xuid, FriendsDB* db) {
  if (!db || !db->is_open()) {
    // No database (failed to open, or a headless/portable run) - behave
    // exactly as before rather than showing an empty friends list.
    return ParseFriendsXUIDs();
  }

  const uint32_t imported = db->ImportXUIDs(owner_xuid, ParseFriendsXUIDs());
  if (imported) {
    XELOGI("{}: imported {} friend(s) from config for {:016X}", __func__,
           imported, owner_xuid);
  }

  return db->GetFriendXUIDs(owner_xuid);
}

std::vector<uint8_t> GetFriendGamerpic(uint64_t owner_xuid,
                                       uint64_t friend_xuid, bool small_tile) {
  auto* xam_state = kernel_state()->xam_state();
  auto* db = xam_state ? xam_state->friends_db() : nullptr;

  std::vector<uint8_t> cached;

  if (db && db->is_open()) {
    const auto record = db->GetFriend(owner_xuid, friend_xuid);
    if (record.has_value()) {
      cached = small_tile ? record->gamerpic_small : record->gamerpic;
    }
  }

  // Us being offline means there is nothing to ask. Returning empty is fine -
  // callers fall back to the default tile.
  if (!kernel_state()->GetXboxLiveAPI()->IsConnectedToServer()) {
    return cached;
  }

  // A friend who is not online cannot have changed their picture in a way we
  // would see, so the cache stands. Only refetch when there is nothing cached
  // at all.
  bool friend_online = false;
  if (auto* friends_manager =
          xam_state ? xam_state->friends_manager() : nullptr) {
    const auto peer = friends_manager->GetFriend(owner_xuid, friend_xuid);
    if (peer.has_value()) {
      friend_online =
          (peer->state.get() & X_ONLINE_FRIENDSTATE_FLAG_ONLINE) != 0;
    }
  }

  if (!friend_online && !cached.empty()) {
    return cached;
  }

  const auto downloaded = kernel_state()->GetXboxLiveAPI()->GetUserGamerpicTile(
      friend_xuid, small_tile);

  // A failed download must not overwrite what we had, and must not write an
  // empty blob that would later be mistaken for a cached picture.
  if (downloaded.empty()) {
    return cached;
  }

  // Only touch the database when the picture actually changed - an unchanged
  // tile would otherwise rewrite the row on every card that is opened.
  if (downloaded != cached && db && db->is_open()) {
    const auto record = db->GetFriend(owner_xuid, friend_xuid);
    if (record.has_value()) {
      // Keeps the other size as it was; this only replaces the one fetched.
      db->SetGamerpic(owner_xuid, friend_xuid, record->gamerpic_key,
                      small_tile ? record->gamerpic : downloaded,
                      small_tile ? downloaded : record->gamerpic_small);
    }
  }

  return downloaded;
}

void AddFriendToConfig(uint64_t xuid) {
  const auto delimeter = cvars::friends_xuids.empty() ? "" : ",";
  const auto& xuids =
      cvars::friends_xuids + fmt::format("{}{:016X}", delimeter, xuid);

  std::vector<std::string> friend_xuids =
      ParseDelimitedList(xuids, X_ONLINE_MAX_FRIENDS);

  // Remove duplicate xuids
  std::sort(friend_xuids.begin(), friend_xuids.end());
  friend_xuids.erase(std::unique(friend_xuids.begin(), friend_xuids.end()),
                     friend_xuids.end());

  const std::string friends_list =
      BuildCSVFromVector(friend_xuids, X_ONLINE_MAX_FRIENDS);

  OVERRIDE_string(friends_xuids, friends_list);
}

void RemoveFriendFromConfig(uint64_t xuid) {
  auto xuid_str = fmt::format("{:016X}", xuid);

  std::vector<std::string> friend_xuids =
      ParseDelimitedList(cvars::friends_xuids, X_ONLINE_MAX_FRIENDS);

  friend_xuids.erase(
      std::remove(friend_xuids.begin(), friend_xuids.end(), xuid_str),
      friend_xuids.end());

  const std::string friends_list =
      BuildCSVFromVector(friend_xuids, X_ONLINE_MAX_FRIENDS);

  OVERRIDE_string(friends_xuids, friends_list);
}

}  // namespace kernel
}  // namespace xe
