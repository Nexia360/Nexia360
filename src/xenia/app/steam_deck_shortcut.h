/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_STEAM_DECK_SHORTCUT_H_
#define XENIA_APP_STEAM_DECK_SHORTCUT_H_

#include <filesystem>

namespace xe {
namespace app {

// The one name the library entry, the icon and the log all use.
inline constexpr char kSteamDeckShortcutName[] = "Nexia On Steamdeck";

enum class SteamShortcutResult {
  // Not running on Valve's hardware, so there is nothing to register with.
  kNotADeck,
  // Already in the library - left exactly as it is.
  kAlreadyPresent,
  kAdded,
  // Steam has the shortcut list in memory and writes it over the file when
  // it exits, so anything added now would be thrown away.
  kSteamRunning,
  kFailed,
};

// Puts Nexia in Steam's library as a non-Steam game, pointing at the running
// executable, when this is a Steam Deck and it is not already there. Never
// touches an entry that exists.
SteamShortcutResult EnsureSteamDeckShortcut(
    const std::filesystem::path& storage_root);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_STEAM_DECK_SHORTCUT_H_
