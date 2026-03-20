/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_X64_X64_DRIFT_CLOCK_H_
#define XENIA_CPU_BACKEND_X64_X64_DRIFT_CLOCK_H_

#include <atomic>
#include <cstdint>

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

// Tracks per-thread progress and enforces bounded drift between guest threads.
//
// Each guest thread increments its own progress counter at JIT sync points
// (function prologues, backward branches). The drift clock maintains the
// minimum progress across all active (non-waiting) threads. A thread that
// races too far ahead of the minimum is forced to yield until slower threads
// catch up.
//
// Threads that enter kernel waits are excluded from the minimum calculation
// to prevent the convoy problem (one sleeping thread stalling everyone).

struct DriftClockThread {
  // This thread's progress counter. Incremented at sync points.
  // Accessed from JIT code — must be at a stable address.
  std::atomic<uint64_t> progress{0};

  // True if this thread is actively running guest code.
  // False if in a kernel wait, suspended, or exited.
  std::atomic<bool> active{false};
};

class DriftClock {
 public:
  static constexpr int kMaxThreads = 32;

  DriftClock();

  // Register a thread slot. Returns a pointer to the thread's progress atomic.
  // The returned DriftClockThread is owned by the DriftClock.
  DriftClockThread* RegisterThread();

  // Unregister a thread slot.
  void UnregisterThread(DriftClockThread* thread);

  // Mark a thread as entering a wait (excluded from min calculation).
  void ThreadEnteringWait(DriftClockThread* thread);

  // Mark a thread as leaving a wait (re-included in min calculation).
  // Resets its progress to the current minimum so it doesn't appear "behind."
  void ThreadLeavingWait(DriftClockThread* thread);

  // Compute the current minimum progress across all active threads
  // and update the cached value. Returns the new minimum.
  uint64_t ComputeAndUpdateMinProgress();

  // Pointer to the cached min_progress value, readable from JIT code.
  uint64_t* min_progress_ptr() { return &cached_min_progress_; }

  // Number of currently active threads (not in waits).
  int active_thread_count() const;

 private:
  DriftClockThread threads_[kMaxThreads];
  std::atomic<int> thread_count_{0};

  // Cached minimum progress, updated by threads when they yield.
  // Not perfectly precise — threads read this without locking for speed.
  uint64_t cached_min_progress_{0};
};

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_X64_X64_DRIFT_CLOCK_H_
