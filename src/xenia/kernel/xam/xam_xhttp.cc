/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

#include "xenia/base/platform.h"

#include <curl/curl.h>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/string_util.h"
#include "xenia/base/utf8.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_xhttp.h"
#include "xenia/kernel/xnet.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

XHttpManager* XHttpManager::Get() {
  static XHttpManager instance;
  return &instance;
}

uint32_t XHttpManager::Create(XHttpHandleType type, uint32_t parent) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto handle = std::make_unique<XHttpHandle>();
  handle->type = type;
  handle->handle = next_handle_++;
  handle->parent = parent;
  const uint32_t id = handle->handle;
  handles_.emplace(id, std::move(handle));
  return id;
}

XHttpHandle* XHttpManager::Lookup(uint32_t handle) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto it = handles_.find(handle);
  return it == handles_.end() ? nullptr : it->second.get();
}

XHttpHandle* XHttpManager::Lookup(uint32_t handle, XHttpHandleType type) {
  auto* object = Lookup(handle);
  if (object && object->type != type) {
    return nullptr;
  }
  return object;
}

bool XHttpManager::Close(uint32_t handle) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto it = handles_.find(handle);
  if (it == handles_.end()) {
    return false;
  }
  std::vector<uint32_t> children;
  for (const auto& entry : handles_) {
    if (entry.second->parent == handle) {
      children.push_back(entry.first);
    }
  }
  handles_.erase(it);
  for (uint32_t child : children) {
    Close(child);
  }
  return true;
}

void XHttpManager::CloseAll() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  handles_.clear();
}

XHttpHandle* XHttpManager::ConnectOf(XHttpHandle* handle) {
  if (!handle) {
    return nullptr;
  }
  if (handle->type == XHttpHandleType::kConnect) {
    return handle;
  }
  return Lookup(handle->parent, XHttpHandleType::kConnect);
}

XHttpHandle* XHttpManager::SessionOf(XHttpHandle* handle) {
  if (!handle) {
    return nullptr;
  }
  if (handle->type == XHttpHandleType::kSession) {
    return handle;
  }
  auto* connect = ConnectOf(handle);
  return connect ? Lookup(connect->parent, XHttpHandleType::kSession) : nullptr;
}

bool XHttpManager::IsAsync(XHttpHandle* handle) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto* session = SessionOf(handle);
  return session && (session->flags & XHTTP_FLAG_ASYNC) != 0;
}

void XHttpManager::Notify(XHttpHandle* request, uint32_t status,
                          uint32_t info_ptr, uint32_t info_length,
                          uint32_t free_info) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!request || !IsAsync(request)) {
    if (free_info) {
      kernel_memory()->SystemHeapFree(free_info);
    }
    return;
  }
  // The callback may be set on the request or inherited from the session.
  auto* session = SessionOf(request);
  uint32_t callback = request->callback;
  uint32_t flags = request->callback_flags;
  if (!callback && session) {
    callback = session->callback;
    flags = session->callback_flags;
  }
  if (!callback || !(flags & status)) {
    if (free_info) {
      kernel_memory()->SystemHeapFree(free_info);
    }
    return;
  }

  XHttpNotification notification = {};
  notification.session = session ? session->handle : 0;
  notification.handle = request->handle;
  notification.callback = callback;
  notification.context = request->context;
  notification.status = status;
  notification.info_ptr = info_ptr;
  notification.info_length = info_length;
  notification.free_info = free_info;
  notifications_.push_back(notification);
}

bool XHttpManager::TakeNotification(uint32_t session, XHttpNotification& out) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (auto it = notifications_.begin(); it != notifications_.end(); ++it) {
    if (session && it->session != session && it->handle != session) {
      continue;
    }
    out = *it;
    notifications_.erase(it);
    return true;
  }
  return false;
}

namespace {

std::string GuestString(uint32_t address) {
  if (!address) {
    return "";
  }
  return kernel_memory()->TranslateVirtual<const char*>(address);
}

std::string GuestStringW(uint32_t address) {
  if (!address) {
    return "";
  }
  const auto* host_ptr =
      kernel_memory()->TranslateVirtual<const char16_t*>(address);
  return xe::to_utf8(xe::load_and_swap<std::u16string>(host_ptr));
}

size_t WriteBody(char* data, size_t size, size_t count, void* userdata) {
  auto* body = reinterpret_cast<std::vector<uint8_t>*>(userdata);
  const size_t total = size * count;
  body->insert(body->end(), data, data + total);
  return total;
}

size_t WriteHeader(char* data, size_t size, size_t count, void* userdata) {
  auto* handle = reinterpret_cast<XHttpHandle*>(userdata);
  const size_t total = size * count;
  std::string line(data, total);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.pop_back();
  }
  if (line.empty()) {
    return total;
  }
  if (line.rfind("HTTP/", 0) == 0) {
    handle->raw_headers.clear();
    handle->status_text.clear();
    const size_t code_at = line.find(' ');
    if (code_at != std::string::npos) {
      const size_t text_at = line.find(' ', code_at + 1);
      if (text_at != std::string::npos) {
        handle->status_text = line.substr(text_at + 1);
      }
    }
  }
  handle->raw_headers += line;
  handle->raw_headers += "\r\n";
  return total;
}

std::string HeaderValue(const XHttpHandle* handle, const std::string& name) {
  size_t at = 0;
  while (at < handle->raw_headers.size()) {
    const size_t end = handle->raw_headers.find("\r\n", at);
    if (end == std::string::npos) {
      break;
    }
    const std::string line = handle->raw_headers.substr(at, end - at);
    const size_t colon = line.find(':');
    if (colon != std::string::npos &&
        xe::utf8::equal_case(line.substr(0, colon), name)) {
      size_t value_at = colon + 1;
      while (value_at < line.size() && line[value_at] == ' ') {
        ++value_at;
      }
      return line.substr(value_at);
    }
    at = end + 2;
  }
  return "";
}

