/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/mousehook_config.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <string_view>

#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/prettywriter.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/platform.h"
#include "xenia/base/string_util.h"
#include "xenia/base/utf8.h"
#include "xenia/hid/hid_flags.h"
#include "xenia/ui/virtual_key.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif
#include "xenia/base/logging.h"

namespace xe {
namespace hid {

namespace {

constexpr uint32_t kMaxAction = static_cast<uint32_t>(MouseButtonAction::kY);

MouseButtonAction ActionFromInt(int value, MouseButtonAction fallback) {
  if (value < 0 || static_cast<uint32_t>(value) > kMaxAction) {
    return fallback;
  }
  return static_cast<MouseButtonAction>(value);
}

// Name / output / stock default for every controller input, pulled straight
// from the binding table so the two can never drift apart.
struct BindingTableEntry {
  const char* name;
  ui::VirtualKey output;
  const char* default_keys;
};

static const BindingTableEntry kBindingTable[] = {
#define XE_HID_WINKEY_BINDING(button, description, cvar_name, \
                              cvar_default_value)             \
  {#cvar_name, ui::VirtualKey::kXInputPad##button, cvar_default_value},
#include "xenia/hid/winkey/winkey_binding_table.inc"
#undef XE_HID_WINKEY_BINDING
};

// Mousehook's own defaults, overriding the stock ones above. Keyboard-and-mouse
// layout: WASD moves, Q/E triggers, 1/4 bumpers, Space jumps, R reloads.
struct MousehookDefault {
  const char* name;
  const char* keys;
};

static const MousehookDefault kMousehookDefaults[] = {
    {"keybind_left_thumb_up", "W"},
    {"keybind_left_thumb_left", "A"},
    {"keybind_left_thumb_down", "S"},
    {"keybind_left_thumb_right", "D"},
    {"keybind_left_trigger", "Q"},
    {"keybind_right_trigger", "E"},
    {"keybind_left_shoulder", "1"},
    {"keybind_right_shoulder", "4"},
    {"keybind_a", "0x20"},  // Space
    {"keybind_b", "R"},

    // D-pad on the arrow keys. The stock layout puts it on Shift+WASD, which
    // would fire alongside the plain-WASD stick bindings above.
    {"keybind_dpad_left", "0x25"},
    {"keybind_dpad_up", "0x26"},
    {"keybind_dpad_right", "0x27"},
    {"keybind_dpad_down", "0x28"},

    // The arrows were the stock right-stick bindings; the mouse drives that
    // stick under mousehook, so clear them rather than double-book the keys.
    {"keybind_right_thumb_up", ""},
    {"keybind_right_thumb_down", ""},
    {"keybind_right_thumb_left", ""},
    {"keybind_right_thumb_right", ""},
};

// Host key state. Mirrors the WinKey driver's helpers; a no-op elsewhere.
bool IsHostKeyDown(uint16_t vk) {
#if XE_PLATFORM_WIN32
  return (GetAsyncKeyState(vk) & 0x8000) == 0x8000;
#else
  return false;
#endif
}

void ApplyBoundKey(X_INPUT_GAMEPAD* gamepad, uint16_t output_key) {
  auto press = [&](uint16_t mask) {
    gamepad->buttons = gamepad->buttons.get() | mask;
  };
  auto axis = [](xe::be<int16_t>& field, int32_t delta) {
    int32_t v = static_cast<int32_t>(field.get()) + delta;
    field = static_cast<int16_t>(
        std::clamp(v, int32_t(SHRT_MIN), int32_t(SHRT_MAX)));
  };

  switch (static_cast<ui::VirtualKey>(output_key)) {
    case ui::VirtualKey::kXInputPadA:
      press(X_INPUT_GAMEPAD_A);
      break;
    case ui::VirtualKey::kXInputPadB:
      press(X_INPUT_GAMEPAD_B);
      break;
    case ui::VirtualKey::kXInputPadX:
      press(X_INPUT_GAMEPAD_X);
      break;
    case ui::VirtualKey::kXInputPadY:
      press(X_INPUT_GAMEPAD_Y);
      break;
    case ui::VirtualKey::kXInputPadGuide:
      press(X_INPUT_GAMEPAD_GUIDE);
      break;
    case ui::VirtualKey::kXInputPadDpadLeft:
      press(X_INPUT_GAMEPAD_DPAD_LEFT);
      break;
    case ui::VirtualKey::kXInputPadDpadRight:
      press(X_INPUT_GAMEPAD_DPAD_RIGHT);
      break;
    case ui::VirtualKey::kXInputPadDpadDown:
      press(X_INPUT_GAMEPAD_DPAD_DOWN);
      break;
    case ui::VirtualKey::kXInputPadDpadUp:
      press(X_INPUT_GAMEPAD_DPAD_UP);
      break;
    case ui::VirtualKey::kXInputPadRThumbPress:
      press(X_INPUT_GAMEPAD_RIGHT_THUMB);
      break;
    case ui::VirtualKey::kXInputPadLThumbPress:
      press(X_INPUT_GAMEPAD_LEFT_THUMB);
      break;
    case ui::VirtualKey::kXInputPadBack:
      press(X_INPUT_GAMEPAD_BACK);
      break;
    case ui::VirtualKey::kXInputPadStart:
      press(X_INPUT_GAMEPAD_START);
      break;
    case ui::VirtualKey::kXInputPadLShoulder:
      press(X_INPUT_GAMEPAD_LEFT_SHOULDER);
      break;
    case ui::VirtualKey::kXInputPadRShoulder:
      press(X_INPUT_GAMEPAD_RIGHT_SHOULDER);
      break;
    case ui::VirtualKey::kXInputPadLTrigger:
      gamepad->left_trigger = 0xFF;
      break;
    case ui::VirtualKey::kXInputPadRTrigger:
      gamepad->right_trigger = 0xFF;
      break;
    case ui::VirtualKey::kXInputPadLThumbLeft:
      axis(gamepad->thumb_lx, SHRT_MIN);
      break;
    case ui::VirtualKey::kXInputPadLThumbRight:
      axis(gamepad->thumb_lx, SHRT_MAX);
      break;
    case ui::VirtualKey::kXInputPadLThumbDown:
      axis(gamepad->thumb_ly, SHRT_MIN);
      break;
    case ui::VirtualKey::kXInputPadLThumbUp:
      axis(gamepad->thumb_ly, SHRT_MAX);
      break;
    case ui::VirtualKey::kXInputPadRThumbUp:
      axis(gamepad->thumb_ry, SHRT_MAX);
      break;
    case ui::VirtualKey::kXInputPadRThumbDown:
      axis(gamepad->thumb_ry, SHRT_MIN);
      break;
    case ui::VirtualKey::kXInputPadRThumbRight:
      axis(gamepad->thumb_rx, SHRT_MAX);
      break;
    case ui::VirtualKey::kXInputPadRThumbLeft:
      axis(gamepad->thumb_rx, SHRT_MIN);
      break;
    default:
      break;
  }
}

bool IsHostCapital() {
#if XE_PLATFORM_WIN32
  return ((GetKeyState(VK_CAPITAL) & 0x1) == 0x1) || IsHostKeyDown(VK_SHIFT);
#else
  return false;
#endif
}

}  // namespace

void MousehookConfig::SeedDefaultKeybinds() {
  {
    std::lock_guard<std::mutex> lock(keybinds_mutex_);
    for (const auto& entry : kBindingTable) {
      keybinds_[entry.name] = entry.default_keys;
    }
    for (const auto& def : kMousehookDefaults) {
      keybinds_[def.name] = def.keys;
    }
  }
  bindings_generation_.fetch_add(1, std::memory_order_release);
}

void MousehookConfig::ApplyToGamepad(X_INPUT_GAMEPAD* gamepad) {
  // Rebuild the parsed binding list when the table changes.
  const uint32_t generation = bindings_generation();
  if (generation != parsed_generation_) {
    parsed_generation_ = generation;
    parsed_binds_.clear();

    const auto binds = keybinds();
    for (const auto& entry : kBindingTable) {
      auto it = binds.find(entry.name);
      if (it == binds.end()) {
        continue;
      }

      for (const std::string_view token : utf8::split(it->second, " ", true)) {
        ParsedBind bind = {};
        bind.output_key = static_cast<uint16_t>(entry.output);

        std::string_view key = token;
        if (utf8::starts_with(key, "_")) {
          bind.lowercase = true;
          key = key.substr(1);
        } else if (utf8::starts_with(key, "^")) {
          bind.uppercase = true;
          key = key.substr(1);
        }

        if (utf8::starts_with(key, "0x")) {
          bind.input_vk =
              string_util::from_string<uint16_t>(key.substr(2), true);
        } else if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') ||
                                       (key[0] >= '0' && key[0] <= '9'))) {
          bind.input_vk = static_cast<uint16_t>(key[0]);
        } else {
          continue;
        }

        parsed_binds_.push_back(bind);
      }
    }
  }

