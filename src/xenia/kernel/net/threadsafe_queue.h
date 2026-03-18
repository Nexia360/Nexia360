/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NET_THREADSAFE_QUEUE_H_
#define XENIA_KERNEL_NET_THREADSAFE_QUEUE_H_

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace xe {
namespace kernel {
namespace net {

// Simple thread-safe queue using standard library primitives.
// Matches Xenia's existing patterns (std::mutex + std::condition_variable).
template <typename T>
class ThreadSafeQueue {
 public:
  ThreadSafeQueue() = default;
  ~ThreadSafeQueue() = default;

  // Non-copyable, non-movable
  ThreadSafeQueue(const ThreadSafeQueue&) = delete;
  ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

  // Push item to queue (never blocks, always succeeds)
  void Push(T item) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push(std::move(item));
    }
    cv_.notify_one();
  }

  // Push multiple items atomically
  void PushBulk(std::vector<T> items) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& item : items) {
        queue_.push(std::move(item));
      }
    }
    cv_.notify_all();
  }

  // Try to pop item without blocking
  // Returns true if item was popped, false if queue was empty
  bool TryPop(T& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    item = std::move(queue_.front());
    queue_.pop();
    return true;
  }

  // Try to pop item, returning optional
  std::optional<T> TryPop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return std::nullopt;
    }
    T item = std::move(queue_.front());
    queue_.pop();
    return item;
  }

  // Pop with timeout
  // Returns true if item was popped, false on timeout
  bool WaitPop(T& item, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
      return false;
    }
    item = std::move(queue_.front());
    queue_.pop();
    return true;
  }

  // Pop with timeout, returning optional
  std::optional<T> WaitPop(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
      return std::nullopt;
    }
    T item = std::move(queue_.front());
    queue_.pop();
    return item;
  }

  // Blocking pop (waits indefinitely)
  T WaitPop() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
    if (shutdown_ && queue_.empty()) {
      // Return default-constructed T on shutdown with empty queue
      return T{};
    }
    T item = std::move(queue_.front());
    queue_.pop();
    return item;
  }

  // Pop multiple items (up to max_count)
  // Returns number of items popped
  size_t TryPopBulk(std::vector<T>& items, size_t max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    while (!queue_.empty() && count < max_count) {
      items.push_back(std::move(queue_.front()));
      queue_.pop();
      ++count;
    }
    return count;
  }

  // Check if queue is empty
  bool Empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

  // Get approximate size (may be stale immediately after return)
  size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  // Clear all items from queue
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::queue<T> empty;
    std::swap(queue_, empty);
  }

  // Signal shutdown - wakes all waiting threads
  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutdown_ = true;
    }
    cv_.notify_all();
  }

  // Check if shutdown was signaled
  bool IsShutdown() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutdown_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<T> queue_;
  bool shutdown_ = false;
};

}  // namespace net
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NET_THREADSAFE_QUEUE_H_