std::string BuildUrl(XHttpHandle* request) {
  auto* manager = XHttpManager::Get();
  auto* connect = manager->ConnectOf(request);
  if (!connect) {
    return "";
  }
  uint16_t port = connect->port;
  // The dash's XMan client connects to port 443 but does not pass
  // XHTTP_FLAG_SECURE on the request (on hardware the Live stack picks the
  // scheme from the port). Sending plaintext to 443 gets a 400 out of nginx
  // before the request ever reaches the hub, so treat 443 as https.
  const bool secure = request->secure || port == 443;
  if (!port) {
    port = secure ? 443 : 80;
  }
  std::string url = secure ? "https://" : "http://";
  url += connect->host;
  if ((secure && port != 443) || (!secure && port != 80)) {
    url += ":" + std::to_string(port);
  }
  std::string object_name = request->object_name;
  if (object_name.empty()) {
    object_name = "/";
  }
  if (object_name.front() != '/') {
    url += "/";
  }
  url += object_name;
  return url;
}

bool Perform(XHttpHandle* request) {
  auto* manager = XHttpManager::Get();
  auto* session = manager->SessionOf(request);

  const std::string url = BuildUrl(request);
  if (url.empty()) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_STATE);
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    XThread::SetLastError(XHTTP_ERROR_INTERNAL_ERROR);
    return false;
  }

  request->response_body.clear();
  request->raw_headers.clear();
  request->read_pos = 0;

  struct curl_slist* headers = nullptr;
  for (const std::string& header : request->request_headers) {
    headers = curl_slist_append(headers, header.c_str());
  }

  std::string verb = request->verb.empty() ? "GET" : request->verb;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, verb.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (headers) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }
  if (session && !session->agent.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, session->agent.c_str());
  } else {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Xbox");
  }
  if (session) {
    if (session->connect_timeout) {
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                       static_cast<long>(session->connect_timeout));
    }
    if (session->receive_timeout) {
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                       static_cast<long>(session->receive_timeout));
    }
    if (session->access_type == XHTTP_ACCESS_TYPE_NAMED_PROXY &&
        !session->proxy.empty()) {
      curl_easy_setopt(curl, CURLOPT_PROXY, session->proxy.c_str());
    } else if (session->access_type == XHTTP_ACCESS_TYPE_NO_PROXY) {
      curl_easy_setopt(curl, CURLOPT_PROXY, "");
    }
  }
  if (!request->user_name.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, request->user_name.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, request->password.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
  }
  if (!request->request_body.empty()) {
    curl_easy_setopt(
        curl, CURLOPT_POSTFIELDS,
        reinterpret_cast<const char*>(request->request_body.data()));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(request->request_body.size()));
  } else if (verb == "POST" || verb == "PUT") {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &request->response_body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, request);

  auto& counters = manager->counters();
  counters.requests_sent = counters.requests_sent + 1;
  counters.connects = counters.connects + 1;
  counters.name_resolutions = counters.name_resolutions + 1;
  counters.bytes_sent =
      counters.bytes_sent + static_cast<uint32_t>(request->request_body.size());

  const CURLcode result = curl_easy_perform(curl);

  if (result == CURLE_OK) {
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    request->status_code = static_cast<uint32_t>(code);
    // Every guest fetch, with the URL the guest actually built. This is how we
    // learn which files a title (the dash's XMan/Epix channel in particular)
    // expects the hub to serve.
    XELOGW("XHttp: {} {} -> {} ({} bytes)", verb, url, code,
           request->response_body.size());
    request->auth_schemes = 0;
    const std::string authenticate = HeaderValue(request, "WWW-Authenticate");
    if (!authenticate.empty()) {
      const std::string lowered = xe::utf8::lower_ascii(authenticate);
      if (lowered.find("basic") != std::string::npos) {
        request->auth_schemes |= XHTTP_AUTH_SCHEME_BASIC;
      }
      if (lowered.find("digest") != std::string::npos) {
        request->auth_schemes |= XHTTP_AUTH_SCHEME_DIGEST;
      }
      if (lowered.find("ntlm") != std::string::npos) {
        request->auth_schemes |= XHTTP_AUTH_SCHEME_NTLM;
      }
      if (lowered.find("negotiate") != std::string::npos) {
        request->auth_schemes |= XHTTP_AUTH_SCHEME_NEGOTIATE;
      }
    }
    counters.responses_received = counters.responses_received + 1;
    counters.bytes_received =
        counters.bytes_received +
        static_cast<uint32_t>(request->response_body.size());
    request->received = true;
  } else {
    counters.errors = counters.errors + 1;
    uint32_t error = XHTTP_ERROR_CONNECTION_ERROR;
    switch (result) {
      case CURLE_UNSUPPORTED_PROTOCOL:
        error = XHTTP_ERROR_UNRECOGNIZED_SCHEME;
        break;
      case CURLE_COULDNT_RESOLVE_HOST:
      case CURLE_COULDNT_RESOLVE_PROXY:
        error = XHTTP_ERROR_NAME_NOT_RESOLVED;
        break;
      case CURLE_OPERATION_TIMEDOUT:
        error = XHTTP_ERROR_TIMEOUT;
        break;
      case CURLE_PEER_FAILED_VERIFICATION:
      case CURLE_SSL_CONNECT_ERROR:
        error = XHTTP_ERROR_SECURE_FAILURE;
        break;
      case CURLE_TOO_MANY_REDIRECTS:
        error = XHTTP_ERROR_REDIRECT_FAILED;
        break;
      default:
        break;
    }
    XThread::SetLastError(error);
    XELOGE("XHttp: {} {} failed: {}", verb, url, curl_easy_strerror(result));
  }

  if (headers) {
    curl_slist_free_all(headers);
  }
  curl_easy_cleanup(curl);

  return result == CURLE_OK;
}

bool EnsureReceived(XHttpHandle* request) {
  if (request->received) {
    return true;
  }
  if (!request->sent) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_STATE);
    return false;
  }
  return Perform(request);
}

bool CopyOut(uint32_t buffer_ptr, uint32_t length_ptr, const void* data,
             uint32_t size) {
  auto* length =
      length_ptr
          ? kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(length_ptr)
          : nullptr;
  if (!buffer_ptr || !length || *length < size) {
    if (length) {
      *length = size;
    }
    XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
    return false;
  }
  std::memcpy(kernel_memory()->TranslateVirtual<void*>(buffer_ptr), data, size);
  *length = size;
  return true;
}

}  // namespace

dword_result_t NetDll_XHttpStartup_entry(dword_t caller, dword_t reserved,
                                         dword_t reserved_ptr) {
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpStartup, kNetworking, kImplemented);

dword_result_t NetDll_XHttpShutdown_entry(dword_t caller) {
  XHttpManager::Get()->CloseAll();
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpShutdown, kNetworking, kImplemented);

