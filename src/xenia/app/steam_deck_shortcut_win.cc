/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/steam_deck_shortcut.h"

namespace xe {
namespace app {

// There is no Steam Deck to be on here.
SteamShortcutResult EnsureSteamDeckShortcut(
    const std::filesystem::path& storage_root) {
  return SteamShortcutResult::kNotADeck;
}

}  // namespace app
}  // namespace xe
