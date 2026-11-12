/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_FIBER_H_
#define XENIA_BASE_FIBER_H_

#include <cstddef>

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#else
#include <ucontext.h>
#endif  // XE_PLATFORM_WIN32

namespace xe {
namespace fiber {

// A stack and the registers to run on it. One per thread runs at a time and
// every switch is explicit - nothing here schedules anything. The kernel's
// user mode is built on these: user code, the trap handler and the kernel
// each get one, so a trap can leave a stack standing and come back to it.
//
// Windows has fibers of its own and this is a thin wrapper over them.
// Everywhere else they are ucontext, which gives the same two things that
// matter: a switch that keeps both stacks, and a captured start that can be
// jumped back to.

using Handle = void*;

using StartRoutine = void (*)(void* param);

// Makes the calling thread a fiber so it can switch to others, or returns the
// one it already is. Null if it cannot be done.
Handle ConvertThreadToFiber();

bool IsThreadAFiber();

// |reserve_bytes| is address space and |commit_bytes| what is expected to be
// touched. Off Windows a stack is mapped lazily, so the reserve is what gets
// mapped and the commit is only a floor.
Handle Create(size_t commit_bytes, size_t reserve_bytes, StartRoutine proc,
              void* param);

// Frees a fiber and its stack. Never the one running.
void Delete(Handle fiber);

// Runs |fiber| and suspends the caller until something switches back.
void SwitchTo(Handle fiber);

Handle Current();

// Where a fiber's procedure began. Restoring one jumps back to it on the
// stack the fiber already has: the frames above are dropped without
// unwinding, and no stack is freed or made.
//
// These are macros because the capture has to happen in the frame being
// returned to. Inside a helper it would capture the helper's frame, whose
// return address sits below the stack pointer where later frames overwrite
// it - the restore would then return into rubble.
#if XE_PLATFORM_WIN32
using Context = CONTEXT;
#define XE_FIBER_CAPTURE(context) ::RtlCaptureContext(&(context))
#define XE_FIBER_RESTORE(context) ::RtlRestoreContext(&(context), nullptr)
#else
using Context = ucontext_t;
#define XE_FIBER_CAPTURE(context) ::getcontext(&(context))
#define XE_FIBER_RESTORE(context) ::setcontext(&(context))
#endif  // XE_PLATFORM_WIN32

}  // namespace fiber
}  // namespace xe

#endif  // XENIA_BASE_FIBER_H_
