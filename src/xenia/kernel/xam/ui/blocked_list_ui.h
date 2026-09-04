/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_BLOCKED_LIST_UI_H_
#define XENIA_KERNEL_XAM_UI_BLOCKED_LIST_UI_H_

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

// Everyone this player has blocked, with a way to undo it.
//
// NOT an ImGuiDialog: it is drawn as a nested popup from INSIDE the friends
// popup, so ImGui stacks it one level deeper - the same shape as Add Friend.
// As a separate root-level dialog it would replace the friends popup, and
// closing it would tear down everything underneath.
class BlockedListUI {
 public:
  BlockedListUI(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile);

  // Draws one frame. Returns false once finished, so the owner can drop it.
  bool Draw();

 private:
  void Refresh();
  void CollectResults();

  std::vector<HubPlayer> blocked_;
  std::future<std::vector<HubPlayer>> blocked_query_;
  std::map<uint64_t, std::shared_ptr<xe::ui::ImmediateTexture>> gamerpics_;

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
