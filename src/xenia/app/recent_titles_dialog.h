/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_RECENT_TITLES_DIALOG_H_
#define XENIA_APP_RECENT_TITLES_DIALOG_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace app {

class EmulatorWindow;

// The recently played list, with each title's own icon.
//
// This used to be two native submenus. A platform menu cannot scroll and
// cannot carry a bitmap through the MenuItem abstraction, so with the recent
// list no longer capped at a handful of entries it ran off the screen and
// showed nothing but names.
class RecentTitlesDialog final : public ui::ImGuiDialog {
 public:
  enum class Mode {
    // Pick a title and run it.
    kLaunch,
    // Pick a title and open its title update selector instead.
    kTitleUpdate,
  };

  RecentTitlesDialog(ui::ImGuiDrawer* imgui_drawer,
                     EmulatorWindow* emulator_window, Mode mode);
  ~RecentTitlesDialog() override;

  // The dialog deletes itself when it closes, so the window is told here
  // rather than being left holding a dangling pointer.
  void set_closed_callback(std::function<void()> callback) {
    closed_callback_ = std::move(callback);
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // A texture can only be created while drawing, so the icons are decoded on
  // first sight and kept by title id for as long as the dialog is up.
  ui::ImmediateTexture* IconFor(size_t index);

  EmulatorWindow* emulator_window_;
  Mode mode_;
  bool opened_ = false;
  std::map<size_t, std::unique_ptr<ui::ImmediateTexture>> icons_;
  std::function<void()> closed_callback_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_RECENT_TITLES_DIALOG_H_
