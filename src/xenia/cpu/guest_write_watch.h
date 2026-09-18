/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_WRITE_WATCH_H_
#define XENIA_CPU_GUEST_WRITE_WATCH_H_

namespace xe {
namespace cpu {

// TEMPORARY INSTRUMENTATION - delete once the question it answers is answered.
//
// Logs EVERY write to one guest address, with the guest caller that made it,
// using a hardware data breakpoint (x86 debug registers). Enable with
//   --guest_write_watch=0x92331B9C
//
// Why a debug register and not page protection: re-arming a protected page
// after each fault needs a single-step dance, so it can only reliably catch the
// FIRST write. DR0 traps exactly four bytes, exactly on write, once per store,
// and leaves the neighbouring globals on the page alone.
//
// Debug registers are per-thread, so every guest thread arms itself as it
// starts executing.
void ArmGuestWriteWatchForCurrentThread();

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_WRITE_WATCH_H_
