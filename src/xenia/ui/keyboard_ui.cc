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

#include "third_party/imgui/imgui.h"
#include "xenia/base/logging.h"
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
    {{"1", "1", 1.0f, kNormalColor}, {"2", "2", 1.0f, kNormalColor},
     {"3", "3", 1.0f, kNormalColor}, {"4", "4", 1.0f, kNormalColor},
     {"5", "5", 1.0f, kNormalColor}, {"6", "6", 1.0f, kNormalColor},
     {"7", "7", 1.0f, kNormalColor}, {"8", "8", 1.0f, kNormalColor},
     {"9", "9", 1.0f, kNormalColor}, {"0", "0", 1.0f, kNormalColor}},
    // Row 1: QWERTY top
    {{"q", "q", 1.0f, kNormalColor}, {"w", "w", 1.0f, kNormalColor},
     {"e", "e", 1.0f, kNormalColor}, {"r", "r", 1.0f, kNormalColor},
     {"t", "t", 1.0f, kNormalColor}, {"y", "y", 1.0f, kNormalColor},
     {"u", "u", 1.0f, kNormalColor}, {"i", "i", 1.0f, kNormalColor},
     {"o", "o", 1.0f, kNormalColor}, {"p", "p", 1.0f, kNormalColor}},
    // Row 2: QWERTY middle
    {{"a", "a", 1.0f, kNormalColor}, {"s", "s", 1.0f, kNormalColor},
     {"d", "d", 1.0f, kNormalColor}, {"f", "f", 1.0f, kNormalColor},
     {"g", "g", 1.0f, kNormalColor}, {"h", "h", 1.0f, kNormalColor},
     {"j", "j", 1.0f, kNormalColor}, {"k", "k", 1.0f, kNormalColor},
     {"l", "l", 1.0f, kNormalColor}, {"'", "'", 1.0f, kNormalColor}},
    // Row 3: QWERTY bottom + punctuation
    {{"z", "z", 1.0f, kNormalColor}, {"x", "x", 1.0f, kNormalColor},
     {"c", "c", 1.0f, kNormalColor}, {"v", "v", 1.0f, kNormalColor},
     {"b", "b", 1.0f, kNormalColor}, {"n", "n", 1.0f, kNormalColor},
     {"m", "m", 1.0f, kNormalColor}, {",", ",", 1.0f, kNormalColor},
     {".", ".", 1.0f, kNormalColor}, {"/", "/", 1.0f, kNormalColor}},
    // Row 4: More punctuation
    {{"-", "-", 1.0f, kNormalColor}, {"_", "_", 1.0f, kNormalColor},
     {":", ":", 1.0f, kNormalColor}, {";", ";", 1.0f, kNormalColor},
     {"\"", "\"", 1.0f, kNormalColor}, {"?", "?", 1.0f, kNormalColor},
     {"!", "!", 1.0f, kNormalColor}, {"@", "@", 1.0f, kNormalColor},
     {"#", "#", 1.0f, kNormalColor}, {"&", "&", 1.0f, kNormalColor}},
    // Row 5: Special keys
    {{"Shift", nullptr, 1.5f, kSpecialColor},
     {"Space", " ", 3.0f, kAccentColor},
     {"<-", nullptr, 1.0f, kDangerColor},
     {"Cancel", nullptr, 1.5f, kDangerColor},
     {"Done", nullptr, 2.0f, kAccentColor}}
};

