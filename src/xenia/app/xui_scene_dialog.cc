/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/xui_scene_dialog.h"

#include <utility>

#include "third_party/imgui/imgui.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/xam/xui_overlay.h"

#if XE_PLATFORM_WIN32
#include <Xinput.h>
#pragma comment(lib, "xinput.lib")
#endif

namespace xe {
namespace app {

namespace xui = kernel::xam::xui;

namespace {

// The dashboard scenes that stand on their own. Each is a .xur in a package,
// beside the Lua module that drives it.
const XuiSceneDialog::Candidate* Candidates(size_t* count);

}  // namespace

XuiSceneDialog::XuiSceneDialog(xe::ui::ImGuiDrawer* drawer,
                               EmulatorWindow* window)
    : xe::ui::ImGuiDialog(drawer), window_(window) {}

XuiSceneDialog::~XuiSceneDialog() {
  CloseOverlay();
  if (closed_callback_) {
    closed_callback_();
  }
}

void XuiSceneDialog::CloseOverlay() {
  if (!screen_) {
    return;
  }
  if (auto* overlay = xui::SharedOverlay()) {
    overlay->Remove(screen_.get());
  }
  screen_.reset();
}

void XuiSceneDialog::Open(const Candidate& candidate) {
  CloseOverlay();
  status_.clear();

  auto* overlay = xui::SharedOverlay();
  if (!overlay) {
    status_ = "no XUI overlay (dashboard assets not installed)";
  } else if (!overlay->ready()) {
    status_ = "XUI overlay has no assets (skin.xur or a font missing)";
  } else if (!overlay->lua()) {
    status_ = "no Lua state for this overlay";
  } else {
    auto screen = std::make_unique<xui::SceneScreen>(
        candidate.package, candidate.scene,
        candidate.script ? candidate.script : std::string());
    if (overlay->Push(screen.get())) {
      screen_ = std::move(screen);
      return;
    }
    status_ = std::string(candidate.scene) + " is not in " + candidate.package;
  }
  XELOGW("xui scene: {}", status_);
}

void XuiSceneDialog::PumpInput() {
  if (!screen_) {
    return;
  }
  using Button = kernel::xam::xui::LuaHost::Button;

#if XE_PLATFORM_WIN32
  uint16_t buttons = 0;
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    XINPUT_STATE state;
    if (XInputGetState(i, &state) == ERROR_SUCCESS) {
      buttons = state.Gamepad.wButtons;
      break;
    }
  }
  const uint16_t was = previous_buttons_;
  previous_buttons_ = buttons;
  auto pressed = [&](uint16_t mask) {
    return (buttons & mask) && !(was & mask);
  };
  if (pressed(XINPUT_GAMEPAD_DPAD_UP)) {
    screen_->HandleInput(Button::kUp);
  }
  if (pressed(XINPUT_GAMEPAD_DPAD_DOWN)) {
    screen_->HandleInput(Button::kDown);
  }
  if (pressed(XINPUT_GAMEPAD_DPAD_LEFT)) {
    screen_->HandleInput(Button::kLeft);
  }
  if (pressed(XINPUT_GAMEPAD_DPAD_RIGHT)) {
    screen_->HandleInput(Button::kRight);
  }
  if (pressed(XINPUT_GAMEPAD_A)) {
    screen_->HandleInput(Button::kAccept);
  }
  if (pressed(XINPUT_GAMEPAD_B)) {
    screen_->HandleInput(Button::kCancel);
  }
#endif

  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
    screen_->HandleInput(Button::kUp);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
    screen_->HandleInput(Button::kDown);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
    screen_->HandleInput(Button::kLeft);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
    screen_->HandleInput(Button::kRight);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
    screen_->HandleInput(Button::kAccept);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
    screen_->HandleInput(Button::kCancel);
  }
}

void XuiSceneDialog::OnDraw(ImGuiIO& io) {
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Dashboard Scene", &open)) {
    ImGui::End();
    if (!open) {
      Close();
    }
    return;
  }

  size_t count = 0;
  const Candidate* candidates = Candidates(&count);
  for (size_t i = 0; i < count; ++i) {
    if (ImGui::RadioButton(candidates[i].label, chosen_ == int(i))) {
      chosen_ = int(i);
    }
  }
  if (ImGui::Button(screen_ ? "Reopen" : "Open")) {
    Open(candidates[chosen_]);
  }
  ImGui::SameLine();
  if (ImGui::Button("Close scene")) {
    CloseOverlay();
  }

  if (!status_.empty()) {
    ImGui::TextWrapped("%s", status_.c_str());
  } else if (screen_) {
    ImGui::TextUnformatted("Arrows or the d-pad move, Enter or A presses.");
  }

  PumpInput();

  ImGui::End();
  if (!open) {
    Close();
  }
}

namespace {

const XuiSceneDialog::Candidate* Candidates(size_t* count) {
  // Package and scene names verified against the 17559 dash.xex resource
  // table, not guessed: the hub scenes are in the dashboard's own controlp,
  // the profile view is in socxzp, and their scripts are in soclua and
  // hubapp.
  static const XuiSceneDialog::Candidate kCandidates[] = {
      {"Hub (dashboard home)", "dashcontrolpack", "HubSceneStandard.xur",
       "hubmain"},
      {"Channel list", "dashcontrolpack", "ChannelListScene.xur",
       "ChannelList"},
      {"Me / profile view", "dashsocial", "meviewscene.xur", "MeViewScene"},
      {"Gamer root", "dashgamer", "GamerRootScene.xur", nullptr},
      {"Aura (background only)", "dashcontrolpack", "AuraScene.xur", nullptr},
  };
  *count = sizeof(kCandidates) / sizeof(kCandidates[0]);
  return kCandidates;
}

}  // namespace

}  // namespace app
}  // namespace xe
