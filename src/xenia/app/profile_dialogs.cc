/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

/**
 * GAMEPAD INPUT HANDLING WITH UIFocusManager
 * ==========================================
 *
 * This file demonstrates proper usage of UIFocusManager for gamepad input
 * routing in dialogs. Key patterns used here:
 *
 * 1. REGISTRATION (static bool pattern):
 *    Each dialog uses a static bool to track if it's registered on the stack.
 *    This handles dialogs that persist across frames.
 *
 *    static bool registered = false;
 *    if (!registered) {
 *      focus_manager->UISetFocus("MyDialog");
 *      registered = true;
 *    }
 *
 * 2. INPUT RETRIEVAL:
 *    const auto& input = focus_manager->XamInputFocus("MyDialog");
 *    // input will be kNoInput if another dialog is on top
 *
 * 3. BUTTON ACTIVATION (release-based):
 *    if (ImGui::Button("OK") || (ImGui::IsItemFocused() && input.Activated()))
 *    // Activated() = A button was just RELEASED
 *
 * 4. CLOSE HANDLING:
 *    Standard dialogs: if (input.ShouldClose()) { ... }  // Back OR B
 *    Keyboard dialogs: if (input.BackClose()) { ... }    // Only Back
 *
 * 5. CLEANUP:
 *    focus_manager->UIDropFocus("MyDialog");
 *    registered = false;
 *
 * 6. OPENING CHILD DIALOGS:
 *    // Parent adds child to stack BEFORE creating it
 *    focus_manager->UISetFocus("ChildDialog");
 *    new ChildDialog(...);
 *
 * See ui/ui_focus_manager.h for full documentation.
 */

#include <algorithm>

#include "build/version.h"
#include "xenia/app/emulator_window.h"
#include "xenia/app/profile_dialogs.h"
#include "xenia/base/png_utils.h"
#include "xenia/base/system.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_host_notification.h"

#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/kernel/xam/ui/gamercard_ui.h"
#include "xenia/kernel/xam/ui/signin_ui.h"
#include "xenia/kernel/xam/ui/title_info_ui.h"

