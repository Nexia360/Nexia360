/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_game_library.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>

#include "third_party/fmt/include/fmt/format.h"

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/string_util.h"
#include "xenia/base/utf8.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/util/played_db.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xna/xna_launcher.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

// Seconds since 1970 to a FILETIME, which is what the scripts read LatestTime
// in: 100ns ticks since 1601.
uint64_t UnixToFileTime(int64_t unix_seconds) {
  if (unix_seconds <= 0) {
    return 0;
  }
  constexpr uint64_t kEpochDelta = 11644473600ULL;
  return (uint64_t(unix_seconds) + kEpochDelta) * 10000000ULL;
}

// Title icons, kept for the process so the scene drawer can reach them by
// path while it draws. Entries are only ever added, so a span handed out
// stays valid - erasing one would leave the drawer holding freed bytes.
std::mutex& IconMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<uint32_t, std::vector<uint8_t>>& Icons() {
  static std::map<uint32_t, std::vector<uint8_t>> icons;
  return icons;
}

void RememberIcon(uint32_t title_id, const std::vector<uint8_t>& png) {
  if (!title_id || png.empty()) {
    return;
  }
  std::lock_guard lock(IconMutex());
  Icons()[title_id] = png;
}

std::string IconPathFor(uint32_t title_id, const std::vector<uint8_t>& png) {
  if (!title_id || png.empty()) {
    return {};
  }
  return std::string(kTitleIconScheme) + fmt::format("{:08X}", title_id);
}

}  // namespace

PlayedSort GameLibrarySortFor(uint32_t sort_order) {
  switch (sort_order) {
    case uint32_t(PlayedSort::kTitleName):
    case uint32_t(PlayedSort::kMostPlayed):
    case uint32_t(PlayedSort::kFirstPlayed):
    case uint32_t(PlayedSort::kByDate):
      return PlayedSort(sort_order);
    // 1 is the dashboard's default, and 0 is a filter that named none, which
    // DataSet.Library also treats as 1.
    default:
      return PlayedSort::kMostRecentlyPlayed;
  }
}

std::vector<GameLibraryItem> BuildGameLibrary(PlayedSort sort) {
  std::vector<GameLibraryItem> items;
  KernelState* state = KernelState::shared();
  if (!state || !state->emulator()) {
    return items;
  }
  Emulator* emulator = state->emulator();

  // What has been launched here. This is the only source with a path, so it
  // decides what can actually be started.
  std::unordered_map<uint32_t, size_t> by_title_id;
  for (const PlayedTitle& played : emulator->played_db()->GetRecent(0, sort)) {
    std::error_code ec;
    if (played.path.empty() || !std::filesystem::exists(played.path, ec)) {
      continue;
    }
    GameLibraryItem item;
    item.title_id = played.title_id;
    item.name = played.title_name;
    item.path = xe::path_to_utf8(played.path);
    item.media_id = played.media_id;
    item.content_type = played.content_type
                            ? played.content_type
                            : uint32_t(XContentType::kXbox360Title);
    item.category = played.category;
    item.is_xna = played.is_xna;
    item.latest_time = UnixToFileTime(played.last_run_utc);
    item.play_seconds = played.play_seconds;
    item.run_count = played.run_count;
    item.icon = played.icon;
    if (item.title_id) {
      by_title_id[item.title_id] = items.size();
    }
    items.push_back(std::move(item));
  }

  // Community titles sitting in the library that have never been started.
  // Every candidate is opened to read its name, which is why this reads the
  // dedicated folder rather than the whole content root.
  const std::filesystem::path library =
      emulator->storage_root() / "xna_library";
  std::error_code ec;
  if (std::filesystem::is_directory(library, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(library, ec)) {
      // A directory counts: a title built locally has no STFS container to
      // arrive in, and IsXnaPackage accepts either.
      if (!entry.is_regular_file() && !entry.is_directory()) {
        continue;
      }
      xna::XnaPackageInfo info;
      if (!xna::IsXnaPackage(entry.path(), &info)) {
        continue;
      }
      const auto found = by_title_id.find(info.title_id);
      if (found != by_title_id.end()) {
        // Already known from having been played; just make sure it reads as
        // the community title it is.
        items[found->second].is_xna = true;
        items[found->second].content_type = uint32_t(XContentType::kXNA);
        continue;
      }
      GameLibraryItem item;
      item.title_id = info.title_id;
      item.name = info.display_name;
      const size_t dot = item.name.find_last_of('.');
      if (dot != std::string::npos) {
        item.name = item.name.substr(0, dot);
      }
      item.path = xe::path_to_utf8(entry.path());
      item.content_type = uint32_t(XContentType::kXNA);
      item.is_xna = true;
      if (item.title_id) {
        by_title_id[item.title_id] = items.size();
      }
      items.push_back(std::move(item));
    }
  }

  // The signed-in profile's play history, which is where the achievement
  // numbers and IsInPlayHistory come from. A title in the history that this
  // machine cannot launch is not added: the tile would have nothing to do
  // with it.
  XamState* xam = state->xam_state();
  UserProfile* profile = xam ? xam->GetUserProfile(uint32_t(0)) : nullptr;
  if (profile && xam->user_tracker()) {
    for (const TitleInfo& played :
         xam->user_tracker()->GetPlayedTitles(profile->xuid())) {
      const auto found = by_title_id.find(played.id);
      if (found == by_title_id.end()) {
        continue;
      }
      GameLibraryItem& item = items[found->second];
      item.in_play_history = true;
      item.achievements_possible = played.achievements_count;
      item.achievements_earned = played.unlocked_achievements_count;
      item.gamerscore_possible = played.gamerscore_amount;
      item.gamerscore_earned = played.title_earned_gamerscore;
      if (item.name.empty()) {
        item.name = xe::to_utf8(played.title_name);
      }
      if (item.icon.empty() && !played.icon.empty()) {
        item.icon.assign(played.icon.begin(), played.icon.end());
      }
    }
  }

  // Nothing should ever have put the dashboard in here, but the tile that
  // lists this would offer to launch itself if anything ever did.
  items.erase(std::remove_if(items.begin(), items.end(),
                             [](const GameLibraryItem& item) {
                               return item.title_id == kDashboardID ||
                                      item.path.empty();
                             }),
              items.end());

  // One ordering over the merged list - the played rows arrive in it already,
  // but the community titles appended above have not been through it.
  switch (sort) {
    case PlayedSort::kTitleName:
      std::stable_sort(items.begin(), items.end(),
                       [](const GameLibraryItem& a, const GameLibraryItem& b) {
                         if (a.name.empty() != b.name.empty()) {
                           return b.name.empty();
                         }
                         return xe::utf8::lower_ascii(a.name) <
                                xe::utf8::lower_ascii(b.name);
                       });
      break;
    case PlayedSort::kMostPlayed:
      std::stable_sort(items.begin(), items.end(),
                       [](const GameLibraryItem& a, const GameLibraryItem& b) {
                         return a.play_seconds > b.play_seconds;
                       });
      break;
    case PlayedSort::kFirstPlayed:
      std::stable_sort(items.begin(), items.end(),
                       [](const GameLibraryItem& a, const GameLibraryItem& b) {
                         return a.latest_time < b.latest_time;
                       });
      break;
    case PlayedSort::kMostRecentlyPlayed:
    case PlayedSort::kByDate:
    default:
      // Most recently played, which is what the hub opens on. A community
      // title never started has no time at all and lands at the end.
      std::stable_sort(items.begin(), items.end(),
                       [](const GameLibraryItem& a, const GameLibraryItem& b) {
                         return a.latest_time > b.latest_time;
                       });
      break;
  }

  // Park the art where the scene drawer can reach it by path, and give each
  // row the path that names it.
  for (GameLibraryItem& item : items) {
    RememberIcon(item.title_id, item.icon);
    item.image_path = IconPathFor(item.title_id, item.icon);
  }
  return items;
}

