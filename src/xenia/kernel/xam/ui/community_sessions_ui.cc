/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/community_sessions_ui.h"
#include "xenia/kernel/XLiveAPI.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

ShowCommunitySessionsUI::ShowCommunitySessionsUI(
    xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile)
    : XamDialog(imgui_drawer), profile_(profile) {
  sessions_ = XLiveAPI::GetTitleSessions();
}

void ShowCommunitySessionsUI::OnDraw(ImGuiIO& io) {
  auto* drawer = imgui_drawer();
  auto* focus_manager = drawer->GetFocusManager();

  if (pending_close_) {
    if (!drawer->IsAnyGamepadActionPressed()) {
      focus_manager->UIDropFocus("SessionsDialog");
      Close();
    }
    return;
  }

  if (!sessions_args.sessions_open) {
    focus_manager->UISetFocus("SessionsDialog");
    sessions_args.sessions_open = true;
    sessions_args.filter_own = true;
    ImGui::OpenPopup("Sessions");
  }

  const auto& input = focus_manager->XamInputFocus("SessionsDialog");

  if (input.ShouldClose()) {
    sessions_args.sessions_open = false;
    pending_close_ = true;
    return;
  }

  xeDrawSessionsContent(imgui_drawer(), profile_, sessions_args, &sessions_);

  if (!sessions_args.sessions_open) {
    pending_close_ = true;
  }
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
