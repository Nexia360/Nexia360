/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/friendfind_ui.h"
#include "third_party/libcurl/include/curl/curl.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/ui/imgui_host_notification.h"

DECLARE_string(api_address);

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

namespace {
size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb,
                         void* userp) {
  size_t total_size = size * nmemb;
  std::string* response = static_cast<std::string*>(userp);
  response->append(static_cast<char*>(contents), total_size);
  return total_size;
}
}  // namespace

FriendFindUI::FriendFindUI(xe::ui::ImGuiDrawer* imgui_drawer,
                           UserProfile* profile)
    : XamDialog(imgui_drawer), profile_(profile) {
  current_title_id_ = kernel_state()->title_id();
  RefreshSessions();
}

void FriendFindUI::RefreshSessions() {
  players_.clear();
  is_loading_ = true;

  // Build URL from api_address cvar
  std::string url = cvars::api_address;
  if (!url.empty() && url.back() != '/') {
    url += '/';
  }
  url += "sessions";

  // Use curl to fetch sessions
  std::string response_data;
  CURL* curl = curl_easy_init();

  if (!curl) {
    is_loading_ = false;
    return;
  }

  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "xenia");

  CURLcode res = curl_easy_perform(curl);

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK || http_code != 200 || response_data.empty()) {
    is_loading_ = false;
    return;
  }

  rapidjson::Document doc;
  doc.Parse(response_data.c_str());

  if (doc.HasParseError() || !doc.IsObject()) {
    is_loading_ = false;
    return;
  }

  // Parse Titles array
  if (!doc.HasMember("Titles") || !doc["Titles"].IsArray()) {
    is_loading_ = false;
    return;
  }

  const auto& titles = doc["Titles"].GetArray();

  for (const auto& title : titles) {
    if (!title.IsObject()) continue;

    std::string title_name;
    uint32_t title_id = 0;

    if (title.HasMember("Name") && title["Name"].IsString()) {
      title_name = title["Name"].GetString();
    }

    if (title.HasMember("TitleId") && title["TitleId"].IsString()) {
      try {
        title_id = std::stoul(title["TitleId"].GetString(), nullptr, 16);
      } catch (...) {
        title_id = 0;
      }
    }

    // Parse sessions array
    if (!title.HasMember("sessions") || !title["sessions"].IsArray()) {
      continue;
    }

    const auto& sessions = title["sessions"].GetArray();

    for (const auto& session : sessions) {
      if (!session.IsObject()) continue;

      SessionPlayer player;
      player.title_name = title_name;
      player.title_id = title_id;

      // Get host info
      if (session.HasMember("host_gamertag") &&
          session["host_gamertag"].IsString()) {
        player.gamertag = session["host_gamertag"].GetString();
        player.gamertag.erase(
            std::remove(player.gamertag.begin(), player.gamertag.end(), '\0'),
            player.gamertag.end());
      }

      if (session.HasMember("host_xuid") && session["host_xuid"].IsString()) {
        try {
          player.xuid =
              std::stoull(session["host_xuid"].GetString(), nullptr, 16);
        } catch (...) {
          player.xuid = 0;
        }
      }

      if (session.HasMember("host_presence") &&
          session["host_presence"].IsString()) {
        player.presence = session["host_presence"].GetString();
      }

      if (session.HasMember("total") && session["total"].IsInt()) {
        player.max_players = session["total"].GetInt();
      }

      if (session.HasMember("players") && session["players"].IsArray()) {
        player.player_count =
            static_cast<int>(session["players"].GetArray().Size());
      }

      if (session.HasMember("media_id") && session["media_id"].IsString()) {
        player.media_id = session["media_id"].GetString();
      }

      if (session.HasMember("version") && session["version"].IsString()) {
        player.version = session["version"].GetString();
      }

      if (player.xuid == 0) continue;

      if (player.xuid == profile_->xuid() ||
          player.xuid == profile_->GetOnlineXUID()) {
        continue;
      }

      if (profile_->IsFriend(player.xuid, nullptr)) {
        continue;
      }

      players_.push_back(player);
    }
  }

  is_loading_ = false;
}

