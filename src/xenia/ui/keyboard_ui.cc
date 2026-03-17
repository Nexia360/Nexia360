/**
 *******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 *******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 *******************************************************************************
 */

#include "xenia/ui/keyboard_ui.h"

#include <algorithm>
#include <cctype>
#include <chrono>

#include "third_party/imgui/imgui.h"
#include "xenia/base/logging.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

#if XE_PLATFORM_WIN32
#include <Xinput.h>
#pragma comment(lib, "xinput.lib")
#endif

namespace xe {
namespace ui {

// Xbox 360 style keyboard layout - 10 keys per row
// Row 0: Numbers
// Row 1-3: Letters QWERTY style
// Row 4: Special keys

// Define keyboard key structure for layout
struct KeyDef {
  const char* label;
  const char* value;  // nullptr means special key
  float width;        // width multiplier (1.0 = standard key)
  ImVec4 color;       // button color
};

// Standard button color
static const ImVec4 kNormalColor = ImVec4(0.15f, 0.56f, 0.11f, 0.60f);
static const ImVec4 kSpecialColor = ImVec4(0.4f, 0.4f, 0.4f, 0.60f);
static const ImVec4 kAccentColor = ImVec4(0.0f, 0.5f, 0.0f, 0.80f);
static const ImVec4 kDangerColor = ImVec4(0.6f, 0.1f, 0.1f, 0.80f);

// Keyboard layouts
static const std::vector<std::vector<KeyDef>> kLowercaseLayout = {
    // Row 0: Numbers
    {{"1", "1", 1.0f, kNormalColor},
     {"2", "2", 1.0f, kNormalColor},
     {"3", "3", 1.0f, kNormalColor},
     {"4", "4", 1.0f, kNormalColor},
     {"5", "5", 1.0f, kNormalColor},
     {"6", "6", 1.0f, kNormalColor},
     {"7", "7", 1.0f, kNormalColor},
     {"8", "8", 1.0f, kNormalColor},
     {"9", "9", 1.0f, kNormalColor},
     {"0", "0", 1.0f, kNormalColor}},
    // Row 1: QWERTY top
    {{"q", "q", 1.0f, kNormalColor},
     {"w", "w", 1.0f, kNormalColor},
     {"e", "e", 1.0f, kNormalColor},
     {"r", "r", 1.0f, kNormalColor},
     {"t", "t", 1.0f, kNormalColor},
     {"y", "y", 1.0f, kNormalColor},
     {"u", "u", 1.0f, kNormalColor},
     {"i", "i", 1.0f, kNormalColor},
     {"o", "o", 1.0f, kNormalColor},
     {"p", "p", 1.0f, kNormalColor}},
    // Row 2: QWERTY middle
    {{"a", "a", 1.0f, kNormalColor},
     {"s", "s", 1.0f, kNormalColor},
     {"d", "d", 1.0f, kNormalColor},
     {"f", "f", 1.0f, kNormalColor},
     {"g", "g", 1.0f, kNormalColor},
     {"h", "h", 1.0f, kNormalColor},
     {"j", "j", 1.0f, kNormalColor},
     {"k", "k", 1.0f, kNormalColor},
     {"l", "l", 1.0f, kNormalColor},
     {"'", "'", 1.0f, kNormalColor}},
    // Row 3: QWERTY bottom + punctuation
    {{"z", "z", 1.0f, kNormalColor},
     {"x", "x", 1.0f, kNormalColor},
     {"c", "c", 1.0f, kNormalColor},
     {"v", "v", 1.0f, kNormalColor},
     {"b", "b", 1.0f, kNormalColor},
     {"n", "n", 1.0f, kNormalColor},
     {"m", "m", 1.0f, kNormalColor},
     {",", ",", 1.0f, kNormalColor},
     {".", ".", 1.0f, kNormalColor},
     {"/", "/", 1.0f, kNormalColor}},
    // Row 4: More punctuation
    {{"-", "-", 1.0f, kNormalColor},
     {"_", "_", 1.0f, kNormalColor},
     {":", ":", 1.0f, kNormalColor},
     {";", ";", 1.0f, kNormalColor},
     {"\"", "\"", 1.0f, kNormalColor},
     {"?", "?", 1.0f, kNormalColor},
     {"!", "!", 1.0f, kNormalColor},
     {"@", "@", 1.0f, kNormalColor},
     {" # ", "#", 1.0f, kNormalColor},
     {"&", "&", 1.0f, kNormalColor}},
    // Row 5: Special keys
    {{"Shift", nullptr, 1.5f, kSpecialColor},
     {"Space", " ", 3.0f, kAccentColor},
     {"<-", nullptr, 1.0f, kDangerColor},
     {"Cancel", nullptr, 1.5f, kDangerColor},
     {"Done", nullptr, 2.0f, kAccentColor}}};

static const std::vector<std::vector<KeyDef>> kUppercaseLayout = {
    // Row 0: Symbols
    {{"!", "!", 1.0f, kNormalColor},
     {"@", "@", 1.0f, kNormalColor},
     {" # ", "#", 1.0f, kNormalColor},
     {"$", "$", 1.0f, kNormalColor},
     {"%", "%", 1.0f, kNormalColor},
     {"^", "^", 1.0f, kNormalColor},
     {"&", "&", 1.0f, kNormalColor},
     {"*", "*", 1.0f, kNormalColor},
     {"(", "(", 1.0f, kNormalColor},
     {")", ")", 1.0f, kNormalColor}},
    // Row 1: QWERTY top uppercase
    {{"Q", "Q", 1.0f, kNormalColor},
     {"W", "W", 1.0f, kNormalColor},
     {"E", "E", 1.0f, kNormalColor},
     {"R", "R", 1.0f, kNormalColor},
     {"T", "T", 1.0f, kNormalColor},
     {"Y", "Y", 1.0f, kNormalColor},
     {"U", "U", 1.0f, kNormalColor},
     {"I", "I", 1.0f, kNormalColor},
     {"O", "O", 1.0f, kNormalColor},
     {"P", "P", 1.0f, kNormalColor}},
    // Row 2: QWERTY middle uppercase
    {{"A", "A", 1.0f, kNormalColor},
     {"S", "S", 1.0f, kNormalColor},
     {"D", "D", 1.0f, kNormalColor},
     {"F", "F", 1.0f, kNormalColor},
     {"G", "G", 1.0f, kNormalColor},
     {"H", "H", 1.0f, kNormalColor},
     {"J", "J", 1.0f, kNormalColor},
     {"K", "K", 1.0f, kNormalColor},
     {"L", "L", 1.0f, kNormalColor},
     {"\"", "\"", 1.0f, kNormalColor}},
    // Row 3: QWERTY bottom uppercase + punctuation
    {{"Z", "Z", 1.0f, kNormalColor},
     {"X", "X", 1.0f, kNormalColor},
     {"C", "C", 1.0f, kNormalColor},
     {"V", "V", 1.0f, kNormalColor},
     {"B", "B", 1.0f, kNormalColor},
     {"N", "N", 1.0f, kNormalColor},
     {"M", "M", 1.0f, kNormalColor},
     {"<", "<", 1.0f, kNormalColor},
     {">", ">", 1.0f, kNormalColor},
     {"?", "?", 1.0f, kNormalColor}},
    // Row 4: More punctuation
    {{"+", "+", 1.0f, kNormalColor},
     {"=", "=", 1.0f, kNormalColor},
     {"{", "{", 1.0f, kNormalColor},
     {"}", "}", 1.0f, kNormalColor},
     {"[", "[", 1.0f, kNormalColor},
     {"]", "]", 1.0f, kNormalColor},
     {"\\", "\\", 1.0f, kNormalColor},
     {"|", "|", 1.0f, kNormalColor},
     {"~", "~", 1.0f, kNormalColor},
     {"`", "`", 1.0f, kNormalColor}},
    // Row 5: Special keys
    {{"Shift", nullptr, 1.5f, kSpecialColor},
     {"Space", " ", 3.0f, kAccentColor},
     {"<-", nullptr, 1.0f, kDangerColor},
     {"Cancel", nullptr, 1.5f, kDangerColor},
     {"Done", nullptr, 2.0f, kAccentColor}}};

static const std::vector<std::vector<KeyDef>> kSymbolLayout = {
    // Row 0: More symbols
    {{"~", "~", 1.0f, kNormalColor},
     {"`", "`", 1.0f, kNormalColor},
     {"|", "|", 1.0f, kNormalColor},
     {"\\", "\\", 1.0f, kNormalColor},
     {"<", "<", 1.0f, kNormalColor},
     {">", ">", 1.0f, kNormalColor},
     {"{", "{", 1.0f, kNormalColor},
     {"}", "}", 1.0f, kNormalColor},
     {"[", "[", 1.0f, kNormalColor},
     {"]", "]", 1.0f, kNormalColor}},
    // Row 1: Punctuation
    {{"!", "!", 1.0f, kNormalColor},
     {"@", "@", 1.0f, kNormalColor},
     {" # ", "#", 1.0f, kNormalColor},
     {"$", "$", 1.0f, kNormalColor},
     {"%", "%", 1.0f, kNormalColor},
     {"^", "^", 1.0f, kNormalColor},
     {"&", "&", 1.0f, kNormalColor},
     {"*", "*", 1.0f, kNormalColor},
     {"(", "(", 1.0f, kNormalColor},
     {")", ")", 1.0f, kNormalColor}},
    // Row 2: More punctuation
    {{"-", "-", 1.0f, kNormalColor},
     {"=", "=", 1.0f, kNormalColor},
     {"+", "+", 1.0f, kNormalColor},
     {"_", "_", 1.0f, kNormalColor},
     {":", ":", 1.0f, kNormalColor},
     {";", ";", 1.0f, kNormalColor},
     {"\"", "\"", 1.0f, kNormalColor},
     {"'", "'", 1.0f, kNormalColor},
     {",", ",", 1.0f, kNormalColor},
     {".", ".", 1.0f, kNormalColor}},
    // Row 3: Extra
    {{"?", "?", 1.0f, kNormalColor},
     {"/", "/", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor}},
    // Row 4: Special keys
    {{"ABC", nullptr, 1.5f, kSpecialColor},
     {"Space", " ", 3.0f, kAccentColor},
     {"<-", nullptr, 1.0f, kDangerColor},
     {"Cancel", nullptr, 1.5f, kDangerColor},
     {"Done", nullptr, 2.0f, kAccentColor}}};

static const std::vector<std::vector<KeyDef>> kNumberLayout = {
    // Number pad style layout
    {{"7", "7", 1.0f, kNormalColor},
     {"8", "8", 1.0f, kNormalColor},
     {"9", "9", 1.0f, kNormalColor}},
    {{"4", "4", 1.0f, kNormalColor},
     {"5", "5", 1.0f, kNormalColor},
     {"6", "6", 1.0f, kNormalColor}},
    {{"1", "1", 1.0f, kNormalColor},
     {"2", "2", 1.0f, kNormalColor},
     {"3", "3", 1.0f, kNormalColor}},
    {{"0", "0", 2.0f, kNormalColor}, {".", ".", 1.0f, kNormalColor}},
    {{"<-", nullptr, 1.0f, kDangerColor},
     {"Cancel", nullptr, 1.0f, kDangerColor},
     {"Done", nullptr, 1.0f, kAccentColor}}};

KeyboardDialog* KeyboardDialog::ShowKeyboard(
    ImGuiDrawer* imgui_drawer, const std::string& title,
    const std::string& initial_text, InputType type, InputCallback callback,
    const std::string& focus_parent, const std::string& focus_name) {
  auto* dialog =
      new KeyboardDialog(imgui_drawer, title, initial_text, type, callback);
  dialog->focus_parent_ = focus_parent;
  dialog->focus_name_ = focus_name;
  return dialog;
}

KeyboardDialog::KeyboardDialog(ImGuiDrawer* imgui_drawer,
                               const std::string& title,
                               const std::string& initial_text, InputType type,
                               InputCallback callback)
    : ImGuiDialog(imgui_drawer),
      title_(title),
      input_text_(initial_text),
      input_type_(type),
      callback_(callback) {
  // Enable controller navigation in the drawer
  if (imgui_drawer) {
    imgui_drawer->SetControllerNavigationEnabled(true);
  }

  // Record open time for input ignore delay
  open_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();
}

KeyboardDialog::~KeyboardDialog() {}

void KeyboardDialog::OnDraw(ImGuiIO& io) {
  // Get current time
  uint64_t current_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  // Check if we're still in the input ignore period
  bool ignore_inputs = (current_time - open_time_) < kInputIgnoreDelayMs;

  // Handle actual keyboard input
  if (!ignore_inputs) {
    // Process text input from keyboard
    if (io.InputQueueCharacters.Size > 0) {
      for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
        ImWchar c = io.InputQueueCharacters[i];
        // Filter valid characters
        if (c >= 32 && c < 127) {  // Printable ASCII
          // For number input, only allow digits and decimal point
          if (input_type_ == InputType::kNumber) {
            if ((c >= '0' && c <= '9') || c == '.') {
              input_text_ += static_cast<char>(c);
            }
          } else {
            input_text_ += static_cast<char>(c);
          }
        }
      }
    }

    // Handle special keys
    // Backspace
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
      if (!input_text_.empty()) {
        input_text_.pop_back();
      }
    }

