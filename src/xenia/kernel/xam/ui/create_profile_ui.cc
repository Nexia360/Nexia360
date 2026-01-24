/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/emulator.h"

#include <string>

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

void CreateProfileUI::OnDraw(ImGuiIO& io) {
  auto* drawer = imgui_drawer();
  auto* focus_manager = drawer->GetFocusManager();

  // UIFocusManager: Wait for button release before closing
  // This prevents input bleed to the parent dialog
  if (pending_close_) {
    if (!drawer->IsAnyGamepadActionPressed()) {
      focus_manager->UIDropFocus("CreateProfileUI");
      Close();
    }
    return;
  }

  auto profile_manager =
      emulator_->kernel_state()->xam_state()->profile_manager();

  // Don't render while keyboard has focus - just keep us alive
  if (keyboard_has_focus_) {
    // Mark that we need to re-open popup when keyboard closes
    has_opened_ = false;
    return;
  }

  if (!has_opened_) {
    ImGui::OpenPopup("Create Profile");
    has_opened_ = true;
  }

  bool dialog_open = true;
  if (!ImGui::BeginPopupModal("Create Profile", &dialog_open,
                              ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
    // Popup failed to open - this shouldn't happen normally
    // Only close if keyboard isn't supposed to be open
    if (!keyboard_has_focus_) {
      focus_manager->UIDropFocus("CreateProfileUI");
      Close();
    }
    return;
  }

  // UIFocusManager: Get input - returns kNoInput if keyboard is open on top
  const auto& input = focus_manager->XamInputFocus("CreateProfileUI");

  ImGui::TextUnformatted("Gamertag:");

  // Keyboard is a child of CreateProfileUI
  std::string display_text =
      strlen(gamertag_) > 0 ? gamertag_ : "(click to enter)";
  if (ImGui::Button(display_text.c_str(), ImVec2(200, 0)) ||
      (ImGui::IsItemFocused() && input.Activated())) {
    // Open keyboard dialog
    if (!keyboard_has_focus_) {
      // Set focus BEFORE opening keyboard
      keyboard_has_focus_ = true;
      XamKeyboardSetFocus(true);

      // Keyboard is child of CreateProfileUI
      keyboard_dialog_ = xe::ui::KeyboardDialog::ShowKeyboard(
          drawer, "Enter Gamertag", gamertag_,
          xe::ui::KeyboardDialog::InputType::kText,
          nullptr,  // Don't use input callback - use close callback instead
          "CreateProfileUI",  // Parent
          "KeyboardDialog");  // This dialog's name

      // Pre-close callback: clear focus BEFORE keyboard closes
      keyboard_dialog_->set_pre_close_callback([this]() {
        keyboard_has_focus_ = false;
        XamKeyboardSetFocus(false);
      });

      // Close callback: handle result AFTER keyboard closes
      keyboard_dialog_->set_close_callback([this]() {
        if (!keyboard_dialog_->was_cancelled()) {
          const std::string& result = keyboard_dialog_->result_text();
          if (!result.empty()) {
            strncpy(gamertag_, result.c_str(), sizeof(gamertag_) - 1);
            gamertag_[sizeof(gamertag_) - 1] = '\0';
          }
        }
        keyboard_dialog_ = nullptr;
      });
    }
  }

  ImGui::Spacing();

  ImGui::Checkbox("Xbox Live Enabled", &live_enabled);

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  const std::string gamertag_string = std::string(gamertag_);
  bool valid = profile_manager->IsGamertagValid(gamertag_string);

  ImGui::BeginDisabled(!valid);
  // UIFocusManager: Use Activated() (A released) for button actions
  if (ImGui::Button("Create") ||
      (ImGui::IsItemFocused() && input.Activated())) {
    bool autologin = (profile_manager->GetAccountCount() == 0);

    uint32_t reserved_flags = 0;

    if (live_enabled) {
      reserved_flags |= X_XAMACCOUNTINFO::AccountReservedFlags::kLiveEnabled;
    }

    if (profile_manager->CreateProfile(gamertag_string, autologin, migration_,
                                       reserved_flags) &&
        migration_) {
      emulator_->DataMigration(0xB13EBABEBABEBABE);
    }
    std::fill(std::begin(gamertag_), std::end(gamertag_), '\0');
    pending_close_ = true;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();

  if (ImGui::Button("Cancel") ||
      (ImGui::IsItemFocused() && input.Activated())) {
    std::fill(std::begin(gamertag_), std::end(gamertag_), '\0');
    pending_close_ = true;
    ImGui::CloseCurrentPopup();
  }

  // UIFocusManager: Standard close - Back OR B closes (not a keyboard dialog)
  if (input.ShouldClose()) {
    std::fill(std::begin(gamertag_), std::end(gamertag_), '\0');
    pending_close_ = true;
    ImGui::CloseCurrentPopup();
  }

  // Show controller hints
  ImGui::Spacing();
  ImGui::TextDisabled("A: Select | B/Back: Cancel");

  if (!dialog_open) {
    pending_close_ = true;
    ImGui::CloseCurrentPopup();
  }

  ImGui::EndPopup();
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
