/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/passcode_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

ProfilePasscodeUI::ProfilePasscodeUI(xe::ui::ImGuiDrawer* imgui_drawer,
                                     std::string_view title,
                                     std::string_view description,
                                     MESSAGEBOX_RESULT* result_ptr)
    : XamDialog(imgui_drawer),
      title_(title),
      description_(description),
      result_ptr_(result_ptr) {
  std::memset(result_ptr, 0, sizeof(MESSAGEBOX_RESULT));

  if (title_.empty()) {
    title_ = "Enter Pass Code";
  }

  if (description_.empty()) {
    description_ = "Enter your Xbox LIVE pass code.";
  }
}

void ProfilePasscodeUI::DrawPasscodeField(uint8_t key_id) {
  const std::string label = fmt::format("##Key {}", key_id);

  if (ImGui::BeginCombo(label.c_str(), labelled_keys_[key_indexes_[key_id]])) {
    for (uint8_t key_index = 0; key_index < keys_map_.size(); key_index++) {
      bool is_selected = key_id == key_index;

      if (ImGui::Selectable(labelled_keys_[key_index], is_selected)) {
        key_indexes_[key_id] = key_index;
      }

      if (is_selected) {
        ImGui::SetItemDefaultFocus();
      }
    }

    ImGui::EndCombo();
  }
}

void ProfilePasscodeUI::OnDraw(ImGuiIO& io) {
  auto* drawer = imgui_drawer();
  auto* focus_manager = drawer->GetFocusManager();

  if (pending_close_) {
    if (!drawer->IsAnyGamepadActionPressed()) {
      focus_manager->UIDropFocus("PasscodeUI");
      Close();
    }
    return;
  }

  if (!has_opened_) {
    focus_manager->UISetFocus("PasscodeUI");
    ImGui::OpenPopup(title_.c_str());
    has_opened_ = true;
  }

  const auto& input = focus_manager->XamInputFocus("PasscodeUI");

  if (input.ShouldClose()) {
    pending_close_ = true;
    return;
  }

  if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (description_.size()) {
      ImGui::Text("%s", description_.c_str());
    }

    for (uint8_t i = 0; i < passcode_length; i++) {
      DrawPasscodeField(i);
    }

    ImGui::NewLine();

    bool signin_clicked = ImGui::Button("Sign In");
    bool signin_focused = ImGui::IsItemFocused();
    if (input.Activated() && signin_focused) {
      signin_clicked = true;
    }
    if (signin_clicked) {
      for (uint8_t i = 0; i < passcode_length; i++) {
        result_ptr_->Passcode[i] =
            keys_map_.at(labelled_keys_[key_indexes_[i]]);
      }
      selected_signed_in_ = true;
      pending_close_ = true;
    }

    ImGui::SameLine();

    bool cancel_clicked = ImGui::Button("Cancel");
    bool cancel_focused = ImGui::IsItemFocused();
    if (input.Activated() && cancel_focused) {
      cancel_clicked = true;
    }
    if (cancel_clicked) {
      pending_close_ = true;
    }

    ImGui::Spacing();
    ImGui::TextDisabled("A: Select | B/Back: Cancel");

    ImGui::EndPopup();
  }
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
