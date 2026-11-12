/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/fiber.h"

#include <sys/mman.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "xenia/base/logging.h"

namespace xe {
namespace fiber {

namespace {

struct PosixFiber {
  ucontext_t context;
  // Null for a thread converted into a fiber: it runs on the stack the
  // thread already had, which this did not map and must not unmap.
  void* stack = nullptr;
  size_t stack_size = 0;
  StartRoutine proc = nullptr;
  void* param = nullptr;
};

// Fibers belong to the thread that made them; nothing here crosses threads.
thread_local PosixFiber* tl_current = nullptr;

// makecontext passes int-sized arguments, so the fiber arrives in halves.
void FiberTrampoline(unsigned int low, unsigned int high) {
  auto fiber = reinterpret_cast<PosixFiber*>(
      (static_cast<uintptr_t>(high) << 32) | static_cast<uintptr_t>(low));
  fiber->proc(fiber->param);
  // uc_link is deliberately null: a fiber procedure here is an endless loop
  // that only ever leaves by switching away, so returning is a bug rather
  // than something to let quietly end the thread.
  XELOGE("fiber: a fiber procedure returned instead of switching away");
  std::abort();
}

}  // namespace

Handle ConvertThreadToFiber() {
  if (tl_current) {
    return tl_current;
  }
  auto fiber = new PosixFiber();
  if (getcontext(&fiber->context) != 0) {
    XELOGE("fiber: getcontext failed for the calling thread");
    delete fiber;
    return nullptr;
  }
  tl_current = fiber;
  return fiber;
}

bool IsThreadAFiber() { return tl_current != nullptr; }

Handle Create(size_t commit_bytes, size_t reserve_bytes, StartRoutine proc,
              void* param) {
  size_t stack_size = std::max(reserve_bytes, commit_bytes);
  if (!stack_size) {
    stack_size = 1024 * 1024;
  }

  // MAP_NORESERVE with lazy paging is what the commit/reserve split buys on
  // Windows: the address space is taken now, the pages when they are touched.
  void* stack =
      mmap(nullptr, stack_size, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_NORESERVE, -1, 0);
  if (stack == MAP_FAILED) {
    XELOGE("fiber: could not map a {} byte stack", stack_size);
    return nullptr;
  }

  auto fiber = new PosixFiber();
  fiber->stack = stack;
  fiber->stack_size = stack_size;
  fiber->proc = proc;
  fiber->param = param;

  if (getcontext(&fiber->context) != 0) {
    XELOGE("fiber: getcontext failed");
    munmap(stack, stack_size);
    delete fiber;
    return nullptr;
  }
  fiber->context.uc_stack.ss_sp = stack;
  fiber->context.uc_stack.ss_size = stack_size;
  fiber->context.uc_link = nullptr;

  const auto address = reinterpret_cast<uintptr_t>(fiber);
  makecontext(&fiber->context, reinterpret_cast<void (*)()>(FiberTrampoline), 2,
              static_cast<unsigned int>(address & 0xFFFFFFFFu),
              static_cast<unsigned int>(address >> 32));
  return fiber;
}

void Delete(Handle handle) {
  auto fiber = static_cast<PosixFiber*>(handle);
  if (!fiber) {
    return;
  }
  if (fiber == tl_current) {
    XELOGE("fiber: refusing to delete the fiber that is running");
    return;
  }
  if (fiber->stack) {
    munmap(fiber->stack, fiber->stack_size);
  }
  delete fiber;
}

void SwitchTo(Handle handle) {
  auto target = static_cast<PosixFiber*>(handle);
  auto previous = tl_current;
  if (!target || target == previous) {
    return;
  }
  if (!previous) {
    XELOGE("fiber: switching from a thread that is not a fiber");
    return;
  }
  tl_current = target;
  swapcontext(&previous->context, &target->context);
  // Back again, so this fiber is the one running. A fiber restarted through
  // XE_FIBER_RESTORE never arrives here - it jumps to the top of its
  // procedure instead, with tl_current already pointing at it.
  tl_current = previous;
}

Handle Current() { return tl_current; }

}  // namespace fiber
}  // namespace xe
