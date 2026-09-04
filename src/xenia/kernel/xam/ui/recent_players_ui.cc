/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/recent_players_ui.h"

#include <set>

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/friends_manager.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

using namespace std::chrono_literals;

RecentPlayersUI::RecentPlayersUI(xe::ui::ImGuiDrawer* imgui_drawer,
                                 UserProfile* profile)
    : imgui_drawer_(imgui_drawer), profile_(profile) {
  Refresh();
}

void RecentPlayersUI::Refresh() {
  players_query_ = kernel_state()->GetXboxLiveAPI()->GetRecentPlayersAsync(
      profile_->GetOnlineXUID());
  loading_ = true;
}

void RecentPlayersUI::CollectResults() {
  if (!loading_ || !players_query_.valid()) {
    return;
  }

  if (players_query_.wait_for(0s) != std::future_status::ready) {
    return;
  }

  players_ = players_query_.get();
  loading_ = false;

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

void RecentPlayersUI::DrawPlayerRow(const HubPlayer& player) {
  ImGui::PushID(static_cast<int>(player.xuid));

  auto* focus_manager = imgui_drawer_->GetFocusManager();
  const auto& input = focus_manager->XamInputFocus("RecentPlayersUI");

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

  if (player.title_id) {
    ImGui::TextDisabled("Last played together in %08X", player.title_id);
  } else {
    ImGui::TextDisabled("Played together recently");
  }

  const bool already_friend = kernel_state()->friends_manager()->IsFriend(
      profile_->xuid(), player.xuid);
  const bool is_self = player.xuid == profile_->GetOnlineXUID();
  const bool full = kernel_state()->friends_manager()->GetFriendsCount(
                        profile_->xuid()) >= X_ONLINE_MAX_FRIENDS;

  const ImVec2 btn_size = ImVec2(90.0f, 0);

  ImGui::BeginDisabled(is_self || already_friend || full);

  if (ImGui::Button(already_friend ? "Friend" : "Add Friend", btn_size) ||
      (ImGui::IsItemFocused() && input.Activated())) {
    // Through the hub, so their privacy setting decides whether this is
    // immediate or waits for approval.
    xeRequestFriend(imgui_drawer_, profile_, player.xuid, player.gamertag);
  }

  ImGui::EndDisabled();

  ImGui::SameLine();

  ImGui::BeginDisabled(is_self);

  if (ImGui::Button("Block", btn_size) ||
      (ImGui::IsItemFocused() && input.Activated())) {
    kernel_state()->GetXboxLiveAPI()->BlockPlayer(profile_->GetOnlineXUID(),
                                                  player.xuid);

    if (already_friend) {
      kernel_state()->friends_manager()->RemoveFriend(profile_->xuid(),
                                                      player.xuid);
    }
  }

  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
    ImGui::SetTooltip("Refuse any future friend request from them, silently.");
  }

  ImGui::EndDisabled();

  ImGui::SameLine();

  if (ImGui::Button("Player Card", btn_size) ||
      (ImGui::IsItemFocused() && input.Activated())) {
    // A nested popup, not the standalone gamercard dialog: a root-level
    // dialog here would replace this popup and collapse everything under it.
    card_xuid_ = player.xuid;
    card_opened_ = false;
  }

  ImGui::SameLine();

  // Messaging is not built yet - the button is here so the row layout is
  // final, and does nothing until it is.
  ImGui::BeginDisabled(true);
  ImGui::Button("Message", btn_size);
  ImGui::EndDisabled();

  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Not implemented yet.");
  }

  ImGui::EndGroup();

  ImGui::PopID();
}

void RecentPlayersUI::DrawPlayerCard() {
  if (!card_xuid_) {
    return;
  }

  auto* focus_manager = imgui_drawer_->GetFocusManager();

  if (!card_opened_) {
    card_opened_ = true;
    focus_manager->UIChildFocus("RecentPlayersUI", "RecentPlayerCard");
    ImGui::OpenPopup("Player Card");
  }

  const auto& input = focus_manager->XamInputFocus("RecentPlayerCard");

  const HubPlayer* player = nullptr;

  for (const auto& entry : players_) {
    if (entry.xuid == card_xuid_) {
      player = &entry;
      break;
    }
  }

  bool keep_open = true;

  if (ImGui::BeginPopupModal("Player Card", &keep_open,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (input.ShouldClose()) {
      keep_open = false;
    }

    if (player) {
      xe::ui::ImmediateTexture* icon = imgui_drawer_->GetLoadingTileIcon();

      const auto cached = gamerpics_.find(player->xuid);

      if (cached != gamerpics_.cend() && cached->second) {
        icon = cached->second.get();
      }

      ImGui::Image(reinterpret_cast<ImTextureID>(icon),
                   xe::ui::default_image_icon_size);

      ImGui::SameLine();

      ImGui::BeginGroup();
      ImGui::TextUnformatted(player->gamertag.c_str());
      ImGui::TextDisabled("%016llX",
                          static_cast<unsigned long long>(player->xuid));

      if (player->title_id) {
        ImGui::TextDisabled("Played together in %08X", player->title_id);
      }

      ImGui::EndGroup();
    } else {
      ImGui::TextUnformatted("That player is no longer listed.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("B/Back: Close");

    if (!keep_open) {
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  } else {
    keep_open = false;
  }

  if (!keep_open) {
    focus_manager->UIDropFocus("RecentPlayerCard");
    card_xuid_ = 0;
    card_opened_ = false;
  }
}

bool RecentPlayersUI::Draw() {
  auto* focus_manager = imgui_drawer_->GetFocusManager();

  if (!has_opened_) {
    has_opened_ = true;
    focus_manager->UIChildFocus("FriendsDialog", "RecentPlayersUI");
    ImGui::OpenPopup("Recent Players");
  }

  const auto& input = focus_manager->XamInputFocus("RecentPlayersUI");

  CollectResults();

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();

  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(520, 200), ImVec2(520, 520));

  bool keep_open = true;

  // Nested inside the friends popup, so ImGui stacks it one level above
  // without disturbing anything beneath it.
  if (ImGui::BeginPopupModal("Recent Players", &keep_open,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (input.ShouldClose()) {
      keep_open = false;
    }

    ImGui::TextDisabled("Players you shared a session with in the last 48h.");
    ImGui::Separator();
    ImGui::Spacing();

    if (loading_) {
      ImGui::TextUnformatted("Loading...");
    } else if (players_.empty()) {
      ImGui::TextUnformatted("Nobody yet - play a session with someone.");
    } else {
      if (ImGui::BeginChild("##RecentResults", ImVec2(0, 340), true)) {
        for (const auto& player : players_) {
          DrawPlayerRow(player);
          ImGui::Separator();
        }
      }

      ImGui::EndChild();
    }

    ImGui::Spacing();

    if (ImGui::Button("Refresh", ImVec2(ImGui::GetContentRegionAvail().x, 0)) ||
        (ImGui::IsItemFocused() && input.Activated())) {
      Refresh();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("A: Select | B/Back: Close");

    // The card nests one level deeper again, from inside this popup.
    DrawPlayerCard();

    if (!keep_open) {
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  } else {
    keep_open = false;
  }

  if (!keep_open) {
    focus_manager->UIDropFocus("RecentPlayersUI");
  }

  return keep_open;
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