dword_result_t NetDll_XHttpOpen_entry(dword_t caller, lpu16string_t agent,
                                      dword_t access_type, lpu16string_t proxy,
                                      lpu16string_t proxy_bypass,
                                      dword_t flags) {
  auto* manager = XHttpManager::Get();
  const uint32_t handle = manager->Create(XHttpHandleType::kSession, 0);
  auto* session = manager->Lookup(handle);
  session->agent = GuestStringW(agent.guest_address());
  session->access_type = access_type;
  session->proxy = GuestStringW(proxy.guest_address());
  session->proxy_bypass = GuestStringW(proxy_bypass.guest_address());
  session->flags = flags;

  XThread::SetLastError(X_ERROR_SUCCESS);
  return handle;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpen, kNetworking, kImplemented);

dword_result_t NetDll_XHttpCloseHandle_entry(dword_t caller, dword_t handle) {
  if (!XHttpManager::Get()->Close(handle)) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCloseHandle, kNetworking, kImplemented);

dword_result_t NetDll_XHttpConnect_entry(dword_t caller, dword_t session_handle,
                                         lpstring_t host, dword_t port,
                                         dword_t flags) {
  auto* manager = XHttpManager::Get();
  auto* session = manager->Lookup(session_handle, XHttpHandleType::kSession);
  if (!session) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }

  const uint32_t handle =
      manager->Create(XHttpHandleType::kConnect, session_handle);
  auto* connect = manager->Lookup(handle);
  connect->host = host ? host.value() : "";
  connect->port = static_cast<uint16_t>(port.value());
  connect->flags = flags;

  XELOGW("XHttpConnect: {}:{}", connect->host, connect->port);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return handle;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpConnect, kNetworking, kImplemented);

static uint32_t CreateRequest(uint32_t connect_handle, const std::string& verb,
                              const std::string& path,
                              const std::string& version,
                              const std::string& referrer, uint32_t flags) {
  auto* manager = XHttpManager::Get();
  auto* connect = manager->Lookup(connect_handle, XHttpHandleType::kConnect);
  if (!connect) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }

  const uint32_t handle =
      manager->Create(XHttpHandleType::kRequest, connect_handle);
  auto* request = manager->Lookup(handle);
  request->verb = verb.empty() ? "GET" : verb;
  request->object_name = path.empty() ? "/" : path;
  request->version = version;
  request->referrer = referrer;
  request->flags = flags;
  request->secure = (flags & XHTTP_FLAG_SECURE) != 0;
  if (!referrer.empty()) {
    request->request_headers.push_back("Referer: " + referrer);
  }

  XELOGI("XHttpOpenRequest: {} {}", request->verb, request->object_name);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return handle;
}

dword_result_t NetDll_XHttpOpenRequest_entry(
    dword_t caller, dword_t connect_handle, lpstring_t verb, lpstring_t path,
    lpstring_t version, lpstring_t referrer, lpstring_t accept_types,
    dword_t flags) {
  return CreateRequest(connect_handle, verb ? verb.value() : "",
                       path ? path.value() : "", version ? version.value() : "",
                       referrer ? referrer.value() : "", flags);
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpenRequest, kNetworking, kImplemented);

dword_result_t NetDll_XHttpOpenRequestUsingMemory_entry(
    dword_t caller, dword_t connect_handle, lpstring_t verb, lpstring_t path,
    lpstring_t version, lpstring_t referrer, lpvoid_t buffer,
    dword_t buffer_size, dword_t flags) {
  return CreateRequest(connect_handle, verb ? verb.value() : "",
                       path ? path.value() : "", version ? version.value() : "",
                       referrer ? referrer.value() : "", flags);
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpenRequestUsingMemory, kNetworking,
                    kImplemented);

dword_result_t NetDll_XHttpSetStatusCallback_entry(dword_t caller,
                                                   dword_t handle,
                                                   dword_t callback,
                                                   dword_t flags,
                                                   dword_t reserved) {
  auto* object = XHttpManager::Get()->Lookup(handle);
  if (!object) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0xFFFFFFFF;
  }
  const uint32_t previous = object->callback;
  object->callback = callback;
  object->callback_flags = flags;
  return previous;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetStatusCallback, kNetworking, kImplemented);

dword_result_t NetDll_XHttpSendRequest_entry(
    dword_t caller, dword_t request_handle, lpstring_t headers,
    dword_t headers_length, lpvoid_t optional, dword_t optional_length,
    dword_t total_length, dword_t context) {
  auto* manager = XHttpManager::Get();
  auto* request = manager->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }

  if (headers) {
    std::string block = headers.value();
    if (headers_length && headers_length != 0xFFFFFFFF &&
        headers_length < block.size()) {
      block = block.substr(0, headers_length);
    }
    size_t at = 0;
    while (at < block.size()) {
      size_t end = block.find("\r\n", at);
      if (end == std::string::npos) {
        end = block.size();
      }
      const std::string line = block.substr(at, end - at);
      if (!line.empty()) {
        request->request_headers.push_back(line);
      }
      at = end + 2;
    }
  }

  request->request_body.clear();
  if (optional && optional_length) {
    const auto* data = kernel_memory()->TranslateVirtual<const uint8_t*>(
        optional.guest_address());
    request->request_body.assign(data, data + optional_length);
  }
  request->expected_body_length = total_length;
  request->context = context;
  request->sent = true;
  request->received = false;

  if (total_length && request->request_body.size() < total_length) {
    XThread::SetLastError(X_ERROR_SUCCESS);
    return 1;
  }

  const bool ok = Perform(request);

  if (manager->IsAsync(request)) {
    // Async: the call always succeeds and the outcome is reported through the
    // callback the next time the guest pumps XHttpDoWork. We fetch
    // synchronously, so the completion is ready immediately.
    if (ok) {
      manager->Notify(request, XHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, 0,
                      0);
    } else {
      // XHTTP_ASYNC_RESULT { dwResult; dwError; } in guest memory.
      const uint32_t block = kernel_memory()->SystemHeapAlloc(8);
      if (block) {
        auto* result =
            kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(block);
        result[0] = XHTTP_API_SEND_REQUEST;
        result[1] = XThread::GetLastError();
        manager->Notify(request, XHTTP_CALLBACK_STATUS_REQUEST_ERROR, block, 8,
                        block);
      }
    }
    XThread::SetLastError(X_ERROR_SUCCESS);
    return 1;
  }

  if (!ok) {
    return 0;
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSendRequest, kNetworking, kImplemented);