static const std::vector<std::vector<KeyDef>> kUppercaseLayout = {
    // Row 0: Symbols
    {{"!", "!", 1.0f, kNormalColor}, {"@", "@", 1.0f, kNormalColor},
     {"#", "#", 1.0f, kNormalColor}, {"$", "$", 1.0f, kNormalColor},
     {"%", "%", 1.0f, kNormalColor}, {"^", "^", 1.0f, kNormalColor},
     {"&", "&", 1.0f, kNormalColor}, {"*", "*", 1.0f, kNormalColor},
     {"(", "(", 1.0f, kNormalColor}, {")", ")", 1.0f, kNormalColor}},
    // Row 1: QWERTY top uppercase
    {{"Q", "Q", 1.0f, kNormalColor}, {"W", "W", 1.0f, kNormalColor},
     {"E", "E", 1.0f, kNormalColor}, {"R", "R", 1.0f, kNormalColor},
     {"T", "T", 1.0f, kNormalColor}, {"Y", "Y", 1.0f, kNormalColor},
     {"U", "U", 1.0f, kNormalColor}, {"I", "I", 1.0f, kNormalColor},
     {"O", "O", 1.0f, kNormalColor}, {"P", "P", 1.0f, kNormalColor}},
    // Row 2: QWERTY middle uppercase
    {{"A", "A", 1.0f, kNormalColor}, {"S", "S", 1.0f, kNormalColor},
     {"D", "D", 1.0f, kNormalColor}, {"F", "F", 1.0f, kNormalColor},
     {"G", "G", 1.0f, kNormalColor}, {"H", "H", 1.0f, kNormalColor},
     {"J", "J", 1.0f, kNormalColor}, {"K", "K", 1.0f, kNormalColor},
     {"L", "L", 1.0f, kNormalColor}, {"\"", "\"", 1.0f, kNormalColor}},
    // Row 3: QWERTY bottom uppercase + punctuation
    {{"Z", "Z", 1.0f, kNormalColor}, {"X", "X", 1.0f, kNormalColor},
     {"C", "C", 1.0f, kNormalColor}, {"V", "V", 1.0f, kNormalColor},
     {"B", "B", 1.0f, kNormalColor}, {"N", "N", 1.0f, kNormalColor},
     {"M", "M", 1.0f, kNormalColor}, {"<", "<", 1.0f, kNormalColor},
     {">", ">", 1.0f, kNormalColor}, {"?", "?", 1.0f, kNormalColor}},
    // Row 4: More punctuation
    {{"+", "+", 1.0f, kNormalColor}, {"=", "=", 1.0f, kNormalColor},
     {"{", "{", 1.0f, kNormalColor}, {"}", "}", 1.0f, kNormalColor},
     {"[", "[", 1.0f, kNormalColor}, {"]", "]", 1.0f, kNormalColor},
     {"\\", "\\", 1.0f, kNormalColor}, {"|", "|", 1.0f, kNormalColor},
     {"~", "~", 1.0f, kNormalColor}, {"`", "`", 1.0f, kNormalColor}},
    // Row 5: Special keys
    {{"Shift", nullptr, 1.5f, kSpecialColor},
     {"Space", " ", 3.0f, kAccentColor},
     {"<-", nullptr, 1.0f, kDangerColor},
     {"Cancel", nullptr, 1.5f, kDangerColor},
     {"Done", nullptr, 2.0f, kAccentColor}}
};

