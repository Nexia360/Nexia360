/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_XHTTP_H_
#define XENIA_KERNEL_XAM_XAM_XHTTP_H_

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/kernel/xnet.h"

namespace xe {
namespace kernel {
namespace xam {

enum class XHttpHandleType : uint32_t {
  kSession = XHTTP_HANDLE_TYPE_SESSION,
  kConnect = XHTTP_HANDLE_TYPE_CONNECT,
  kRequest = XHTTP_HANDLE_TYPE_REQUEST,
};

struct XHttpPerfCounters {
  xe::be<uint32_t> bytes_sent;
  xe::be<uint32_t> bytes_received;
  xe::be<uint32_t> requests_sent;
  xe::be<uint32_t> responses_received;
  xe::be<uint32_t> connects;
  xe::be<uint32_t> name_resolutions;
  xe::be<uint32_t> errors;
};
static_assert_size(XHttpPerfCounters, 0x1C);

struct XHttpHandle {
  XHttpHandleType type;
  uint32_t handle = 0;
  uint32_t parent = 0;
  uint32_t context = 0;
  uint32_t callback = 0;
  uint32_t callback_flags = 0;
  std::map<uint32_t, uint32_t> options;

  std::string agent;
  uint32_t access_type = XHTTP_ACCESS_TYPE_DEFAULT_PROXY;
  std::string proxy;
  std::string proxy_bypass;
  uint32_t resolve_timeout = 0;
  uint32_t connect_timeout = 60000;
  uint32_t send_timeout = 30000;
  uint32_t receive_timeout = 30000;

  std::string host;
  uint16_t port = 0;

  std::string verb;
  std::string object_name;
  std::string version;
  std::string referrer;
  uint32_t flags = 0;
  bool secure = false;
  std::vector<std::string> request_headers;
  std::vector<uint8_t> request_body;
  uint32_t expected_body_length = 0;
  std::string user_name;
  std::string password;

  bool sent = false;
  bool received = false;
  uint32_t status_code = 0;
  std::string status_text;
  std::string raw_headers;
  std::vector<uint8_t> response_body;
  size_t read_pos = 0;
  uint32_t auth_schemes = 0;
};

// One queued async completion. An ASYNC session reports every completion
// through the guest status callback instead of returning it from the call, and
// the guest pumps them by calling XHttpDoWork -- so the callback runs on the
// thread that pumps, which is what the guest's worker expects.
struct XHttpNotification {
  uint32_t session = 0;      // session the request belongs to (DoWork's handle)
  uint32_t handle = 0;       // request handle, passed back as hInternet
  uint32_t callback = 0;     // guest callback address
  uint32_t context = 0;      // the context the guest gave XHttpSendRequest
  uint32_t status = 0;       // XHTTP_CALLBACK_STATUS_*
  uint32_t info_ptr = 0;     // lpvStatusInformation
  uint32_t info_length = 0;  // dwStatusInformationLength
  uint32_t free_info = 0;    // system-heap block to release after dispatch
};

class XHttpManager {
 public:
  static XHttpManager* Get();

  uint32_t Create(XHttpHandleType type, uint32_t parent);
  XHttpHandle* Lookup(uint32_t handle);
  XHttpHandle* Lookup(uint32_t handle, XHttpHandleType type);
  bool Close(uint32_t handle);
  void CloseAll();

  XHttpHandle* SessionOf(XHttpHandle* handle);
  XHttpHandle* ConnectOf(XHttpHandle* handle);

  std::recursive_mutex& mutex() { return mutex_; }
  XHttpPerfCounters& counters() { return counters_; }

  // True when the request's session was opened with XHTTP_FLAG_ASYNC.
  bool IsAsync(XHttpHandle* handle);
  // Queues a completion for a request. No-op unless the session is async and
  // the guest asked for this notification in its callback flags.
  void Notify(XHttpHandle* request, uint32_t status, uint32_t info_ptr,
              uint32_t info_length, uint32_t free_info = 0);
  // Takes the next completion for `session` (or any, when session is 0).
  bool TakeNotification(uint32_t session, XHttpNotification& out);

 private:
  std::recursive_mutex mutex_;
  std::deque<XHttpNotification> notifications_;
  std::unordered_map<uint32_t, std::unique_ptr<XHttpHandle>> handles_;
  uint32_t next_handle_ = 0x48000001;
  XHttpPerfCounters counters_ = {};
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_XHTTP_H_
