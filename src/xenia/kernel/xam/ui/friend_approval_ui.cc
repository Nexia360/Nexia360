/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/friend_approval_ui.h"

#include <set>

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/friends_manager.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

FriendApprovalUI::FriendApprovalUI(xe::ui::ImGuiDrawer* imgui_drawer,
                                   UserProfile* profile,
                                   const std::vector<HubPlayer>& requests)
    : requests_(requests), imgui_drawer_(imgui_drawer), profile_(profile) {
  LoadGamerpics();
}

void FriendApprovalUI::LoadGamerpics() {
  std::set<uint64_t> xuids;

  for (const auto& request : requests_) {
    xuids.insert(request.xuid);
  }

  if (xuids.empty()) {
    return;
  }

  // One batched fetch: the queue is usually short and this runs once, when the
  // prompt appears.
  const auto gamerpics =
      kernel_state()->GetXboxLiveAPI()->GetMultiGamerpicsFromXUIDs(xuids);

  for (const auto& [xuid, gamerpic] : gamerpics) {
    gamerpics_[xuid] = imgui_drawer_->LoadImGuiIcon({gamerpic});
  }
}

void FriendApprovalUI::Respond(Answer answer) {
  const auto& request = requests_[current_];

  auto* xlive_api = kernel_state()->GetXboxLiveAPI();

  const uint64_t online_xuid = profile_->GetOnlineXUID();

  switch (answer) {
    case Answer::kApprove:
      xlive_api->RespondToFriendRequest(online_xuid, request.xuid, true);

      // Approving is what makes the friendship on this side; the requester
      // picks theirs up from the hub on their next poll.
      kernel_state()->friends_manager()->AddFriend(profile_->xuid(),
                                                   request.xuid);
      break;

    case Answer::kDeny:
      // Removes the request. The requester is told nothing.
      xlive_api->RespondToFriendRequest(online_xuid, request.xuid, false);
      break;

    case Answer::kBlock:
      // Blocking clears this request and every future one from them, silently.
      xlive_api->BlockPlayer(online_xuid, request.xuid);
      break;

    case Answer::kLater:
      // Nothing is sent: the request stays on the hub, undrained. It is only
      // suppressed for the rest of this run, so it returns on the next launch.
      xlive_api->DeferFriendRequest(request.xuid);
      break;
  }

  current_++;
}

bool FriendApprovalUI::Draw() {
  auto* focus_manager = imgui_drawer_->GetFocusManager();

  if (requests_.empty() || current_ >= requests_.size()) {
    return false;
  }

  if (!has_opened_) {
    has_opened_ = true;
    focus_manager->UIChildFocus("FriendsDialog", "FriendApprovalUI");
    ImGui::OpenPopup("Friend Request");
  }

  const auto& input = focus_manager->XamInputFocus("FriendApprovalUI");

  const auto& request = requests_[current_];

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();

  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

  bool keep_open = true;

  // Nested inside the friends popup, so ImGui stacks it one level above
  // without disturbing anything beneath it.
  if (ImGui::BeginPopupModal("Friend Request", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // Who is asking, so this is never an anonymous prompt.
    xe::ui::ImmediateTexture* icon = imgui_drawer_->GetLoadingTileIcon();

    const auto cached = gamerpics_.find(request.xuid);

    if (cached != gamerpics_.cend() && cached->second) {
      icon = cached->second.get();
    }

    ImGui::Image(reinterpret_cast<ImTextureID>(icon),
                 xe::ui::default_image_icon_size);

    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::TextUnformatted(request.gamertag.c_str());
    ImGui::TextDisabled("wants to be your friend.");
    ImGui::EndGroup();

    if (requests_.size() > 1) {
      ImGui::Spacing();
      ImGui::TextDisabled("Request %d of %d", static_cast<int>(current_ + 1),
                          static_cast<int>(requests_.size()));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float btn_width = (ImGui::GetContentRegionAvail().x * 0.5f) -
                            (ImGui::GetStyle().ItemSpacing.x * 0.5f);
    const ImVec2 btn_size = ImVec2(btn_width, 25);

    const auto button = [&](const char* label, Answer answer,
                            const char* tooltip) -> bool {
      bool clicked = ImGui::Button(label, btn_size);

      if (ImGui::IsItemFocused() && input.Activated()) {
        clicked = true;
      }

      if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", tooltip);
      }

      return clicked;
    };

    Answer answer = Answer::kLater;
    bool answered = false;

    if (button("Approve", Answer::kApprove, "Add them as a friend.")) {
      answer = Answer::kApprove;
      answered = true;
    }

    ImGui::SameLine();

    if (button("Deny", Answer::kDeny,
               "Refuse this request. They are told nothing.")) {
      answer = Answer::kDeny;
      answered = true;
    }

    if (button("Block", Answer::kBlock,
               "Refuse this and every future request from them, silently.")) {
      answer = Answer::kBlock;
      answered = true;
    }

    ImGui::SameLine();

    if (button("Later", Answer::kLater,
               "Decide another time. Asked again next launch.")) {
      answer = Answer::kLater;
      answered = true;
    }

    ImGui::Spacing();
    ImGui::TextDisabled("A: Select");

    if (answered) {
      Respond(answer);

      // Answering the last one ends the prompt.
      if (current_ >= requests_.size()) {
        keep_open = false;
        ImGui::CloseCurrentPopup();
      }
    }

    ImGui::EndPopup();
  } else {
    keep_open = false;
  }

  if (!keep_open) {
    focus_manager->UIDropFocus("FriendApprovalUI");
  }

  return keep_open;
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
