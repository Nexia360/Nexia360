/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_HOST_KEYBOARD_CAPTURE_H_
#define XENIA_BASE_HOST_KEYBOARD_CAPTURE_H_

#include <atomic>

namespace xe {

// How many on-screen keyboards are up. While any is, the host keyboard is
// typing into it: no key press may reach the guest's keyboard driver,
// mousehook's key bindings or the emulator's own hotkeys. Lives in base, header
// only, because ui raises it and hid reads it, and neither links the other.
inline std::atomic<int> host_keyboard_capture_count{0};

inline bool IsHostKeyboardCaptured() {
  return host_keyboard_capture_count.load(std::memory_order_acquire) > 0;
}

}  // namespace xe

#endif  // XENIA_BASE_HOST_KEYBOARD_CAPTURE_H_