  // Keyboard bindings. Applied here rather than in the WinKey driver so they
  // reach the guest even when another driver owns the slot - same reasoning as
  // the mouse motion below.
  if (!parsed_binds_.empty()) {
    const bool capital = IsHostCapital();
    for (const auto& bind : parsed_binds_) {
      if (!((bind.lowercase == bind.uppercase) ||
            (bind.lowercase && !capital) || (bind.uppercase && capital))) {
        continue;
      }
      if (!IsHostKeyDown(bind.input_vk)) {
        continue;
      }
      ApplyBoundKey(gamepad, bind.output_key);
    }
  }

  const int32_t dx = mouse_dx_.exchange(0, std::memory_order_relaxed);
  const int32_t dy = mouse_dy_.exchange(0, std::memory_order_relaxed);

  const double px = pixels_per_full_deflection();
  const double scale = sensitivity() * (32767.0 / (px > 0.0 ? px : 25.0));

  auto add_axis = [](int32_t base, double delta) -> int16_t {
    double v = static_cast<double>(base) + delta;
    if (v > SHRT_MAX) {
      v = SHRT_MAX;
    }
    if (v < SHRT_MIN) {
      v = SHRT_MIN;
    }
    return static_cast<int16_t>(v);
  };

  double move_x = dx * scale;
  double move_y = invert_y() ? (dy * scale) : -(dy * scale);

