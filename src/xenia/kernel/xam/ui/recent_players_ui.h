/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_RECENT_PLAYERS_UI_H_
#define XENIA_KERNEL_XAM_UI_RECENT_PLAYERS_UI_H_

#include <future>
#include <map>
#include <memory>
#include <vector>

#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/xam/xam_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

// Everyone this player has shared a session with in the last 48 hours. The
// window is the hub's: encounter records expire on their own.
// NOT an ImGuiDialog - drawn as a nested popup from inside the friends popup
// so ImGui stacks it, rather than replacing the popup underneath.
class RecentPlayersUI {
 public:
  RecentPlayersUI(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile);

  // Draws one frame. Returns false once finished, so the owner can drop it.
  bool Draw();

 private:
  void Refresh();
  void CollectResults();
  void DrawPlayerRow(const HubPlayer& player);
  void DrawPlayerCard();

  std::vector<HubPlayer> players_;
  std::future<std::vector<HubPlayer>> players_query_;
  std::map<uint64_t, std::shared_ptr<xe::ui::ImmediateTexture>> gamerpics_;

  // Which row's card is showing. Held by XUID rather than pointer: Refresh()
  // replaces the vector underneath us.
  uint64_t card_xuid_ = 0;
  bool card_opened_ = false;

  bool has_opened_ = false;
  bool loading_ = false;

  xe::ui::ImGuiDrawer* imgui_drawer_;
  UserProfile* profile_;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