    // Delete - same as backspace for simplicity
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
      if (!input_text_.empty()) {
        input_text_.pop_back();
      }
    }

    // Enter/Return - confirm input
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
      cancelled_ = false;
      pending_close_action_ = "Done";
    }

    // Escape - cancel input
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      cancelled_ = true;
      input_text_.clear();
      pending_close_action_ = "Cancel";
    }
  }

  // Poll XInput for controller state - needed since keyboard is a modal popup
#if XE_PLATFORM_WIN32
  bool a_pressed = false;
  bool b_pressed = false;
  bool back_pressed = false;
  bool start_pressed = false;

  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    XINPUT_STATE state;
    if (XInputGetState(i, &state) == ERROR_SUCCESS) {
      const auto& pad = state.Gamepad;

      // Track action button states for custom handling
      // D-pad, sticks, LB, RB are already forwarded to ImGui by
      // imgui_drawer — do NOT duplicate those events here.
      a_pressed = (pad.wButtons & XINPUT_GAMEPAD_A) != 0;
      b_pressed = (pad.wButtons & XINPUT_GAMEPAD_B) != 0;
      back_pressed = (pad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
      start_pressed = (pad.wButtons & XINPUT_GAMEPAD_START) != 0;

      // Only use first connected controller
      break;
    }
  }

  // During ignore period, consume inputs by updating state but not acting on
  // them
  if (ignore_inputs) {
    a_was_pressed_ = a_pressed;
    b_was_pressed_ = b_pressed;
    back_was_pressed_ = back_pressed;
    start_was_pressed_ = start_pressed;
  } else {
    // A button: character keys are handled by ImGui button activation.
    // Done/Cancel are deferred to A release (see a_pending_action_ below).
    if (!a_pressed && a_was_pressed_) {
      // A just released - process any pending Done/Cancel action
      if (!a_pending_action_.empty()) {
        ProcessKeyInput(a_pending_action_);
        a_pending_action_.clear();
      }
    }
    a_was_pressed_ = a_pressed;

    // Start button = Done (activate on release)
    if (!start_pressed && start_was_pressed_) {
      cancelled_ = false;
      pending_close_action_ = "Done";
    }
    start_was_pressed_ = start_pressed;

    // B button = backspace with repeat
    if (b_pressed) {
      if (!b_was_pressed_) {
        // Just pressed - do immediate backspace
        if (!input_text_.empty()) {
          input_text_.pop_back();
        }
        b_press_start_time_ = current_time;
        b_last_repeat_time_ = current_time;
      } else {
        // Held - check for repeat
        uint64_t held_time = current_time - b_press_start_time_;
        if (held_time >= kBRepeatDelayMs) {
          uint64_t since_last = current_time - b_last_repeat_time_;
          if (since_last >= kBRepeatIntervalMs) {
            if (!input_text_.empty()) {
              input_text_.pop_back();
            }
            b_last_repeat_time_ = current_time;
          }
        }
      }
    }
    b_was_pressed_ = b_pressed;

    // Back button = cancel (wait for release)
    if (!back_pressed && back_was_pressed_) {
      // Back just released - trigger cancel
      cancelled_ = true;
      pending_close_action_ = "Back";
    }
    back_was_pressed_ = back_pressed;
  }  // end if (!ignore_inputs)
