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

#include "xenia/base/byte_order.h"
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
std::map<uint64_t, uint32_t> xuid_to_handle_;

uint32_t MakeHandle(uint64_t bytes) {
  const uint8_t b1 = static_cast<uint8_t>((bytes >> 16) & 0xFF);
  const uint8_t b2 = static_cast<uint8_t>((bytes >> 8) & 0xFF);
  uint8_t b3 = static_cast<uint8_t>(bytes & 0xFF);
  if (b3 == 0 || b3 == 0xFF) {
    b3 = 1;
  }

  return htonl((static_cast<uint32_t>(10) << 24) |
               (static_cast<uint32_t>(b1) << 16) |
               (static_cast<uint32_t>(b2) << 8) | b3);
}

// Attempt 0 is the XUID's own low three bytes, so the handle every console
// derives for a player is the same one. Later attempts only come up when that
// handle already belongs to somebody else.
uint32_t HandleCandidate(uint64_t xuid, uint32_t attempt) {
  if (!attempt) {
    return MakeHandle(xuid);
  }

  uint64_t mixed =
      xuid * 0x9E3779B97F4A7C15ULL + attempt * 0xBF58476D1CE4E5B9ULL;
  mixed ^= mixed >> 31;
  mixed *= 0x94D049BB133111EBULL;
  mixed ^= mixed >> 29;

  return MakeHandle(mixed);
}

constexpr uint32_t kHandleAttempts = 64;

}  // namespace

uint32_t XuidToHandle(uint64_t xuid) {
  return xuid ? HandleCandidate(xuid, 0) : 0;
}

uint32_t HandleForXuid(uint64_t xuid) {
  if (!xuid) {
    return 0;
  }

  std::lock_guard lock(handles_mutex_);

  const auto owned = xuid_to_handle_.find(xuid);
  if (owned != xuid_to_handle_.end()) {
    return owned->second;
  }

  // A handle stays with the XUID that took it. Handing the same one to a second
  // player made the title see one peer at one address, so the established peer
  // was dropped and could not rejoin while the newcomer was there.
  for (uint32_t attempt = 0; attempt < kHandleAttempts; ++attempt) {
    const uint32_t handle = HandleCandidate(xuid, attempt);

    const auto it = handle_to_xuid_.find(handle);
    if (it != handle_to_xuid_.end()) {
      continue;
    }

    if (attempt) {
      XELOGE("Transport: handle {:08X} collision - {:016X} moved to {:08X}",
             xe::byte_swap(XuidToHandle(xuid)), xuid, xe::byte_swap(handle));
    }

    handle_to_xuid_[handle] = xuid;
    xuid_to_handle_[xuid] = handle;
    return handle;
  }

  XELOGE("Transport: no free handle for {:016X}", xuid);
  return XuidToHandle(xuid);
}

void RegisterXuidHandle(uint64_t xuid) { HandleForXuid(xuid); }

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
