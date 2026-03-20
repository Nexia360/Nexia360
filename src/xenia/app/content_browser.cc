/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/content_browser.h"

#include <algorithm>
#include <fstream>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/imgui/imgui.h"
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/app/emulator_window.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace app {

static constexpr const char* kFilesystemTomlName = "nexia_filesystem.toml";

ContentBrowser::ContentBrowser(ui::ImGuiDrawer* imgui_drawer,
                               EmulatorWindow* emulator_window)
    : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {
  LoadSources();
  if (source_folders_.empty()) {
    needs_first_run_picker_ = true;
  }
}

void ContentBrowser::LoadSources() {
  source_folders_.clear();

  std::ifstream file(kFilesystemTomlName);
  if (!file.is_open()) {
    return;
  }

  try {
    auto parsed = toml::parse(file);
    if (auto* sources = parsed["sources"].as_table()) {
      for (const auto& [key, value] : *sources) {
        if (auto* str = value.as_string()) {
          auto path = xe::to_path(str->get());
          if (std::filesystem::exists(path)) {
            source_folders_.push_back(path);
          }
        }
      }
    }
  } catch (const toml::parse_error& e) {
    XELOGW("ContentBrowser: Failed to parse {}: {}", kFilesystemTomlName,
           e.what());
  }
}

void ContentBrowser::SaveSources() {
  auto sources_table = toml::table();
  uint32_t index = 0;
  for (const auto& folder : source_folders_) {
    sources_table.insert(std::to_string(index++), xe::path_to_utf8(folder));
  }

  auto root = toml::table();
  root.insert("sources", sources_table);

  std::ofstream file(kFilesystemTomlName, std::ofstream::trunc);
  if (file.is_open()) {
    file << root;
    file.close();
  }
}

std::vector<std::filesystem::path> ContentBrowser::EnumerateDrives() {
  std::vector<std::filesystem::path> drives;

#if XE_PLATFORM_WIN32
  DWORD drive_mask = GetLogicalDrives();
  for (int i = 0; i < 26; i++) {
    if (drive_mask & (1 << i)) {
      char drive_letter[4] = {static_cast<char>('A' + i), ':', '\\', 0};
      drives.push_back(std::filesystem::path(drive_letter));
    }
  }
#else
  // On non-Windows, start from root
  drives.push_back(std::filesystem::path("/"));
#endif

  return drives;
}

bool ContentBrowser::IsSupportedFile(const std::filesystem::path& path) {
  auto ext = path.extension().string();
  if (ext.empty()) {
    // No extension — could be a content package (CON/LIVE/PIRS)
    return true;
  }
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext == ".iso" || ext == ".xex" || ext == ".zar";
}

void ContentBrowser::RefreshCache(const std::filesystem::path& path) {
  if (path == cached_path_) {
    return;
  }
  cached_path_ = path;
  cached_entries_.clear();

  try {
    cached_entries_ = xe::filesystem::ListFiles(path);
  } catch (...) {
    // Directory might not be accessible
  }

  // Sort: files first, then directories, alphabetically within each group
  std::sort(
      cached_entries_.begin(), cached_entries_.end(),
      [](const xe::filesystem::FileInfo& a, const xe::filesystem::FileInfo& b) {
        if (a.type != b.type) {
          return a.type == xe::filesystem::FileInfo::Type::kFile;
        }
        return a.name < b.name;
      });
}

void ContentBrowser::OnDraw(ImGuiIO& io) {
  // First-run: auto-open folder picker
  if (needs_first_run_picker_) {
    needs_first_run_picker_ = false;
    browsing_for_source_ = true;
    browse_path_.clear();
  }

  ImGui::SetNextWindowPos(ImVec2(10, 40), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(960, io.DisplaySize.y - 60),
                           ImGuiCond_FirstUseEver);

  if (!ImGui::Begin("Content Browser", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  // Focus the content browser window on first frame
  static bool first_frame = true;
  if (first_frame) {
    ImGui::SetWindowFocus();
    first_frame = false;
  }

  // Track focus state so EmulatorWindow can disable hotkeys/input passthrough
  is_focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  if (is_focused_) {
    io.WantCaptureKeyboard = true;
    io.WantCaptureMouse = true;
  }

  // Left/Right D-PAD switches between file panel (0) and add sources (1)
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) {
    active_panel_ = 1;  // Add Sources
    needs_focus_set_ = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) {
    active_panel_ = 0;  // File panel
    needs_focus_set_ = true;
  }

  if (browsing_for_source_) {
    DrawFolderPicker();
  } else if (active_panel_ == 1) {
    // Add Sources is focused — highlight the button
    DrawSourcesList();
  } else if (current_path_.empty()) {
    DrawSourcesList();
  } else {
    DrawFolderContents(current_path_);
  }

  ImGui::End();
}

void ContentBrowser::DrawSourcesList() {
  ImGui::Text("Game Sources");
  ImGui::Separator();

  // Scrollable area for sources
  float footer_height = ImGui::GetFrameHeightWithSpacing() * 3 + 8;
  ImGui::BeginChild("SourcesList", ImVec2(0, -footer_height), true);

  // Focus the first item when file panel becomes active
  if (active_panel_ == 0 && needs_focus_set_ && !source_folders_.empty()) {
    ImGui::SetKeyboardFocusHere();
    needs_focus_set_ = false;
  }

  for (size_t i = 0; i < source_folders_.size(); i++) {
    auto folder_name = source_folders_[i].filename().string();
    if (folder_name.empty()) {
      folder_name = xe::path_to_utf8(source_folders_[i]);
    }

    ImGui::PushID(static_cast<int>(i));

    std::string label = fmt::format("[DIR] {}", folder_name);
    bool selected = (selected_source_index_ == static_cast<int>(i));
    if (ImGui::Selectable(label.c_str(), selected)) {
      current_path_ = source_folders_[i];
      cached_path_.clear();
    }
    if (ImGui::IsItemFocused()) {
      selected_source_index_ = static_cast<int>(i);
    }
    if (i == 0) {
      ImGui::SetItemDefaultFocus();
    }

    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", xe::path_to_utf8(source_folders_[i]).c_str());
    }

    ImGui::PopID();
  }

  if (source_folders_.empty()) {
    ImGui::TextDisabled("No sources configured.");
    ImGui::TextDisabled("Click '+ Add Sources' below.");
  }

  ImGui::EndChild();

  // Y button opens options popup for selected source
  if (selected_source_index_ >= 0 &&
      selected_source_index_ < static_cast<int>(source_folders_.size()) &&
      ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp)) {
    ImGui::OpenPopup("SourceOptions");
  }

  if (ImGui::BeginPopup("SourceOptions")) {
    ImGui::Text(
        "Options: %s",
        source_folders_[selected_source_index_].filename().string().c_str());
    ImGui::Separator();

    if (ImGui::Selectable("Remove from list")) {
      source_folders_.erase(source_folders_.begin() + selected_source_index_);
      selected_source_index_ = -1;
      SaveSources();
    }
    if (selected_source_index_ > 0 && ImGui::Selectable("Move up")) {
      std::swap(source_folders_[selected_source_index_],
                source_folders_[selected_source_index_ - 1]);
      selected_source_index_--;
      SaveSources();
    }
    if (selected_source_index_ >= 0 &&
        selected_source_index_ < static_cast<int>(source_folders_.size()) - 1 &&
        ImGui::Selectable("Move down")) {
      std::swap(source_folders_[selected_source_index_],
                source_folders_[selected_source_index_ + 1]);
      selected_source_index_++;
      SaveSources();
    }

    ImGui::EndPopup();
  }

  // Bottom bar with legend
  ImGui::Separator();

  // Add Sources button — highlight if active_panel_ == 1
  if (active_panel_ == 1) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.3f, 1.0f));
  }
  bool add_clicked = ImGui::Button("+ Add Sources");
  if (active_panel_ == 1) {
    ImGui::PopStyleColor();
    // A button activates Add Sources when panel is focused
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
      add_clicked = true;
    }
  }
  if (add_clicked) {
    browsing_for_source_ = true;
    browse_path_.clear();
  }

  // Legend
  ImGui::TextDisabled("A:Open | Y:Options | D-Pad:Navigate | LR:Switch Panel");
}