dword_result_t NetDll_XHttpWriteData_entry(dword_t caller,
                                           dword_t request_handle,
                                           lpvoid_t buffer, dword_t size,
                                           lpdword_t out_written) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  if (!request->sent) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_STATE);
    return 0;
  }

  if (buffer && size) {
    const auto* data = kernel_memory()->TranslateVirtual<const uint8_t*>(
        buffer.guest_address());
    request->request_body.insert(request->request_body.end(), data,
                                 data + size);
  }
  if (out_written) {
    *out_written = size.value();
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpWriteData, kNetworking, kImplemented);

dword_result_t NetDll_XHttpReceiveResponse_entry(dword_t caller,
                                                 dword_t request_handle,
                                                 lpvoid_t reserved) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  if (!EnsureReceived(request)) {
    return 0;
  }
  XHttpManager::Get()->Notify(request, XHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,
                              0, 0);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpReceiveResponse, kNetworking, kImplemented);

dword_result_t NetDll_XHttpQueryHeaders_entry(
    dword_t caller, dword_t request_handle, dword_t info_level, lpstring_t name,
    lpvoid_t buffer, lpdword_t buffer_length, lpdword_t index) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  if (!EnsureReceived(request)) {
    return 0;
  }

  const uint32_t level = info_level & XHTTP_QUERY_HEADER_MASK;
  const bool as_number = (info_level & XHTTP_QUERY_FLAG_NUMBER) != 0;

  // Every level from ACCEPT (0) to X_ERR (41) names one header, so a table
  // reads better than forty cases. The gap at RAW_HEADERS_CRLF is not a header
  // and is handled below; MAX (42) is the end of the range, not a level.
  static const char* const kHeaderForLevel[XHTTP_QUERY_MAX] = {
      "Accept",
      "Accept-Charset",
      "Accept-Encoding",
      "Accept-Language",
      "Accept-Ranges",
      "Allow",
      "Cache-Control",
      "Connection",
      "Content-Language",
      "Content-Length",
      "Content-Transfer-Encoding",
      "Content-Type",
      "Date",
      "Expires",
      "Ext",
      "Host",
      "If-Match",
      "If-Modified-Since",
      "If-None-Match",
      "If-Range",
      "If-Unmodified-Since",
      "Last-Modified",
      nullptr,  // XHTTP_QUERY_RAW_HEADERS_CRLF
      "Man",
      "MIME-Version",
      "MX",
      "NT",
      "NTS",
      "Range",
      "Referer",
      "Server",
      "Seq",
      "SID",
      "ST",
      "Timeout",
      "Transfer-Encoding",
      "Unless-Modified-Since",
      "User-Agent",
      "USN",
      "X-Delay",
      "X-Delayflags",
      "X-Err",
  };

  std::string value;
  switch (level) {
    case XHTTP_QUERY_STATUS_CODE:
      value = std::to_string(request->status_code);
      break;
    case XHTTP_QUERY_RAW_HEADERS_CRLF:
      value = request->raw_headers;
      break;
    case XHTTP_QUERY_CUSTOM:
      if (!name) {
        XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
        return 0;
      }
      value = HeaderValue(request, name.value());
      break;
    default: {
      if (level >= XHTTP_QUERY_MAX || !kHeaderForLevel[level]) {
        XELOGW("XHttpQueryHeaders: unhandled level {:X} (info_level {:08X})",
               level, info_level.value());
        XThread::SetLastError(XHTTP_ERROR_HEADER_NOT_FOUND);
        return 0;
      }
      value = HeaderValue(request, kHeaderForLevel[level]);
      // curl can consume Content-Length on a chunked or decompressed response;
      // what we actually hold is authoritative.
      if (value.empty() && level == XHTTP_QUERY_CONTENT_LENGTH) {
        value = std::to_string(request->response_body.size());
      }
      break;
    }
  }

  if (value.empty() && level != XHTTP_QUERY_RAW_HEADERS_CRLF) {
    XELOGW("XHttpQueryHeaders: level {:X} not present in the response", level);
    XThread::SetLastError(XHTTP_ERROR_HEADER_NOT_FOUND);
    return 0;
  }

  XELOGW("XHttpQueryHeaders: level {:X}{} -> '{}'", level,
         as_number ? " (number)" : "", value);

  if (as_number) {
    const uint64_t number = std::strtoull(value.c_str(), nullptr, 10);
    // The same level answers 32- or 64-bit depending on the buffer the caller
    // sizes: GetResponseContentLength passes 4, GetResponseContentLength64
    // passes 8. Writing 4 bytes into the 64-bit caller's buffer left it
    // reading the high half and seeing a zero-length body.
    const uint32_t capacity =
        buffer_length ? static_cast<uint32_t>(*buffer_length) : 0;
    if (capacity >= sizeof(uint64_t)) {
      xe::be<uint64_t> encoded = number;
      if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                   &encoded, sizeof(encoded))) {
        return 0;
      }
    } else {
      xe::be<uint32_t> encoded = static_cast<uint32_t>(number);
      if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                   &encoded, sizeof(encoded))) {
        return 0;
      }
    }
  } else {
    if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                 value.c_str(), static_cast<uint32_t>(value.size() + 1))) {
      return 0;
    }
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryHeaders, kNetworking, kImplemented);

dword_result_t NetDll_XHttpReadData_entry(dword_t caller,
                                          dword_t request_handle,
                                          lpvoid_t buffer, dword_t size,
                                          lpdword_t out_read) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  if (!EnsureReceived(request)) {
    return 0;
  }

  const size_t remaining = request->response_body.size() - request->read_pos;
  const size_t count = std::min(static_cast<size_t>(size.value()), remaining);
  if (count && buffer) {
    std::memcpy(
        kernel_memory()->TranslateVirtual<void*>(buffer.guest_address()),
        request->response_body.data() + request->read_pos, count);
    request->read_pos += count;
  }
  if (out_read) {
    *out_read = static_cast<uint32_t>(count);
  }

  // READ_COMPLETE hands the guest back its own buffer and the byte count; a
  // zero-length completion is how the transfer's end is signalled.
  XHttpManager::Get()->Notify(request, XHTTP_CALLBACK_STATUS_READ_COMPLETE,
                              buffer.guest_address(),
                              static_cast<uint32_t>(count));

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpReadData, kNetworking, kImplemented);

