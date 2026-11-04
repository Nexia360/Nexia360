/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_GUIDE_DIALOG_H_
#define XENIA_APP_GUIDE_DIALOG_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"

namespace xe {
namespace app {

class EmulatorWindow;

// One line on a blade. `value` is drawn right-aligned beside the label (a
// count, say) and is read every frame, so it stays current while the guide is
// up. `action` runs after the guide has closed, never inside its frame.
struct GuideItem {
  std::string label;
  std::function<void()> action;
  std::function<std::string()> value;
};

struct GuideBlade {
  std::string label;
  std::vector<GuideItem> items;
};

// The global menu the Guide button opens, laid out as the console's guide:
// the blades not in use stacked either side of the open one, its items on a
// pale panel with the green bar on the selection, and the button legend under
// it. What the blades hold is up to EmulatorWindow, which builds them when the
// guide opens.
class GuideDialog final : public ui::ImGuiDialog {
 public:
  GuideDialog(ui::ImGuiDrawer* imgui_drawer, EmulatorWindow* emulator_window,
              std::vector<GuideBlade> blades, size_t open_blade,
              std::function<void()> leave_game);
  ~GuideDialog() override;

  // Closes the guide from outside it - the Guide button pressed again.
  void Dismiss() { Finish(nullptr); }

  // The dialog deletes itself when it closes, so the window is told here
  // rather than being left holding a dangling pointer.
  void set_closed_callback(std::function<void()> callback) {
    closed_callback_ = std::move(callback);
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Closes the guide and runs `action` once it is gone.
  void Finish(std::function<void()> action);
  void SelectBlade(size_t index);
  ui::ImmediateTexture* Gamerpic();

  EmulatorWindow* emulator_window_;
  std::vector<GuideBlade> blades_;
  size_t blade_ = 0;
  size_t item_ = 0;
  // Y: empty when there is no game to leave.
  std::function<void()> leave_game_;
  bool opened_ = false;
  bool finished_ = false;

  // A texture can only be made while drawing, so the gamerpic is loaded on the
  // first frame; `gamerpic_loaded_` keeps a profile without one from being
  // retried every frame.
  std::unique_ptr<ui::ImmediateTexture> gamerpic_;
  bool gamerpic_loaded_ = false;

  std::function<void()> closed_callback_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GUIDE_DIALOG_H_