static const std::vector<std::vector<KeyDef>> kSymbolLayout = {
    // Row 0: More symbols
    {{"~", "~", 1.0f, kNormalColor}, {"`", "`", 1.0f, kNormalColor},
     {"|", "|", 1.0f, kNormalColor}, {"\\", "\\", 1.0f, kNormalColor},
     {"<", "<", 1.0f, kNormalColor}, {">", ">", 1.0f, kNormalColor},
     {"{", "{", 1.0f, kNormalColor}, {"}", "}", 1.0f, kNormalColor},
     {"[", "[", 1.0f, kNormalColor}, {"]", "]", 1.0f, kNormalColor}},
    // Row 1: Punctuation
    {{"!", "!", 1.0f, kNormalColor}, {"@", "@", 1.0f, kNormalColor},
     {"#", "#", 1.0f, kNormalColor}, {"$", "$", 1.0f, kNormalColor},
     {"%", "%", 1.0f, kNormalColor}, {"^", "^", 1.0f, kNormalColor},
     {"&", "&", 1.0f, kNormalColor}, {"*", "*", 1.0f, kNormalColor},
     {"(", "(", 1.0f, kNormalColor}, {")", ")", 1.0f, kNormalColor}},
    // Row 2: More punctuation
    {{"-", "-", 1.0f, kNormalColor}, {"=", "=", 1.0f, kNormalColor},
     {"+", "+", 1.0f, kNormalColor}, {"_", "_", 1.0f, kNormalColor},
     {":", ":", 1.0f, kNormalColor}, {";", ";", 1.0f, kNormalColor},
     {"\"", "\"", 1.0f, kNormalColor}, {"'", "'", 1.0f, kNormalColor},
     {",", ",", 1.0f, kNormalColor}, {".", ".", 1.0f, kNormalColor}},
    // Row 3: Extra
    {{"?", "?", 1.0f, kNormalColor}, {"/", "/", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor}, {"", "", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor}, {"", "", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor}, {"", "", 1.0f, kNormalColor},
     {"", "", 1.0f, kNormalColor}, {"", "", 1.0f, kNormalColor}},
    // Row 4: Special keys
    {{"ABC", nullptr, 1.5f, kSpecialColor},
     {"Space", " ", 3.0f, kAccentColor},
     {"<-", nullptr, 1.0f, kDangerColor},
     {"Cancel", nullptr, 1.5f, kDangerColor},
     {"Done", nullptr, 2.0f, kAccentColor}}
};

static const std::vector<std::vector<KeyDef>> kNumberLayout = {
    // Number pad style layout
    {{"7", "7", 1.0f, kNormalColor}, {"8", "8", 1.0f, kNormalColor},
     {"9", "9", 1.0f, kNormalColor}},
    {{"4", "4", 1.0f, kNormalColor}, {"5", "5", 1.0f, kNormalColor},
     {"6", "6", 1.0f, kNormalColor}},
    {{"1", "1", 1.0f, kNormalColor}, {"2", "2", 1.0f, kNormalColor},
     {"3", "3", 1.0f, kNormalColor}},
    {{"0", "0", 2.0f, kNormalColor}, {".", ".", 1.0f, kNormalColor}},
    {{"<-", nullptr, 1.0f, kDangerColor}, 
     {"Cancel", nullptr, 1.0f, kDangerColor},
     {"Done", nullptr, 1.0f, kAccentColor}}
};

KeyboardDialog* KeyboardDialog::ShowKeyboard(ImGuiDrawer* imgui_drawer,
                                            const std::string& title,
                                            const std::string& initial_text,
                                            InputType type,
                                            InputCallback callback) {
  return new KeyboardDialog(imgui_drawer, title, initial_text, type, callback);
}

KeyboardDialog::KeyboardDialog(ImGuiDrawer* imgui_drawer,
                               const std::string& title,
                               const std::string& initial_text,
                               InputType type,
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
}

KeyboardDialog::~KeyboardDialog() {}

