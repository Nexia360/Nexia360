/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_FRIEND_APPROVAL_UI_H_
#define XENIA_KERNEL_XAM_UI_FRIEND_APPROVAL_UI_H_

#include <map>
#include <memory>
#include <vector>

#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/xam/xam_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

// Asks the player to approve or decline the friend requests being held for
// them. One prompt handles the whole queue, showing who is asking - gamerpic
// and gamertag - one at a time.
// NOT an ImGuiDialog - drawn as a nested popup from inside the friends popup
// so ImGui stacks it, rather than replacing the popup underneath.
class FriendApprovalUI {
 public:
  FriendApprovalUI(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile,
                   const std::vector<HubPlayer>& requests);

  // Draws one frame. Returns false once finished, so the owner can drop it.
  bool Draw();

 private:
  void LoadGamerpics();

  enum class Answer {
    kApprove,
    kDeny,
    kBlock,
    // Answers nothing: the request stays on the hub and simply is not shown
    // again until the next launch.
    kLater,
  };

  void Respond(Answer answer);

  std::vector<HubPlayer> requests_;
  std::map<uint64_t, std::shared_ptr<xe::ui::ImmediateTexture>> gamerpics_;

  size_t current_ = 0;

  bool has_opened_ = false;

  xe::ui::ImGuiDrawer* imgui_drawer_;
  UserProfile* profile_;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
