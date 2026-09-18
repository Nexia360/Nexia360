/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_XUI_SCENE_DIALOG_H_
#define XENIA_APP_XUI_SCENE_DIALOG_H_

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xenia/kernel/xam/xui_scene_screen.h"
#include "xenia/ui/imgui_dialog.h"

namespace xe {
namespace app {

class EmulatorWindow;

// Opens one of the console's own dashboard scenes over the frame and drives
// it: the scene's Lua runs, its controls take focus and animate, and the pad
// and the keyboard move through it.
class XuiSceneDialog final : public xe::ui::ImGuiDialog {
 public:
  // Which scene to put up, chosen in the window rather than hardcoded so any
  // of them can be tried.
  struct Candidate {
    const char* label;
    const char* package;
    const char* scene;
    const char* script;
  };

  XuiSceneDialog(xe::ui::ImGuiDrawer* drawer, EmulatorWindow* window);
  ~XuiSceneDialog() override;

  void set_closed_callback(std::function<void()> callback) {
    closed_callback_ = std::move(callback);
  }

  using xe::ui::ImGuiDialog::Close;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void Open(const Candidate& candidate);
  void CloseOverlay();
  void PumpInput();

  EmulatorWindow* window_ = nullptr;
  std::function<void()> closed_callback_;
  std::unique_ptr<kernel::xam::xui::SceneScreen> screen_;
  std::string status_;
  int chosen_ = 0;
  uint16_t previous_buttons_ = 0;
};

}  // namespace app
}  // namespace xe

#endif
