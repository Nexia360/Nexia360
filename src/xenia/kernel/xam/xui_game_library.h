/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_GAME_LIBRARY_H_
#define XENIA_KERNEL_XAM_XUI_GAME_LIBRARY_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xenia/kernel/util/played_db.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// The scheme a library row's ImagePath uses. A title icon is a PNG in
// played.db, not an entry in any XZP, so it cannot be named the way package
// art is - the scene drawer asks GameLibraryIcon for anything under this.
inline constexpr const char* kTitleIconScheme = "titleicon://";

// One row of the dashboard's My Games tile, in the shape its scripts read.
//
// The property names below are the ones dash.xex's own binding table declares
// for Xbox.Object.GameLibraryTitleInfo (luatitleinfo.cpp, which prints
// "Invalid Property (%s) for item: %S" for anything else), so this is not an
// invented interface - it is the console's, filled from what this machine has.
struct GameLibraryItem {
  uint32_t title_id = 0;
  std::string name;
  // The host path this title launches from. The scripts pass it straight back
  // to Launch, so it is the whole identity of a row as far as they care.
  std::string path;
  std::string media_id;
  // An XContentType. kXbox360Title for a disc or an installed game, kXNA for
  // a community title.
  uint32_t content_type = 0;
  // XContentMetadata::category. Traced out of dash.xex: its title record holds
  // ContentType at +0x430 and Category at +0x434, both copied from the content
  // record xam hands it (+0x1C and +0x14C), and ConvertToLibraryItem turns
  // "kXbox360Title with a non-zero category" into media type 61, which is the
  // Apps type. Zero means game.
  uint32_t category = 0;
  bool is_xna = false;

  // Which of the two hub tiles this row belongs to.
  bool is_app() const {
    return content_type == uint32_t(XContentType::kXbox360Title) && category;
  }
  // Whether the signed-in profile has actually played it, which is what the
  // dashboard's IsInPlayHistory means and what gates the achievement line.
  bool in_play_history = false;
  // FILETIME, the unit LatestTime is read in.
  uint64_t latest_time = 0;
  int64_t play_seconds = 0;
  int64_t run_count = 0;
  uint32_t achievements_earned = 0;
  uint32_t achievements_possible = 0;
  uint32_t gamerscore_earned = 0;
  uint32_t gamerscore_possible = 0;
  // The title's own icon as a PNG, when one is known, and the path that names
  // it - "titleicon://XXXXXXXX", or empty when there is no icon.
  std::vector<uint8_t> icon;
  std::string image_path;
};

// Everything this machine can launch, newest first.
//
// Three sources, merged on title id: played.db for what has been run (and so
// for paths, icons and play time), the xna_library folder for community titles
// that are installed but have never been started, and the signed-in profile's
// play history for achievements. The dashboard is never in it - nothing
// records it, because the My Games tile would otherwise offer to launch the
// thing drawing it.
std::vector<GameLibraryItem> BuildGameLibrary(
    PlayedSort sort = PlayedSort::kMostRecentlyPlayed);

// The ordering a dashboard filter's SortOrder asks for. Zero means the filter
// stated none, which DataSet.Library treats as 1 - most recently played.
PlayedSort GameLibrarySortFor(uint32_t sort_order);

// The PNG behind a "titleicon://XXXXXXXX" path, or empty for anything else.
// BuildGameLibrary fills this as it goes, so a row's art is already in hand by
// the time the scene draws it. The bytes live for the process.
std::span<const uint8_t> GameLibraryIcon(const std::string& image_path);

// Starts a title by the host path a library row carries. This is the same
// in-place switch XamLoaderLaunchTitle performs - it sets the loader data and
// raises Emulator::on_title_switch, which the UI thread turns into
// EmulatorWindow::SwitchTitle. False when there is nothing to launch.
bool LaunchGameLibraryItem(const std::string& host_path);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XUI_GAME_LIBRARY_H_
