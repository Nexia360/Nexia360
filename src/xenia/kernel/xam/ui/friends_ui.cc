/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/friends_ui.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/xam/ui/friendfind_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

FriendsUI::FriendsUI(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile)
    : XamDialog(imgui_drawer), profile_(profile) {}

void FriendsUI::OnDraw(ImGuiIO& io) {
  auto* drawer = imgui_drawer();
  auto* focus_manager = drawer->GetFocusManager();

  if (pending_close_) {
    if (!drawer->IsAnyGamepadActionPressed()) {
      focus_manager->UIDropFocus("FriendsDialog");
      Close();
    }
    return;
  }

  if (!args.friends_open) {
    focus_manager->UISetFocus("FriendsDialog");
    args.first_draw = true;
    args.refresh_presence_sync = true;
    args.friends_open = true;

    ImGui::OpenPopup("Friends");

    if (XLiveAPI::IsConnectedToServer()) {
      args.filter_offline = true;
    }
  }

  const auto& input = focus_manager->XamInputFocus("FriendsDialog");

  if (input.ShouldClose()) {
    args.friends_open = false;
    pending_close_ = true;
    return;
  }

  // Handle Find Players dialog opening
  if (args.find_players_open) {
    args.find_players_open = false;
    // Open as child dialog
    focus_manager->UIChildFocus("FriendsDialog", "FriendFindUI");
    new FriendFindUI(drawer, profile_);
  }

  xeDrawFriendsContent(imgui_drawer(), drawer->GetFocusManager(), profile_,
                       args, &presences);

  if (!args.friends_open) {
    pending_close_ = true;
  }
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
