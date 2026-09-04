/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/title_update_dialog.h"

#include <chrono>
#include <cstring>
#include <thread>

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

    DrawDownloadSection();

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

void TitleUpdateDialog::StartCatalogueQuery() {
  catalogue_queried_ = true;

  // Prefer the media id recorded in recent.toml the last time this game ran.
  if (media_id_.empty()) {
    media_id_ = emulator_window_->GetRecentMediaId(launch_path_);
  }

  // Never launched by this build, so nothing was recorded: read it from the
  // game on disk. This selector runs BEFORE the title is loaded, so there is
  // no module to ask.
  if (media_id_.empty()) {
    media_id_ = kernel::util::TitleUpdateDownloader::ReadMediaId(launch_path_);
  }

  if (media_id_.empty()) {
    return;
  }

  catalogue_loading_ = true;

  const uint32_t title_id = title_id_;
  const std::string media_id = media_id_;

  catalogue_query_ = std::async(std::launch::async, [title_id, media_id]() {
    return kernel::util::TitleUpdateDownloader::List(title_id, media_id);
  });
}

void TitleUpdateDialog::StartDownload(
    const kernel::util::RemoteTitleUpdate& update) {
  download_received_ = 0;
  download_total_ = 0;
  download_cancel_ = false;
  download_active_ = true;
  download_finished_ = false;
  download_succeeded_ = false;
  downloading_version_ = update.version;
  download_status_.clear();

  download_path_ = std::filesystem::temp_directory_path() /
                   fmt::format("nexia_tu_{:08X}_{}.bin", title_id_, update.id);

  const std::filesystem::path dest = download_path_;

  // Off the UI thread: these packages run to tens of megabytes.
  std::thread worker([this, update, dest]() {
    const bool ok = kernel::util::TitleUpdateDownloader::Download(
        update, dest,
        [this](uint64_t received, uint64_t total) {
          download_received_ = received;
          download_total_ = total;
        },
        &download_cancel_);

    download_succeeded_ = ok;
    download_finished_ = true;
    download_active_ = false;
  });

  worker.detach();
}

void TitleUpdateDialog::DrawDownloadSection() {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::TextUnformatted("Download Title Updates");

  if (!catalogue_queried_) {
    StartCatalogueQuery();
  }

  if (media_id_.empty()) {
    ImGui::TextDisabled("Media ID could not be read from this game.");
    return;
  }

  if (catalogue_loading_ && catalogue_query_.valid() &&
      catalogue_query_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    catalogue_ = catalogue_query_.get();
    catalogue_loading_ = false;
  }

  // An install can only be started from the UI thread, so the handoff to the
  // installer happens here rather than on the download worker.
  if (download_finished_.exchange(false)) {
    if (download_succeeded_) {
      download_status_ = "Installing...";
      emulator_window_->InstallContentPackages({download_path_});
      Reload();
      download_status_ = fmt::format("Installed TU {}", downloading_version_);
    } else {
      download_status_ = download_cancel_ ? "Cancelled" : "Download failed";
    }
  }

  if (catalogue_loading_) {
    ImGui::TextDisabled("Checking for updates...");
    return;
  }

  if (catalogue_.empty()) {
    ImGui::TextDisabled("No updates published for this game (media %s).",
                        media_id_.c_str());
    return;
  }

  if (download_active_) {
    const uint64_t received = download_received_.load();
    const uint64_t total = download_total_.load();

    const float fraction =
        total ? static_cast<float>(static_cast<double>(received) /
                                   static_cast<double>(total))
              : 0.0f;

    ImGui::ProgressBar(fraction, ImVec2(480.0f, 0),
                       fmt::format("TU {} - {:.1f} / {:.1f} MiB",
                                   downloading_version_,
                                   received / 1048576.0, total / 1048576.0)
                           .c_str());

    if (ImGui::Button("Cancel Download")) {
      download_cancel_ = true;
    }

    return;
  }

  if (!download_status_.empty()) {
    ImGui::TextUnformatted(download_status_.c_str());
  }

  ImGui::BeginChild("##tu_download_list", ImVec2(480.0f, 140.0f), true);

  for (const auto& update : catalogue_) {
    ImGui::PushID(static_cast<int>(update.id));

    // Size is published in KiB.
    ImGui::Text("TU %s      %.1f MiB      %s", update.version.c_str(),
                update.listed_size / 1024.0,
                update.upload_date.substr(0, 10).c_str());

    ImGui::SameLine(360.0f);

    if (ImGui::Button("Download")) {
      StartDownload(update);
    }

    ImGui::PopID();
  }

  ImGui::EndChild();
}

}  // namespace app
}  // namespace xe