dword_result_t NetDll_XHttpQueryOption_entry(dword_t caller, dword_t handle,
                                             dword_t option, lpvoid_t buffer,
                                             lpdword_t buffer_length) {
  auto* manager = XHttpManager::Get();
  auto* object = manager->Lookup(handle);
  if (!object) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }

  auto write_dword = [&](uint32_t value) {
    xe::be<uint32_t> encoded = value;
    return CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                   &encoded, sizeof(encoded));
  };

  switch (option) {
    case XHTTP_OPTION_HANDLE_TYPE:
      if (!write_dword(static_cast<uint32_t>(object->type))) {
        return 0;
      }
      break;
    case XHTTP_OPTION_PARENT_HANDLE:
      if (!write_dword(object->parent)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_CONTEXT_VALUE:
      if (!write_dword(object->context)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_RESOLVE_TIMEOUT:
      if (!write_dword(object->resolve_timeout)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_CONNECT_TIMEOUT:
      if (!write_dword(object->connect_timeout)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_SEND_TIMEOUT:
      if (!write_dword(object->send_timeout)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_RECEIVE_TIMEOUT:
      if (!write_dword(object->receive_timeout)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_SECURITY_FLAGS:
      if (!write_dword(0)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_URL: {
      if (object->type != XHttpHandleType::kRequest) {
        XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
        return 0;
      }
      const std::string url = BuildUrl(object);
      if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                   url.c_str(), static_cast<uint32_t>(url.size() + 1))) {
        return 0;
      }
      break;
    }
    case XHTTP_OPTION_USER_AGENT: {
      auto* session = manager->SessionOf(object);
      const std::string agent = session ? session->agent : "";
      if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                   agent.c_str(), static_cast<uint32_t>(agent.size() + 1))) {
        return 0;
      }
      break;
    }
    default: {
      auto it = object->options.find(option);
      if (it != object->options.end()) {
        if (!write_dword(it->second)) {
          return 0;
        }
        break;
      }
      // An option we do not model. Answering INVALID_OPTION is how the dash's
      // XRL connection died: it queries option 0x17 on the request, and a
      // failure there sends it down the teardown-and-retry path (dash
      // 0x9226B710), which is the "Status Code: 80072ee9" box -- 0x2EE9 is
      // 12009, INVALID_OPTION, straight out of this branch. The options it
      // asks about are request-state flags it only acts on when set, so a zero
      // is the honest "not set" answer and keeps the transaction alive. Logged,
      // so an option that turns out to matter still names itself.
      XELOGW("XHttpQueryOption: unmodelled option {} on handle {:08X}; 0",
             option.value(), handle.value());
      if (!write_dword(0)) {
        return 0;
      }
      break;
    }
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryOption, kNetworking, kImplemented);

dword_result_t NetDll_XHttpSetOption_entry(dword_t caller, dword_t handle,
                                           dword_t option, lpvoid_t buffer,
                                           dword_t buffer_length) {
  auto* object = XHttpManager::Get()->Lookup(handle);
  if (!object) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }

  uint32_t value = 0;
  if (buffer && buffer_length >= sizeof(uint32_t)) {
    value = *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(
        buffer.guest_address());
  }

  switch (option) {
    case XHTTP_OPTION_HANDLE_TYPE:
    case XHTTP_OPTION_PARENT_HANDLE:
      XThread::SetLastError(XHTTP_ERROR_OPTION_NOT_SETTABLE);
      return 0;
    case XHTTP_OPTION_CONTEXT_VALUE:
      object->context = value;
      break;
    case XHTTP_OPTION_RESOLVE_TIMEOUT:
      object->resolve_timeout = value;
      break;
    case XHTTP_OPTION_CONNECT_TIMEOUT:
      object->connect_timeout = value;
      break;
    case XHTTP_OPTION_SEND_TIMEOUT:
      object->send_timeout = value;
      break;
    case XHTTP_OPTION_RECEIVE_TIMEOUT:
      object->receive_timeout = value;
      break;
    case XHTTP_OPTION_USER_AGENT:
      object->agent = GuestString(buffer.guest_address());
      break;
    case XHTTP_OPTION_PROXY:
      object->proxy = GuestString(buffer.guest_address());
      break;
    default:
      object->options[option] = value;
      break;
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetOption, kNetworking, kImplemented);

// The pump for an ASYNC session: the guest calls this from its own worker and
// expects every completion queued since the last call to be delivered to its
// status callback, on this thread, before returning. Without it the dash's XMan
// worker spins here forever waiting for a SENDREQUEST_COMPLETE that never
// arrives, and the home channel sits on a loading ring.
dword_result_t NetDll_XHttpDoWork_entry(dword_t caller, dword_t handle,
                                        dword_t reserved,
                                        const ppc_context_t& ctx) {
  auto* manager = XHttpManager::Get();

  XHttpNotification notification = {};
  while (manager->TakeNotification(handle.value(), notification)) {
    XELOGW("XHttpDoWork: callback {:08X} status {:08X} handle {:08X} len {}",
           notification.callback, notification.status, notification.handle,
           notification.info_length);
    uint64_t args[] = {notification.handle, notification.context,
                       notification.status, notification.info_ptr,
                       notification.info_length};
    ctx->processor->Execute(ctx->thread_state, notification.callback, args, 5);
    if (notification.free_info) {
      kernel_memory()->SystemHeapFree(notification.free_info);
    }
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpDoWork, kNetworking, kImplemented);

dword_result_t NetDll_XHttpSetCredentials_entry(
    dword_t caller, dword_t request_handle, dword_t auth_target,
    dword_t auth_scheme, lpstring_t user_name, lpstring_t password,
    lpvoid_t reserved) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  request->user_name = user_name ? user_name.value() : "";
  request->password = password ? password.value() : "";
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetCredentials, kNetworking, kImplemented);

dword_result_t NetDll_XHttpQueryAuthSchemes_entry(dword_t caller,
                                                  dword_t request_handle,
                                                  lpdword_t supported,
                                                  lpdword_t first,
                                                  lpdword_t target) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  if (!request->auth_schemes) {
    XThread::SetLastError(XHTTP_ERROR_INVALID_SERVER_RESPONSE);
    return 0;
  }

  uint32_t preferred = XHTTP_AUTH_SCHEME_BASIC;
  for (uint32_t scheme : {XHTTP_AUTH_SCHEME_NEGOTIATE, XHTTP_AUTH_SCHEME_NTLM,
                          XHTTP_AUTH_SCHEME_DIGEST, XHTTP_AUTH_SCHEME_BASIC}) {
    if (request->auth_schemes & scheme) {
      preferred = scheme;
      break;
    }
  }

  if (supported) {
    *supported = request->auth_schemes;
  }
  if (first) {
    *first = preferred;
  }
  if (target) {
    *target = XHTTP_AUTH_TARGET_SERVER;
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryAuthSchemes, kNetworking, kImplemented);

dword_result_t NetDll_XHttpCrackUrl_entry(
    dword_t caller, lpstring_t url_ptr, dword_t url_length, dword_t flags,
    pointer_t<XHTTP_URL_COMPONENTS> url_components_ptr) {
  if (!url_ptr || !url_components_ptr ||
      url_components_ptr->struct_size != sizeof(XHTTP_URL_COMPONENTS)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return false;
  }

  // X_ICU_ESCAPE is unsupported ignore it.

  bool insufficient_buffer =
      url_components_ptr->scheme_ptr && !url_components_ptr->scheme_length ||
      url_components_ptr->host_name_ptr &&
          !url_components_ptr->host_name_length ||
      url_components_ptr->user_name_ptr &&
          !url_components_ptr->user_name_length ||
      url_components_ptr->password_ptr &&
          !url_components_ptr->password_length ||
      url_components_ptr->url_path_ptr &&
          !url_components_ptr->url_path_length ||
      url_components_ptr->extra_info_ptr &&
          !url_components_ptr->extra_info_length;

  auto decode_string = [](const std::string encoded_component) -> std::string {
    CURL* curl = curl_easy_init();

    if (!curl) {
      return "";
    }

    std::string decoded_component;
    int component_length = 0;

    char* decoded_output =
        curl_easy_unescape(curl, encoded_component.c_str(),
                           decoded_component.size(), &component_length);

    if (decoded_output) {
      decoded_component = std::string(decoded_output, component_length);
      curl_free(decoded_output);
    }

    curl_easy_cleanup(curl);

    return decoded_component;
  };

  std::string url_to_process = url_ptr.value();

  if (url_length) {
    url_to_process = url_ptr.value().substr(0, url_length);
  }

  CURLU* url = curl_url();

  if (url) {
    CURLUcode rc = curl_url_set(url, CURLUPART_URL, url_to_process.c_str(), 0);

    // Assert if URL is bad format
    assert_zero(rc);

    if (rc) {
      url_components_ptr->scheme = -1;
    }

    curl_url_cleanup(url);
  }

  std::regex url_regex(
      R"(^([a-zA-Z]+)://(?:([^:@]+)(?::([^:@]*))?@)?([^/:]+)(?::(\d+))?((/[^?#]*)(\?[^#]*)?(#[^ ]*)?)?$)",
      std::regex_constants::icase);

  std::smatch matches;

  auto ProcessComponent = [decode_string, flags, kernel_state = kernel_state()](
                              const uint32_t component_result_ptr,
                              uint32_t& component_ptr,
                              uint32_t& component_length_ptr, uint32_t size) {
    if (component_ptr) {
      // Include null terminator
      const uint32_t min_buffer_size = size + 1;

      if (!component_length_ptr || component_length_ptr < min_buffer_size) {
        component_length_ptr = min_buffer_size;
        return false;
      }

      char* result_dst_ptr =
          kernel_state->memory()->TranslateVirtual<char*>(component_ptr);

      char* result_src_ptr =
          kernel_state->memory()->TranslateVirtual<char*>(component_result_ptr);

      const std::string component_data(result_src_ptr, size);
      const std::string processed_data =
          flags & X_ICU_DECODE ? decode_string(component_data) : component_data;

      xe::string_util::copy_truncating(result_dst_ptr, processed_data.c_str(),
                                       component_length_ptr);
      component_length_ptr = processed_data.size();
    } else if (component_length_ptr) {
      component_ptr = component_result_ptr;
      component_length_ptr = size;
    }

    return true;
  };

  bool result = true;
  bool unrecognized_scheme = false;

  if (std::regex_match(url_to_process, matches, url_regex)) {
    for (size_t i = 0; i < matches.size(); ++i) {
      std::ssub_match sub_match = matches[i];

      if (sub_match.matched) {
        const uint32_t result_ptr = url_ptr.guest_address() +
                                    static_cast<uint32_t>(matches.position(i));

        const uint32_t length = static_cast<uint32_t>(sub_match.length());

        const X_URL_COMPONENTS current_component =
            static_cast<X_URL_COMPONENTS>(i);

        switch (current_component) {
          case X_URL_COMPONENTS::Full: {
            // Skip
            continue;
          } break;
          case X_URL_COMPONENTS::Protocol: {
            uint32_t scheme_ptr_out = url_components_ptr->scheme_ptr;
            uint32_t scheme_length_out = url_components_ptr->scheme_length;

            const bool component_result = ProcessComponent(
                result_ptr, scheme_ptr_out, scheme_length_out, length);

            url_components_ptr->scheme_length = scheme_length_out;

            if (component_result) {
              if (!url_components_ptr->scheme_ptr) {
                url_components_ptr->scheme_ptr = scheme_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }

            const char* scheme_data_ptr =
                kernel_state()->memory()->TranslateVirtual<char*>(result_ptr);

            std::string schema_data = std::string(scheme_data_ptr, length);

            X_INTERNET_SCHEME scheme_type = {};

            // Set default scheme and port
            if (utf8::equal_case(schema_data.c_str(), "http")) {
              scheme_type = X_INTERNET_SCHEME::HTTP;
              url_components_ptr->port = 80;
            } else if (utf8::equal_case(schema_data.c_str(), "https")) {
              scheme_type = X_INTERNET_SCHEME::HTTPS;
              url_components_ptr->port = 443;
            } else {
              // XHTTP only speaks http and https; anything else is the
              // caller's own scheme and must be reported as unrecognised, the
              // way WinHTTP does. Succeeding here told the dash that
              // "epix://Offline_Slot_Home.jpg" was a remote URL, so it tried
              // to resolve a host named Offline_Slot_Home.jpg for every asset
              // in the manifest instead of loading them locally.
              unrecognized_scheme = true;
            }

            url_components_ptr->scheme = static_cast<uint32_t>(scheme_type);
          } break;
          case X_URL_COMPONENTS::Username: {
            uint32_t username_ptr_out = url_components_ptr->user_name_ptr;
            uint32_t username_length_out = url_components_ptr->user_name_length;

            const bool component_result = ProcessComponent(
                result_ptr, username_ptr_out, username_length_out, length);

            url_components_ptr->user_name_length = username_length_out;

            if (component_result) {
              if (!url_components_ptr->user_name_ptr) {
                url_components_ptr->user_name_ptr = username_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }
          } break;
          case X_URL_COMPONENTS::Password: {
            uint32_t password_ptr_out = url_components_ptr->password_ptr;
            uint32_t password_length_out = url_components_ptr->password_length;

            const bool component_result = ProcessComponent(
                result_ptr, password_ptr_out, password_length_out, length);

            url_components_ptr->password_length = password_length_out;

            if (component_result) {
              if (!url_components_ptr->password_ptr) {
                url_components_ptr->password_ptr = password_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }
          } break;
          case X_URL_COMPONENTS::Host: {
            uint32_t host_ptr_out = url_components_ptr->host_name_ptr;
            uint32_t host_length_out = url_components_ptr->host_name_length;

            const bool component_result = ProcessComponent(
                result_ptr, host_ptr_out, host_length_out, length);

            url_components_ptr->host_name_length = host_length_out;

            if (component_result) {
              if (!url_components_ptr->host_name_ptr) {
                url_components_ptr->host_name_ptr = host_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }
          } break;
          case X_URL_COMPONENTS::Port: {
            const char* port_str_ptr =
                kernel_memory()->TranslateVirtual<char*>(result_ptr);

            std::string port_str = std::string(port_str_ptr, length);

            const uint16_t port =
                xe::string_util::from_string<uint16_t>(port_str);

            url_components_ptr->port = port;
          } break;
          case X_URL_COMPONENTS::Path: {
            uint32_t path_ptr_out = url_components_ptr->url_path_ptr;
            uint32_t path_length_out = url_components_ptr->url_path_length;

            const bool component_result = ProcessComponent(
                result_ptr, path_ptr_out, path_length_out, length);

            url_components_ptr->url_path_length = path_length_out;

            if (component_result) {
              if (!url_components_ptr->url_path_ptr) {
                url_components_ptr->url_path_ptr = path_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }
          } break;
          case X_URL_COMPONENTS::Query: {
            uint32_t extra_ptr_out = url_components_ptr->extra_info_ptr;
            uint32_t extra_length_out = url_components_ptr->extra_info_length;

            const bool component_result = ProcessComponent(
                result_ptr, extra_ptr_out, extra_length_out, length);

            url_components_ptr->extra_info_length = extra_length_out;

            if (component_result) {
              if (!url_components_ptr->extra_info_ptr) {
                url_components_ptr->extra_info_ptr = extra_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }
          } break;
        }
      }
    }
  } else {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    result = false;
  }

  // Return after processing so the component length is set
  if (insufficient_buffer) {
    XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
    result = false;
  }

  if (unrecognized_scheme) {
    XELOGW("XHttpCrackUrl: unrecognised scheme in '{}'", url_to_process);
    XThread::SetLastError(XHTTP_ERROR_UNRECOGNIZED_SCHEME);
    result = false;
  }

  return result;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCrackUrl, kNetworking, kImplemented);

dword_result_t NetDll_XHttpCrackUrlW_entry(
    dword_t caller, lpu16string_t url_ptr, dword_t url_length, dword_t flags,
    pointer_t<XHTTP_URL_COMPONENTS> url_components_ptr) {
  if (!url_ptr || !url_components_ptr ||
      url_components_ptr->struct_size != sizeof(XHTTP_URL_COMPONENTS)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }

  std::string url = GuestStringW(url_ptr.guest_address());
  if (url_length && url_length < url.size()) {
    url = url.substr(0, url_length);
  }

  std::regex url_regex(
      R"(^([a-zA-Z]+)://(?:([^:@]+)(?::([^:@]*))?@)?([^/:]+)(?::(\d+))?((/[^?#]*)(\?[^#]*)?(#[^ ]*)?)?$)",
      std::regex_constants::icase);

  std::smatch matches;
  if (!std::regex_match(url, matches, url_regex)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }

  bool insufficient_buffer = false;

  auto store = [&](size_t index, xe::be<uint32_t>& component_ptr,
                   xe::be<uint32_t>& component_length) {
    std::ssub_match sub_match = matches[index];
    if (!sub_match.matched) {
      return;
    }
    const std::u16string value = xe::to_utf16(sub_match.str());
    const uint32_t units = static_cast<uint32_t>(value.size());
    if (component_ptr) {
      if (component_length < units + 1) {
        component_length = units + 1;
        insufficient_buffer = true;
        return;
      }
      auto* dst = kernel_memory()->TranslateVirtual<char16_t*>(component_ptr);
      xe::string_util::copy_and_swap_truncating(dst, value, component_length);
      component_length = units;
    } else {
      component_ptr =
          url_ptr.guest_address() +
          static_cast<uint32_t>(matches.position(index) * sizeof(char16_t));
      component_length = units;
    }
  };

  xe::be<uint32_t> scheme_ptr = url_components_ptr->scheme_ptr;
  xe::be<uint32_t> scheme_length = url_components_ptr->scheme_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Protocol), scheme_ptr,
        scheme_length);
  url_components_ptr->scheme_ptr = scheme_ptr;
  url_components_ptr->scheme_length = scheme_length;

  const std::string scheme = matches[1].str();
  X_INTERNET_SCHEME scheme_type = {};
  if (utf8::equal_case(scheme, "http")) {
    scheme_type = X_INTERNET_SCHEME::HTTP;
    url_components_ptr->port = 80;
  } else if (utf8::equal_case(scheme, "https")) {
    scheme_type = X_INTERNET_SCHEME::HTTPS;
    url_components_ptr->port = 443;
  } else {
    // See the ANSI entry point: XHTTP knows only http and https, and the dash
    // cracks its own "epix://" asset URIs through here. Reporting success made
    // every manifest asset look like a remote host.
    url_components_ptr->scheme = static_cast<uint32_t>(scheme_type);
    XELOGW("XHttpCrackUrlW: unrecognised scheme in '{}'", url);
    XThread::SetLastError(XHTTP_ERROR_UNRECOGNIZED_SCHEME);
    return 0;
  }
  url_components_ptr->scheme = static_cast<uint32_t>(scheme_type);

  xe::be<uint32_t> user_ptr = url_components_ptr->user_name_ptr;
  xe::be<uint32_t> user_length = url_components_ptr->user_name_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Username), user_ptr, user_length);
  url_components_ptr->user_name_ptr = user_ptr;
  url_components_ptr->user_name_length = user_length;

  xe::be<uint32_t> password_ptr = url_components_ptr->password_ptr;
  xe::be<uint32_t> password_length = url_components_ptr->password_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Password), password_ptr,
        password_length);
  url_components_ptr->password_ptr = password_ptr;
  url_components_ptr->password_length = password_length;

  xe::be<uint32_t> host_ptr = url_components_ptr->host_name_ptr;
  xe::be<uint32_t> host_length = url_components_ptr->host_name_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Host), host_ptr, host_length);
  url_components_ptr->host_name_ptr = host_ptr;
  url_components_ptr->host_name_length = host_length;

  if (matches[static_cast<size_t>(X_URL_COMPONENTS::Port)].matched) {
    url_components_ptr->port = xe::string_util::from_string<uint16_t>(
        matches[static_cast<size_t>(X_URL_COMPONENTS::Port)].str());
  }

  xe::be<uint32_t> path_ptr = url_components_ptr->url_path_ptr;
  xe::be<uint32_t> path_length = url_components_ptr->url_path_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Path), path_ptr, path_length);
  url_components_ptr->url_path_ptr = path_ptr;
  url_components_ptr->url_path_length = path_length;

  xe::be<uint32_t> extra_ptr = url_components_ptr->extra_info_ptr;
  xe::be<uint32_t> extra_length = url_components_ptr->extra_info_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Query), extra_ptr, extra_length);
  url_components_ptr->extra_info_ptr = extra_ptr;
  url_components_ptr->extra_info_length = extra_length;

  if (insufficient_buffer) {
    XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCrackUrlW, kNetworking, kImplemented);

