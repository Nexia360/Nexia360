/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/avatar_editor_dialog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xna/xna_avatar.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

namespace avatar = kernel::xna::avatar;

namespace {

struct PosePreset {
  const char* name;
  int32_t clip;
  float seconds;
};

constexpr PosePreset kPoses[] = {
    {"Standing", 3, 0.0f}, {"Wave", 7, 1.2f},      {"Clap", 6, 0.8f},
    {"Celebrate", 8, 1.0f}, {"Bind pose", -1, 0.0f},
};

constexpr uint32_t kPreviewWidth = 300;
constexpr uint32_t kPreviewHeight = 450;
constexpr float kComboWidth = 250.0f;

constexpr uint32_t kFaceColors[] = {
    avatar::kColorSkin,       avatar::kColorHair,    avatar::kColorEyebrow,
    avatar::kColorIris,       avatar::kColorLips,    avatar::kColorFacialHair,
    avatar::kColorEyeShadow,
};

}  // namespace

AvatarEditorDialog::AvatarEditorDialog(ui::ImGuiDrawer* imgui_drawer,
                                       EmulatorWindow* emulator_window)
    : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {
  catalog_ = kernel::xna::XnaAvatarCatalog();
  RefreshProfiles();
  if (!profiles_.empty()) {
    SelectProfile(0);
  } else if (catalog_) {
    description_ = avatar::RandomDescription(*catalog_, rng_, -1);
  }
}

AvatarEditorDialog::~AvatarEditorDialog() {
  if (closed_callback_) {
    closed_callback_();
  }
}

void AvatarEditorDialog::RefreshProfiles() {
  profiles_.clear();
  auto* state = kernel::kernel_state();
  if (!state || !state->xam_state()) {
    return;
  }
  auto* manager = state->xam_state()->profile_manager();
  if (!manager) {
    return;
  }
  for (uint8_t slot = 0; slot < 4; ++slot) {
    auto* profile = manager->GetProfile(slot);
    if (profile) {
      profiles_.push_back({profile->xuid(), slot, profile->name()});
    }
  }
}

void AvatarEditorDialog::SelectProfile(int index) {
  if (index < 0 || index >= int(profiles_.size())) {
    return;
  }
  selected_profile_ = index;
  const ProfileChoice& choice = profiles_[index];
  avatar::Description loaded;
  if (kernel::xna::XnaAvatarLoadProfile(choice.xuid, &loaded)) {
    description_ = loaded;
    status_ = "Loaded the saved avatar.";
  } else {
    const auto bytes = kernel::xna::XnaAvatarDescriptionForGamer(choice.slot);
    avatar::ParseDescription(bytes.data(), bytes.size(), &description_);
    status_ = "No saved avatar yet - this is the one titles are given.";
  }
  unsaved_ = false;
  dirty_ = true;
}

void AvatarEditorDialog::Changed() {
  dirty_ = true;
  unsaved_ = true;
}

void AvatarEditorDialog::SetBody(uint8_t body) {
  if (description_.body == body) {
    return;
  }
  description_.body = body;
  const uint32_t bit = body ? 1u : 2u;
  for (uint32_t slot = 0; slot < avatar::kSlotCount; ++slot) {
    const uint16_t item = description_.items[slot];
    if (item == avatar::kNoItem) {
      continue;
    }
    const avatar::Entry* entry = catalog_->Find(item);
    if (!entry || !(entry->BodyMask() & bit)) {
      description_.items[slot] = avatar::kNoItem;
    }
  }
  Changed();
}

void AvatarEditorDialog::RebuildPreview() {
  dirty_ = false;
  if (!catalog_) {
    preview_.reset();
    return;
  }
  const avatar::Scene scene = avatar::BuildScene(*catalog_, description_);
  avatar::Matrix local[avatar::kMaxJoints];
  avatar::BindPose(avatar::MainSkeleton(), local);
  const PosePreset& pose = kPoses[pose_];
  if (pose.clip >= 0) {
    auto clip = catalog_->LoadClip(uint32_t(pose.clip));
    if (clip) {
      avatar::SamplePose(*clip, avatar::MainSkeleton(), pose.seconds, local);
    }
  }
  std::vector<uint8_t> rgba;
  avatar::RenderPreview(scene, local, avatar::Expression(), kPreviewWidth,
                        kPreviewHeight, yaw_, &rgba);
  const std::vector<uint8_t> png =
      avatar::EncodePng(kPreviewWidth, kPreviewHeight, rgba);
  preview_ = imgui_drawer()->LoadImGuiIcon(png);
}