void KeyboardDialog::OnDraw(ImGuiIO& io) {
  // Poll XInput for controller state - needed since keyboard is a modal popup
#if XE_PLATFORM_WIN32
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    XINPUT_STATE state;
    if (XInputGetState(i, &state) == ERROR_SUCCESS) {
      const auto& pad = state.Gamepad;
      
      // Map buttons to ImGui - A=FaceUp for activation (matches imgui_drawer mapping)
      io.AddKeyEvent(ImGuiKey_GamepadFaceUp, (pad.wButtons & XINPUT_GAMEPAD_A) != 0);
      io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (pad.wButtons & XINPUT_GAMEPAD_B) != 0);
      io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, (pad.wButtons & XINPUT_GAMEPAD_X) != 0);
      io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (pad.wButtons & XINPUT_GAMEPAD_Y) != 0);
      io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
      io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);
      io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0);
      io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
      io.AddKeyEvent(ImGuiKey_GamepadL1, (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
      io.AddKeyEvent(ImGuiKey_GamepadR1, (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
      io.AddKeyEvent(ImGuiKey_GamepadStart, (pad.wButtons & XINPUT_GAMEPAD_START) != 0);
      io.AddKeyEvent(ImGuiKey_GamepadBack, (pad.wButtons & XINPUT_GAMEPAD_BACK) != 0);
      
      // Left stick with deadzone
      const SHORT STICK_DEADZONE = 7849;
      float lx = 0.0f, ly = 0.0f;
      if (pad.sThumbLX < -STICK_DEADZONE) {
        lx = (float)(pad.sThumbLX + STICK_DEADZONE) / (32768.0f - STICK_DEADZONE);
      } else if (pad.sThumbLX > STICK_DEADZONE) {
        lx = (float)(pad.sThumbLX - STICK_DEADZONE) / (32767.0f - STICK_DEADZONE);
      }
      if (pad.sThumbLY < -STICK_DEADZONE) {
        ly = (float)(pad.sThumbLY + STICK_DEADZONE) / (32768.0f - STICK_DEADZONE);
      } else if (pad.sThumbLY > STICK_DEADZONE) {
        ly = (float)(pad.sThumbLY - STICK_DEADZONE) / (32767.0f - STICK_DEADZONE);
      }
      io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, lx < 0.0f, lx < 0.0f ? -lx : 0.0f);
      io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx > 0.0f, lx > 0.0f ? lx : 0.0f);
      io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, ly > 0.0f, ly > 0.0f ? ly : 0.0f);
      io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, ly < 0.0f, ly < 0.0f ? -ly : 0.0f);
      
      // Only use first connected controller
      break;
    }
  }
#endif

  if (!has_opened_) {
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
  if (ImGui::BeginPopupModal(title_.c_str(), &popup_open, flags)) {
    // Draw input text field
    DrawTextInput();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Draw keyboard layout
    DrawKeyboardLayout();

    // Handle gamepad B button to cancel (but not A - that's handled by buttons)
    //if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) ||
        //ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      // Cancel - close without saving
      //ImGui::CloseCurrentPopup();
      //Close();
    // Back button closes dialog (like X button)
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack)) {
      cancelled_ = true;
      pending_close_action_ = "Back";
    }

    ImGui::EndPopup();
  }
  
  // X button clicked with mouse - close immediately (no gamepad involved)
  if (!popup_open) {
    cancelled_ = true;
    Close();
    return;
  }
  
  // Check if we're waiting to close (Done/Cancel/Back was pressed via gamepad)
  // Wait until A button AND Back button are released before actually closing
  if (!pending_close_action_.empty()) {
    bool a_pressed = ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp);
    bool back_pressed = ImGui::IsKeyDown(ImGuiKey_GamepadBack);
    if (!a_pressed && !back_pressed) {
      // Buttons released, safe to close now
      pending_close_action_.clear();
      Close();
    }
  }
}

void KeyboardDialog::OnClose() {
  // If user closed the dialog, call callback with current text
  if (callback_) {
    callback_(input_text_);
  }
  // Call XAM close callback if set
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

  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(key_spacing, key_spacing));
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

      std::string button_id = std::string(key.label) + "##" + std::to_string(key_index);
      bool pressed = ImGui::Button(button_id.c_str(),
                                   ImVec2(key_size * key.width, key_size));

      ImGui::PopStyleColor(3);

      if (pressed) {
        ProcessKeyInput(key.label);
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
    ImGui::TextWrapped("Use D-Pad/Left Stick to navigate, A to select, B to backspace");
  } else {
    ImGui::TextWrapped(
        "D-Pad/Stick: Navigate | A: Select | B: Backspace | "
        "LB: Symbols | RB: Shift");
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
  //if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp)) {
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
    cancelled_ = false;
    if (callback_) {
      callback_(input_text_);
    }
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
