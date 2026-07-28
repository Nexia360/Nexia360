/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_MOUSEHOOK_CONFIG_H_
#define XENIA_HID_MOUSEHOOK_CONFIG_H_

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/hid/input.h"

namespace xe {
namespace hid {

// Mouse-look ("mousehook") settings, persisted to mousehook.json next to the
// other host storage. Deliberately NOT cvars: these are edited live from the
// Console settings UI while a title is running, and are read every input poll
// on the guest thread - hence the atomics.
//
// Button mapping values are X_INPUT_GAMEPAD_* / trigger selectors, see
// MouseButtonAction below.
enum class MouseButtonAction : uint32_t {
  kNone = 0,
  kRightTrigger,     // fire
  kLeftTrigger,      // aim down sights
  kRightThumbPress,  // stick click
  kLeftThumbPress,
  kA,
  kB,
  kX,
  kY,
};

class MousehookConfig {
 public:
  static MousehookConfig& Get();

  // Points the config at <storage_root>/mousehook.json and loads it. A missing
  // or malformed file leaves the defaults in place (and is not an error - the
  // file is written on first Save()).
  void Load(const std::filesystem::path& path);
  void Save() const;

  const std::filesystem::path& path() const { return path_; }

  // --- Live values (read from the guest input thread) ---------------------
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  void set_enabled(bool value) {
    enabled_.store(value, std::memory_order_relaxed);
  }

  // Right-stick deflection per pixel of mouse motion.
  double sensitivity() const {
    return sensitivity_.load(std::memory_order_relaxed);
  }
  void set_sensitivity(double value) {
    sensitivity_.store(value, std::memory_order_relaxed);
  }

  // Controller slot mousehook drives. Mousehook presents a pad on this slot on
  // its own - it does NOT require the keyboard to be enabled for the slot.
  uint32_t user_index() const {
    return user_index_.load(std::memory_order_relaxed);
  }
  void set_user_index(uint32_t value) {
    user_index_.store(value, std::memory_order_relaxed);
  }

  // When set, mouse motion drives the LEFT thumbstick instead of the right.
  bool swap_thumbsticks() const {
    return swap_thumbsticks_.load(std::memory_order_relaxed);
  }
  void set_swap_thumbsticks(bool value) {
    swap_thumbsticks_.store(value, std::memory_order_relaxed);
  }

  bool invert_y() const { return invert_y_.load(std::memory_order_relaxed); }
  void set_invert_y(bool value) {
    invert_y_.store(value, std::memory_order_relaxed);
  }

  // Fraction of full deflection (0..1) that any non-zero mouse motion is
  // lifted to, so slow movement isn't swallowed by the title's stick deadzone
  // (~0.27 of full range on a stock pad). 0 disables the compensation.
  double deadzone_compensation() const {
    return deadzone_compensation_.load(std::memory_order_relaxed);
  }
  void set_deadzone_compensation(double value) {
    deadzone_compensation_.store(value, std::memory_order_relaxed);
  }

  // Pixels of motion in one poll that equal full stick deflection at
  // sensitivity 1.0. Lower = twitchier.
  double pixels_per_full_deflection() const {
    return pixels_per_full_deflection_.load(std::memory_order_relaxed);
  }
  void set_pixels_per_full_deflection(double value) {
    pixels_per_full_deflection_.store(value, std::memory_order_relaxed);
  }

  MouseButtonAction left_button() const {
    return left_button_.load(std::memory_order_relaxed);
  }
  void set_left_button(MouseButtonAction value) {
    left_button_.store(value, std::memory_order_relaxed);
  }

  MouseButtonAction right_button() const {
    return right_button_.load(std::memory_order_relaxed);
  }
  void set_right_button(MouseButtonAction value) {
    right_button_.store(value, std::memory_order_relaxed);
  }

  MouseButtonAction middle_button() const {
    return middle_button_.load(std::memory_order_relaxed);
  }
  void set_middle_button(MouseButtonAction value) {
    middle_button_.store(value, std::memory_order_relaxed);
  }

  // --- Keyboard bindings ---------------------------------------------------
  // Keyed by the binding's cvar name (e.g. "keybind_a"), value is the same
  // space-separated key list the cvars use ("^A _D 0x08"). Seeded from the
  // cvar defaults on first run so the file starts out matching current
  // behaviour. Guarded by a mutex - edited from the UI thread, read by the
  // input driver when it rebuilds its table.
  std::map<std::string, std::string> keybinds() const {
    std::lock_guard<std::mutex> lock(keybinds_mutex_);
    return keybinds_;
  }
  std::string keybind(const std::string& name) const {
    std::lock_guard<std::mutex> lock(keybinds_mutex_);
    auto it = keybinds_.find(name);
    return it == keybinds_.end() ? std::string() : it->second;
  }
  void set_keybind(const std::string& name, const std::string& keys) {
    {
      std::lock_guard<std::mutex> lock(keybinds_mutex_);
      keybinds_[name] = keys;
    }
    bindings_generation_.fetch_add(1, std::memory_order_release);
  }
  bool has_keybinds() const {
    std::lock_guard<std::mutex> lock(keybinds_mutex_);
    return !keybinds_.empty();
  }

