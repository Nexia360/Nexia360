/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/gamercard_from_xuid_ui.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/util/friends_util.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

GamercardFromXUIDUI::GamercardFromXUIDUI(xe::ui::ImGuiDrawer* imgui_drawer,
                                         const uint64_t xuid,
                                         UserProfile* profile)
    : XamDialog(imgui_drawer), xuid_(xuid), profile_(profile) {
  is_self = xuid_ == profile_->xuid() || xuid_ == profile_->GetOnlineXUID();

  if (!is_self) {
    assert_true(IsOnlineXUID(xuid_));
  }

  if (!kernel_state()->GetXboxLiveAPI()->IsConnectedToServer()) {
    if (is_self) {
      presence_.Gamertag(profile_->name());
      presence_.RichPresence(profile_->GetPresenceString());
      presence_.XUID(profile_->GetOnlineXUID());
      presence_.TitleID(fmt::format("{:08X}", kernel_state()->title_id()));
    } else if (!is_self) {
      // Cached friend presence
      const auto friend_info =
          kernel_state()->friends_manager()->GetFriend(profile_->xuid(), xuid);

      presence_.Gamertag("Xenia User");
      presence_.RichPresence(xe::to_utf16("Unknown"));

      if (friend_info.has_value()) {
        are_friends = true;

        presence_.XUID(friend_info->xuid);

        if (friend_info->title_id) {
          presence_.TitleID(fmt::format("{:08X}", friend_info->title_id.get()));
        }
      }
    }
  } else {
    const auto presences =
        kernel_state()->presence_manager()->GetFriendsPresence(profile_->xuid(),
                                                               {xuid_});

    const uint64_t owner_xuid = profile_->xuid();

    immediate_gamerpic_ =
        std::async(std::launch::async, [owner_xuid, xuid, imgui_drawer]() {
          // Served from friends.sqlite when it is there, downloaded and
          // cached when it is not, and whatever is cached when offline.
          const auto gamerpic = GetFriendGamerpic(owner_xuid, xuid, false);

          std::shared_ptr<xe::ui::ImmediateTexture> shared_gamerpic =
              std::move(imgui_drawer->LoadImGuiIcon({gamerpic}));

          return shared_gamerpic;
        });

    presence_.XUID(xuid_);

    // GetFriendsPresence hands back a null object when the profile lookup
    // fails or the response cannot be deserialized - a hub hiccup while the
    // card is opening must not take the title down with it.
    if (presences && !presences->PlayersPresence().empty()) {
      presence_ = presences->PlayersPresence().front();

      if (is_self) {
        presence_.RichPresence(profile_->GetPresenceString());
      }
    }
  }

  if (is_self) {
    // profile_ is the profile that would have been looked up here - the call
    // site already guarantees it is non-null, so re-fetching it by xuid only
    // added an unguarded dereference.
    const auto gamerpic = profile_->GetProfileIcon(XTileType::kGamerTile);

    // No icon stored for this profile: leave the texture unset so
    // xeDrawFriendContent falls back to the default tile rather than
    // uploading an empty image.
    if (!gamerpic.empty()) {
      immediate_gamerpic_ =
          std::async(std::launch::async, [gamerpic, imgui_drawer]() {
            std::shared_ptr<xe::ui::ImmediateTexture> shared_gamerpic =
                std::move(imgui_drawer->LoadImGuiIcon({gamerpic}));

            return shared_gamerpic;
          });
    }
  }
}

void GamercardFromXUIDUI::OnDraw(ImGuiIO& io) {
  if (!dialog_open) {
    dialog_open = true;
    ImGui::OpenPopup(title_.c_str());
  }

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();

  if (!gamerpic_texture_ && immediate_gamerpic_.valid()) {
    if (immediate_gamerpic_.wait_for(0s) == std::future_status::ready) {
      gamerpic_texture_ = immediate_gamerpic_.get();
    }
  }

  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal(title_.c_str(), &dialog_open,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_GamepadFaceRight, false)) {
      ImGui::CloseCurrentPopup();
    }

    friend_presence_ = presence_.GetFriendPresence();

    xeDrawFriendContent(imgui_drawer(), profile_, gamerpic_texture_,
                        friend_presence_, nullptr, nullptr);

    ImGui::EndPopup();
  }

  if (!dialog_open) {
    Close();
  }
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
