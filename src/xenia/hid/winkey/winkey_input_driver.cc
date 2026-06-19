/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/winkey/winkey_input_driver.h"

#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/hid/hid_flags.h"
#include "xenia/hid/input_system.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/ui/window.h"

#define XE_HID_WINKEY_BINDING(button, description, cvar_name, \
                              cvar_default_value)             \
  DEFINE_string(cvar_name, cvar_default_value,                \
                "List of keys to bind to " description        \
                ", separated by spaces",                      \
                "HID.WinKey")
#include "winkey_binding_table.inc"
#undef XE_HID_WINKEY_BINDING

DEFINE_int32(keyboard_mode, 0,
             "Allows user do specify keyboard working mode. Possible values: 0 "
             "- Disabled, 1 - Enabled, 2 - Passthrough. Passthrough requires "
             "controller being connected!",
             "HID");

DEFINE_int32(
    keyboard_user_index, 0,
    "Controller port that keyboard emulates. [0, 3] - Keyboard is assigned to "
    "selected slot. Passthrough does not require assigning slot.",
    "HID");

DEFINE_bool(
    mousehook, false,
    "Mouse-look (mousehook): lock the cursor to the window and map "
    "relative mouse motion to the right thumbstick (FPS aim). Left "
    "mouse = right trigger (fire), right mouse = left trigger (aim), "
    "middle mouse = right-thumb press. Requires keyboard_mode=1 for the "
    "same controller slot (keyboard_user_index).",
    "HID");

DEFINE_double(
    mousehook_sensitivity, 1.0,
    "Mousehook aim sensitivity multiplier (right-stick deflection per "
    "pixel of mouse motion).",
    "HID");

DEFINE_bool(mousehook_invert_y, false,
            "Mousehook: invert the vertical (look up/down) aim axis.", "HID");

namespace xe {
namespace hid {
namespace winkey {

bool static IsPassthroughEnabled() {
  return static_cast<KeyboardMode>(cvars::keyboard_mode) ==
         KeyboardMode::Passthrough;
}

bool static IsKeyboardForUserEnabled(uint32_t user_index) {
  if (static_cast<KeyboardMode>(cvars::keyboard_mode) !=
      KeyboardMode::Enabled) {
    return false;
  }

  return cvars::keyboard_user_index == user_index;
}

bool __inline IsKeyToggled(uint8_t key) {
  return (GetKeyState(key) & 0x1) == 0x1;
}

bool __inline IsKeyDown(uint8_t key) {
  return (GetAsyncKeyState(key) & 0x8000) == 0x8000;
}

bool __inline IsKeyDown(ui::VirtualKey virtual_key) {
  return IsKeyDown(static_cast<uint8_t>(virtual_key));
}

void WinKeyInputDriver::ParseKeyBinding(ui::VirtualKey output_key,
                                        const std::string_view description,
                                        const std::string_view source_tokens) {
  for (const std::string_view source_token :
       utf8::split(source_tokens, " ", true)) {
    KeyBinding key_binding;
    key_binding.output_key = output_key;

    std::string_view token = source_token;

    if (utf8::starts_with(token, "_")) {
      key_binding.lowercase = true;
      token = token.substr(1);
    } else if (utf8::starts_with(token, "^")) {
      key_binding.uppercase = true;
      token = token.substr(1);
    }

    if (utf8::starts_with(token, "0x")) {
      token = token.substr(2);
      key_binding.input_key = static_cast<ui::VirtualKey>(
          string_util::from_string<uint16_t>(token, true));
    } else if (token.size() == 1 && (token[0] >= 'A' && token[0] <= 'Z') ||
               (token[0] >= '0' && token[0] <= '9')) {
      key_binding.input_key = static_cast<ui::VirtualKey>(token[0]);
    }

    if (key_binding.input_key == ui::VirtualKey::kNone) {
      XELOGW("winkey: failed to parse binding \"{}\" for controller input {}.",
             source_token, description);
      continue;
    }

    key_bindings_.push_back(key_binding);
    XELOGI("winkey: \"{}\" binds key 0x{:X} to controller input {}.",
           source_token, static_cast<uint16_t>(key_binding.input_key),
           description);
  }
}

WinKeyInputDriver::WinKeyInputDriver(xe::ui::Window* window,
                                     size_t window_z_order)
    : InputDriver(window, window_z_order), window_input_listener_(*this) {
#define XE_HID_WINKEY_BINDING(button, description, cvar_name,          \
                              cvar_default_value)                      \
  ParseKeyBinding(xe::ui::VirtualKey::kXInputPad##button, description, \
                  cvars::cvar_name);
#include "winkey_binding_table.inc"
#undef XE_HID_WINKEY_BINDING

