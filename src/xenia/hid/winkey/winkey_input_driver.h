/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_WINKEY_WINKEY_INPUT_DRIVER_H_
#define XENIA_HID_WINKEY_WINKEY_INPUT_DRIVER_H_

#include <atomic>
#include <queue>

#include "xenia/base/mutex.h"
#include "xenia/hid/input_driver.h"
#include "xenia/ui/virtual_key.h"

namespace xe {
namespace hid {
namespace winkey {

enum class KeyboardMode { Disabled, Enabled, Passthrough };

class WinKeyInputDriver final : public InputDriver {
 public:
  explicit WinKeyInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~WinKeyInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  virtual InputType GetInputType() const override;

 protected:
  struct KeyEvent {
    ui::VirtualKey virtual_key = ui::VirtualKey::kNone;
    int repeat_count = 0;
    bool transition = false;  // going up(false) or going down(true)
    bool prev_state = false;  // down(true) or up(false)
  };

  struct KeyBinding {
    ui::VirtualKey input_key = ui::VirtualKey::kNone;
    ui::VirtualKey output_key = ui::VirtualKey::kNone;
    bool uppercase = false;
    bool lowercase = false;
  };

  class WinKeyWindowInputListener final : public ui::WindowInputListener {
   public:
    explicit WinKeyWindowInputListener(WinKeyInputDriver& driver)
        : driver_(driver) {}

    void OnKeyDown(ui::KeyEvent& e) override;
    void OnKeyUp(ui::KeyEvent& e) override;
    void OnMouseDown(ui::MouseEvent& e) override;
    void OnMouseMove(ui::MouseEvent& e) override;
    void OnMouseUp(ui::MouseEvent& e) override;

   private:
    WinKeyInputDriver& driver_;
  };

  void ParseKeyBinding(ui::VirtualKey virtual_key,
                       const std::string_view description,
                       const std::string_view binding);

  void OnKey(ui::KeyEvent& e, bool is_down);

  // Mousehook: lock the cursor to the window centre and accumulate relative
  // mouse motion (consumed in GetState as a right-thumbstick deflection), plus
  // mouse-button state. Gated by cvars::mousehook.
  void OnMouseMove(ui::MouseEvent& e);
  void OnMouseButton(ui::MouseEvent& e, bool is_down);

  WinKeyWindowInputListener window_input_listener_;

  xe::global_critical_region global_critical_region_;
  std::queue<KeyEvent> key_events_;
  std::vector<KeyBinding> key_bindings_;
  uint8_t key_map_[256];
  uint32_t packet_number_ = 1;

  // Mousehook state (written on the UI thread, read on the guest thread).
  std::atomic<int32_t> mouse_dx_{0};
  std::atomic<int32_t> mouse_dy_{0};
  std::atomic<bool> mouse_left_{false};
  std::atomic<bool> mouse_right_{false};
  std::atomic<bool> mouse_middle_{false};
  bool cursor_hidden_ = false;
};

}  // namespace winkey
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_WINKEY_WINKEY_INPUT_DRIVER_H_