  // Deadzone compensation. The mouse is a relative device but the stick is an
  // absolute one, so a slow drag produces a tiny deflection that the title
  // discards as deadzone - motion only "takes" once it crosses the threshold,
  // which reads as jerky. Lift any non-zero motion to at least the deadzone
  // magnitude and scale the remaining range above it, keeping the direction.
  const double compensation = deadzone_compensation();
  if (compensation > 0.0 && (move_x != 0.0 || move_y != 0.0)) {
    const double magnitude = std::sqrt(move_x * move_x + move_y * move_y);
    if (magnitude > 0.0) {
      const double floor_units = compensation * 32767.0;
      double adjusted = floor_units + (std::min(magnitude, 32767.0) / 32767.0) *
                                          (32767.0 - floor_units);
      adjusted = std::min(adjusted, 32767.0);

      const double factor = adjusted / magnitude;
      move_x *= factor;
      move_y *= factor;
    }
  }

  // Mouse drives the right stick (look) by default, or the left stick (move)
  // when the sticks are swapped.
  if (swap_thumbsticks()) {
    gamepad->thumb_lx = add_axis(gamepad->thumb_lx.get(), move_x);
    gamepad->thumb_ly = add_axis(gamepad->thumb_ly.get(), move_y);
  } else {
    gamepad->thumb_rx = add_axis(gamepad->thumb_rx.get(), move_x);
    gamepad->thumb_ry = add_axis(gamepad->thumb_ry.get(), move_y);
  }

