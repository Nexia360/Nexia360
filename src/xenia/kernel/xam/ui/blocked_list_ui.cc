/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/blocked_list_ui.h"

#include <set>

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

using namespace std::chrono_literals;

BlockedListUI::BlockedListUI(xe::ui::ImGuiDrawer* imgui_drawer,
                             UserProfile* profile)
    : imgui_drawer_(imgui_drawer), profile_(profile) {
  Refresh();
}

void BlockedListUI::Refresh() {
  blocked_query_ = kernel_state()->GetXboxLiveAPI()->GetBlockedPlayersAsync(
      profile_->GetOnlineXUID());
  loading_ = true;
}

void BlockedListUI::CollectResults() {
  if (!loading_ || !blocked_query_.valid()) {
    return;
  }

  if (blocked_query_.wait_for(0s) != std::future_status::ready) {
    return;
  }

  blocked_ = blocked_query_.get();
  loading_ = false;

  std::set<uint64_t> xuids;

  for (const auto& player : blocked_) {
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

bool BlockedListUI::Draw() {
  auto* focus_manager = imgui_drawer_->GetFocusManager();

  if (!has_opened_) {
    has_opened_ = true;
    focus_manager->UIChildFocus("FriendsDialog", "BlockedListUI");
    ImGui::OpenPopup("Blocked Players");
  }

  const auto& input = focus_manager->XamInputFocus("BlockedListUI");

  CollectResults();

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();

  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(400, 180), ImVec2(400, 500));

  bool keep_open = true;

  // Nested inside the friends popup, so ImGui stacks it one level above
  // without disturbing anything beneath it.
  if (ImGui::BeginPopupModal("Blocked Players", &keep_open,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (input.ShouldClose()) {
      keep_open = false;
    }

    if (loading_) {
      ImGui::TextUnformatted("Loading...");
    } else if (blocked_.empty()) {
      ImGui::TextUnformatted("You have not blocked anyone.");
    } else {
      ImGui::TextDisabled("Blocked players cannot send you friend requests.");
      ImGui::Separator();
      ImGui::Spacing();

      if (ImGui::BeginChild("##BlockedResults", ImVec2(0, 300), true)) {
        for (const auto& player : blocked_) {
          ImGui::PushID(static_cast<int>(player.xuid));

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
          ImGui::TextDisabled("%016llX",
                              static_cast<unsigned long long>(player.xuid));
          ImGui::EndGroup();

          ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80.0f);

          if (ImGui::Button("Unblock", ImVec2(80.0f, 0)) ||
              (ImGui::IsItemFocused() && input.Activated())) {
            kernel_state()->GetXboxLiveAPI()->BlockPlayer(
                profile_->GetOnlineXUID(), player.xuid, false);

            // Re-read rather than dropping the row locally, so the list always
            // shows what the hub actually holds.
            Refresh();

            ImGui::PopID();
            break;
          }

          ImGui::PopID();
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

    if (!keep_open) {
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  } else {
    // Popup gone - closed by us, by the X, or by ImGui itself.
    keep_open = false;
  }

  if (!keep_open) {
    focus_manager->UIDropFocus("BlockedListUI");
  }

  return keep_open;
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
