/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_AVATAR_EDITOR_DIALOG_H_
#define XENIA_APP_AVATAR_EDITOR_DIALOG_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "xenia/kernel/xna/xna_avatar_format.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"

namespace xe {
namespace app {

class EmulatorWindow;

class AvatarEditorDialog final : public ui::ImGuiDialog {
 public:
  AvatarEditorDialog(ui::ImGuiDrawer* imgui_drawer,
                     EmulatorWindow* emulator_window);
  ~AvatarEditorDialog() override;

  void set_closed_callback(std::function<void()> callback) {
    closed_callback_ = std::move(callback);
  }

  using ui::ImGuiDialog::Close;

  uint32_t saved_user_mask() const { return saved_user_mask_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct ProfileChoice {
    uint64_t xuid = 0;
    uint8_t slot = 0;
    std::string name;
  };

  void RefreshProfiles();
  void SelectProfile(int index);
  void RebuildPreview();
  void DrawEditor();
  void DrawSlot(uint32_t slot);
  void DrawColors();
  bool ColorEdit(const char* label, uint32_t* argb);
  void SetBody(uint8_t body);
  void Changed();
  bool SaveCurrent();
  void RequestExit();
  void DrawConfirm();

  EmulatorWindow* emulator_window_;
  kernel::xna::avatar::Catalog* catalog_ = nullptr;
  std::vector<ProfileChoice> profiles_;
  int selected_profile_ = -1;
  kernel::xna::avatar::Description description_;
  bool dirty_ = true;
  bool unsaved_ = false;
  std::string status_;
  std::unique_ptr<ui::ImmediateTexture> preview_;
  float yaw_ = 0.0f;
  int pose_ = 0;
  std::mt19937 rng_{std::random_device{}()};
  std::function<void()> closed_callback_;
  uint32_t saved_user_mask_ = 0;
  bool confirm_requested_ = false;
  bool pending_close_ = false;
  bool back_blocked_ = true;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_AVATAR_EDITOR_DIALOG_H_
