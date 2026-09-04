/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/friends_ui.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/xam/ui/blocked_list_ui.h"
#include "xenia/kernel/xam/ui/friend_approval_ui.h"
#include "xenia/kernel/xam/ui/player_search_ui.h"
#include "xenia/kernel/xam/ui/recent_players_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

FriendsUI::FriendsUI(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile)
    : XamDialog(imgui_drawer), profile_(profile) {
  friends_ui_args_.friends_presence =
      kernel_state()->xam_state()->presence_manager()->GetFriendsPresenceSorted(
          profile_->xuid());

  friends_ui_args_.immediate_gamerpics =
      kernel_state()->GetXboxLiveAPI()->GetFriendsGamerpicsAsync(
          profile->xuid(), imgui_drawer);
}

void FriendsUI::OnDraw(ImGuiIO& io) {
  auto* drawer = imgui_drawer();
  auto* focus_manager = drawer->GetFocusManager();

  if (pending_close_) {
    if (!drawer->IsAnyGamepadActionPressed()) {
      focus_manager->UIDropFocus("FriendsDialog");
      Close();
    }
    return;
  }

  if (!has_opened_) {
    has_opened_ = true;
    focus_manager->UISetFocus("FriendsDialog");
    friends_ui_args_.content_args.first_draw = true;
    friends_ui_args_.content_args.friends_open = true;

    if (kernel_state()->GetXboxLiveAPI()->IsConnectedToServer()) {
      friends_ui_args_.content_args.filter_offline = true;
    }
  }

  const auto& input = focus_manager->XamInputFocus("FriendsDialog");

  if (input.ShouldClose()) {
    friends_ui_args_.content_args.friends_open = false;
    pending_close_ = true;
    return;
  }

  if (!xeDrawFriendsUI(imgui_drawer(), profile_, friends_ui_args_)) {
    friends_ui_args_.content_args.first_draw = false;
    pending_close_ = true;
  }
}

void xeDrawFriendsChildren(xe::ui::ImGuiDrawer* imgui_drawer,
                           UserProfile* profile,
                           FriendsUIArgs& friends_ui_args) {
  // Each child draws a popup nested in the friends popup we are inside of, and
  // reports when it is finished so it can be dropped.
  if (friends_ui_args.approval_prompt &&
      !friends_ui_args.approval_prompt->Draw()) {
    friends_ui_args.approval_prompt.reset();
    friends_ui_args.approval_prompt_open = false;
  }

  if (friends_ui_args.player_search && !friends_ui_args.player_search->Draw()) {
    friends_ui_args.player_search.reset();
  }

  if (friends_ui_args.recent_players &&
      !friends_ui_args.recent_players->Draw()) {
    friends_ui_args.recent_players.reset();
  }

  if (friends_ui_args.blocked_list && !friends_ui_args.blocked_list->Draw()) {
    friends_ui_args.blocked_list.reset();
  }
}