bool AvatarEditorDialog::ColorEdit(const char* label, uint32_t* argb) {
  float color[3] = {float((*argb >> 16) & 0xFF) / 255.0f,
                    float((*argb >> 8) & 0xFF) / 255.0f,
                    float(*argb & 0xFF) / 255.0f};
  if (!ImGui::ColorEdit3(label, color, ImGuiColorEditFlags_NoInputs)) {
    return false;
  }
  const auto channel = [](float value) {
    return uint32_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
  };
  *argb = 0xFF000000u | (channel(color[0]) << 16) | (channel(color[1]) << 8) |
          channel(color[2]);
  return true;
}

void AvatarEditorDialog::DrawSlot(uint32_t slot) {
  const uint16_t current = description_.items[slot];
  const avatar::Entry* entry =
      current == avatar::kNoItem ? nullptr : catalog_->Find(current);
  const std::string shown =
      entry ? (entry->name.empty() ? std::string("(unnamed)") : entry->name)
            : std::string("None");
  ImGui::PushID(int(slot));
  ImGui::SetNextItemWidth(kComboWidth);
  if (ImGui::BeginCombo(avatar::SlotName(slot), shown.c_str())) {
    if (ImGui::Selectable("None", current == avatar::kNoItem)) {
      avatar::PlaceItem(*catalog_, &description_, slot, avatar::kNoItem);
      Changed();
    }
    for (uint32_t item :
         avatar::ItemsForSlot(*catalog_, slot, description_.body)) {
      const avatar::Entry* candidate = catalog_->Find(item);
      const bool selected = item == current;
      ImGui::PushID(int(item));
      if (ImGui::Selectable(candidate->name.empty() ? "(unnamed)"
                                                    : candidate->name.c_str(),
                            selected)) {
        avatar::PlaceItem(*catalog_, &description_, slot, uint16_t(item));
        Changed();
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  if (slot < avatar::kClothingSlotCount && slot != avatar::kSlotHair &&
      description_.items[slot] != avatar::kNoItem) {
    for (int k = 0; k < 3; ++k) {
      ImGui::SameLine();
      char label[16];
      std::snprintf(label, sizeof(label), "##custom%d", k);
      if (ColorEdit(label, &description_.custom[slot][k])) {
        Changed();
      }
    }
  }
  ImGui::PopID();
}

void AvatarEditorDialog::DrawColors() {
  for (uint32_t color : kFaceColors) {
    if (ColorEdit(avatar::ColorName(color), &description_.colors[color])) {
      Changed();
    }
  }
}

void AvatarEditorDialog::DrawEditor() {
  ImGui::BeginChild("##preview", ImVec2(float(kPreviewWidth) + 16.0f, 0.0f),
                    true);
  if (preview_) {
    ImGui::Image(reinterpret_cast<ImTextureID>(preview_.get()),
                 ImVec2(float(kPreviewWidth), float(kPreviewHeight)));
  }
  ImGui::SetNextItemWidth(float(kPreviewWidth));
  if (ImGui::SliderAngle("##yaw", &yaw_, -180.0f, 180.0f)) {
    dirty_ = true;
  }
  ImGui::SetNextItemWidth(float(kPreviewWidth) * 0.6f);
  if (ImGui::BeginCombo("Pose", kPoses[pose_].name)) {
    for (int i = 0; i < int(sizeof(kPoses) / sizeof(kPoses[0])); ++i) {
      if (ImGui::Selectable(kPoses[i].name, i == pose_)) {
        pose_ = i;
        dirty_ = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##controls", ImVec2(0.0f, 0.0f), false);
  if (profiles_.empty()) {
    ImGui::TextWrapped(
        "No profile is signed in. The avatar can be edited, but signing in "
        "a profile is what lets it be saved.");
  } else {
    ImGui::SetNextItemWidth(kComboWidth);
    if (ImGui::BeginCombo("Profile",
                          profiles_[selected_profile_].name.c_str())) {
      for (int i = 0; i < int(profiles_.size()); ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable(profiles_[i].name.c_str(),
                              i == selected_profile_)) {
          SelectProfile(i);
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
  }
  if (ImGui::RadioButton("Male", description_.body == 1)) {
    SetBody(1);
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Female", description_.body == 0)) {
    SetBody(0);
  }
  int height = description_.height;
  ImGui::SetNextItemWidth(kComboWidth);
  if (ImGui::SliderInt("##height", &height, 0, 255, "")) {
    description_.height = uint8_t(height);
    unsaved_ = true;
  }
  ImGui::SameLine();
  ImGui::Text("Height %.2f m", avatar::DescriptionHeight(description_));
  int weight = description_.weight;
  ImGui::SetNextItemWidth(kComboWidth);
  if (ImGui::SliderInt("Weight", &weight, 0, 255)) {
    description_.weight = uint8_t(weight);
    unsaved_ = true;
  }
  ImGui::SeparatorText("Clothing");
  for (uint32_t slot = 0; slot < avatar::kClothingSlotCount; ++slot) {
    DrawSlot(slot);
  }
  ImGui::SeparatorText("Face");
  for (uint32_t slot = avatar::kClothingSlotCount; slot < avatar::kSlotCount;
       ++slot) {
    DrawSlot(slot);
  }
  ImGui::SeparatorText("Colours");
  DrawColors();
  ImGui::Separator();
  if (ImGui::Button("Randomize")) {
    description_ =
        avatar::RandomDescription(*catalog_, rng_, int32_t(description_.body));
    Changed();
  }
  ImGui::SameLine();
  if (ImGui::Button("Random body")) {
    description_ = avatar::RandomDescription(*catalog_, rng_, -1);
    Changed();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(selected_profile_ < 0);
  if (ImGui::Button("Revert")) {
    SelectProfile(selected_profile_);
  }
  ImGui::SameLine();
  if (ImGui::Button(unsaved_ ? "Save *" : "Save")) {
    if (kernel::xna::XnaAvatarSaveProfile(profiles_[selected_profile_].xuid,
                                          description_)) {
      unsaved_ = false;
      status_ = "Saved. Titles started from now on use this avatar.";
      kernel::kernel_state()->BroadcastNotification(
          kXNotificationSystemAvatarChanged,
          1u << profiles_[selected_profile_].slot);
    } else {
      status_ = "The avatar could not be saved - see the log.";
    }
  }
  ImGui::EndDisabled();
  if (!status_.empty()) {
    ImGui::TextWrapped("%s", status_.c_str());
  }
  ImGui::EndChild();
}

void AvatarEditorDialog::OnDraw(ImGuiIO& io) {
  if (dirty_ && !ImGui::IsAnyItemActive()) {
    RebuildPreview();
  }
  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
      ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(880.0f, 640.0f), ImGuiCond_FirstUseEver);
  bool open = true;
  if (ImGui::Begin("Avatar Editor##nexia_avatar_editor", &open,
                   ImGuiWindowFlags_NoCollapse)) {
    if (!catalog_) {
      ImGui::TextWrapped(
          "The avatar assets are not installed. Install the Xbox 360 Avatar "
          "update (the .zip holding $SystemUpdate) with File > Install "
          "Content..., then press Retry.");
      if (ImGui::Button("Retry")) {
        catalog_ = kernel::xna::XnaAvatarCatalog();
        if (catalog_ && profiles_.empty()) {
          description_ = avatar::RandomDescription(*catalog_, rng_, -1);
        } else if (catalog_) {
          SelectProfile(selected_profile_);
        }
        dirty_ = true;
      }
    } else {
      DrawEditor();
    }
  }
  ImGui::End();
  if (!open) {
    Close();
  }
}

}  // namespace app
}  // namespace xe