#endif

  if (!has_opened_) {
    // UIFocusManager: Register as child of parent if set
    if (!focus_name_.empty()) {
      auto* focus_manager = imgui_drawer()->GetFocusManager();
      if (focus_manager) {
        if (!focus_parent_.empty()) {
          focus_manager->UIChildFocus(focus_parent_, focus_name_);
        } else {
          focus_manager->UISetFocus(focus_name_);
        }
      }
    }

    ImGui::SetNextWindowSize(ImVec2(700, 450), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::OpenPopup(title_.c_str());
    has_opened_ = true;
  }

  // Enable keyboard/gamepad navigation
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

  ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoCollapse;

  bool popup_open = true;
  bool should_close = false;

  if (ImGui::BeginPopupModal(title_.c_str(), &popup_open, flags)) {
    // Draw input text field
    DrawTextInput();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Draw keyboard layout
    DrawKeyboardLayout();

    // Check if we're waiting to close (Done/Cancel/Back was pressed via
    // gamepad) Wait until buttons are released before actually closing
    if (!pending_close_action_.empty()) {
#if XE_PLATFORM_WIN32
      bool any_pressed = false;
      for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state;
        if (XInputGetState(i, &state) == ERROR_SUCCESS) {
          const auto& pad = state.Gamepad;
          if ((pad.wButtons & XINPUT_GAMEPAD_A) ||
              (pad.wButtons & XINPUT_GAMEPAD_B) ||
              (pad.wButtons & XINPUT_GAMEPAD_BACK) ||
              (pad.wButtons & XINPUT_GAMEPAD_START)) {
            any_pressed = true;
          }
          break;
        }
      }
      if (!any_pressed) {
        pending_close_action_.clear();
        should_close = true;
        ImGui::CloseCurrentPopup();  // Close THIS popup only
      }
#else
      pending_close_action_.clear();
      should_close = true;
      ImGui::CloseCurrentPopup();  // Close THIS popup only
#endif
    }

    ImGui::EndPopup();
  } else if (!popup_open) {
    // X button was clicked - popup already closing
    cancelled_ = true;
    should_close = true;
  }

  if (should_close) {
    Close();
  }
}