void ContentBrowser::DrawFolderContents(
    const std::filesystem::path& folder_path) {
  // Header with current path
  auto path_str = xe::path_to_utf8(folder_path);
  ImGui::Text("%s", path_str.c_str());
  ImGui::Separator();

  // Back button (or gamepad B)
  bool go_back =
      ImGui::Button("<- Back") ||
      (is_focused_ && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight));
  if (go_back) {
    // Check if this is a source root
    bool is_source_root = false;
    for (const auto& src : source_folders_) {
      if (folder_path == src) {
        is_source_root = true;
        break;
      }
    }
    if (is_source_root) {
      current_path_.clear();
    } else {
      current_path_ = folder_path.parent_path();
    }
    cached_path_.clear();  // Force cache refresh
    return;
  }

  ImGui::Separator();

  // Refresh cache if needed
  RefreshCache(folder_path);

  // Scrollable content area
  float footer_height = ImGui::GetFrameHeightWithSpacing() * 3 + 8;
  ImGui::BeginChild("FolderContents", ImVec2(0, -footer_height), true);

  // Focus first item when file panel becomes active
  if (active_panel_ == 0 && needs_focus_set_ && !cached_entries_.empty()) {
    ImGui::SetKeyboardFocusHere();
    needs_focus_set_ = false;
  }

  bool first_item = true;
  for (const auto& entry : cached_entries_) {
    if (entry.type == xe::filesystem::FileInfo::Type::kDirectory) {
      std::string label = fmt::format("[DIR] {}", xe::path_to_utf8(entry.name));
      if (ImGui::Selectable(label.c_str())) {
        current_path_ = folder_path / entry.name;
        cached_path_.clear();
      }
    } else if (IsSupportedFile(entry.name)) {
      std::string label = fmt::format("      {}", xe::path_to_utf8(entry.name));
      if (ImGui::Selectable(label.c_str())) {
        auto full_path = folder_path / entry.name;
        emulator_window_->RunTitle(full_path);
      }
    }
    if (first_item) {
      ImGui::SetItemDefaultFocus();
      first_item = false;
    }
  }

  if (cached_entries_.empty()) {
    ImGui::TextDisabled("(empty)");
  }

  ImGui::EndChild();

  // Bottom bar with legend
  ImGui::Separator();

  if (active_panel_ == 1) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.3f, 1.0f));
  }
  bool add_clicked = ImGui::Button("+ Add Sources");
  if (active_panel_ == 1) {
    ImGui::PopStyleColor();
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
      add_clicked = true;
    }
  }
  if (add_clicked) {
    browsing_for_source_ = true;
    browse_path_.clear();
  }

  // Legend
  ImGui::TextDisabled("A:Launch | B:Back | D-Pad:Navigate | LR:Switch Panel");
}

