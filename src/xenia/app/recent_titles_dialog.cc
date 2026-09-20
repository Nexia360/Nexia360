/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/recent_titles_dialog.h"

#include <filesystem>
#include <utility>

#include "xenia/app/emulator_window.h"
#include "xenia/base/filesystem.h"
#include "xenia/emulator.h"

namespace xe {
namespace app {

namespace {

constexpr float kIconSize = 48.0f;
constexpr float kRowHeight = kIconSize + 8.0f;

}  // namespace

RecentTitlesDialog::RecentTitlesDialog(ui::ImGuiDrawer* imgui_drawer,
                                       EmulatorWindow* emulator_window,
                                       Mode mode)
    : ui::ImGuiDialog(imgui_drawer),
      emulator_window_(emulator_window),
      mode_(mode) {}

RecentTitlesDialog::~RecentTitlesDialog() {
  if (closed_callback_) {
    closed_callback_();
  }
}

ui::ImmediateTexture* RecentTitlesDialog::IconFor(size_t index) {
  const auto found = icons_.find(index);
  if (found != icons_.end()) {
    return found->second.get();
  }
  const auto& titles = emulator_window_->recently_launched_titles();
  if (index >= titles.size() || titles[index].icon.empty()) {
    // Remembered as absent, so a title without art is not decoded every frame.
    icons_[index] = nullptr;
    return nullptr;
  }
  icons_[index] = imgui_drawer()->LoadImGuiIcon(titles[index].icon);
  return icons_[index].get();
}

void RecentTitlesDialog::OnDraw(ImGuiIO& io) {
  const char* title =
      mode_ == Mode::kLaunch ? "Open Recent" : "Open Recent with Title Update";
  if (!opened_) {
    ImGui::OpenPopup(title);
    opened_ = true;
  }

  bool open = true;
  if (!ImGui::BeginPopupModal(title, &open,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    Close();
    return;
  }

  const auto& titles = emulator_window_->recently_launched_titles();
  if (titles.empty()) {
    ImGui::TextUnformatted("Nothing has been played yet.");
  } else {
    ImGui::TextDisabled("%zu title(s), most recent first", titles.size());
  }
  ImGui::Separator();

  // The whole point of replacing the native menu: a fixed frame that scrolls
  // rather than a list that runs off the bottom of the screen.
  ImGui::BeginChild("##recent_list", ImVec2(560.0f, 360.0f), true);
  std::filesystem::path chosen;
  uint32_t chosen_title_id = 0;
  bool picked = false;

  for (size_t i = 0; i < titles.size(); ++i) {
    const RecentTitleEntry& entry = titles[i];
    const std::string name = entry.title_name.empty()
                                 ? xe::path_to_utf8(entry.path_to_file)
                                 : entry.title_name;

    ImGui::PushID(int(i));
    const ImVec2 cursor = ImGui::GetCursorPos();
    if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_None,
                          ImVec2(0.0f, kRowHeight))) {
      chosen = entry.path_to_file;
      chosen_title_id = entry.title_id;
      picked = true;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::SetCursorPos(ImVec2(cursor.x + 4.0f, cursor.y + 4.0f));

    if (ui::ImmediateTexture* icon = IconFor(i)) {
      ImGui::Image(reinterpret_cast<ImTextureID>(icon),
                   ImVec2(kIconSize, kIconSize));
    } else {
      // Keeps the text column in line whether or not a title has art.
      ImGui::Dummy(ImVec2(kIconSize, kIconSize));
    }
    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::TextUnformatted(name.c_str());
    if (entry.title_id) {
      ImGui::TextDisabled("%08X", entry.title_id);
    } else {
      ImGui::TextDisabled("unknown title id");
    }
    ImGui::EndGroup();
    ImGui::SetCursorPos(ImVec2(cursor.x, cursor.y + kRowHeight));

    if (hovered) {
      ImGui::SetTooltip("%s", xe::path_to_utf8(entry.path_to_file).c_str());
    }
    ImGui::PopID();
  }
  ImGui::EndChild();

  ImGui::Separator();
  if (ImGui::Button("Cancel")) {
    open = false;
  }

  ImGui::EndPopup();

  if (picked) {
    // Never from inside the frame: launching tears down and rebuilds the
    // graphics system, and opening the update selector puts up another modal
    // while this one is still on the popup stack. Both wait for the draw to
    // finish, exactly as the gamepad hotkey path does.
    EmulatorWindow* window = emulator_window_;
    const Mode mode = mode_;
    Close();
    window->app_context().CallInUIThread(
        [window, mode, chosen, chosen_title_id]() {
          if (mode == Mode::kLaunch) {
            window->RunTitle(chosen);
          } else {
            window->OpenTitleUpdateSelector(chosen, chosen_title_id);
          }
        });
    return;
  }

  if (!open) {
    Close();
  }
}

}  // namespace app
}  // namespace xe