void KeyboardDialog::OnClose() {
  // Call pre-close callback FIRST - allows parent to clear keyboard_has_focus
  if (pre_close_callback_) {
    pre_close_callback_();
  }

  // UIFocusManager: Drop focus if we were registered
  if (!focus_name_.empty()) {
    auto* focus_manager = imgui_drawer()->GetFocusManager();
    if (focus_manager) {
      focus_manager->UIDropFocus(focus_name_);
    }
  }

  // If user confirmed (Done), call the input callback
  // Don't call it for Cancel - that's what cancelled_ flag is for
  if (callback_ && !cancelled_) {
    callback_(input_text_);
  }
  // Call XAM close callback if set (always, for cleanup)
  if (close_callback_) {
    close_callback_();
  }
}

void KeyboardDialog::DrawTextInput() {
  // Show the current input with a cursor
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 0.9f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 10));

  std::string display_text = input_text_;
  if (input_type_ == InputType::kPassword) {
    display_text = std::string(input_text_.length(), '*');
  }

  // Add blinking cursor
  static float cursor_timer = 0.0f;
  cursor_timer += ImGui::GetIO().DeltaTime;
  if (fmod(cursor_timer, 1.0f) < 0.5f) {
    display_text += "_";
  }

  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  ImGui::InputText("##display", const_cast<char*>(display_text.c_str()),
                   display_text.size() + 1, ImGuiInputTextFlags_ReadOnly);

  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // Character count
  ImGui::TextDisabled("Characters: %zu", input_text_.length());
}

