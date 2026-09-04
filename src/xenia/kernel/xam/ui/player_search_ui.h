/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_PLAYER_SEARCH_UI_H_
#define XENIA_KERNEL_XAM_UI_PLAYER_SEARCH_UI_H_

#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/xam/xam_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

// Player browser: with an empty search box this lists everyone recently active
// on NexiaHub, and typing narrows it to a gamertag search. Either way a row can
// be added as a friend without knowing the XUID.
// NOT an ImGuiDialog - drawn as a nested popup from inside the friends popup
// so ImGui stacks it, rather than replacing the popup underneath.
class PlayerSearchUI {
 public:
  PlayerSearchUI(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile);

  // Draws one frame. Returns false once finished, so the owner can drop it.
  bool Draw();

 private:
  void Refresh();
  void CollectResults();
  void DrawPlayerRow(const HubPlayer& player);

  // Rows the hub returned, newest activity first.
  std::vector<HubPlayer> players_;
  std::future<std::vector<HubPlayer>> players_query_;

  // Gamerpics are fetched per row and cached for the life of the dialog.
  std::map<uint64_t, std::shared_ptr<xe::ui::ImmediateTexture>> gamerpics_;

  char search_[32] = {};
  std::string active_search_;

  bool has_opened_ = false;
  bool searching_ = false;
  bool keyboard_open_ = false;
  bool added_friend_ = false;

  // Capped so a busy hub cannot flood the list.
  static constexpr uint32_t kResultLimit = 100;

  xe::ui::ImGuiDrawer* imgui_drawer_;
  UserProfile* profile_;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
