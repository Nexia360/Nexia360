/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/xuid_handles.h"

#include <map>
#include <mutex>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#ifdef XE_PLATFORM_WIN32
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace xe {
namespace kernel {

namespace {

// Registered from hub worker threads and read from guest threads.
std::mutex handles_mutex_;
std::map<uint32_t, uint64_t> handle_to_xuid_;

}  // namespace

uint32_t XuidToHandle(uint64_t xuid) {
  if (!xuid) {
    return 0;
  }

  const uint8_t b1 = static_cast<uint8_t>((xuid >> 16) & 0xFF);
  const uint8_t b2 = static_cast<uint8_t>((xuid >> 8) & 0xFF);
  uint8_t b3 = static_cast<uint8_t>(xuid & 0xFF);
  if (b3 == 0 || b3 == 0xFF) {
    b3 = 1;
  }

  return htonl((static_cast<uint32_t>(10) << 24) |
               (static_cast<uint32_t>(b1) << 16) |
               (static_cast<uint32_t>(b2) << 8) | b3);
}

void RegisterXuidHandle(uint64_t xuid) {
  const uint32_t handle = XuidToHandle(xuid);
  if (!handle) {
    return;
  }

  std::lock_guard lock(handles_mutex_);

  // First registration wins: only the low three bytes are used, so two XUIDs
  // can collide, and an established peer must not be hijacked by a newcomer.
  auto it = handle_to_xuid_.find(handle);
  if (it != handle_to_xuid_.end()) {
    if (it->second != xuid) {
      XELOGE("Transport: handle collision - {:016X} and {:016X}", it->second,
             xuid);
    }
    return;
  }

  handle_to_xuid_[handle] = xuid;
}

uint64_t XuidForHandle(uint32_t handle) {
  std::lock_guard lock(handles_mutex_);
  auto it = handle_to_xuid_.find(handle);
  return it == handle_to_xuid_.end() ? 0 : it->second;
}

size_t RegisteredHandleCount() {
  std::lock_guard lock(handles_mutex_);
  return handle_to_xuid_.size();
}

}  // namespace kernel
}  // namespace xe