void KeyboardDialog::DrawKeyboardLayout() {
  // Select layout based on current mode
  const std::vector<std::vector<KeyDef>>* layout = &kLowercaseLayout;

  if (input_type_ == InputType::kNumber) {
    layout = &kNumberLayout;
  } else if (is_symbol_mode_) {
    layout = &kSymbolLayout;
  } else if (is_shifted_ || is_caps_lock_) {
    layout = &kUppercaseLayout;
  }

  const float key_size = 50.0f;
  const float key_spacing = 4.0f;
  const float row_height = key_size + key_spacing;

  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                      ImVec2(key_spacing, key_spacing));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

  int key_index = 0;
  for (size_t row = 0; row < layout->size(); ++row) {
    const auto& row_keys = (*layout)[row];

    // Center the row if it has fewer keys
    float row_width = 0;
    for (const auto& key : row_keys) {
      row_width += key_size * key.width + key_spacing;
    }
    row_width -= key_spacing;  // Remove last spacing

    float available_width = ImGui::GetContentRegionAvail().x;
    float offset = (available_width - row_width) * 0.5f;
    if (offset > 0) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
    }

    for (size_t col = 0; col < row_keys.size(); ++col) {
      const auto& key = row_keys[col];

      if (col > 0) {
        ImGui::SameLine();
      }

      // Skip empty keys
      if (key.label[0] == '\0') {
        ImGui::Dummy(ImVec2(key_size * key.width, key_size));
        continue;
      }

      ImGui::PushStyleColor(ImGuiCol_Button, key.color);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(key.color.x + 0.1f, key.color.y + 0.1f,
                                   key.color.z + 0.1f, key.color.w));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                            ImVec4(key.color.x + 0.2f, key.color.y + 0.2f,
                                   key.color.z + 0.2f, key.color.w));

      // Set focus to first key when dialog opens
      if (needs_focus_ && key_index == 0) {
        ImGui::SetKeyboardFocusHere();
        needs_focus_ = false;
      }

      std::string button_id =
          std::string(key.label) + "##" + std::to_string(key_index);
      bool pressed = ImGui::Button(button_id.c_str(),
                                   ImVec2(key_size * key.width, key_size));

      ImGui::PopStyleColor(3);

      if (pressed) {
        if (key.value) {
          // Regular character key — activate immediately
          ProcessKeyInput(key.value);
        } else {
          std::string key_label = key.label;
          if (key_label == "Done" || key_label == "Cancel") {
            // Done/Cancel: defer until A button is released so the action
            // only commits when the user lifts the button.  Mouse clicks
            // go through immediately since A won't be held.
            bool a_is_held =
                ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
            if (a_is_held) {
              a_pending_action_ = key_label;
            } else {
              // Mouse click or keyboard Enter — process immediately
              ProcessKeyInput(key_label);
            }
          } else {
            // Other special keys (Shift, ABC, etc.) — immediate
            ProcessKeyInput(key_label);
          }
        }
      }

      key_index++;
    }
  }

  ImGui::PopStyleVar(2);

  // Show hint text at bottom
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
  if (input_type_ == InputType::kNumber) {
    ImGui::TextWrapped(
        "Keyboard: Type numbers | Enter: Done | Esc: Cancel | D-Pad: Navigate "
        "| A: Select | B: Backspace | Start: Done");
  } else {
    ImGui::TextWrapped(
        "Keyboard: Type directly | Enter: Done | Esc: Cancel | "
        "D-Pad: Navigate | A: Select | B: Backspace | Start: Done | LB: "
        "Symbols | RB: Shift");
  }
  ImGui::PopStyleColor();

  // Handle gamepad shortcuts
  auto& imgui_io = ImGui::GetIO();

  // X button = Backspace
  // if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft)) {
  //   if (!input_text_.empty()) {
  //     input_text_.pop_back();
  //   }
  //}

  // Y button = Space
  // if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp)) {
  //  input_text_ += " ";
  //}

  // B button = Backspace (not cancel - game expects input)
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
    if (!input_text_.empty()) {
      input_text_.pop_back();
    }
  }

  // LB = Toggle symbols
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) {
    is_symbol_mode_ = !is_symbol_mode_;
    if (is_symbol_mode_) {
      is_shifted_ = false;
    }
  }

  // RB = Toggle shift
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) {
    is_shifted_ = !is_shifted_;
    if (is_shifted_) {
      is_symbol_mode_ = false;
    }
  }
}

