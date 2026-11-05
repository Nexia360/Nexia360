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
#include <vector>

#include "xenia/app/emulator_window.h"

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace app {

class EmulatorWindow;

// The recently played list, with each title's own icon, drawn flat in the
// guide's style: one panel, the green bar on the selected row, d-pad / arrow
// keys to move, A / Enter to pick, B / Esc to back out.
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
  // Y cycles through these in order.
  enum class Sort {
    kRecent,                  // most recently played first
    kRecentDescending,        // least recently played first
    kAlphabetical,            // A to Z
    kAlphabeticalDescending,  // Z to A
  };
  static const char* SortName(Sort sort);

  // Re-orders entries_ for sort_, keeping the selected title selected.
  void ApplySort();

  // A texture can only be created while drawing, so the icons are decoded on
  // first sight and kept per title file - not per row, which a re-sort moves -
  // for as long as the dialog is up.
  ui::ImmediateTexture* IconFor(const RecentTitleEntry& entry);

  // Closes and, when a title was picked, launches it or opens its update
  // selector once this frame is over.
  void Finish(bool picked);

  EmulatorWindow* emulator_window_;
  Mode mode_;
  bool opened_ = false;
  bool finished_ = false;
  // This dialog's own copy of the recent list, in the chosen order.
  std::vector<RecentTitleEntry> entries_;
  Sort sort_ = Sort::kRecent;
  // The row under the green bar, and the first row on screen.
  size_t selected_ = 0;
  size_t first_row_ = 0;
  // Grab offset within the scrollbar thumb while it is being dragged.
  float drag_offset_ = 0.0f;
  std::map<std::string, std::unique_ptr<ui::ImmediateTexture>> icons_;
  std::function<void()> closed_callback_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_RECENT_TITLES_DIALOG_H_