bool xeDrawFriendsUI(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile,
                     FriendsUIArgs& friends_ui_args) {
  if (!profile) {
    return false;
  }

  // Automatically sync/update friends presence information.
  friends_ui_args.friends_presence =
      kernel_state()->xam_state()->presence_manager()->GetFriendsPresenceSorted(
          profile->xuid());

  if (friends_ui_args.content_args.refresh_presence) {
    friends_ui_args.content_args.refresh_presence = false;

    friends_ui_args.friends_presence_sync =
        kernel_state()->presence_manager()->SyncPresenceAsync(profile->xuid());

    friends_ui_args.immediate_gamerpics =
        kernel_state()->GetXboxLiveAPI()->GetFriendsGamerpicsAsync(
            profile->xuid(), imgui_drawer);
  }

  if (friends_ui_args.immediate_gamerpics.valid()) {
    if (friends_ui_args.immediate_gamerpics.wait_for(0s) ==
        std::future_status::ready) {
      friends_ui_args.immediate_gamerpics_result =
          friends_ui_args.immediate_gamerpics.get();
    }
  }

  xeDrawFriendsContent(imgui_drawer, profile, friends_ui_args,
                       friends_ui_args.friends_presence,
                       friends_ui_args.immediate_gamerpics_result);

  auto* xlive_api = kernel_state()->GetXboxLiveAPI();

  const uint64_t online_xuid = profile->GetOnlineXUID();

  // Read the stored setting once, so the selector shows what the hub has
  // rather than defaulting to Anyone every time this opens.
  if (!friends_ui_args.content_args.privacy_loaded) {
    friends_ui_args.content_args.privacy_loaded = true;
    friends_ui_args.content_args.friend_privacy =
        static_cast<int>(xlive_api->GetFriendPrivacy(online_xuid));
  }

  // Publish the setting along with our friend list - the hub keeps no friend
  // graph of its own and needs ours to judge a friend-of-friends request.
  if (friends_ui_args.content_args.publish_privacy) {
    friends_ui_args.content_args.publish_privacy = false;

    const auto friends_xuids =
        kernel_state()->friends_manager()->GetFriendsXUIDs(profile->xuid());

    xlive_api->PublishFriendPrivacy(
        online_xuid,
        static_cast<XLiveAPI::FriendPrivacy>(
            friends_ui_args.content_args.friend_privacy),
        std::vector<uint64_t>(friends_xuids.cbegin(), friends_xuids.cend()));
  }

  // Poll the hub on an interval, off the UI thread. Asking every frame would
  // be one HTTP round trip per frame and would stall drawing on each.
  const auto now = std::chrono::steady_clock::now();

  if (now >= friends_ui_args.next_friend_poll &&
      !friends_ui_args.incoming_requests_query.valid() &&
      !friends_ui_args.approvals_query.valid()) {
    friends_ui_args.next_friend_poll = now + std::chrono::seconds(5);

    friends_ui_args.approvals_query =
        xlive_api->DrainFriendApprovalsAsync(online_xuid);

    if (!friends_ui_args.approval_prompt_open) {
      friends_ui_args.incoming_requests_query =
          xlive_api->GetIncomingFriendRequestsAsync(online_xuid);
    }
  }

  // Anyone who approved us while we were away: add them locally now. Reading
  // consumed them on the hub, so this happens exactly once each.
  if (friends_ui_args.approvals_query.valid() &&
      friends_ui_args.approvals_query.wait_for(0s) ==
          std::future_status::ready) {
    for (const uint64_t approver : friends_ui_args.approvals_query.get()) {
      kernel_state()->friends_manager()->AddFriend(profile->xuid(), approver);
      friends_ui_args.content_args.refresh_presence = true;
    }
  }

  // Requests waiting on us raise the approval prompt, with the requester's
  // gamerpic so it is never an anonymous ask.
  if (friends_ui_args.incoming_requests_query.valid() &&
      friends_ui_args.incoming_requests_query.wait_for(0s) ==
          std::future_status::ready) {
    auto incoming = friends_ui_args.incoming_requests_query.get();

    // Drop anything answered with "Later" this run. The hub still holds them -
    // they are only hidden until the next launch.
    std::erase_if(incoming, [xlive_api](const HubPlayer& request) {
      return xlive_api->IsFriendRequestDeferred(request.xuid);
    });

    if (!incoming.empty() && !friends_ui_args.approval_prompt_open) {
      friends_ui_args.approval_prompt_open = true;

      friends_ui_args.approval_prompt =
          std::make_unique<FriendApprovalUI>(imgui_drawer, profile, incoming);
    }
  }

  if (friends_ui_args.content_args.recent_players_open) {
    friends_ui_args.content_args.recent_players_open = false;

    friends_ui_args.recent_players =
        std::make_unique<RecentPlayersUI>(imgui_drawer, profile);
  }

  if (friends_ui_args.content_args.blocked_list_open) {
    friends_ui_args.content_args.blocked_list_open = false;

    friends_ui_args.blocked_list =
        std::make_unique<BlockedListUI>(imgui_drawer, profile);
  }

  if (friends_ui_args.content_args.player_search_open) {
    friends_ui_args.content_args.player_search_open = false;

    friends_ui_args.player_search =
        std::make_unique<PlayerSearchUI>(imgui_drawer, profile);
  }

  return friends_ui_args.content_args.friends_open;
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