std::span<const uint8_t> GameLibraryIcon(const std::string& image_path) {
  const std::string_view scheme(kTitleIconScheme);
  if (image_path.size() != scheme.size() + 8 ||
      image_path.compare(0, scheme.size(), scheme) != 0) {
    return {};
  }
  const std::string digits = image_path.substr(scheme.size());
  if (digits.find_first_not_of("0123456789ABCDEFabcdef") != std::string::npos) {
    return {};
  }
  const uint32_t title_id =
      string_util::from_string<uint32_t>(digits, /*force_hex=*/true);
  std::lock_guard lock(IconMutex());
  const auto found = Icons().find(title_id);
  if (found == Icons().end()) {
    return {};
  }
  return std::span<const uint8_t>(found->second);
}

bool LaunchGameLibraryItem(const std::string& host_path) {
  KernelState* state = KernelState::shared();
  if (host_path.empty() || !state || !state->emulator()) {
    return false;
  }
  std::error_code ec;
  const std::filesystem::path path = xe::to_path(host_path);
  if (!std::filesystem::exists(path, ec)) {
    XELOGW("GameLibrary: {} is gone; not launching it", host_path);
    return false;
  }

  auto xam = state->GetKernelModule<XamModule>("xam.xex");
  if (!xam) {
    return false;
  }
  RecordLaunchOrigin();
  XamModule::LoaderData& loader_data = xam->loader_data();
  loader_data.host_path = xe::path_to_utf8(std::filesystem::absolute(path, ec));
  // ResolveLaunchTarget strips the filename off a .xex and rejoins the launch
  // path, so a bare .xex has to name itself or it resolves to its folder.
  loader_data.launch_path = path.extension() == ".xex"
                                ? xe::path_to_utf8(path.filename())
                                : std::string();
  loader_data.launch_flags = 0;
  loader_data.launch_data.clear();
  loader_data.command_line.clear();

  XELOGI("GameLibrary: launching {}", loader_data.host_path);
  // Not ReloadForLaunch: that terminates the calling GUEST thread, and this
  // runs on the host thread that ticks the scene. The delegate marshals to
  // the UI thread, which is where a title switch has to happen anyway.
  state->emulator()->on_title_switch();
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
