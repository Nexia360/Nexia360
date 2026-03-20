/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/updater_dialog.h"

#include "third_party/imgui/imgui.h"

namespace xe {
namespace app {

void UpdaterDialog::OnDraw(ImGuiIO& io) {
  bool open = true;
  if (ImGui::Begin(
          "Updater", &open,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("This is still being worked on, sorry for the inconvenience.");
    ImGui::End();
  }

  if (!open) {
    Close();
  }
}

}  // namespace app
}  // namespace xe