  // Bumped whenever a binding changes; the input driver compares this against
  // the generation it last built its table from and rebuilds on a mismatch.
  uint32_t bindings_generation() const {
    return bindings_generation_.load(std::memory_order_acquire);
  }

  // --- Live mouse state ----------------------------------------------------
  // Fed by the window listener (UI thread), drained by ApplyToGamepad on the
  // guest thread. Held here rather than in the WinKey driver because the mouse
  // must augment whichever driver owns the slot - with a real controller
  // plugged in, InputSystem::GetState never reaches the WinKey driver at all.
  // Motion accumulates into monotonic totals. Reading is NON-DESTRUCTIVE: a
  // consumer takes the difference against its own cursor, so one reader can
  // never swallow motion belonging to another. Previously this was an
  // exchange(0), which meant whichever caller asked first consumed everything
  // and the guest got nothing.
  void AccumulateMouseMotion(int32_t dx, int32_t dy) {
    mouse_dx_.fetch_add(dx, std::memory_order_relaxed);
    mouse_dy_.fetch_add(dy, std::memory_order_relaxed);
  }
  void set_mouse_left(bool down) {
    mouse_left_.store(down, std::memory_order_relaxed);
  }
  void set_mouse_right(bool down) {
    mouse_right_.store(down, std::memory_order_relaxed);
  }
  void set_mouse_middle(bool down) {
    mouse_middle_.store(down, std::memory_order_relaxed);
  }
  // Motion since the caller's cursor was last advanced. Pass consume=false
  // to look without taking - a UI poll must never remove motion that the
  // guest has not seen yet.
  void ReadMouseMotion(int32_t* out_dx, int32_t* out_dy, bool consume) {
    const int32_t total_x = mouse_dx_.load(std::memory_order_relaxed);
    const int32_t total_y = mouse_dy_.load(std::memory_order_relaxed);

    *out_dx = total_x - mouse_cursor_x_;
    *out_dy = total_y - mouse_cursor_y_;

    if (consume) {
      mouse_cursor_x_ = total_x;
      mouse_cursor_y_ = total_y;
    }
  }

  void ResetMouseState() {
    mouse_dx_.store(0, std::memory_order_relaxed);
    mouse_dy_.store(0, std::memory_order_relaxed);
    mouse_cursor_x_ = 0;
    mouse_cursor_y_ = 0;
    mouse_left_.store(false, std::memory_order_relaxed);
    mouse_right_.store(false, std::memory_order_relaxed);
    mouse_middle_.store(false, std::memory_order_relaxed);
  }

  // Folds the accumulated motion/buttons AND the keyboard bindings into an
  // already-populated gamepad state. consume_motion=false leaves the motion
  // for the guest - use it for any poll that is not the guest's own.
  void ApplyToGamepad(X_INPUT_GAMEPAD* gamepad, bool consume_motion = true);

  // Seeds the binding table with mousehook's defaults (WASD etc). Called when
  // mousehook.json has no keybinds yet.
  void SeedDefaultKeybinds();

 private:
  MousehookConfig() = default;

  std::filesystem::path path_;

  std::atomic<bool> enabled_{false};
  std::atomic<double> sensitivity_{1.0};
  std::atomic<bool> invert_y_{false};
  std::atomic<bool> swap_thumbsticks_{false};
  std::atomic<uint32_t> user_index_{0};
  std::atomic<double> pixels_per_full_deflection_{25.0};
  // 8689/32767 - the stock right-stick deadzone, i.e. just enough to clear it.
  std::atomic<double> deadzone_compensation_{0.27};

  std::atomic<MouseButtonAction> left_button_{MouseButtonAction::kRightTrigger};
  std::atomic<MouseButtonAction> right_button_{MouseButtonAction::kLeftTrigger};
  std::atomic<MouseButtonAction> middle_button_{
      MouseButtonAction::kRightThumbPress};

  // Monotonic totals, never reset by a read.
  std::atomic<int32_t> mouse_dx_{0};
  std::atomic<int32_t> mouse_dy_{0};
  // How much of the total the guest has already been given.
  int32_t mouse_cursor_x_ = 0;
  int32_t mouse_cursor_y_ = 0;
  std::atomic<bool> mouse_left_{false};
  std::atomic<bool> mouse_right_{false};
  std::atomic<bool> mouse_middle_{false};

  mutable std::mutex keybinds_mutex_;
  std::map<std::string, std::string> keybinds_;
  std::atomic<uint32_t> bindings_generation_{0};

  // Parsed form of keybinds_, rebuilt when the generation changes. Only touched
  // on the guest thread inside ApplyToGamepad.
  struct ParsedBind {
    uint16_t input_vk;
    bool uppercase;
    bool lowercase;
    uint16_t output_key;  // ui::VirtualKey::kXInputPad*
  };
  std::vector<ParsedBind> parsed_binds_;
  uint32_t parsed_generation_ = 0xFFFFFFFF;
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_MOUSEHOOK_CONFIG_H_
