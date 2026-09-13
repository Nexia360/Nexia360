/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Console entry points answered by Nexia's own kernel exports.
//
// XNA's XINPUT module is a thin renaming of XAM's input calls, so these do not
// reimplement anything - they forward through XnaBridge into
// XamInputGetState and friends, which is where the emulator's controller
// handling already lives. Each XAM call takes (user_index, flags, pointer),
// where XNA's takes (PlayerIndex, ref struct), so flags is supplied as zero.
//
// BYTE ORDER IS NOT INCIDENTAL HERE. Nexia's X_INPUT_* are GUEST structures and
// their fields are big-endian; the console's managed code used to run on a
// big-endian CPU and now runs on x64, so it reads them little-endian. Copying
// the bytes across would swap every button mask and thumbstick axis. Each field
// is converted explicitly below rather than memcpy'd.

#include <cstdint>
#include <cstring>

#include "xenia/base/byte_order.h"
#include "xenia/hid/input.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xna/xna_bridge.h"
#include "xenia/kernel/xna/xna_exports.h"

namespace {

using xe::kernel::xna::XnaArg;
using xe::kernel::xna::XnaBridge;

// ErrorCodes, as the managed side reads them.
constexpr uint32_t kSuccess = 0;
constexpr uint32_t kDeviceNotConnected = 0x0000048F;

// Host-side mirrors of the XNA structures: same layout, little-endian, which
// is what the managed code now expects.
#pragma pack(push, 1)
struct HostGamepad {
  uint16_t buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
  int16_t thumb_lx;
  int16_t thumb_ly;
  int16_t thumb_rx;
  int16_t thumb_ry;
};
struct HostState {
  uint32_t packet_number;
  HostGamepad gamepad;
};
struct HostVibration {
  uint16_t left_motor_speed;
  uint16_t right_motor_speed;
};
struct HostCapabilities {
  uint8_t type;
  uint8_t sub_type;
  uint16_t flags;
  HostGamepad gamepad;
  HostVibration vibration;
};
struct HostKeystroke {
  uint16_t virtual_key;
  uint16_t unicode;
  uint16_t flags;
  uint8_t user_index;
  uint8_t hid_code;
};
#pragma pack(pop)

static_assert(sizeof(HostGamepad) == 12, "gamepad layout");
static_assert(sizeof(HostState) == 16, "state layout");
static_assert(sizeof(HostCapabilities) == 20, "capabilities layout");
static_assert(sizeof(HostKeystroke) == 8, "keystroke layout");

void FromGuest(const xe::hid::X_INPUT_GAMEPAD& in, HostGamepad* out) {
  out->buttons = in.buttons;
  out->left_trigger = in.left_trigger;
  out->right_trigger = in.right_trigger;
  out->thumb_lx = in.thumb_lx;
  out->thumb_ly = in.thumb_ly;
  out->thumb_rx = in.thumb_rx;
  out->thumb_ry = in.thumb_ry;
}

// Calls one of XAM's input exports with the (user, flags, buffer) shape they
// all share. `guest` is filled by the call.
bool CallXamInput(const char* name, uint32_t user_index, void* guest,
                  uint32_t size, uint64_t* out_result) {
  const XnaArg args[] = {
      XnaArg::Scalar(user_index),
      XnaArg::Scalar(0),
      XnaArg::Out(guest, size),
  };
  return XnaBridge::Instance().Invoke("xam.xex", name, args, 3, out_result);
}

}  // namespace

extern "C" uint32_t xna_XINPUT_XInput_GetState(uint32_t player, void* out_state) {
  xe::hid::X_INPUT_STATE guest = {};
  uint64_t result = 0;
  if (!CallXamInput("XamInputGetState", player, &guest, sizeof(guest),
                    &result)) {
    xe::kernel::xna::XnaExportUnimplemented("XINPUT!XInput_GetState");
    return kDeviceNotConnected;
  }
  if (out_state) {
    auto* state = reinterpret_cast<HostState*>(out_state);
    state->packet_number = guest.packet_number;
    FromGuest(guest.gamepad, &state->gamepad);
  }
  return static_cast<uint32_t>(result);
}

extern "C" uint32_t xna_XINPUT_XInput_SetState(uint32_t player,
                                               uint32_t vibration_packed) {
  // XNA passes XINPUT_VIBRATION by VALUE: two 16-bit motor speeds, which the
  // x64 convention delivers as one 32-bit register rather than a pointer.
  xe::hid::X_INPUT_VIBRATION guest = {};
  guest.left_motor_speed = static_cast<uint16_t>(vibration_packed & 0xFFFF);
  guest.right_motor_speed = static_cast<uint16_t>(vibration_packed >> 16);

  const XnaArg args[] = {
      XnaArg::Scalar(player),
      XnaArg::Scalar(0),
      XnaArg::In(&guest, sizeof(guest)),
  };
  uint64_t result = 0;
  if (!XnaBridge::Instance().Invoke("xam.xex", "XamInputSetState", args, 3,
                                    &result)) {
    xe::kernel::xna::XnaExportUnimplemented("XINPUT!XInput_SetState");
    return kDeviceNotConnected;
  }
  return static_cast<uint32_t>(result);
}

extern "C" uint32_t xna_XINPUT_XInput_GetCaps(uint32_t player, void* out_caps) {
  xe::hid::X_INPUT_CAPABILITIES guest = {};
  uint64_t result = 0;
  if (!CallXamInput("XamInputGetCapabilities", player, &guest, sizeof(guest),
                    &result)) {
    xe::kernel::xna::XnaExportUnimplemented("XINPUT!XInput_GetCaps");
    return kDeviceNotConnected;
  }
  if (out_caps) {
    auto* caps = reinterpret_cast<HostCapabilities*>(out_caps);
    caps->type = guest.type;
    caps->sub_type = guest.sub_type;
    caps->flags = guest.flags;
    FromGuest(guest.gamepad, &caps->gamepad);
    caps->vibration.left_motor_speed = guest.vibration.left_motor_speed;
    caps->vibration.right_motor_speed = guest.vibration.right_motor_speed;
  }
  return static_cast<uint32_t>(result);
}

extern "C" uint32_t xna_XINPUT_XInput_GetKeyStroke(uint32_t player,
                                                   void* out_keystroke) {
  xe::hid::X_INPUT_KEYSTROKE guest = {};
  uint64_t result = 0;
  if (!CallXamInput("XamInputGetKeystroke", player, &guest, sizeof(guest),
                    &result)) {
    xe::kernel::xna::XnaExportUnimplemented("XINPUT!XInput_GetKeyStroke");
    return kDeviceNotConnected;
  }
  if (out_keystroke) {
    auto* keystroke = reinterpret_cast<HostKeystroke*>(out_keystroke);
    keystroke->virtual_key = guest.virtual_key;
    keystroke->unicode = guest.unicode;
    keystroke->flags = guest.flags;
    keystroke->user_index = guest.user_index;
    keystroke->hid_code = guest.hid_code;
  }
  return static_cast<uint32_t>(result);
}

extern "C" uint32_t xna_XAM_XAM_IsGuideVisible() {
  // Answered directly: the emulator tracks this in C++, so going out through a
  // kernel export and a guest context would be a longer road to the same bool.
  auto* state = xe::kernel::kernel_state();
  if (!state || !state->xam_state()) {
    return 0;
  }
  return state->xam_state()->IsUIActive() ? 1u : 0u;
}