namespace {

std::string ComposeUrl(pointer_t<XHTTP_URL_COMPONENTS> components, bool wide) {
  auto read = [&](uint32_t address, uint32_t length) -> std::string {
    if (!address) {
      return "";
    }
    if (wide) {
      std::u16string value = xe::load_and_swap<std::u16string>(
          kernel_memory()->TranslateVirtual<const void*>(address));
      if (length && length < value.size()) {
        value = value.substr(0, length);
      }
      return xe::to_utf8(value);
    }
    std::string value = kernel_memory()->TranslateVirtual<const char*>(address);
    if (length && length < value.size()) {
      value = value.substr(0, length);
    }
    return value;
  };

  std::string scheme = read(components->scheme_ptr, components->scheme_length);
  if (scheme.empty()) {
    scheme =
        components->scheme == static_cast<uint32_t>(X_INTERNET_SCHEME::HTTPS)
            ? "https"
            : "http";
  }

  const std::string host =
      read(components->host_name_ptr, components->host_name_length);
  const std::string user =
      read(components->user_name_ptr, components->user_name_length);
  const std::string password =
      read(components->password_ptr, components->password_length);
  const std::string path =
      read(components->url_path_ptr, components->url_path_length);
  const std::string extra =
      read(components->extra_info_ptr, components->extra_info_length);

  std::string url = scheme + "://";
  if (!user.empty()) {
    url += user;
    if (!password.empty()) {
      url += ":" + password;
    }
    url += "@";
  }
  url += host;

  const uint16_t port = components->port;
  const bool secure = utf8::equal_case(scheme, "https");
  if (port && ((secure && port != 443) || (!secure && port != 80))) {
    url += ":" + std::to_string(port);
  }

  if (!path.empty() && path.front() != '/') {
    url += "/";
  }
  url += path.empty() ? "/" : path;
  url += extra;
  return url;
}

}  // namespace