void ContentBrowser::DrawFolderPicker() {
  ImGui::Text("Select Folder to Add");
  ImGui::Separator();

  // Show current browse path
  if (!browse_path_.empty()) {
    ImGui::Text("%s", xe::path_to_utf8(browse_path_).c_str());
    ImGui::Separator();

    // Back button (or gamepad B)
    bool go_back =
        ImGui::Button("<- Back") ||
        (is_focused_ && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight));
    if (go_back) {
      auto parent = browse_path_.parent_path();
      if (parent == browse_path_) {
        // At root — go back to drive list
        browse_path_.clear();
      } else {
        browse_path_ = parent;
      }
      cached_path_.clear();
      return;
    }
    ImGui::Separator();
  }

  // Scrollable content
  float footer_height = ImGui::GetFrameHeightWithSpacing() * 3 + 12;
  ImGui::BeginChild("FolderPicker", ImVec2(0, -footer_height), true);

  // Focus first item when entering folder picker
  if (needs_focus_set_) {
    ImGui::SetKeyboardFocusHere();
    needs_focus_set_ = false;
  }

  if (browse_path_.empty()) {
    // Show drive roots
    auto drives = EnumerateDrives();
    for (const auto& drive : drives) {
      std::string label = fmt::format("[DRV] {}", xe::path_to_utf8(drive));
      if (ImGui::Selectable(label.c_str())) {
        browse_path_ = drive;
        cached_path_.clear();
      }
    }
  } else {
    // Show subdirectories only
    RefreshCache(browse_path_);

    bool any_dirs = false;
    for (const auto& entry : cached_entries_) {
      if (entry.type != xe::filesystem::FileInfo::Type::kDirectory) {
        continue;
      }
      any_dirs = true;
      std::string label = fmt::format("[DIR] {}", xe::path_to_utf8(entry.name));
      if (ImGui::Selectable(label.c_str())) {
        browse_path_ = browse_path_ / entry.name;
        cached_path_.clear();
      }
    }

    if (!any_dirs) {
      ImGui::TextDisabled("(no subfolders)");
    }
  }

  ImGui::EndChild();

  // Bottom bar with Cancel and Select Folder
  ImGui::Separator();
  bool cancel =
      ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight);
  if (cancel) {
    browsing_for_source_ = false;
    browse_path_.clear();
    cached_path_.clear();
  }

  if (!browse_path_.empty()) {
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    bool select = ImGui::Button("Select Folder") ||
                  ImGui::IsKeyPressed(ImGuiKey_GamepadStart);
    if (select) {
      // Check for duplicates
      bool already_added = false;
      for (const auto& src : source_folders_) {
        if (src == browse_path_) {
          already_added = true;
          break;
        }
      }
      if (!already_added) {
        source_folders_.push_back(browse_path_);
        SaveSources();
      }
      browsing_for_source_ = false;
      browse_path_.clear();
      cached_path_.clear();
    }
  }

  // Legend
  ImGui::TextDisabled("A:Open Folder | START:Select Folder | B:Cancel");
}

}  // namespace app
}  // namespace xe
