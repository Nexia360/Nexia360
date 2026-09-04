/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_FRIENDS_UI_H_
#define XENIA_KERNEL_XAM_UI_FRIENDS_UI_H_

#include <chrono>
#include <future>
#include <memory>
#include <vector>

#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/xam/ui/blocked_list_ui.h"
#include "xenia/kernel/xam/ui/friend_approval_ui.h"
#include "xenia/kernel/xam/ui/player_search_ui.h"
#include "xenia/kernel/xam/ui/recent_players_ui.h"
#include "xenia/kernel/xam/xam_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

struct FriendsUIArgs {
  FriendsContentArgs content_args = {};
  std::future<void> friends_presence_sync;
  std::vector<X_ONLINE_FRIEND> friends_presence;
  std::future<std::map<uint64_t, std::shared_ptr<xe::ui::ImmediateTexture>>>
      immediate_gamerpics;
  std::map<uint64_t, std::shared_ptr<xe::ui::ImmediateTexture>>
      immediate_gamerpics_result;
  // One approval prompt at a time - it works through the whole queue itself.
  bool approval_prompt_open = false;

  // Child dialogs. Owned here and drawn from INSIDE the friends popup, so
  // ImGui nests them one level deeper. As separate root-level dialogs they
  // replaced the friends popup and collapsed everything under it.
  std::unique_ptr<PlayerSearchUI> player_search;
  std::unique_ptr<RecentPlayersUI> recent_players;
  std::unique_ptr<BlockedListUI> blocked_list;
  std::unique_ptr<FriendApprovalUI> approval_prompt;

  // Friend-request polling. Both run off the UI thread and on an interval:
  // asking the hub every frame would be a request per frame and would stall
  // drawing on the round trip.
  std::future<std::vector<HubPlayer>> incoming_requests_query;
  std::future<std::vector<uint64_t>> approvals_query;
  std::chrono::steady_clock::time_point next_friend_poll = {};
};

class FriendsUI : public XamDialog {
 public:
  FriendsUI(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile);

 private:
  void OnDraw(ImGuiIO& io) override;

  bool has_opened_ = false;
  bool pending_close_ = false;
  UserProfile* profile_;
  ui::FriendsUIArgs friends_ui_args_ = {};
};

bool xeDrawFriendsUI(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile,
                     FriendsUIArgs& args);

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
