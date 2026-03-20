/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_UPDATER_DIALOG_H_
#define XENIA_APP_UPDATER_DIALOG_H_

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace app {

class EmulatorWindow;

class UpdaterDialog final : public ui::ImGuiDialog {
 public:
  UpdaterDialog(ui::ImGuiDrawer* imgui_drawer, EmulatorWindow* emulator_window)
      : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  EmulatorWindow* emulator_window_ = nullptr;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_UPDATER_DIALOG_H_
