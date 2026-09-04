/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/player_search_ui.h"

#include <algorithm>
#include <set>

#include "xenia/base/string_util.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/friends_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/ui/keyboard_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

using namespace std::chrono_literals;

PlayerSearchUI::PlayerSearchUI(xe::ui::ImGuiDrawer* imgui_drawer,
                               UserProfile* profile)
    : imgui_drawer_(imgui_drawer), profile_(profile) {
  // Opens on the recently-active list, so there is something to look at before
  // anything is typed.
  Refresh();
}

void PlayerSearchUI::Refresh() {
  active_search_ = std::string(search_);
  players_query_ =
      kernel_state()->GetXboxLiveAPI()->GetHubPlayersAsync(active_search_,
                                                           kResultLimit);
  searching_ = true;
}

void PlayerSearchUI::CollectResults() {
  if (!searching_ || !players_query_.valid()) {
    return;
  }

  if (players_query_.wait_for(0s) != std::future_status::ready) {
    return;
  }

  players_ = players_query_.get();
  searching_ = false;

  // One batched gamerpic fetch for the rows we are about to show, rather than
  // a request per row.
  std::set<uint64_t> xuids;

  for (const auto& player : players_) {
    if (!gamerpics_.count(player.xuid)) {
      xuids.insert(player.xuid);
    }
  }

  if (xuids.empty()) {
    return;
  }

  const auto gamerpics =
      kernel_state()->GetXboxLiveAPI()->GetMultiGamerpicsFromXUIDs(xuids);

  for (const auto& [xuid, gamerpic] : gamerpics) {
    gamerpics_[xuid] = imgui_drawer_->LoadImGuiIcon({gamerpic});
  }
}

void PlayerSearchUI::DrawPlayerRow(const HubPlayer& player) {
  ImGui::PushID(static_cast<int>(player.xuid));

  auto* focus_manager = imgui_drawer_->GetFocusManager();
  const auto& input = focus_manager->XamInputFocus("PlayerSearchUI");

  xe::ui::ImmediateTexture* icon = imgui_drawer_->GetLoadingTileIcon();

  const auto cached = gamerpics_.find(player.xuid);

  if (cached != gamerpics_.cend() && cached->second) {
    icon = cached->second.get();
  }

  ImGui::Image(reinterpret_cast<ImTextureID>(icon),
               xe::ui::default_image_icon_size);

  ImGui::SameLine();

  ImGui::BeginGroup();

  ImGui::TextUnformatted(player.gamertag.c_str());

  if (!player.rich_presence.empty()) {
    ImGui::TextDisabled("%s", player.rich_presence.c_str());
  } else if (player.title_id) {
    ImGui::TextDisabled("In title %08X", player.title_id);
  } else {
    ImGui::TextDisabled("Offline");
  }

  ImGui::EndGroup();

  const bool is_self = player.xuid == profile_->GetOnlineXUID();
  const bool already_friend =
      kernel_state()->friends_manager()->IsFriend(profile_->xuid(),
                                                  player.xuid);
  const bool full = kernel_state()->friends_manager()->GetFriendsCount(
                        profile_->xuid()) >= X_ONLINE_MAX_FRIENDS;

  ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70.0f);

  ImGui::BeginDisabled(is_self || already_friend || full);

  const std::string add_label = already_friend ? "Friend" : "Add";

  if (ImGui::Button(add_label.c_str(), ImVec2(70.0f, 0)) ||
      (ImGui::IsItemFocused() && input.Activated())) {
    // Goes through the hub, so the target's privacy setting decides whether
    // this is immediate or has to wait for their approval.
    if (xeRequestFriend(imgui_drawer_, profile_, player.xuid,
                        player.gamertag)) {
      added_friend_ = true;
    }
  }

  ImGui::EndDisabled();

  if (is_self && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("This is you.");
  } else if (full && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Friends list is full!");
  }

  ImGui::PopID();
}

bool PlayerSearchUI::Draw() {
  auto* drawer = imgui_drawer_;
  auto* focus_manager = drawer->GetFocusManager();

  if (!has_opened_) {
    has_opened_ = true;
    focus_manager->UIChildFocus("FriendsDialog", "PlayerSearchUI");
    ImGui::OpenPopup("Player Search");
  }

  const auto& input = focus_manager->XamInputFocus("PlayerSearchUI");

  CollectResults();

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();

  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(420, 200), ImVec2(420, 520));

  bool keep_open = true;

  // Nested inside the friends popup, so ImGui stacks it one level above
  // without disturbing anything beneath it.
  if (ImGui::BeginPopupModal("Player Search", &keep_open,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // Not while the keyboard is up: it owns input then.
    if (!keyboard_open_ && input.ShouldClose()) {
      keep_open = false;
    }

    const float window_width = ImGui::GetContentRegionAvail().x;

    ImGui::TextUnformatted("Gamertag:");

    // Entry goes through the on-screen keyboard so it works on a controller.
    const std::string search_display =
        search_[0] ? std::string(search_) : "(click to enter)";

    if (ImGui::Button(search_display.c_str(), ImVec2(window_width - 80.0f, 0)) ||
        (ImGui::IsItemFocused() && input.Activated())) {
      if (!keyboard_open_) {
        keyboard_open_ = true;

        auto* keyboard = xe::ui::KeyboardDialog::ShowKeyboard(
            drawer, "Enter Gamertag", std::string(search_),
            xe::ui::KeyboardDialog::InputType::kText, nullptr,
            "PlayerSearchUI", "OnScreenKeyboard");

        keyboard->set_close_callback([this, keyboard]() {
          if (!keyboard->was_cancelled()) {
            xe::string_util::copy_truncating(search_, keyboard->result_text(),
                                             sizeof(search_));
            Refresh();
          }

          keyboard_open_ = false;
        });
      }
    }

    ImGui::SameLine();

    if (ImGui::Button("Clear", ImVec2(70.0f, 0)) ||
        (ImGui::IsItemFocused() && input.Activated())) {
      search_[0] = '\0';
      Refresh();
    }

    ImGui::Spacing();

    if (active_search_.empty()) {
      ImGui::TextDisabled("Recently active on NexiaHub");
    } else {
      ImGui::TextDisabled("Results for \"%s\"", active_search_.c_str());
    }

    ImGui::Separator();
    ImGui::Spacing();

    if (searching_) {
      ImGui::TextUnformatted("Searching...");
    } else if (players_.empty()) {
      ImGui::TextUnformatted(active_search_.empty()
                                 ? "Nobody has been active recently."
                                 : "No players matched that gamertag.");
    } else {
      if (ImGui::BeginChild("##PlayerSearchResults", ImVec2(0, 320), true)) {
        for (const auto& player : players_) {
          DrawPlayerRow(player);
          ImGui::Separator();
        }
      }

      ImGui::EndChild();
    }

    ImGui::Spacing();

    if (ImGui::Button("Refresh", ImVec2(window_width, 0)) ||
        (ImGui::IsItemFocused() && input.Activated())) {
      Refresh();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("A: Select | B/Back: Close");

    if (!keep_open) {
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  } else if (!keyboard_open_) {
    // Popup gone - closed by us, the X, or ImGui. Not while the keyboard is
    // up: that is a deliberate hand-off, and this comes back afterwards.
    keep_open = false;
  }

  if (!keep_open) {
    focus_manager->UIDropFocus("PlayerSearchUI");
  }

  return keep_open;
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
