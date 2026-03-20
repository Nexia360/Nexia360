/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/x64_drift_clock.h"

#include <algorithm>
#include <limits>

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

DriftClock::DriftClock() {
  for (int i = 0; i < kMaxThreads; ++i) {
    threads_[i].progress.store(0, std::memory_order_relaxed);
    threads_[i].active.store(false, std::memory_order_relaxed);
  }
}

DriftClockThread* DriftClock::RegisterThread() {
  for (int i = 0; i < kMaxThreads; ++i) {
    bool expected = false;
    if (threads_[i].active.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel)) {
      // Reset progress to current min so new thread doesn't start at 0
      // and drag everyone down.
      threads_[i].progress.store(cached_min_progress_,
                                 std::memory_order_relaxed);
      thread_count_.fetch_add(1, std::memory_order_relaxed);
      return &threads_[i];
    }
  }
  return nullptr;  // All slots full
}

void DriftClock::UnregisterThread(DriftClockThread* thread) {
  if (!thread) return;
  thread->active.store(false, std::memory_order_release);
  thread_count_.fetch_sub(1, std::memory_order_relaxed);
}

void DriftClock::ThreadEnteringWait(DriftClockThread* thread) {
  if (!thread) return;
  thread->active.store(false, std::memory_order_release);
}

void DriftClock::ThreadLeavingWait(DriftClockThread* thread) {
  if (!thread) return;
  // Reset progress to current min so this thread doesn't appear far behind
  // and let other threads race ahead unchecked.
  thread->progress.store(cached_min_progress_, std::memory_order_relaxed);
  thread->active.store(true, std::memory_order_release);
}

uint64_t DriftClock::ComputeAndUpdateMinProgress() {
  uint64_t min_val = std::numeric_limits<uint64_t>::max();
  int active_count = 0;

  for (int i = 0; i < kMaxThreads; ++i) {
    if (threads_[i].active.load(std::memory_order_acquire)) {
      uint64_t p = threads_[i].progress.load(std::memory_order_relaxed);
      if (p < min_val) {
        min_val = p;
      }
      active_count++;
    }
  }

  if (active_count <= 1) {
    // Only one active thread (or none) — no drift possible.
    // Set min to max so the lone thread never blocks.
    cached_min_progress_ = (active_count == 1) ? min_val : cached_min_progress_;
    return std::numeric_limits<uint64_t>::max();
  }

  cached_min_progress_ = min_val;
  return min_val;
}

int DriftClock::active_thread_count() const {
  int count = 0;
  for (int i = 0; i < kMaxThreads; ++i) {
    if (threads_[i].active.load(std::memory_order_acquire)) {
      count++;
    }
  }
  return count;
}

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
