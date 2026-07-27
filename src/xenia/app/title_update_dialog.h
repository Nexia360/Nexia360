/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_TITLE_UPDATE_DIALOG_H_
#define XENIA_APP_TITLE_UPDATE_DIALOG_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xenia/kernel/util/title_update_manager.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace app {

class EmulatorWindow;

// Shift+F9 title update selector: lists the installed updates for a title,
// lets the user rename / delete / pick the active one, then launches the title.
class TitleUpdateDialog final : public ui::ImGuiDialog {
 public:
  TitleUpdateDialog(ui::ImGuiDrawer* imgui_drawer,
                    EmulatorWindow* emulator_window, uint32_t title_id,
                    std::filesystem::path launch_path);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void Reload();

  // Profile the save import applies to - content is per-profile, so this is
  // the signed-in local XUID.
  uint64_t ImportXuid() const;

  EmulatorWindow* emulator_window_;
  uint32_t title_id_;
  std::filesystem::path launch_path_;

  std::vector<kernel::util::TitleUpdateEntry> entries_;
  std::vector<std::array<char, 128>> name_buffers_;
  std::string active_id_;
  std::string selected_id_;
  bool opened_ = false;

  std::vector<std::string> import_conflicts_;
  std::string import_status_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_TITLE_UPDATE_DIALOG_H_
