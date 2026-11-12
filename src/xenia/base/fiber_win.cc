/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/fiber.h"

#include "xenia/base/logging.h"

namespace xe {
namespace fiber {

// FIBER_FLAG_FLOAT_SWITCH throughout: the guest's floating point and vector
// state lives in these registers between switches, and without it the x87
// and MMX state is shared rather than saved.

Handle ConvertThreadToFiber() {
  Handle fiber = ::ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
  if (!fiber) {
    XELOGE("fiber: ConvertThreadToFiberEx failed ({})", GetLastError());
  }
  return fiber;
}

bool IsThreadAFiber() { return ::IsThreadAFiber() != FALSE; }

Handle Create(size_t commit_bytes, size_t reserve_bytes, StartRoutine proc,
              void* param) {
  // Xenia is x64 and ARM64 only, where there is one calling convention and
  // WINAPI is it, so the start routine needs no adapter.
  Handle fiber = ::CreateFiberEx(
      static_cast<SIZE_T>(commit_bytes), static_cast<SIZE_T>(reserve_bytes),
      FIBER_FLAG_FLOAT_SWITCH, reinterpret_cast<LPFIBER_START_ROUTINE>(proc),
      param);
  if (!fiber) {
    XELOGE("fiber: CreateFiberEx failed ({})", GetLastError());
  }
  return fiber;
}

void Delete(Handle fiber) {
  if (fiber) {
    ::DeleteFiber(fiber);
  }
}

void SwitchTo(Handle fiber) { ::SwitchToFiber(fiber); }

Handle Current() { return ::GetCurrentFiber(); }

}  // namespace fiber
}  // namespace xe