namespace xe {
namespace app {

void NoProfileDialog::OnDraw(ImGuiIO& io) {
  auto* imgui_drawer = emulator_window_->imgui_drawer();
  auto* focus_manager = imgui_drawer->GetFocusManager();

  // UIFocusManager: Register on first draw using static bool pattern
  static bool registered = false;
  if (!registered) {
    focus_manager->UISetFocus("NoProfileDialog");
    registered = true;
  }

  auto profile_manager = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager();

  if (profile_manager->GetAccountCount()) {
    // UIFocusManager: Drop focus when closing
    focus_manager->UIDropFocus("NoProfileDialog");
    registered = false;
    delete this;
    return;
  }

  const auto window_position =
      ImVec2(GetIO().DisplaySize.x * 0.35f, GetIO().DisplaySize.y * 0.4f);

  ImGui::SetNextWindowPos(window_position, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(1.0f);

  bool dialog_open = true;
  if (!ImGui::Begin("No Profiles Found", &dialog_open,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::End();
    focus_manager->UIDropFocus("NoProfileDialog");
    registered = false;
    delete this;
    return;
  }

  // UIFocusManager: Get input - returns kNoInput if not focused
  const auto& input = focus_manager->XamInputFocus("NoProfileDialog");

  const std::string message =
      "There is no profile available! You will not be able to save without "
      "one.\n\nWould you like to create one?";

  ImGui::TextUnformatted(message.c_str());

  ImGui::Separator();
  ImGui::NewLine();

  const auto content_files = xe::filesystem::ListDirectories(
      emulator_window_->emulator()->content_root());

  if (content_files.empty()) {
    if (ImGui::Button("Create Profile") ||
        (ImGui::IsItemFocused() && input.Activated())) {
      // CreateProfileUI is child of NoProfileDialog
      focus_manager->UIChildFocus("NoProfileDialog", "CreateProfileUI");
      new kernel::xam::ui::CreateProfileUI(emulator_window_->imgui_drawer(),
                                           emulator_window_->emulator());
    }
  } else {
    if (ImGui::Button("Create profile & migrate data") ||
        (ImGui::IsItemFocused() && input.Activated())) {
      focus_manager->UIChildFocus("NoProfileDialog", "CreateProfileUI");
      new kernel::xam::ui::CreateProfileUI(emulator_window_->imgui_drawer(),
                                           emulator_window_->emulator(), true);
    }
  }

  ImGui::SameLine();
  if (ImGui::Button("Open profile menu") ||
      (ImGui::IsItemFocused() && input.Activated())) {
    emulator_window_->ToggleProfilesConfigDialog();
  }

  ImGui::SameLine();
  if (ImGui::Button("Close") || (ImGui::IsItemFocused() && input.Activated())) {
    pending_close_ = true;
  }

  // Back or B button closes
  if (input.ShouldClose()) {
    pending_close_ = true;
  }

  if (pending_close_) {
    ImGui::End();
    focus_manager->UIDropFocus("NoProfileDialog");
    registered = false;
    emulator_window_->SetHotkeysState(true);
    delete this;
    return;
  }

  // X button clicked (mouse)
  if (!dialog_open) {
    focus_manager->UIDropFocus("NoProfileDialog");
    registered = false;
    emulator_window_->SetHotkeysState(true);
    delete this;
    return;
  }

  ImGui::End();
}

void ProfileConfigDialog::LoadProfileIcon() {
  if (!emulator_window_) {
    return;
  }

  for (uint8_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    const auto profile = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager()
                             ->GetProfile(user_index);

    if (!profile) {
      continue;
    }
    LoadProfileIcon(profile->xuid());
  }
}

void ProfileConfigDialog::LoadProfileIcon(const uint64_t xuid) {
  if (!emulator_window_) {
    return;
  }

  const auto profile_manager = emulator_window_->emulator()
                                   ->kernel_state()
                                   ->xam_state()
                                   ->profile_manager();
  if (!profile_manager) {
    return;
  }

  const auto profile = profile_manager->GetProfile(xuid);

  if (!profile) {
    if (profile_icon_.contains(xuid)) {
      profile_icon_[xuid].release();
    }
    return;
  }

  const auto profile_icon =
      profile->GetProfileIcon(kernel::xam::XTileType::kGamerTile);
  if (profile_icon.empty()) {
    return;
  }

  profile_icon_[xuid].release();
  profile_icon_[xuid] = imgui_drawer()->LoadImGuiIcon(profile_icon);
}

void ProfileConfigDialog::OnDraw(ImGuiIO& io) {
  auto* drawer = imgui_drawer();
  auto* focus_manager = drawer->GetFocusManager();

  // Tree 0 = main dialogs
  static bool registered = false;
  if (!registered) {
    focus_manager->UISetFocus("ProfileConfigDialog");
    registered = true;
  }

  if (!emulator_window_->emulator() ||
      !emulator_window_->emulator()->kernel_state() ||
      !emulator_window_->emulator()->kernel_state()->xam_state()) {
    return;
  }

  auto profile_manager = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager();
  if (!profile_manager) {
    return;
  }

  auto profiles = profile_manager->GetAccounts();

  ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.8f);

  bool dialog_open = true;
  if (!ImGui::Begin("Profiles Menu", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::End();
    return;
  }

  // Get input only if we have focus
  const auto& input = focus_manager->XamInputFocus("ProfileConfigDialog");

  if (profiles->empty()) {
    ImGui::TextUnformatted("No profiles found!");
    ImGui::Spacing();
    ImGui::Separator();
  }

  const ImVec2 next_window_position =
      ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x + 20.f,
             ImGui::GetWindowPos().y);

  for (auto& [xuid, account] : *profiles) {
    ImGui::PushID(static_cast<int>(xuid));

    const uint8_t user_index =
        profile_manager->GetUserIndexAssignedToProfile(xuid);

    const auto profile_icon = profile_icon_.find(xuid) != profile_icon_.cend()
                                  ? profile_icon_[xuid].get()
                                  : nullptr;

    auto context_menu_fun = [=, this]() -> bool {
      if (ImGui::BeginPopupContextItem("Profile Menu")) {
        //*selected_xuid = xuid;
        if (user_index == XUserIndexAny) {
          if (ImGui::MenuItem("Login")) {
            profile_manager->Login(xuid);
            if (!profile_manager->GetProfile(xuid)
                     ->GetProfileIcon(kernel::xam::XTileType::kGamerTile)
                     .empty()) {
              LoadProfileIcon(xuid);
            }
          }
          if (ImGui::BeginMenu("Login to slot:")) {
            for (uint8_t i = 1; i <= XUserMaxUserCount; i++) {
              if (ImGui::MenuItem(fmt::format("slot {}", i).c_str())) {
                profile_manager->Login(xuid, i - 1);
              }
            }
            ImGui::EndMenu();
          }
        } else {
          if (ImGui::MenuItem("Logout")) {
            profile_manager->Logout(user_index);
            LoadProfileIcon(xuid);
          }
        }

        if (ImGui::MenuItem("Modify")) {
          new kernel::xam::ui::GamercardUI(
              emulator_window_->window(), emulator_window_->imgui_drawer(),
              emulator_window_->emulator()->kernel_state(), xuid);
        }

        if (ImGui::BeginMenu("Copy")) {
          if (ImGui::MenuItem("Gamertag")) {
            ImGui::SetClipboardText(account.GetGamertagString().c_str());
          }

          if (ImGui::MenuItem("XUID")) {
            ImGui::SetClipboardText(fmt::format("{:016X}", xuid).c_str());
          }

          if (account.IsLiveEnabled()) {
            if (ImGui::MenuItem("XUID Online")) {
              ImGui::SetClipboardText(
                  fmt::format("{:016X}", account.xuid_online.get()).c_str());
            }
          }

          ImGui::EndMenu();
        }

        const bool is_signedin = profile_manager->GetProfile(xuid) != nullptr;
        ImGui::BeginDisabled(!is_signedin);
        if (ImGui::MenuItem("Show Played Titles")) {
          new kernel::xam::ui::TitleListUI(
              emulator_window_->imgui_drawer(), next_window_position,
              profile_manager->GetProfile(user_index));
        }
        ImGui::EndDisabled();

        if (ImGui::MenuItem("Show Content Directory")) {
          const auto path = profile_manager->GetProfileContentPath(
              xuid, emulator_window_->emulator()->kernel_state()->title_id());

          if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(path);
          }

          std::thread path_open(LaunchFileExplorer, path);
          path_open.detach();
        }

        if (!emulator_window_->emulator()->is_title_open()) {
          ImGui::Separator();

          if (account.IsLiveEnabled()) {
            if (ImGui::BeginMenu("Convert to Offline Profile")) {
              ImGui::BeginTooltip();
              ImGui::TextUnformatted(
                  fmt::format(
                      "You're about to convert profile: {} (XUID: {:016X}) "
                      "to an offline profile. Are you sure?",
                      account.GetGamertagString(), xuid)
                      .c_str());
              ImGui::EndTooltip();

              if (ImGui::MenuItem("Yes, convert it!")) {
                profile_manager->ConvertToOfflineProfile(xuid);
                ImGui::EndMenu();
                ImGui::EndPopup();
                return false;
              }

              ImGui::EndMenu();
            }
          } else {
            if (ImGui::BeginMenu("Convert to Xbox Live-Enabled Profile")) {
              ImGui::BeginTooltip();
              ImGui::TextUnformatted(
                  fmt::format(
                      "You're about to convert profile: {} (XUID: {:016X}) "
                      "to an Xbox Live-Enabled profile. Are you sure?",
                      account.GetGamertagString(), xuid)
                      .c_str());
              ImGui::EndTooltip();

              if (ImGui::MenuItem("Yes, convert it!")) {
                profile_manager->ConvertToXboxLiveEnabledProfile(xuid);
                ImGui::EndMenu();
                ImGui::EndPopup();
                return false;
              }

              ImGui::EndMenu();
            }
          }

          if (ImGui::BeginMenu("Delete Profile")) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(
                fmt::format(
                    "You're about to delete profile: {} (XUID: {:016X}). "
                    "This will remove all data assigned to this profile "
                    "including savefiles. Are you sure?",
                    account.GetGamertagString(), xuid)
                    .c_str());
            ImGui::EndTooltip();

            if (ImGui::MenuItem("Yes, delete it!")) {
              profile_manager->DeleteProfile(xuid);
              ImGui::EndMenu();
              ImGui::EndPopup();
              return false;
            }

            ImGui::EndMenu();
          }
        }
        ImGui::EndPopup();
      }
      return true;
    };

    // Draw profile content manually for click/long-press handling
    const ImVec2 start_position = ImGui::GetCursorPos();

    ImGui::BeginGroup();
    {
      if (profile_icon) {
        ImGui::Image(reinterpret_cast<ImTextureID>(profile_icon),
                     xe::ui::default_image_icon_size);
      } else {
        if (user_index < XUserMaxUserCount) {
          const auto icon = imgui_drawer()->GetNotificationIcon(user_index);
          ImGui::Image(reinterpret_cast<ImTextureID>(icon),
                       xe::ui::default_image_icon_size);
        } else {
          ImGui::Dummy(xe::ui::default_image_icon_size);
        }
      }

      ImGui::SameLine();

      ImGui::BeginGroup();
      {
        ImGui::TextUnformatted(
            fmt::format("User: {}\n", account.GetGamertagString()).c_str());
        ImGui::TextUnformatted(fmt::format("XUID: {:016X}  \n", xuid).c_str());

        const std::string live_enabled =
            fmt::format("Xbox Live Enabled: {}",
                        account.IsLiveEnabled() ? "True" : "False");
        ImGui::TextUnformatted(live_enabled.c_str());

        if (user_index != XUserIndexAny) {
          ImGui::TextUnformatted(
              fmt::format("Assigned to slot: {}\n", user_index + 1).c_str());
        } else {
          ImGui::TextUnformatted("Profile is not signed in");
        }
      }
      ImGui::EndGroup();
    }
    ImGui::EndGroup();

    // Create selectable overlay for click detection
    const ImVec2 end_draw_position =
        ImVec2(ImGui::GetCursorPos().x - start_position.x,
               ImGui::GetCursorPos().y - start_position.y);

    ImGui::SetCursorPos(start_position);

    // Track selection and press state
    bool is_selected = (selected_xuid_ == xuid);
    if (ImGui::Selectable("##ProfileSelectable", is_selected,
                          ImGuiSelectableFlags_SpanAllColumns,
                          end_draw_position)) {
      selected_xuid_ = xuid;
    }

    // Handle gamepad A button for this item
    bool item_focused = ImGui::IsItemFocused();
    bool a_pressed = input.a_pressed;
    bool a_released = input.a_released;

    // Also handle mouse click
    bool mouse_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool mouse_released = ImGui::IsItemDeactivated();

    // Y button opens Modify Profile when item is focused
    if (item_focused && input.y_released) {
      new kernel::xam::ui::GamercardUI(
          emulator_window_->window(), emulator_window_->imgui_drawer(),
          emulator_window_->emulator()->kernel_state(), xuid);
    }

    // A button or mouse click toggles login state
    if ((item_focused && a_released) || (mouse_clicked && mouse_released)) {
      if (user_index == XUserIndexAny) {
        // Not logged in - log in
        profile_manager->Login(xuid);
        if (!profile_manager->GetProfile(xuid)
                 ->GetProfileIcon(kernel::xam::XTileType::kGamerTile)
                 .empty()) {
          LoadProfileIcon(xuid);
        }
      } else {
        // Logged in - log out
        profile_manager->Logout(user_index);
        LoadProfileIcon(xuid);
      }
    }

    // Right-click context menu still works
    if (!context_menu_fun()) {
      ImGui::PopID();
      ImGui::End();
      return;
    }

    ImGui::PopID();
    ImGui::Separator();
  }

