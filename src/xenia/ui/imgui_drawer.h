/**
 *******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 *******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 *******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_DRAWER_H_
#define XENIA_UI_IMGUI_DRAWER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "third_party/imgui/imgui.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/ui_focus_manager.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"

struct ImDrawData;
struct ImGuiContext;
struct ImGuiIO;
enum ImGuiKey : int;

namespace xe {
namespace ui {

class ImGuiDialog;
class ImGuiNotification;
class KeyboardDialog;
class Window;

using IconsData = std::map<uint32_t, std::span<const uint8_t>>;

constexpr ImVec2 default_image_icon_size = ImVec2(64.f, 64.f);

class ImGuiDrawer : public WindowInputListener, public UIDrawer {
 public:
  ImGuiDrawer(Window* window, size_t z_order);
  ~ImGuiDrawer();

  ImGuiIO& GetIO();

  void AddDialog(ImGuiDialog* dialog);
  void RemoveDialog(ImGuiDialog* dialog);

  void AddNotification(ImGuiNotification* notification);
  void RemoveNotification(ImGuiNotification* notification);

  // SetPresenter may be called from the destructor.
  void SetPresenter(Presenter* new_presenter);
  void SetImmediateDrawer(ImmediateDrawer* new_immediate_drawer);
  void SetPresenterAndImmediateDrawer(Presenter* new_presenter,
                                      ImmediateDrawer* new_immediate_drawer) {
    SetPresenter(new_presenter);
    SetImmediateDrawer(new_immediate_drawer);
  }

  void Draw(UIDrawContext& ui_draw_context) override;

  void ClearDialogs();
  void EnableNotifications(bool enable) { are_notifications_enabled_ = enable; }

  std::unique_ptr<ImmediateTexture> LoadImGuiIcon(
      std::span<const uint8_t> data);
  std::map<uint32_t, std::unique_ptr<ImmediateTexture>> LoadIcons(
      IconsData data);

  ImmediateTexture* GetNotificationIcon(uint8_t user_index) {
    if (user_index >= notification_icon_textures_.size()) {
      user_index = 0;
    }
    return notification_icon_textures_.at(user_index).get();
  }

  ImmediateTexture* GetLockedAchievementIcon() {
    return locked_achievement_icon_.get();
  }

  ImFont* GetTitleFont() {
    if (!GetIO().Fonts->Fonts[1]->IsLoaded()) {
      return GetIO().Fonts->Fonts[0];
    }
    return GetIO().Fonts->Fonts[1];
  }

  // Controller navigation support
  bool IsControllerNavigationEnabled() const {
    return controller_navigation_enabled_;
  }
  void SetControllerNavigationEnabled(bool enabled) {
    controller_navigation_enabled_ = enabled;
  }

  // Get the UI Focus Manager for dialog navigation
  UIFocusManager* GetFocusManager() { return &focus_manager_; }

  // Show on-screen keyboard for text input fields when using controller
  void CheckAndShowKeyboardForTextInput();

 protected:
  void OnKeyDown(KeyEvent& e) override;
  void OnKeyUp(KeyEvent& e) override;
  void OnKeyChar(KeyEvent& e) override;
  void OnMouseDown(MouseEvent& e) override;
  void OnMouseMove(MouseEvent& e) override;
  void OnMouseUp(MouseEvent& e) override;
  void OnMouseWheel(MouseEvent& e) override;
  void OnTouchEvent(TouchEvent& e) override;
  // For now, no need for OnDpiChanged because redrawing is done continuously.

 private:
  void Initialize();
  void InitializeFonts(const float font_size);
  bool LoadCustomFont(ImGuiIO& io, ImFontConfig& font_config, float font_size);
  bool LoadWindowsFont(ImGuiIO& io, ImFontConfig& font_config, float font_size);
  bool LoadJapaneseFont(ImGuiIO& io, float font_size);

  void SetupNotificationTextures();
  void SetupFontTexture();

  void RenderDrawLists(ImDrawData* data, UIDrawContext& ui_draw_context);

  void ClearInput();
  void OnKey(KeyEvent& e, bool is_down);
  void UpdateMousePosition(float x, float y);
  void SwitchToPhysicalMouseAndUpdateMousePosition(const MouseEvent& e);

  bool IsDrawingDialogs() const { return dialog_loop_next_index_ != SIZE_MAX; }
  void DetachIfLastWindowRemoved();

  std::optional<ImGuiKey> VirtualKeyToImGuiKey(VirtualKey vkey);

  // Controller support
  bool IsControllerKey(VirtualKey vkey) const;
  void PollXInput();

  Window* window_;
  size_t z_order_;

  ImGuiContext* internal_state_ = nullptr;

  // All currently-attached dialogs that get drawn.
  std::vector<ImGuiDialog*> dialogs_;

  // All queued notifications. Notification at index 0 is currently presented
  // one.
  std::vector<ImGuiNotification*> notifications_;
  // Using an index, not an iterator, because after the erasure, the adjustment
  // must be done for the vector element indices that would be in the iterator
  // range that would be invalidated.
  // SIZE_MAX if not currently in the dialog loop.
  size_t dialog_loop_next_index_ = SIZE_MAX;

  Presenter* presenter_ = nullptr;

  ImmediateDrawer* immediate_drawer_ = nullptr;
  // Resources specific to an immediate drawer - must be destroyed before
  // detaching the presenter.
  std::unique_ptr<ImmediateTexture> font_texture_;
  std::unique_ptr<ImmediateTexture> locked_achievement_icon_;

  std::vector<std::unique_ptr<ImmediateTexture>> notification_icon_textures_;

  // If there's an active pointer, the ImGui mouse is controlled by this touch.
  // If it's TouchEvent::kPointerIDNone, the ImGui mouse is controlled by the
  // mouse.
  uint32_t touch_pointer_id_ = TouchEvent::kPointerIDNone;
  // Whether after the next frame (since the mouse up event needs to be handled
  // with the correct mouse position still), the ImGui mouse position should be
  // reset (for instance, after releasing a touch), so it's not hovering over
  // anything.
  bool reset_mouse_position_after_next_frame_ = false;

  double frame_time_tick_frequency_;
  uint64_t last_frame_time_ticks_;

  bool are_notifications_enabled_ = true;

  // Controller navigation state
  bool controller_navigation_enabled_ = false;

  // UI Focus Manager for gamepad dialog navigation
  UIFocusManager focus_manager_;

  // Active on-screen keyboard dialog (if any)
  KeyboardDialog* active_keyboard_dialog_ = nullptr;

  // Guide button handling
  bool guide_button_pressed_ = false;
  uint64_t guide_button_press_time_ = 0;
  static constexpr uint64_t kGuideLongPressMs = 500;  // 500ms for long press

  // Callbacks for guide button
  std::function<void(uint32_t)> on_guide_short_press_;  // user_index
  std::function<void(uint32_t)> on_guide_long_press_;   // user_index

  // Gamepad button states - tracked internally, not sent to ImGui
  bool gamepad_a_pressed_ = false;
  bool gamepad_b_pressed_ = false;
  bool gamepad_back_pressed_ = false;
  bool gamepad_start_pressed_ = false;
  bool gamepad_buttons_were_pressed_ = false;

  // Previous frame button states for release detection
  bool gamepad_a_was_pressed_ = false;
  bool gamepad_b_was_pressed_ = false;
  bool gamepad_back_was_pressed_ = false;

  // "Just released" flags - set for one frame when button released
  bool gamepad_a_just_released_ = false;
  bool gamepad_b_just_released_ = false;
  bool gamepad_back_just_released_ = false;

  // Close request - set when Back/B pressed, cleared when all buttons released
  bool gamepad_close_requested_ = false;

  // Gamepad button release callback system
  std::function<void()> pending_gamepad_callback_;

 public:
  void SetGuideButtonCallbacks(std::function<void(uint32_t)> short_press,
                               std::function<void(uint32_t)> long_press) {
    on_guide_short_press_ = short_press;
    on_guide_long_press_ = long_press;
  }

  // Query current button states (for dialogs to check)
  bool IsGamepadAPressed() const { return gamepad_a_pressed_; }
  bool IsGamepadBPressed() const { return gamepad_b_pressed_; }
  bool IsGamepadBackPressed() const { return gamepad_back_pressed_; }
  bool IsGamepadStartPressed() const { return gamepad_start_pressed_; }
  bool IsAnyGamepadActionPressed() const {
    return gamepad_a_pressed_ || gamepad_b_pressed_ || gamepad_back_pressed_ ||
           gamepad_start_pressed_;
  }

  // Check if button was just released this frame
  bool WasGamepadAJustReleased() const { return gamepad_a_just_released_; }
  bool WasGamepadBJustReleased() const { return gamepad_b_just_released_; }
  bool WasGamepadBackJustReleased() const {
    return gamepad_back_just_released_;
  }

  // Helper: Check if a button should be "clicked" via gamepad
  // Call this AFTER drawing a button with ImGui::Button()
  // Returns true if A was just released AND the button is focused
  bool GamepadButtonActivated() const {
    return gamepad_a_just_released_ && ImGui::IsItemFocused();
  }

  // Request close - call when Back/B pressed
  void RequestGamepadClose() { gamepad_close_requested_ = true; }

  // Check if close requested AND all buttons released (safe to close)
  bool ShouldCloseFromGamepad() {
    if (gamepad_close_requested_ && !IsAnyGamepadActionPressed()) {
      gamepad_close_requested_ = false;
      return true;
    }
    return false;
  }

  // Check if close is pending (buttons still held)
  bool IsGamepadClosePending() const { return gamepad_close_requested_; }

  // Queue a callback to be fired when all gamepad buttons are released
  void QueueGamepadCallback(std::function<void()> callback) {
    pending_gamepad_callback_ = callback;
  }

  // Check if a gamepad callback is pending
  bool HasPendingGamepadCallback() const {
    return pending_gamepad_callback_ != nullptr;
  }
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IMGUI_DRAWER_H_