void FriendFindUI::DrawPlayerEntry(const SessionPlayer& player, int index) {
  auto* drawer = imgui_drawer();
  auto* focus_manager = drawer->GetFocusManager();
  const auto& input = focus_manager->XamInputFocus("FriendFindUI");

  ImGui::TableNextRow();
  ImGui::TableNextColumn();

  bool is_selected = (selected_index_ == index);
  std::string label = fmt::format("{}##{}", player.gamertag, index);

  if (ImGui::Selectable(label.c_str(), is_selected,
                        ImGuiSelectableFlags_SpanAllColumns)) {
    selected_index_ = index;
  }

  if (is_selected && input.Activated()) {
    uint32_t user_index =
        kernel_state()->xam_state()->GetUserIndexAssignedToProfileFromXUID(
            profile_->GetOnlineXUID());

    bool added = profile_->AddFriendFromXUID(player.xuid);

    if (added) {
      XLiveAPI::AddFriend(player.xuid);

      kernel_state()->BroadcastNotification(kXNotificationFriendsFriendAdded,
                                            user_index);

      new xe::ui::HostNotificationWindow(drawer, "Added Friend",
                                         player.gamertag, 0);
    } else {
      new xe::ui::HostNotificationWindow(drawer, "Error",
                                         "Failed to add friend", 0);
    }
  }

  ImGui::TableNextColumn();
  ImGui::TextUnformatted(player.title_name.c_str());

  ImGui::TableNextColumn();
  ImGui::Text("%d/%d", player.player_count, player.max_players);

  ImGui::TableNextColumn();
  std::string presence = player.presence;
  if (presence.length() > 25) {
    presence = presence.substr(0, 22) + "...";
  }
  ImGui::TextUnformatted(presence.c_str());
}

void FriendFindUI::OnDraw(ImGuiIO& io) {
  auto* drawer = imgui_drawer();
  auto* focus_manager = drawer->GetFocusManager();

  if (pending_close_) {
    if (!drawer->IsAnyGamepadActionPressed()) {
      focus_manager->UIDropFocus("FriendFindUI");
      Close();
    }
    return;
  }

  if (!has_opened_) {
    focus_manager->UISetFocus("FriendFindUI");
    ImGui::OpenPopup("Find Players");
    has_opened_ = true;
  }

  const auto& input = focus_manager->XamInputFocus("FriendFindUI");

  if (input.ShouldClose()) {
    pending_close_ = true;
    return;
  }

  if (!players_.empty()) {
    if (input.NavDown()) {
      selected_index_ =
          std::min(selected_index_ + 1, static_cast<int>(players_.size()) - 1);
      if (selected_index_ < 0) selected_index_ = 0;
    }
    if (input.NavUp()) {
      selected_index_ = std::max(selected_index_ - 1, 0);
    }
  }

  if (input.y_released) {
    RefreshSessions();
  }

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(550, 400), ImGuiCond_FirstUseEver);

  bool dialog_open = true;
  if (ImGui::BeginPopupModal("Find Players", &dialog_open,
                             ImGuiWindowFlags_NoCollapse)) {
    ImGui::Checkbox("Show only current game", &filter_same_game_);
    ImGui::SameLine();

    bool refresh_clicked = ImGui::Button("Refresh");
    if (ImGui::IsItemFocused() && input.Activated()) {
      refresh_clicked = true;
    }
    if (refresh_clicked) {
      RefreshSessions();
    }

    ImGui::Separator();

    if (is_loading_) {
      ImGui::TextUnformatted("Loading sessions...");
    } else if (players_.empty()) {
      ImGui::TextUnformatted("No players found.");
      ImGui::TextDisabled("Players who are already friends are hidden.");
    } else {
      int visible_count = 0;
      for (const auto& p : players_) {
        if (!filter_same_game_ || p.title_id == current_title_id_) {
          visible_count++;
        }
      }

      ImGui::Text("Found %d player(s)", visible_count);
      ImGui::Separator();

      if (visible_count == 0) {
        ImGui::TextUnformatted("No players in current game found.");
        ImGui::TextDisabled("Uncheck filter to see all players.");
      } else {
        if (ImGui::BeginTable(
                "##PlayersTable", 4,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                ImVec2(0, 250))) {
          ImGui::TableSetupColumn("Gamertag", ImGuiTableColumnFlags_WidthFixed,
                                  140);
          ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthFixed,
                                  140);
          ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed,
                                  60);
          ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableHeadersRow();

          for (size_t i = 0; i < players_.size(); i++) {
            const auto& player = players_[i];

            if (filter_same_game_ && player.title_id != current_title_id_) {
              continue;
            }

            DrawPlayerEntry(player, static_cast<int>(i));
          }

          ImGui::EndTable();
        }
      }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("A: Add Friend | Y: Refresh | B/Back: Close");

    ImGui::EndPopup();
  }

  if (!dialog_open) {
    pending_close_ = true;
  }
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