  ImGui::Spacing();

  if (ImGui::Button("Create Profile") ||
      (ImGui::IsItemFocused() && input.Activated())) {
    focus_manager->UISetFocus("CreateProfileUI");
    new kernel::xam::ui::CreateProfileUI(emulator_window_->imgui_drawer(),
                                         emulator_window_->emulator());
  }

  // Controller/keyboard hints
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextDisabled("A: Toggle Login/Logout | Y: Modify Profile");
  ImGui::TextDisabled("B/Back: Close | Right-click: More Options");

  // Back or B button closes
  if (input.ShouldClose()) {
    pending_close_ = true;
  }

  if (pending_close_) {
    focus_manager->UIDropFocus("ProfileConfigDialog");
    registered = false;
    ImGui::End();
    emulator_window_->ToggleProfilesConfigDialog();
    return;
  }

  ImGui::End();

  // X button clicked (mouse)
  if (!dialog_open) {
    focus_manager->UIDropFocus("ProfileConfigDialog");
    registered = false;
    emulator_window_->ToggleProfilesConfigDialog();
  }
}

void ManagerDialog::OnDraw(ImGuiIO& io) {
  auto* drawer = imgui_drawer();
  auto* focus_manager = drawer->GetFocusManager();

  // Tree 0 = main dialogs
  static bool registered = false;
  if (!manager_opened_) {
    manager_opened_ = true;
    focus_manager->UISetFocus("ManagerDialog");
    registered = true;
    ImGui::OpenPopup("Manager");

    if (kernel::XLiveAPI::IsConnectedToServer()) {
      friends_args.filter_offline = true;
    }

    sessions_args.filter_own = true;
  }

  // Add profile dropdown selector?
  const uint32_t user_index = 0;

  auto profile =
      emulator_window_->emulator()->kernel_state()->xam_state()->GetUserProfile(
          user_index);

  const bool is_profile_signed_in = profile == nullptr;

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();

  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

  bool popup_open = true;
  if (ImGui::BeginPopupModal("Manager", &popup_open,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // Get input only if we have focus
    const auto& input = focus_manager->XamInputFocus("ManagerDialog");

    ImVec2 btn_size = ImVec2(200, 40);

    if (is_profile_signed_in) {
      ImGui::Text("You're not logged into a profile!");
      ImGui::Separator();
    }

    ImGui::SetWindowFontScale(1.2f);

    ImGui::BeginDisabled(is_profile_signed_in);
    if (ImGui::Button("Friends", btn_size) ||
        (ImGui::IsItemFocused() && input.Activated())) {
      focus_manager->UISetFocus("FriendsDialog");
      friends_args.friends_open = true;
      ImGui::OpenPopup("Friends");
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(is_profile_signed_in ||
                         !kernel::XLiveAPI::IsConnectedToServer());
    if (ImGui::Button("Sessions", btn_size) ||
        (ImGui::IsItemFocused() && input.Activated())) {
      focus_manager->UISetFocus("SessionsDialog");
      sessions_args.sessions_open = true;
      ImGui::OpenPopup("Sessions");
    }
    ImGui::EndDisabled();

    if (kernel::XLiveAPI::xuid_mismatch) {
      ImVec2 button_pos = ImGui::GetCursorScreenPos();
      ImVec2 button_end =
          ImVec2(button_pos.x + btn_size.x, button_pos.y + btn_size.y);

      ImDrawList* draw_list = ImGui::GetWindowDrawList();

      draw_list->AddRect(button_pos, button_end, IM_COL32(255, 0, 0, 255), 0.0f,
                         0, 3.0f);
    }

    if (ImGui::Button("Delete Netplay Profiles", btn_size) ||
        drawer->GamepadButtonActivated()) {
      ImGui::OpenPopup("Delete Profiles");
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Delete profiles to fix XUID mismatch error.");
    }

    ImGui::SameLine();

    ImGui::BeginDisabled(is_profile_signed_in);
    if (ImGui::Button("Refresh Presence", btn_size) ||
        drawer->GamepadButtonActivated()) {
      emulator_window_->emulator()->kernel_state()->BroadcastNotification(
          kXNotificationFriendsPresenceChanged, user_index);

      emulator_window_->emulator()
          ->display_window()
          ->app_context()
          .CallInUIThread([&]() {
            new xe::ui::HostNotificationWindow(
                imgui_drawer(), "Refreshed Presence", "Success", 0);
          });
    }
    ImGui::EndDisabled();

    ImGui::SetWindowFontScale(1.0f);

    if (!friends_args.friends_open) {
      friends_args.first_draw = false;
      friends_args.refresh_presence_sync = true;
      presences = {};
    }

    if (!sessions_args.sessions_open) {
      sessions_args.first_draw = false;
      sessions_args.refresh_sessions_sync = true;
      sessions.clear();
    }

    xeDrawFriendsContent(imgui_drawer(), focus_manager, profile, friends_args,
                         &presences);

    xeDrawSessionsContent(imgui_drawer(), focus_manager, profile, sessions_args,
                          &sessions);

    if (!deletion_args.deleted_profiles_open) {
      deletion_args.first_draw = false;
      deleted_profiles = {};
    }

    bool open_deleted_profiles = false;

    float btn_height = 25;
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(225, -1), ImVec2(225, -1));
    if (ImGui::BeginPopupModal("Delete Profiles", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      float btn_width = (ImGui::GetContentRegionAvail().x * 0.5f) -
                        (ImGui::GetStyle().ItemSpacing.x * 0.5f);
      ImVec2 btn_size = ImVec2(btn_width, btn_height);

      const std::string desc = "Are you sure?";
      const std::string desc2 = "You will be signed out.";

      ImVec2 desc_size = ImGui::CalcTextSize(desc.c_str());
      ImVec2 desc2_size = ImGui::CalcTextSize(desc2.c_str());

      ImGui::SetCursorPosX((ImGui::GetWindowWidth() - desc_size.x) * 0.5f);
      ImGui::Text(desc.c_str());

      if (!is_profile_signed_in) {
        ImGui::Spacing();

        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - desc2_size.x) * 0.5f);
        ImGui::Text(desc2.c_str());
      }

      ImGui::Separator();

      if (ImGui::Button("Yes", btn_size) || drawer->GamepadButtonActivated()) {
        if (!is_profile_signed_in) {
          std::map<uint8_t, uint64_t> xuids;

          kernel::xam::XamState* xam_state =
              emulator_window_->emulator()->kernel_state()->xam_state();

          for (uint32_t i = 0; i < XUserMaxUserCount; i++) {
            if (xam_state->IsUserSignedIn(i)) {
              xuids[i] = xam_state->GetUserProfile(i)->xuid();
            }
          }

          xam_state->profile_manager()->LogoutMultiple(xuids);
        }

        deleted_profiles = kernel::XLiveAPI::DeleteMyProfiles();

        open_deleted_profiles = true;

        ImGui::CloseCurrentPopup();
      }

      ImGui::SameLine();

      if (ImGui::Button("Cancel", btn_size) ||
          drawer->GamepadButtonActivated()) {
        ImGui::CloseCurrentPopup();
      }

      ImGui::EndPopup();
    }

    if (open_deleted_profiles) {
      kernel::XLiveAPI::xuid_mismatch = false;

      focus_manager->UISetFocus("DeletedProfilesDialog");
      deletion_args.deleted_profiles_open = true;
      ImGui::OpenPopup("Deleted Profiles");
    }

    xe::kernel::xam::xeDrawMyDeletedProfiles(imgui_drawer(), focus_manager,
                                             deletion_args, &deleted_profiles);

    // Back or B button closes
    if (input.ShouldClose()) {
      pending_close_ = true;
    }

    if (pending_close_) {
      focus_manager->UIDropFocus("ManagerDialog");
      registered = false;
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      emulator_window_->ToggleFriendsDialog();
      return;
    }

    ImGui::EndPopup();
  } else {
    // Popup not open
    if (registered) {
      focus_manager->UIDropFocus("ManagerDialog");
      registered = false;
    }
  }

  // X button clicked (mouse)
  if (!popup_open) {
    focus_manager->UIDropFocus("ManagerDialog");
    registered = false;
    emulator_window_->ToggleFriendsDialog();
  }
}

// UpdaterDialog::OnDraw moved to updater_dialog.cc

}  // namespace app
}  // namespace xe
