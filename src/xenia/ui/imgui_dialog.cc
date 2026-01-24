/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_dialog.h"

#include "third_party/imgui/imgui.h"
#include "xenia/base/assert.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace ui {

std::atomic<uint64_t> ImGuiDialog::next_window_id_ = 0;

ImGuiDialog::ImGuiDialog(ImGuiDrawer* imgui_drawer)
    : imgui_drawer_(imgui_drawer) {
  imgui_drawer_->AddDialog(this);
  next_window_id_++;
}

ImGuiDialog::~ImGuiDialog() {
  imgui_drawer_->RemoveDialog(this);
  for (auto fence : waiting_fences_) {
    fence->Signal();
  }
}

void ImGuiDialog::Then(xe::threading::Fence* fence) {
  waiting_fences_.push_back(fence);
}

void ImGuiDialog::Close() { has_close_pending_ = true; }

ImGuiIO& ImGuiDialog::GetIO() { return imgui_drawer()->GetIO(); }

void ImGuiDialog::Draw() {
  // Draw UI.
  OnDraw(GetIO());

  // Check to see if the UI closed itself and needs to be deleted.
  if (has_close_pending_) {
    OnClose();
    delete this;
  }
}

class MessageBoxDialog final : public ImGuiDialog {
 public:
  MessageBoxDialog(ImGuiDrawer* imgui_drawer, std::string title,
                   std::string body)
      : ImGuiDialog(imgui_drawer),
        title_(std::move(title)),
        body_(std::move(body)) {}

  void OnDraw(ImGuiIO& io) override {
    auto* drawer = imgui_drawer();

    // Enable gamepad navigation
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
    }

    // Check if should close (buttons released after close request)
    if (drawer->ShouldCloseFromGamepad()) {
      Close();
      return;
    }

    // If close pending, skip drawing
    if (drawer->IsGamepadClosePending()) {
      return;
    }

    bool popup_open = true;
    if (ImGui::BeginPopupModal(title_.c_str(), &popup_open,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      char* text = const_cast<char*>(body_.c_str());
      ImGui::InputTextMultiline(
          "##body", text, body_.size() + 1, ImVec2(600, 0),
          ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_ReadOnly);

      // Handle OK button - can be activated by clicking or Enter
      // A button is handled via imgui_drawer
      if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        drawer->RequestGamepadClose();
      }

      // A button activates OK (drawer handles on release)
      if (drawer->IsGamepadAPressed()) {
        // Will close on release
      }

      // Back or B button closes dialog
      if (drawer->IsGamepadBackPressed() || drawer->IsGamepadBPressed()) {
        drawer->RequestGamepadClose();
      }

      ImGui::EndPopup();
    }

    // X button clicked (mouse)
    if (!popup_open) {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string body_;
};

ImGuiDialog* ImGuiDialog::ShowMessageBox(ImGuiDrawer* imgui_drawer,
                                         std::string title, std::string body) {
  return new MessageBoxDialog(imgui_drawer, std::move(title), std::move(body));
}

}  // namespace ui
}  // namespace xe