  // A real thumbstick's reference is ROUND - magnitude never exceeds 32767 in
  // any direction. Clamping each axis on its own (above, and for the keys)
  // makes it SQUARE, so a diagonal reaches ~46340 and the corners sit 41%
  // further out than a cardinal. The title's own radial clamp then pulls
  // diagonals back and they read as weaker or snapped. Scale onto the circle,
  // keeping direction.
  auto clamp_to_circle = [](xe::be<int16_t>& x, xe::be<int16_t>& y) {
    const double vx = static_cast<double>(x.get());
    const double vy = static_cast<double>(y.get());
    const double magnitude = std::sqrt(vx * vx + vy * vy);
    if (magnitude <= 32767.0 || magnitude <= 0.0) {
      return;
    }
    const double factor = 32767.0 / magnitude;
    x = static_cast<int16_t>(vx * factor);
    y = static_cast<int16_t>(vy * factor);
  };

  clamp_to_circle(gamepad->thumb_lx, gamepad->thumb_ly);
  clamp_to_circle(gamepad->thumb_rx, gamepad->thumb_ry);

  auto apply_button = [&](MouseButtonAction action) {
    switch (action) {
      case MouseButtonAction::kRightTrigger:
        gamepad->right_trigger = 0xFF;
        break;
      case MouseButtonAction::kLeftTrigger:
        gamepad->left_trigger = 0xFF;
        break;
      case MouseButtonAction::kRightThumbPress:
        gamepad->buttons = gamepad->buttons.get() | X_INPUT_GAMEPAD_RIGHT_THUMB;
        break;
      case MouseButtonAction::kLeftThumbPress:
        gamepad->buttons = gamepad->buttons.get() | X_INPUT_GAMEPAD_LEFT_THUMB;
        break;
      case MouseButtonAction::kA:
        gamepad->buttons = gamepad->buttons.get() | X_INPUT_GAMEPAD_A;
        break;
      case MouseButtonAction::kB:
        gamepad->buttons = gamepad->buttons.get() | X_INPUT_GAMEPAD_B;
        break;
      case MouseButtonAction::kX:
        gamepad->buttons = gamepad->buttons.get() | X_INPUT_GAMEPAD_X;
        break;
      case MouseButtonAction::kY:
        gamepad->buttons = gamepad->buttons.get() | X_INPUT_GAMEPAD_Y;
        break;
      default:
        break;
    }
  };

  if (mouse_left_.load(std::memory_order_relaxed)) {
    apply_button(left_button());
  }
  if (mouse_right_.load(std::memory_order_relaxed)) {
    apply_button(right_button());
  }
  if (mouse_middle_.load(std::memory_order_relaxed)) {
    apply_button(middle_button());
  }
}

MousehookConfig& MousehookConfig::Get() {
  static MousehookConfig instance;
  return instance;
}

