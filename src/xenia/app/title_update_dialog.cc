/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/title_update_dialog.h"

#include <cstring>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/imgui/imgui_internal.h"
#include "xenia/app/emulator_window.h"
#include "xenia/emulator.h"

namespace xe {
namespace app {

TitleUpdateDialog::TitleUpdateDialog(ui::ImGuiDrawer* imgui_drawer,
                                     EmulatorWindow* emulator_window,
                                     uint32_t title_id,
                                     std::filesystem::path launch_path)
    : ui::ImGuiDialog(imgui_drawer),
      emulator_window_(emulator_window),
      title_id_(title_id),
      launch_path_(std::move(launch_path)) {
  Reload();
}

void TitleUpdateDialog::Reload() {
  auto* manager = emulator_window_->emulator()->title_update_manager();
  entries_.clear();
  active_id_.clear();
  if (manager) {
    entries_ = manager->List(title_id_);
    active_id_ = manager->GetActive(title_id_);
  }
  selected_id_ = active_id_;
  name_buffers_.clear();
  name_buffers_.resize(entries_.size());
  for (size_t i = 0; i < entries_.size(); ++i) {
    name_buffers_[i].fill('\0');
    std::strncpy(name_buffers_[i].data(), entries_[i].name.c_str(),
                 name_buffers_[i].size() - 1);
  }
}

void TitleUpdateDialog::OnDraw(ImGuiIO& io) {
  if (!opened_) {
    ImGui::OpenPopup("Select Title Update");
    opened_ = true;
  }

  bool open = true;
  if (ImGui::BeginPopupModal("Select Title Update", &open,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    auto* manager = emulator_window_->emulator()->title_update_manager();

    ImGui::Text("Title %08X", title_id_);
    ImGui::Separator();

    if (entries_.empty()) {
      ImGui::TextUnformatted("No title updates installed for this title.");
    }

    ImGui::BeginChild("##tu_list", ImVec2(480.0f, 240.0f), true);
    if (ImGui::RadioButton("None (run without an update)",
                           selected_id_.empty())) {
      selected_id_.clear();
    }

    bool mutated = false;
    for (size_t i = 0; i < entries_.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      const auto& entry = entries_[i];

      if (ImGui::RadioButton("##select", selected_id_ == entry.id)) {
        selected_id_ = entry.id;
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(220.0f);
      ImGui::InputText("##name", name_buffers_[i].data(),
                       name_buffers_[i].size());
      ImGui::SameLine();
      ImGui::Text("v%s  (%.1f MB)", entry.version.c_str(),
                  entry.size_bytes / (1024.0 * 1024.0));
      if (entry.id == active_id_) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "[active]");
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Delete")) {
        if (manager) {
          manager->Remove(title_id_, entry.id);
        }
        mutated = true;
      }
      ImGui::PopID();
      if (mutated) {
        break;
      }
    }

    ImGui::EndChild();

    if (mutated) {
      Reload();
      ImGui::EndPopup();
      return;
    }

    ImGui::Separator();
    if (ImGui::Button("Load")) {
      if (manager) {
        for (size_t i = 0; i < entries_.size(); ++i) {
          std::string new_name = name_buffers_[i].data();
          if (new_name != entries_[i].name) {
            manager->Rename(title_id_, entries_[i].id, new_name);
          }
        }
        manager->SetActive(title_id_, selected_id_);
      }
      ImGui::CloseCurrentPopup();
      Close();
      // Launch after this draw completes so the dialog is gone first.
      EmulatorWindow* window = emulator_window_;
      std::filesystem::path path = launch_path_;
      window->app_context().CallInUIThread(
          [window, path]() { window->RunTitle(path); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
      Close();
    }
    ImGui::EndPopup();
  } else {
    Close();
  }

  if (!open) {
    Close();
  }
}

}  // namespace app
}  // namespace xe