dword_result_t NetDll_XHttpCreateUrl_entry(
    dword_t caller, pointer_t<XHTTP_URL_COMPONENTS> components, dword_t flags,
    lpvoid_t buffer, lpdword_t buffer_length) {
  if (!components || components->struct_size != sizeof(XHTTP_URL_COMPONENTS)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }

  const std::string url = ComposeUrl(components, false);
  if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
               url.c_str(), static_cast<uint32_t>(url.size() + 1))) {
    return 0;
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCreateUrl, kNetworking, kImplemented);

dword_result_t NetDll_XHttpCreateUrlW_entry(
    dword_t caller, pointer_t<XHTTP_URL_COMPONENTS> components, dword_t flags,
    lpvoid_t buffer, lpdword_t buffer_length) {
  if (!components || components->struct_size != sizeof(XHTTP_URL_COMPONENTS)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }

  const std::u16string url = xe::to_utf16(ComposeUrl(components, true));
  const uint32_t bytes =
      static_cast<uint32_t>((url.size() + 1) * sizeof(char16_t));

  auto* length = buffer_length
                     ? kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(
                           buffer_length.guest_address())
                     : nullptr;
  if (!buffer || !length || *length < url.size() + 1) {
    if (length) {
      *length = static_cast<uint32_t>(url.size() + 1);
    }
    XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }

  auto* dst =
      kernel_memory()->TranslateVirtual<char16_t*>(buffer.guest_address());
  xe::string_util::copy_and_swap_truncating(dst, url, *length);
  *length = static_cast<uint32_t>(url.size());

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCreateUrlW, kNetworking, kImplemented);

dword_result_t NetDll_XHttpResetPerfCounters_entry(dword_t caller) {
  XHttpManager::Get()->counters() = {};
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpResetPerfCounters, kNetworking, kImplemented);

dword_result_t NetDll_XHttpGetPerfCounters_entry(dword_t caller,
                                                 lpvoid_t counters_ptr) {
  if (!counters_ptr) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  std::memcpy(
      kernel_memory()->TranslateVirtual<void*>(counters_ptr.guest_address()),
      &XHttpManager::Get()->counters(), sizeof(XHttpPerfCounters));
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpGetPerfCounters, kNetworking, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(XHttp);