void KeyboardDialog::ProcessKeyInput(const std::string& key) {
  if (key == "Shift") {
    is_shifted_ = !is_shifted_;
    is_symbol_mode_ = false;
  } else if (key == "<-") {
    // Backspace
    if (!input_text_.empty()) {
      input_text_.pop_back();
    }
  } else if (key == "Done") {
    // Submit - but wait for A button release before closing
    // callback_ will be called in OnClose()
    cancelled_ = false;
    pending_close_action_ = "Done";  // Will close when A is released
  } else if (key == "Cancel") {
    // Cancel - wait for A button release before closing
    cancelled_ = true;
    input_text_.clear();
    pending_close_action_ = "Cancel";  // Will close when A is released
  } else if (key == "ABC") {
    // Switch back to letters
    is_symbol_mode_ = false;
  } else if (key == "&123" || key == "Symbols") {
    // Switch to symbols
    is_symbol_mode_ = true;
    is_shifted_ = false;
  } else if (key == "Space") {
    input_text_ += " ";
  } else {
    // Regular character
    input_text_ += key;

    // Auto-unshift after typing a character (unless caps lock)
    if (is_shifted_ && !is_caps_lock_) {
      is_shifted_ = false;
    }
  }
}

}  // namespace ui
}  // namespace xe
