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
                                      InputType type,
                                      InputCallback callback);

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
  std::function<void()> close_callback_ = nullptr;  // For XAM dispatch

  bool is_shifted_ = false;
  bool is_symbol_mode_ = false;
  bool is_caps_lock_ = false;
  bool cancelled_ = false;  // Track if dialog was cancelled
  
  // Controller support
  bool controller_navigation_enabled_ = true;
  
  // Navigation state
  bool has_opened_ = false;
  bool needs_focus_ = true;  // Set focus to first key on open
  
  // For Cancel/Done buttons: only activate on release to prevent input bleed
  std::string pending_close_action_;  // "Done" or "Cancel" when button pressed
  bool a_was_pressed_ = false;        // Track A button state for release detection
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_KEYBOARD_UI_H_
