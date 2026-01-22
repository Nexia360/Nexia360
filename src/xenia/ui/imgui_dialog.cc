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
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
    }
    
    // Wait for button release before closing
    if (pending_close_) {
      if (!ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp) &&
          !ImGui::IsKeyDown(ImGuiKey_GamepadBack) &&
          !ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight)) {
        pending_close_ = false;
        Close();
      }
      return;
    }
    
    bool popup_open = true;
    if (ImGui::BeginPopupModal(title_.c_str(), &popup_open,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      char* text = const_cast<char*>(body_.c_str());
      ImGui::InputTextMultiline(
          "##body", text, body_.size() + 1, ImVec2(600, 0),
          ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_ReadOnly);
      
      // Handle OK button - can be activated by clicking, Enter, or gamepad A
      if (ImGui::Button("OK") || 
          ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp) ||
          ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        ImGui::CloseCurrentPopup();
        pending_close_ = true;
      }
      
      // Back or B button closes dialog (has close button so this is safe)
      if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack) ||
          ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        popup_open = false;
        pending_close_ = true;
      }
      
      ImGui::EndPopup();
    }
    
    if (!popup_open) {
      pending_close_ = true;
    }
  }

 private:
  bool has_opened_ = false;
  bool pending_close_ = false;
  std::string title_;
  std::string body_;
};

ImGuiDialog* ImGuiDialog::ShowMessageBox(ImGuiDrawer* imgui_drawer,
                                         std::string title, std::string body) {
  return new MessageBoxDialog(imgui_drawer, std::move(title), std::move(body));
}

}  // namespace ui
}  // namespace xe