  window->AddInputListener(&window_input_listener_, window_z_order);
}

WinKeyInputDriver::~WinKeyInputDriver() {
  window()->RemoveInputListener(&window_input_listener_);
}

X_STATUS WinKeyInputDriver::Setup() { return X_STATUS_SUCCESS; }

X_RESULT WinKeyInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                            X_INPUT_CAPABILITIES* out_caps) {
  if (!IsKeyboardForUserEnabled(user_index) && !IsPassthroughEnabled()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (IsPassthroughEnabled()) {
    out_caps->type = X_INPUT_DEVTYPE::XINPUT_DEVTYPE_KEYBOARD;
    out_caps->sub_type = X_INPUT_DEVSUBTYPE::XINPUT_DEVSUBTYPE_USB_KEYBOARD;
    return X_ERROR_SUCCESS;
  }

  out_caps->type = X_INPUT_DEVTYPE::XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = X_INPUT_DEVSUBTYPE::XINPUT_DEVSUBTYPE_GAMEPAD;
  out_caps->flags = 0;
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = (int16_t)0xFFFFu;
  out_caps->gamepad.thumb_ly = (int16_t)0xFFFFu;
  out_caps->gamepad.thumb_rx = (int16_t)0xFFFFu;
  out_caps->gamepad.thumb_ry = (int16_t)0xFFFFu;
  out_caps->vibration.left_motor_speed = 0;
  out_caps->vibration.right_motor_speed = 0;
  return X_ERROR_SUCCESS;
}

X_RESULT WinKeyInputDriver::GetState(uint32_t user_index,
                                     X_INPUT_STATE* out_state) {
  if (!IsKeyboardForUserEnabled(user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  packet_number_++;

  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;

  if (window()->HasFocus() && is_active()) {
    bool capital = IsKeyToggled(VK_CAPITAL) || IsKeyDown(VK_SHIFT);
    for (const KeyBinding& b : key_bindings_) {
      if (((b.lowercase == b.uppercase) || (b.lowercase && !capital) ||
           (b.uppercase && capital)) &&
          IsKeyDown(b.input_key)) {
        switch (b.output_key) {
          case ui::VirtualKey::kXInputPadA:
            buttons |= X_INPUT_GAMEPAD_A;
            break;
          case ui::VirtualKey::kXInputPadY:
            buttons |= X_INPUT_GAMEPAD_Y;
            break;
          case ui::VirtualKey::kXInputPadB:
            buttons |= X_INPUT_GAMEPAD_B;
            break;
          case ui::VirtualKey::kXInputPadX:
            buttons |= X_INPUT_GAMEPAD_X;
            break;
          case ui::VirtualKey::kXInputPadGuide:
            buttons |= X_INPUT_GAMEPAD_GUIDE;
            break;
          case ui::VirtualKey::kXInputPadDpadLeft:
            buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
            break;
          case ui::VirtualKey::kXInputPadDpadRight:
            buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
            break;
          case ui::VirtualKey::kXInputPadDpadDown:
            buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
            break;
          case ui::VirtualKey::kXInputPadDpadUp:
            buttons |= X_INPUT_GAMEPAD_DPAD_UP;
            break;
          case ui::VirtualKey::kXInputPadRThumbPress:
            buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
            break;
          case ui::VirtualKey::kXInputPadLThumbPress:
            buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
            break;
          case ui::VirtualKey::kXInputPadBack:
            buttons |= X_INPUT_GAMEPAD_BACK;
            break;
          case ui::VirtualKey::kXInputPadStart:
            buttons |= X_INPUT_GAMEPAD_START;
            break;
          case ui::VirtualKey::kXInputPadLShoulder:
            buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
            break;
          case ui::VirtualKey::kXInputPadRShoulder:
            buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
            break;
          case ui::VirtualKey::kXInputPadLTrigger:
            left_trigger = 0xFF;
            break;
          case ui::VirtualKey::kXInputPadRTrigger:
            right_trigger = 0xFF;
            break;
          case ui::VirtualKey::kXInputPadLThumbLeft:
            thumb_lx += SHRT_MIN;
            break;
          case ui::VirtualKey::kXInputPadLThumbRight:
            thumb_lx += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadLThumbDown:
            thumb_ly += SHRT_MIN;
            break;
          case ui::VirtualKey::kXInputPadLThumbUp:
            thumb_ly += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadRThumbUp:
            thumb_ry += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadRThumbDown:
            thumb_ry += SHRT_MIN;
            break;
          case ui::VirtualKey::kXInputPadRThumbRight:
            thumb_rx += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadRThumbLeft:
            thumb_rx += SHRT_MIN;
            break;
        }
      }
    }

    // Mousehook: fold accumulated relative mouse motion into the right stick as
    // a velocity-style deflection (pixels moved this frame -> deflection; the
    // accumulator is drained each poll so the stick recentres when the mouse
    // stops). Mouse buttons map to triggers / right-thumb press.
    if (cvars::mousehook) {
      const int dx = mouse_dx_.exchange(0, std::memory_order_relaxed);
      const int dy = mouse_dy_.exchange(0, std::memory_order_relaxed);
      // ~25 px of motion in one poll == full deflection at sensitivity 1.0.
      const double scale = cvars::mousehook_sensitivity * (32767.0 / 25.0);
      auto add_axis = [](int base, double delta) -> int16_t {
        double v = static_cast<double>(base) + delta;
        if (v > SHRT_MAX) v = SHRT_MAX;
        if (v < SHRT_MIN) v = SHRT_MIN;
        return static_cast<int16_t>(v);
      };
      thumb_rx = add_axis(thumb_rx, dx * scale);
      // Screen-down (dy > 0) = look down = stick down (negative) unless
      // inverted.
      thumb_ry = add_axis(
          thumb_ry, cvars::mousehook_invert_y ? (dy * scale) : -(dy * scale));
      if (mouse_left_.load(std::memory_order_relaxed)) {
        right_trigger = 0xFF;  // fire
      }
      if (mouse_right_.load(std::memory_order_relaxed)) {
        left_trigger = 0xFF;  // aim down sights
      }
      if (mouse_middle_.load(std::memory_order_relaxed)) {
        buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
      }
    }
  }

  out_state->packet_number = packet_number_;
  out_state->gamepad.buttons = buttons;
  out_state->gamepad.left_trigger = left_trigger;
  out_state->gamepad.right_trigger = right_trigger;
  out_state->gamepad.thumb_lx = thumb_lx;
  out_state->gamepad.thumb_ly = thumb_ly;
  out_state->gamepad.thumb_rx = thumb_rx;
  out_state->gamepad.thumb_ry = thumb_ry;

  if (IsPassthroughEnabled()) {
    memset(out_state, 0, sizeof(out_state));
  }

  return X_ERROR_SUCCESS;
}

X_RESULT WinKeyInputDriver::SetState(uint32_t user_index,
                                     X_INPUT_VIBRATION* vibration) {
  if (!IsKeyboardForUserEnabled(user_index) && !IsPassthroughEnabled()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  return X_ERROR_SUCCESS;
}

X_RESULT WinKeyInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                         X_INPUT_KEYSTROKE* out_keystroke) {
  if (!is_active()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (!IsKeyboardForUserEnabled(user_index) && !IsPassthroughEnabled()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // Pop from the queue.
  KeyEvent evt;
  {
    auto global_lock = global_critical_region_.Acquire();
    if (key_events_.empty()) {
      // No keys!
      return X_ERROR_EMPTY;
    }
    evt = key_events_.front();
    key_events_.pop();
  }

  X_RESULT result = X_ERROR_EMPTY;

  ui::VirtualKey xinput_virtual_key = ui::VirtualKey::kNone;
  uint16_t unicode = 0;
  uint16_t keystroke_flags = 0;
  uint8_t hid_code = 0;

  bool capital = IsKeyToggled(VK_CAPITAL) || IsKeyDown(VK_SHIFT);

  if (!IsPassthroughEnabled()) {
    if (IsKeyboardForUserEnabled(user_index)) {
      for (const KeyBinding& b : key_bindings_) {
        if (b.input_key == evt.virtual_key &&
            ((b.lowercase == b.uppercase) || (b.lowercase && !capital) ||
             (b.uppercase && capital))) {
          xinput_virtual_key = b.output_key;
        }
      }
    }
  } else {
    xinput_virtual_key = evt.virtual_key;

    if (capital) {
      keystroke_flags |= 0x0008;  // XINPUT_KEYSTROKE_SHIFT
    }

    if (IsKeyToggled(VK_CONTROL)) {
      keystroke_flags |= 0x0010;  // XINPUT_KEYSTROKE_CTRL
    }

    if (IsKeyToggled(VK_MENU)) {
      keystroke_flags |= 0x0020;  // XINPUT_KEYSTROKE_ALT
    }
  }

  if (xinput_virtual_key != ui::VirtualKey::kNone) {
    if (evt.transition == true) {
      keystroke_flags |= 0x0001;  // XINPUT_KEYSTROKE_KEYDOWN
      if (evt.prev_state == evt.transition) {
        keystroke_flags |= 0x0004;  // XINPUT_KEYSTROKE_REPEAT
      }
    } else if (evt.transition == false) {
      keystroke_flags |= 0x0002;  // XINPUT_KEYSTROKE_KEYUP
    }

    if (IsPassthroughEnabled()) {
      if (GetKeyboardState(key_map_)) {
        WCHAR buf;
        if (ToUnicode(uint8_t(xinput_virtual_key), 0, key_map_, &buf, 1, 0) ==
            1) {
          keystroke_flags |= 0x1000;  // XINPUT_KEYSTROKE_VALIDUNICODE
          unicode = buf;
        }
      }
    }

    result = X_ERROR_SUCCESS;
  }

  out_keystroke->virtual_key = uint16_t(xinput_virtual_key);
  out_keystroke->unicode = unicode;
  out_keystroke->flags = keystroke_flags;
  out_keystroke->user_index = user_index;
  out_keystroke->hid_code = hid_code;

  // X_ERROR_EMPTY if no new keys
  // X_ERROR_DEVICE_NOT_CONNECTED if no device
  // X_ERROR_SUCCESS if key
  return result;
}

void WinKeyInputDriver::WinKeyWindowInputListener::OnKeyDown(ui::KeyEvent& e) {
  driver_.OnKey(e, true);
}

void WinKeyInputDriver::WinKeyWindowInputListener::OnKeyUp(ui::KeyEvent& e) {
  driver_.OnKey(e, false);
}

void WinKeyInputDriver::OnKey(ui::KeyEvent& e, bool is_down) {
  if (!is_active() || static_cast<KeyboardMode>(cvars::keyboard_mode) ==
                          KeyboardMode::Disabled) {
    return;
  }

  KeyEvent key;
  key.virtual_key = e.virtual_key();
  key.transition = is_down;
  key.prev_state = e.prev_state();
  key.repeat_count = e.repeat_count();

  auto global_lock = global_critical_region_.Acquire();
  key_events_.push(key);
}

void WinKeyInputDriver::WinKeyWindowInputListener::OnMouseDown(
    ui::MouseEvent& e) {
  driver_.OnMouseButton(e, true);
}

void WinKeyInputDriver::WinKeyWindowInputListener::OnMouseUp(
    ui::MouseEvent& e) {
  driver_.OnMouseButton(e, false);
}

void WinKeyInputDriver::WinKeyWindowInputListener::OnMouseMove(
    ui::MouseEvent& e) {
  driver_.OnMouseMove(e);
}

void WinKeyInputDriver::OnMouseButton(ui::MouseEvent& e, bool is_down) {
  if (!cvars::mousehook) {
    return;
  }
  switch (e.button()) {
    case ui::MouseEvent::Button::kLeft:
      mouse_left_.store(is_down, std::memory_order_relaxed);
      break;
    case ui::MouseEvent::Button::kRight:
      mouse_right_.store(is_down, std::memory_order_relaxed);
      break;
    case ui::MouseEvent::Button::kMiddle:
      mouse_middle_.store(is_down, std::memory_order_relaxed);
      break;
    default:
      break;
  }
}

void WinKeyInputDriver::OnMouseMove(ui::MouseEvent& e) {
  // Cursor-lock relative aim. Only active when mousehook is on, this driver is
  // active, and the window is focused; otherwise restore the system cursor.
  const bool active = cvars::mousehook && is_active() && window()->HasFocus();
  HWND hwnd = GetForegroundWindow();
  if (!active || !hwnd) {
    if (cursor_hidden_) {
      ShowCursor(TRUE);
      cursor_hidden_ = false;
    }
    return;
  }

  RECT client_rect;
  if (!GetClientRect(hwnd, &client_rect)) {
    return;
  }
  const int center_x = client_rect.right / 2;
  const int center_y = client_rect.bottom / 2;
  POINT screen_center{center_x, center_y};
  ClientToScreen(hwnd, &screen_center);

  if (!cursor_hidden_) {
    // Just entered the lock: hide + re-centre, but ignore this initial jump so
    // the aim doesn't lurch when the cursor was sitting elsewhere.
    SetCursorPos(screen_center.x, screen_center.y);
    ShowCursor(FALSE);
    cursor_hidden_ = true;
    return;
  }

  // MouseEvent x()/y() are client-relative; the delta from centre is the
  // relative motion since our last re-centre.
  const int dx = e.x() - center_x;
  const int dy = e.y() - center_y;
  if (dx == 0 && dy == 0) {
    // The move our own SetCursorPos generated.
    return;
  }
  mouse_dx_.fetch_add(dx, std::memory_order_relaxed);
  mouse_dy_.fetch_add(dy, std::memory_order_relaxed);
  // Re-centre so the cursor can never leave the window (unbounded aim).
  SetCursorPos(screen_center.x, screen_center.y);
}

InputType WinKeyInputDriver::GetInputType() const {
  switch (static_cast<KeyboardMode>(cvars::keyboard_mode)) {
    case KeyboardMode::Disabled:
      return InputType::None;
    case KeyboardMode::Enabled:
      return InputType::Controller;
    case KeyboardMode::Passthrough:
      return InputType::Keyboard;
    default:
      break;
  }
  return InputType::Controller;
}

}  // namespace winkey
}  // namespace hid
}  // namespace xe
