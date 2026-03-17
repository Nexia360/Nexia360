/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_FRIENDFIND_UI_H_
#define XENIA_KERNEL_XAM_UI_FRIENDFIND_UI_H_

#include "xenia/kernel/xam/xam_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

// Represents a player found in an active session
struct SessionPlayer {
  std::string gamertag;
  uint64_t xuid;
  std::string title_name;
  uint32_t title_id;
  std::string media_id;
  std::string version;
  std::string presence;
  int player_count;
  int max_players;
};

class FriendFindUI : public XamDialog {
 public:
  FriendFindUI(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile);

 private:
  void OnDraw(ImGuiIO& io) override;
  void RefreshSessions();
  void DrawPlayerEntry(const SessionPlayer& player, int index);

  bool has_opened_ = false;
  bool pending_close_ = false;
  bool is_loading_ = false;
  bool filter_same_game_ = true;
  int selected_index_ = -1;

  UserProfile* profile_;
  std::vector<SessionPlayer> players_;
  uint32_t current_title_id_ = 0;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
