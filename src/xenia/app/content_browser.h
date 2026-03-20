/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_CONTENT_BROWSER_H_
#define XENIA_APP_CONTENT_BROWSER_H_

#include <filesystem>
#include <string>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/ui/imgui_dialog.h"

namespace xe {
namespace app {

class EmulatorWindow;

class ContentBrowser final : public ui::ImGuiDialog {
 public:
  ContentBrowser(ui::ImGuiDrawer* imgui_drawer,
                 EmulatorWindow* emulator_window);

  bool is_focused() const { return is_focused_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void LoadSources();
  void SaveSources();

  std::vector<std::filesystem::path> EnumerateDrives();

  void DrawSourcesList();
  void DrawFolderContents(const std::filesystem::path& folder_path);
  void DrawFolderPicker();

  void RefreshCache(const std::filesystem::path& path);

  static bool IsSupportedFile(const std::filesystem::path& path);

  EmulatorWindow* emulator_window_;

  // Configured source folders
  std::vector<std::filesystem::path> source_folders_;

  // Content browsing state
  std::filesystem::path current_path_;

  // Folder picker state (for "Add Sources")
  bool browsing_for_source_ = false;
  std::filesystem::path browse_path_;

  // First-run detection
  bool needs_first_run_picker_ = false;

  // Directory listing cache
  std::vector<xe::filesystem::FileInfo> cached_entries_;
  std::filesystem::path cached_path_;

  // Focus tracking
  bool is_focused_ = false;

  // Panel focus: 0 = file panel, 1 = add sources button
  int active_panel_ = 0;
  bool needs_focus_set_ = true;

  // Selected source index for Y-button options
  int selected_source_index_ = -1;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_CONTENT_BROWSER_H_
