/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_GAMERCARD_FROM_XUID_UI_H_
#define XENIA_KERNEL_XAM_UI_GAMERCARD_FROM_XUID_UI_H_

#include <future>

#include "xenia/kernel/xam/xam_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

class GamercardFromXUIDUI : public XamDialog {
 public:
  GamercardFromXUIDUI(xe::ui::ImGuiDrawer* imgui_drawer, const uint64_t xuid,
                      UserProfile* profile);

 private:
  void OnDraw(ImGuiIO& io) override;

  bool dialog_open = false;
  bool is_self = false;
  bool are_friends = false;
  std::string title_ = "Gamercard";
  const uint64_t xuid_;
  std::shared_future<std::shared_ptr<xe::ui::ImmediateTexture>>
      immediate_gamerpic_;
  // Owns the gamerpic for the lifetime of the dialog. ImGui::Image only
  // records the raw texture pointer into the draw list, which is rendered
  // after OnDraw returns - holding it in a local shared_ptr freed the texture
  // before it was drawn, and left later frames with no picture at all because
  // future::get() had already consumed the result.
  std::shared_ptr<xe::ui::ImmediateTexture> gamerpic_texture_;
  UserProfile* profile_;
  FriendPresenceObjectJSON presence_;
  X_ONLINE_FRIEND friend_presence_ = {};
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
