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
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace app {

uint64_t TitleUpdateDialog::ImportXuid() const {
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state || !kernel_state->xam_state()) {
    return 0;
  }
  // Slot 0 - saves are per-profile and this dialog acts for the signed-in user.
  const auto profile = kernel_state->xam_state()->GetUserProfile(uint32_t(0));
  return profile ? profile->xuid() : 0;
}

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

    // Overwrite confirmation is a MODE of this popup, not a second one.
    // ImGui allows a single popup per level - opening a nested modal
    // corrupts the popup stack, which breaks the on-screen keyboard and
    // loses text typed into any form drawn above it.
    if (!import_conflicts_.empty()) {
      ImGui::Text("%zu file(s) already exist in this update and would be",
                  import_conflicts_.size());
      ImGui::TextUnformatted("overwritten:");
      ImGui::BeginChild("##conflicts", ImVec2(420.0f, 160.0f), true);
      for (const auto& file : import_conflicts_) {
        ImGui::TextUnformatted(file.c_str());
      }
      ImGui::EndChild();
      if (ImGui::Button("Overwrite") && manager) {
        manager->ImportNoTuContent(title_id_, selected_id_, ImportXuid(),
                                   /*dry_run=*/false);
        import_status_ = "Saves imported (overwritten).";
        import_conflicts_.clear();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        import_conflicts_.clear();
      }
      ImGui::EndPopup();
      return;
    }

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

    // Fixed columns rather than a SameLine chain: the name field then scrolls
    // its own text instead of pushing Delete off the right edge.
    if (!entries_.empty() &&
        ImGui::BeginTable(
            "##tu_table", 4,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX)) {
      ImGui::TableSetupColumn("##sel", ImGuiTableColumnFlags_WidthFixed, 24.0f);
      ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed,
                              220.0f);
      ImGui::TableSetupColumn("##ver", ImGuiTableColumnFlags_WidthFixed,
                              140.0f);
      ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 60.0f);

      for (size_t i = 0; i < entries_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const auto& entry = entries_[i];

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::RadioButton("##select", selected_id_ == entry.id)) {
          selected_id_ = entry.id;
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##name", name_buffers_[i].data(),
                         name_buffers_[i].size());

        ImGui::TableSetColumnIndex(2);
        if (entry.id == active_id_) {
          ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "v%s [active]",
                             entry.version.c_str());
        } else {
          ImGui::Text("v%s  (%.1f MB)", entry.version.c_str(),
                      entry.size_bytes / (1024.0 * 1024.0));
        }

        ImGui::TableSetColumnIndex(3);
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

      ImGui::EndTable();
    }

    ImGui::EndChild();

    if (mutated) {
      Reload();
      ImGui::EndPopup();
      return;
    }

    ImGui::Separator();

    // Pull the pre-update saves (the "None" overlay) into the selected update.
    // NO_TU is left intact so it can be imported into several updates.
    const bool can_import = manager && !selected_id_.empty();
    if (!can_import) {
      ImGui::BeginDisabled();
    }
    if (ImGui::Button("Import Saves from None...")) {
      import_conflicts_ = manager->ImportNoTuContent(
          title_id_, selected_id_, ImportXuid(), /*dry_run=*/true);
      if (import_conflicts_.empty()) {
        manager->ImportNoTuContent(title_id_, selected_id_, ImportXuid(),
                                   /*dry_run=*/false);
        import_status_ = "Saves imported.";
      }
    }
    if (!can_import) {
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::TextDisabled("(select an update first)");
    }

    if (!import_status_.empty()) {
      ImGui::SameLine();
      ImGui::TextUnformatted(import_status_.c_str());
    }

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
