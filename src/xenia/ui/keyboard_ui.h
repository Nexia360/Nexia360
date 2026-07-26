/**
 *******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 *******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 *******************************************************************************
 */

#ifndef XENIA_UI_KEYBOARD_UI_H_
#define XENIA_UI_KEYBOARD_UI_H_

#include <memory>
#include <string>
#include <vector>

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/ui_focus_manager.h"

namespace xe {
namespace ui {

class KeyboardDialog : public ImGuiDialog {
 public:
  enum class InputType {
    kText,
    kNumber,
    kPassword,
  };

  // Callback for when the user confirms input
  using InputCallback = std::function<void(const std::string&)>;

  static KeyboardDialog* ShowKeyboard(ImGuiDrawer* imgui_drawer,
                                      const std::string& title,
                                      const std::string& initial_text,
                                      InputType type, InputCallback callback,
                                      const std::string& focus_parent = "",
                                      const std::string& focus_name = "");

  ~KeyboardDialog();

  // Enable/disable controller navigation
  void SetControllerNavigationEnabled(bool enabled) {
    controller_navigation_enabled_ = enabled;
  }

  // For XAM integration - get result after dialog closes
  bool was_cancelled() const { return cancelled_; }
  const std::string& result_text() const { return input_text_; }

  // For XAM dispatch compatibility
  void set_close_callback(std::function<void()> close_callback) {
    close_callback_ = close_callback;
  }

  // Called BEFORE keyboard closes - use to clear parent's keyboard_has_focus
  void set_pre_close_callback(std::function<void()> pre_close_callback) {
    pre_close_callback_ = pre_close_callback;
  }

  // UIFocusManager integration - set parent and focus name for this dialog
  void set_focus_parent(const std::string& parent) { focus_parent_ = parent; }
  void set_focus_name(const std::string& name) { focus_name_ = name; }

 protected:
  KeyboardDialog(ImGuiDrawer* imgui_drawer, const std::string& title,
                 const std::string& initial_text, InputType type,
                 InputCallback callback);

  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  void DrawKeyboardLayout();
  void DrawTextInput();
  void ProcessKeyInput(const std::string& key);

  std::string title_;
  std::string input_text_;
  InputType input_type_;
  InputCallback callback_;
  std::function<void()> close_callback_ = nullptr;      // For XAM dispatch
  std::function<void()> pre_close_callback_ = nullptr;  // Called BEFORE close

  // UIFocusManager integration
  std::string
      focus_parent_;        // Parent dialog name (empty = no focus management)
  std::string focus_name_;  // This dialog's focus name

  bool is_shifted_ = false;
  bool is_symbol_mode_ = false;
  bool is_number_mode_ = false;  // LB: numbers-only layout
  bool is_caps_lock_ = false;
  bool cancelled_ = false;  // Track if dialog was cancelled

  // Controller support
  bool controller_navigation_enabled_ = true;

  // Navigation state
  bool has_opened_ = false;
  bool needs_focus_ = true;  // Set focus to first key on open
  // Set when the user types on a REAL keyboard. Focus is moved to the Done
  // button so a subsequent Enter/Space/A commits instead of activating
  // whichever on-screen key happened to be focused (spurious input).
  bool focus_done_button_ = false;

  // Input ignore delay on open (500ms)
  uint64_t open_time_ = 0;
  static constexpr uint64_t kInputIgnoreDelayMs = 500;

  // For Cancel/Done buttons: only activate on release to prevent input bleed
  std::string pending_close_action_;  // "Done" or "Cancel" when button pressed

  // A button: track for Done/Cancel release detection
  bool a_was_pressed_ = false;
  std::string a_pending_action_;  // "Done" or "Cancel" deferred until A release

  // Start button = Done (release detection)
  bool start_was_pressed_ = false;

  // B button backspace with repeat
  bool b_was_pressed_ = false;
  uint64_t b_press_start_time_ = 0;
  uint64_t b_last_repeat_time_ = 0;
  static constexpr uint64_t kBRepeatDelayMs =
      500;  // Initial delay before repeat
  static constexpr uint64_t kBRepeatIntervalMs =
      100;  // Repeat interval (faster than 300ms)

  // Back button cancel (wait for release)
  bool back_was_pressed_ = false;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_KEYBOARD_UI_H_