void MousehookConfig::Load(const std::filesystem::path& path) {
  path_ = path;

  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    // No file yet - defaults stand, written out on the first Save().
    return;
  }

  FILE* file = xe::filesystem::OpenFile(path_, "rb");
  if (!file) {
    XELOGW("Mousehook: cannot open {}", xe::path_to_utf8(path_));
    return;
  }

  std::string text;
  char buffer[4096];
  size_t read = 0;
  while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, read);
  }
  fclose(file);

  rapidjson::Document doc;
  doc.Parse(text.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    XELOGW("Mousehook: {} is not valid JSON, using defaults",
           xe::path_to_utf8(path_));
    return;
  }

  if (doc.HasMember("enabled") && doc["enabled"].IsBool()) {
    set_enabled(doc["enabled"].GetBool());
  }
  if (doc.HasMember("sensitivity") && doc["sensitivity"].IsNumber()) {
    set_sensitivity(std::clamp(doc["sensitivity"].GetDouble(), 0.05, 20.0));
  }
  if (doc.HasMember("invert_y") && doc["invert_y"].IsBool()) {
    set_invert_y(doc["invert_y"].GetBool());
  }
  if (doc.HasMember("swap_thumbsticks") && doc["swap_thumbsticks"].IsBool()) {
    set_swap_thumbsticks(doc["swap_thumbsticks"].GetBool());
  }
  if (doc.HasMember("user_index") && doc["user_index"].IsUint()) {
    set_user_index(std::min(doc["user_index"].GetUint(), 3u));
  }
  if (doc.HasMember("deadzone_compensation") &&
      doc["deadzone_compensation"].IsNumber()) {
    set_deadzone_compensation(
        std::clamp(doc["deadzone_compensation"].GetDouble(), 0.0, 0.95));
  }
  if (doc.HasMember("pixels_per_full_deflection") &&
      doc["pixels_per_full_deflection"].IsNumber()) {
    set_pixels_per_full_deflection(
        std::clamp(doc["pixels_per_full_deflection"].GetDouble(), 1.0, 500.0));
  }
  if (doc.HasMember("left_button") && doc["left_button"].IsInt()) {
    set_left_button(ActionFromInt(doc["left_button"].GetInt(), left_button()));
  }
  if (doc.HasMember("right_button") && doc["right_button"].IsInt()) {
    set_right_button(
        ActionFromInt(doc["right_button"].GetInt(), right_button()));
  }
  if (doc.HasMember("middle_button") && doc["middle_button"].IsInt()) {
    set_middle_button(
        ActionFromInt(doc["middle_button"].GetInt(), middle_button()));
  }

  if (doc.HasMember("keybinds") && doc["keybinds"].IsObject()) {
    std::lock_guard<std::mutex> lock(keybinds_mutex_);
    keybinds_.clear();
    for (auto it = doc["keybinds"].MemberBegin();
         it != doc["keybinds"].MemberEnd(); ++it) {
      if (it->name.IsString() && it->value.IsString()) {
        keybinds_[it->name.GetString()] = it->value.GetString();
      }
    }
    bindings_generation_.fetch_add(1, std::memory_order_release);
  }

  XELOGI("Mousehook: loaded {}", xe::path_to_utf8(path_));
}

void MousehookConfig::Save() const {
  if (path_.empty()) {
    return;
  }

  rapidjson::Document doc;
  doc.SetObject();
  auto& alloc = doc.GetAllocator();

  doc.AddMember("enabled", enabled(), alloc);
  doc.AddMember("sensitivity", sensitivity(), alloc);
  doc.AddMember("invert_y", invert_y(), alloc);
  doc.AddMember("swap_thumbsticks", swap_thumbsticks(), alloc);
  doc.AddMember("user_index", user_index(), alloc);
  doc.AddMember("deadzone_compensation", deadzone_compensation(), alloc);
  doc.AddMember("pixels_per_full_deflection", pixels_per_full_deflection(),
                alloc);
  doc.AddMember("left_button", static_cast<int>(left_button()), alloc);
  doc.AddMember("right_button", static_cast<int>(right_button()), alloc);
  doc.AddMember("middle_button", static_cast<int>(middle_button()), alloc);

  {
    rapidjson::Value binds(rapidjson::kObjectType);
    std::lock_guard<std::mutex> lock(keybinds_mutex_);
    for (const auto& [name, keys] : keybinds_) {
      binds.AddMember(rapidjson::Value(name.c_str(), alloc).Move(),
                      rapidjson::Value(keys.c_str(), alloc).Move(), alloc);
    }
    doc.AddMember("keybinds", binds, alloc);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  xe::filesystem::CreateParentFolder(path_);

  FILE* file = xe::filesystem::OpenFile(path_, "wb");
  if (!file) {
    XELOGW("Mousehook: cannot write {}", xe::path_to_utf8(path_));
    return;
  }
  fwrite(buffer.GetString(), 1, buffer.GetSize(), file);
  fclose(file);
}

}  // namespace hid
}  // namespace xe
