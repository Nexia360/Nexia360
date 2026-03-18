/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "src/xenia/kernel/xsocket.h"
#include <cstring>
#include "xenia/base/platform.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#ifdef XE_PLATFORM_WIN32
#include <windows.h>
#endif
using namespace std::chrono_literals;

// Network thread priority setting
DEFINE_int32(network_priority, 3,
             "Network thread priority setting: 0 - Leave Alone, 1 - Below "
             "Normal, 2 - Above Normal, 3 - High",
             "Live");

namespace xe {
namespace kernel {
XSocket::XSocket(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}
XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}
XSocket::~XSocket() { Close(); }
X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;
  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }
  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}
X_STATUS XSocket::Close() {
  // Close the native socket first — this will cause any pending WSAPoll calls
  // in worker threads to fail, unblocking them.
  std::unique_lock send_socket_lock(send_socket_mutex_);
  std::unique_lock receive_socket_lock(receive_socket_mutex_);
#if XE_PLATFORM_WIN32
  int ret = closesocket(native_handle_);
#elif XE_PLATFORM_LINUX
  int ret = close(native_handle_);
#endif
  send_socket_lock.unlock();
  receive_socket_lock.unlock();

  // Wait for all in-flight async tasks to complete after socket is closed.
  {
    std::unique_lock send_lock(send_mutex_);
    for (auto& task : send_tasks_) {
      if (task.valid()) task.wait();
    }
    send_tasks_.clear();
  }
  {
    std::unique_lock receive_lock(receive_mutex_);
    for (auto& task : receive_tasks_) {
      if (task.valid()) task.wait();
    }
    receive_tasks_.clear();
  }

  if (ret != 0) {
    XELOGE("Failed to close socket with error: {}", GetLastWSAError());
    return X_STATUS_UNSUCCESSFUL;
  }
  // Ensure the socket is unbound and resources are released
  native_handle_ = -1;
  bound_ = false;
  bound_port_ = 0;
  return X_STATUS_SUCCESS;
}

void XSocket::CleanupCompletedTasks(std::vector<std::future<int>>& tasks) {
  tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
                             [](std::future<int>& f) {
                               return !f.valid() ||
                                      f.wait_for(0ms) ==
                                          std::future_status::ready;
                             }),
              tasks.end());
}
X_STATUS XSocket::GetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                            uint32_t* optlen) {
  int ret =
      getsockopt(native_handle_, level, optname, static_cast<char*>(optval_ptr),
                 reinterpret_cast<socklen_t*>(optlen));
  // Because values provided in optval_ptr are in LE we must to somehow save
  // them in BE.
  switch (*optlen) {
    case 1:
      xe::copy_and_swap<uint8_t>((uint8_t*)optval_ptr, (uint8_t*)optval_ptr, 1);
      break;
    case 4:
      xe::copy_and_swap<uint32_t>((uint32_t*)optval_ptr, (uint32_t*)optval_ptr,
                                  1);
      break;
    case 8:
      xe::copy_and_swap<uint64_t>((uint64_t*)optval_ptr, (uint64_t*)optval_ptr,
                                  1);
      break;
    default:
      XELOGE("XSocket::GetOption - Unhandled optlen: {}", *optlen);
      break;
  }
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}
X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                            uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }
  void* proper_ptr =
      GetOptValueWithProperEndianness(optval_ptr, optname, optlen);
  int ret = setsockopt(native_handle_, level, optname, (const char*)proper_ptr,
                       optlen);
  // Cheezy way to check if we created some additional allocation.
  if (optval_ptr != proper_ptr) {
    free(proper_ptr);
  }
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }
  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }
  return X_STATUS_SUCCESS;
}
X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
#ifdef XE_PLATFORM_WIN32
  int ret = ioctlsocket(native_handle_, cmd, (u_long*)arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
#elif XE_PLATFORM_LINUX
  return X_STATUS_UNSUCCESSFUL;
#endif
}
X_STATUS XSocket::Connect(const XSOCKADDR_IN* name, int name_len) {
  XSOCKADDR_IN sa_in = XSOCKADDR_IN();
  memcpy(&sa_in, name, sizeof(XSOCKADDR_IN));
  if (XLiveAPI::upnp_handler) {
    sa_in.address_port =
        XLiveAPI::upnp_handler->GetMappedConnectPort(name->address_port);
  }
  sockaddr addr = sa_in.to_host();
  int ret = connect(native_handle_, &addr, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}
X_STATUS XSocket::Bind(const XSOCKADDR_IN* name, int name_len) {
  XSOCKADDR_IN sa_in = XSOCKADDR_IN();
  memcpy(&sa_in, name, sizeof(XSOCKADDR_IN));
  if (XLiveAPI::upnp_handler) {
    sa_in.address_port =
        XLiveAPI::upnp_handler->GetMappedBindPort(name->address_port);
  }
  sockaddr addr = sa_in.to_host();
  int ret = bind(native_handle_, &addr, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
  bound_port_ = sa_in.address_port;
  if (!bound_port_) {
    XSOCKADDR_IN sa = *name;
    if (!GetSockName(&sa, &name_len)) {
      bound_port_ = sa.address_port;
    }
  }
  bound_ = true;
  return X_STATUS_SUCCESS;
}
X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}
object_ref<XSocket> XSocket::Accept(XSOCKADDR_IN* name, int* name_len) {
  sockaddr sa = {};
  int addrlen = 0;
  const bool is_name_and_name_len_available = name && name_len;
  if (is_name_and_name_len_available) {
    addrlen = byte_swap(*name_len);
  }
  const uint64_t ret = accept(native_handle_, name ? &sa : nullptr,
                              name_len ? &addrlen : nullptr);
  if (ret == -1) {
    return nullptr;
  }
  if (is_name_and_name_len_available) {
    name->to_guest(&sa);
    *name_len = byte_swap(addrlen);
  }
  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;
  return socket;
}
int XSocket::Shutdown(int how) { return shutdown(native_handle_, how); }
int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}
int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                      XSOCKADDR_IN* from, uint32_t* from_len) {
  sockaddr sa{};
  if (from) {
    sa = from->to_host();
  }
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len,
                     flags, from ? &sa : nullptr, (int*)from_len);
  if (from) {
    from->to_guest(&sa);
  }
  return ret;
}
struct WSASendToData {
  XWSABUF* buffers;
  uint32_t num_buffers;
  uint32_t flags;
  XSOCKADDR_IN* to;
  uint32_t to_len;
  XWSAOVERLAPPED* overlapped;
  bool heap_allocated;  // true when buffers/to were heap-copied for async
  uint32_t completion_routine;    // guest function pointer for APC callback
  uint32_t overlapped_guest_ptr;  // guest address of overlapped struct
  object_ref<XThread> calling_thread;  // thread to enqueue APC to
};
struct WSARecvFromData {
  XWSABUF* buffers;
  uint32_t num_buffers;
  uint32_t flags;
  XSOCKADDR_IN* from;
  xe::be<uint32_t>* from_len;
  XWSAOVERLAPPED* overlapped;
  bool heap_allocated;          // true when buffers were heap-copied for async
  uint32_t completion_routine;  // guest function pointer for APC callback
  uint32_t overlapped_guest_ptr;       // guest address of overlapped struct
  object_ref<XThread> calling_thread;  // thread to enqueue APC to
};
int XSocket::WSASendTo(XWSABUF* buffers, uint32_t num_buffers,
                       xe::be<uint32_t>* num_bytes_sent_ptr, uint32_t flags,
                       XSOCKADDR_IN* to_ptr, uint32_t to_len,
                       XWSAOVERLAPPED* overlapped_ptr,
                       uint32_t completion_routine,
                       uint32_t overlapped_guest_ptr) {
  if (!buffers || !num_buffers || !num_bytes_sent_ptr ||
      (to_ptr && (to_len < sizeof(XSOCKADDR_IN) ||
                  to_ptr->address_family != X_AF_INET))) {
    SetLastWSAError(X_WSAError::X_WSA_INVALID_PARAMETER);
    return -1;
  }
  WSASendToData send_async_data = {};
  send_async_data.buffers = buffers;
  send_async_data.num_buffers = num_buffers;
  send_async_data.flags = flags;
  send_async_data.to = to_ptr;
  send_async_data.to_len = to_len;
  send_async_data.heap_allocated = false;
  // Use a temporary overlapped for the synchronous probe to avoid
  // races with async workers writing to the real overlapped.
  XWSAOVERLAPPED probe_overlapped = {};
  if (overlapped_ptr) {
    overlapped_ptr->offset_high |= WSAInfo::sendto_flag;
  }
  send_async_data.overlapped = &probe_overlapped;
  int ret = PushWSASendTo(false, send_async_data);
  if (ret < 0) {
    auto wsa_error = probe_overlapped.internal_high.get();
    SetLastWSAError((X_WSAError)wsa_error);
    if (overlapped_ptr && wsa_error == (uint32_t)X_WSAError::X_WSAEWOULDBLOCK) {
      std::lock_guard lock(send_mutex_);
      CleanupCompletedTasks(send_tasks_);
      if (send_tasks_.empty()) {
        // Point async worker at the REAL overlapped, not the probe.
        send_async_data.overlapped = overlapped_ptr;
        send_async_data.buffers = new XWSABUF[num_buffers];
        std::memcpy(send_async_data.buffers, buffers,
                    num_buffers * sizeof(XWSABUF));
        if (to_ptr) {
          auto* to_copy = new XSOCKADDR_IN;
          std::memcpy(to_copy, to_ptr, sizeof(XSOCKADDR_IN));
          send_async_data.to = to_copy;
        }
        send_async_data.heap_allocated = true;
        send_async_data.completion_routine = completion_routine;
        send_async_data.overlapped_guest_ptr = overlapped_guest_ptr;
        if (completion_routine) {
          send_async_data.calling_thread =
              retain_object(XThread::GetCurrentThread());
        }
        overlapped_ptr->offset_high &= ~WSAInfo::complete;
        overlapped_ptr->offset_high |= WSAInfo::sendto_flag;
        if (overlapped_ptr->event_handle) {
          xboxkrnl::xeNtClearEvent(overlapped_ptr->event_handle);
        }
        send_tasks_.push_back(std::async(std::launch::async,
                                         &XSocket::PushWSASendTo, this, true,
                                         send_async_data));
      }
      SetLastWSAError(X_WSAError::X_WSA_IO_PENDING);
      if (num_bytes_sent_ptr) {
        *num_bytes_sent_ptr = 0;
      }
      return 0;
    }
  } else {
    // Synchronous success — copy probe results to real overlapped.
    if (overlapped_ptr) {
      overlapped_ptr->internal = probe_overlapped.internal;
      overlapped_ptr->internal_high = probe_overlapped.internal_high;
      overlapped_ptr->offset = probe_overlapped.offset;
      overlapped_ptr->offset_high |= WSAInfo::complete;
      if (overlapped_ptr->event_handle) {
        xboxkrnl::xeNtSetEvent(overlapped_ptr->event_handle, nullptr);
      }
    }
    if (num_bytes_sent_ptr) {
      *num_bytes_sent_ptr = probe_overlapped.internal;
    }
  }
  return ret;
}
int XSocket::PushWSASendTo(bool wait, WSASendToData send_async_data) {
  // Set thread priority for async operations based on config
  if (cvars::network_priority > 0) {
#ifdef XE_PLATFORM_WIN32
    if (wait) {
      switch (cvars::network_priority) {
        case 1:  // Below Normal
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
          break;
        case 2:  // Above Normal
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
          break;
        case 3:  // High
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
          break;
      }
    }
#elif defined(XE_PLATFORM_LINUX)
    if (wait) {
      struct sched_param param;
      param.sched_priority = 0;
      switch (cvars::network_priority) {
        case 1:  // Below Normal
          if (sched_setscheduler(0, SCHED_OTHER, &param) == -1) {
            nice(10);  // Adjust nice value to make thread less important
          }
          break;
        case 2:  // Above Normal
          if (sched_setscheduler(0, SCHED_OTHER, &param) == -1) {
            nice(-5);  // Adjust nice value to make thread more important
          }
          break;
        case 3:  // High
          if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
            nice(-10);  // Adjust nice value to make thread very important
          }
          break;
      }
    }
#endif
  }

  send_async_data.overlapped->internal_high = 0;
  WSAPOLLFD fds = {};
  fds.fd = native_handle_;
  fds.events = POLLOUT;
  DWORD bytes_sent = 0;
  DWORD flags = send_async_data.flags;
  WSABUF* buffers = nullptr;
  sockaddr addr = {};
  if (send_async_data.to) {
    addr = send_async_data.to->to_host();
  }
  int ret;
  do {
    ret = WSAPoll(&fds, 1, wait ? 1000 : 0);
    if (send_async_data.overlapped->offset_high & WSAInfo::closed) {
      send_async_data.overlapped->internal_high =
          (uint32_t)X_WSAError::X_WSA_OPERATION_ABORTED;
      ret = -1;
      goto threadexit;
    }
  } while (ret == 0 && wait);
  if (ret < 0) {
    auto poll_err = WSAGetLastError();
    if (poll_err == WSAENOTSOCK || poll_err == WSAEINVAL) {
      // Socket closed while we were polling — abort cleanly.
      send_async_data.overlapped->internal_high =
          (uint32_t)X_WSAError::X_WSA_OPERATION_ABORTED;
    } else {
      XELOGE("XSocket send thread failed polling with error {}", poll_err);
      send_async_data.overlapped->internal_high =
          (uint32_t)X_WSAError::X_WSAENETDOWN;
    }
    ret = -1;
    goto threadexit;
  } else if (ret == 0) {
    send_async_data.overlapped->internal_high =
        (uint32_t)X_WSAError::X_WSAEWOULDBLOCK;
    ret = -1;
    goto threadexit;
  }
  buffers = new WSABUF[send_async_data.num_buffers];
  for (uint32_t i = 0; i < send_async_data.num_buffers; i++) {
    buffers[i].len = send_async_data.buffers[i].len;
    buffers[i].buf =
        reinterpret_cast<CHAR*>(kernel_state()->memory()->TranslateVirtual(
            send_async_data.buffers[i].buf_ptr));
  }
  for (int send_retry = 0; send_retry < 5; send_retry++) {
    ret = ::WSASendTo(
        native_handle_, buffers, send_async_data.num_buffers, &bytes_sent,
        send_async_data.flags, send_async_data.to ? &addr : nullptr,
        send_async_data.to ? send_async_data.to_len : 0, nullptr, nullptr);
    if (ret >= 0) break;
    auto host_err = WSAGetLastError();
    // Retryable errors: reestablish socket and retry up to 5 times.
    if (host_err == WSAENETRESET || host_err == WSAECONNRESET ||
        host_err == WSAETIMEDOUT) {
      XELOGW("WSASendTo retry {}/5 after error {}", send_retry + 1, host_err);
      // Reestablish the socket.
      {
        std::lock_guard lock(send_socket_mutex_);
        SOCKET old = native_handle_;
        SOCKET fresh = socket(af_, type_,
                              proto_ == Protocol::X_IPPROTO_VDP
                                  ? (int)Protocol::X_IPPROTO_UDP
                                  : (int)proto_);
        if (fresh != INVALID_SOCKET) {
          closesocket(old);
          native_handle_ = fresh;
          fds.fd = native_handle_;
          // Re-bind if previously bound.
          if (bound_ && bound_port_) {
            sockaddr_in bind_addr = {};
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_port = htons(bound_port_);
            bind_addr.sin_addr.s_addr = INADDR_ANY;
            ::bind(native_handle_, (sockaddr*)&bind_addr, sizeof(bind_addr));
          }
        }
      }
      Sleep(10 * (send_retry + 1));
      continue;
    }
    // Non-retryable errors.
    switch (host_err) {
      case WSAEWOULDBLOCK:
        send_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSAEWOULDBLOCK;
        break;
      case WSAENOTSOCK:
      case WSAEINVAL:
        send_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSA_OPERATION_ABORTED;
        delete[] buffers;
        buffers = nullptr;
        goto threadexit;
      case WSAEMSGSIZE:
        send_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSAEMSGSIZE;
        break;
      case WSAENETDOWN:
        send_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSAENETDOWN;
        break;
      case WSAEHOSTDOWN:
      case WSAEHOSTUNREACH:
      case WSAECONNABORTED:
      case WSAENOTCONN:
      case WSAESHUTDOWN:
        XELOGE("WSASendTo failed with network error {}", host_err);
        send_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSAENETDOWN;
        break;
      default:
        XELOGW("WSASendTo unknown host error {}", host_err);
        send_async_data.overlapped->internal_high = 0;
        ret = 0;
        break;
    }
    break;  // Non-retryable — exit loop.
  }
  if (ret >= 0) {
    send_async_data.overlapped->internal_high = 0;
    send_async_data.overlapped->internal = bytes_sent;
  }
  send_async_data.overlapped->offset = flags;
  delete[] buffers;
  buffers = nullptr;
threadexit:
  if (send_async_data.heap_allocated) {
    delete[] send_async_data.buffers;
    delete send_async_data.to;
  }
  // Ensure all overlapped field writes are visible before setting complete.
  MemoryBarrier();
  send_async_data.overlapped->offset_high |= WSAInfo::complete;
  if (send_async_data.overlapped->event_handle) {
    auto ev = kernel_state()->object_table()->LookupObject<XEvent>(
        send_async_data.overlapped->event_handle);
    if (ev) {
      xboxkrnl::xeNtSetEvent(send_async_data.overlapped->event_handle, nullptr);
    }
  }
  // Fire completion routine APC if one was provided.
  if (wait && send_async_data.completion_routine &&
      send_async_data.calling_thread) {
    // WSA completion routine: void CALLBACK(DWORD dwError, DWORD cbTransferred,
    //                                       LPWSAOVERLAPPED lpOverlapped,
    //                                       DWORD dwFlags)
    send_async_data.calling_thread->EnqueueApc(
        send_async_data.completion_routine,
        send_async_data.overlapped->internal_high,  // dwError
        send_async_data.overlapped->internal,       // cbTransferred
        send_async_data.overlapped_guest_ptr);      // lpOverlapped
  }
  send_cv_.notify_all();
  return ret;
}
int XSocket::PollWSARecvFrom(bool wait, WSARecvFromData receive_async_data) {
  // Set thread priority for async operations based on config
  if (cvars::network_priority > 0) {
#ifdef XE_PLATFORM_WIN32
    if (wait) {
      switch (cvars::network_priority) {
        case 1:  // Below Normal
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
          break;
        case 2:  // Above Normal
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
          break;
        case 3:  // High
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
          break;
      }
    }
#elif defined(XE_PLATFORM_LINUX)
    if (wait) {
      struct sched_param param;
      param.sched_priority = 0;
      switch (cvars::network_priority) {
        case 1:  // Below Normal
          if (sched_setscheduler(0, SCHED_OTHER, &param) == -1) {
            nice(10);  // Adjust nice value to make thread less important
          }
          break;
        case 2:  // Above Normal
          if (sched_setscheduler(0, SCHED_OTHER, &param) == -1) {
            nice(-5);  // Adjust nice value to make thread more important
          }
          break;
        case 3:  // High
          if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
            nice(-10);  // Adjust nice value to make thread very important
          }
          break;
      }
    }
#endif
  }

  receive_async_data.overlapped->internal_high = 0;
  WSAPOLLFD fds = {};
  fds.fd = native_handle_;
  fds.events = POLLIN;
  DWORD bytes_received = 0;
  DWORD flags = receive_async_data.flags;
  WSABUF* buffers = nullptr;
  sockaddr addr = {};
  sockaddr* sa = nullptr;
  if (receive_async_data.from) {
    addr = receive_async_data.from->to_host();
    sa = &addr;
  }
  int ret;

  // Async workers (wait=true) loop continuously, delivering each received
  // packet to the overlapped and signaling the event. This keeps the worker
  // alive for the socket's lifetime instead of exiting after one packet.
recv_loop:
  do {
#ifdef XE_PLATFORM_WIN32
    ret = WSAPoll(&fds, 1, wait ? 1000 : 0);
#else
    ret = poll(&fds, 1, wait ? 1000 : 0);
#endif
    if (receive_async_data.overlapped->offset_high & WSAInfo::closed) {
      receive_async_data.overlapped->internal_high =
          (uint32_t)X_WSAError::X_WSA_OPERATION_ABORTED;
      ret = -1;
      goto threadexit;
    }
    if (ret == 0 && wait) {
      Sleep(1);
    }
  } while (ret == 0 && wait);
  if (ret < 0) {
    auto poll_err = WSAGetLastError();
    if (poll_err == WSAENOTSOCK || poll_err == WSAEINVAL) {
      // Socket closed while we were polling — abort cleanly.
      receive_async_data.overlapped->internal_high =
          (uint32_t)X_WSAError::X_WSA_OPERATION_ABORTED;
    } else {
      XELOGE("XSocket receive thread failed polling with error {}", poll_err);
      receive_async_data.overlapped->internal_high =
          (uint32_t)X_WSAError::X_WSAENETDOWN;
    }
    ret = -1;
    goto threadexit;
  } else if (ret == 0) {
    // No data ready and not waiting — signal WOULDBLOCK so caller goes async.
    receive_async_data.overlapped->internal_high =
        (uint32_t)X_WSAError::X_WSAEWOULDBLOCK;
    ret = -1;
    goto threadexit;
  }
#ifdef XE_PLATFORM_WIN32
  buffers = new WSABUF[receive_async_data.num_buffers];
  for (auto i = 0u; i < receive_async_data.num_buffers; i++) {
    buffers[i].len = receive_async_data.buffers[i].len;
    buffers[i].buf =
        reinterpret_cast<CHAR*>(kernel_state()->memory()->TranslateVirtual(
            receive_async_data.buffers[i].buf_ptr));
  }
  for (int recv_retry = 0; recv_retry < 5; recv_retry++) {
    ret = ::WSARecvFrom(native_handle_, buffers, receive_async_data.num_buffers,
                        &bytes_received, &flags, sa,
                        (LPINT)receive_async_data.from_len, nullptr, nullptr);
    if (ret >= 0) break;
    auto host_err = WSAGetLastError();
    // Retryable errors: reestablish socket and retry up to 5 times.
    if (host_err == WSAENETRESET || host_err == WSAECONNRESET ||
        host_err == WSAETIMEDOUT) {
      XELOGW("WSARecvFrom retry {}/5 after error {}", recv_retry + 1, host_err);
      {
        std::lock_guard lock(receive_socket_mutex_);
        SOCKET old = native_handle_;
        SOCKET fresh = socket(af_, type_,
                              proto_ == Protocol::X_IPPROTO_VDP
                                  ? (int)Protocol::X_IPPROTO_UDP
                                  : (int)proto_);
        if (fresh != INVALID_SOCKET) {
          closesocket(old);
          native_handle_ = fresh;
          fds.fd = native_handle_;
          if (bound_ && bound_port_) {
            sockaddr_in bind_addr = {};
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_port = htons(bound_port_);
            bind_addr.sin_addr.s_addr = INADDR_ANY;
            ::bind(native_handle_, (sockaddr*)&bind_addr, sizeof(bind_addr));
          }
        }
      }
      Sleep(10 * (recv_retry + 1));
      // Need to re-poll after reestablish.
      delete[] buffers;
      buffers = nullptr;
      goto recv_loop;
    }
    // Non-retryable errors.
    switch (host_err) {
      case WSAEWOULDBLOCK:
        // Poll said ready but recv says no data — race. Loop back.
        delete[] buffers;
        buffers = nullptr;
        if (wait) goto recv_loop;
        receive_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSAEWOULDBLOCK;
        break;
      case WSAENOTSOCK:
      case WSAEINVAL:
        receive_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSA_OPERATION_ABORTED;
        delete[] buffers;
        buffers = nullptr;
        goto threadexit;
      case WSAEMSGSIZE:
        receive_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSAEMSGSIZE;
        break;
      case WSAENETDOWN:
        receive_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSAENETDOWN;
        break;
      case WSAEHOSTDOWN:
      case WSAEHOSTUNREACH:
      case WSAECONNABORTED:
      case WSAENOTCONN:
      case WSAESHUTDOWN:
        XELOGI("WSARecvFrom failed with network error {}", host_err);
        receive_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSAENETDOWN;
        break;
      default:
        XELOGW("WSARecvFrom unknown host error {}", host_err);
        receive_async_data.overlapped->internal_high = 0;
        ret = 0;
        break;
    }
    break;  // Non-retryable — exit loop.
  }
  if (ret >= 0) {
    receive_async_data.overlapped->internal_high = 0;
    receive_async_data.overlapped->internal = bytes_received;
    if (wait && bytes_received > 0) {
      XELOGI("XSocket async recv: {} bytes received", bytes_received);
    }
  }
  if (receive_async_data.from && sa) {
    receive_async_data.from->to_guest(sa);
  }
  receive_async_data.overlapped->offset = flags;
#else
// Linux-specific code for recvmsg
#endif
  delete[] buffers;
  buffers = nullptr;

  // Signal completion for this packet, then loop back to wait for more.
  if (wait && ret >= 0) {
    MemoryBarrier();
    receive_async_data.overlapped->offset_high |= WSAInfo::complete;
    if (receive_async_data.overlapped->event_handle) {
      auto ev = kernel_state()->object_table()->LookupObject<XEvent>(
          receive_async_data.overlapped->event_handle);
      if (ev) {
        xboxkrnl::xeNtSetEvent(receive_async_data.overlapped->event_handle,
                               nullptr);
      }
    }
    receive_cv_.notify_all();
    // Clear complete for the next iteration.
    receive_async_data.overlapped->offset_high &= ~WSAInfo::complete;
    goto recv_loop;
  }
threadexit:
  if (receive_async_data.heap_allocated) {
    delete[] receive_async_data.buffers;
  }
  // Ensure all overlapped field writes are visible before setting complete.
  MemoryBarrier();
  receive_async_data.overlapped->offset_high |= WSAInfo::complete;
  if (receive_async_data.overlapped->event_handle) {
    auto ev = kernel_state()->object_table()->LookupObject<XEvent>(
        receive_async_data.overlapped->event_handle);
    if (ev) {
      xboxkrnl::xeNtSetEvent(receive_async_data.overlapped->event_handle,
                             nullptr);
    }
  }
  // Fire completion routine APC if one was provided.
  if (wait && receive_async_data.completion_routine &&
      receive_async_data.calling_thread) {
    receive_async_data.calling_thread->EnqueueApc(
        receive_async_data.completion_routine,
        receive_async_data.overlapped->internal_high,  // dwError
        receive_async_data.overlapped->internal,       // cbTransferred
        receive_async_data.overlapped_guest_ptr);      // lpOverlapped
  }
  receive_cv_.notify_all();
  return ret;
}
int XSocket::WSARecvFrom(XWSABUF* buffers, uint32_t num_buffers,
                         xe::be<uint32_t>* num_bytes_recv_ptr,
                         xe::be<uint32_t>* flags_ptr, XSOCKADDR_IN* from_ptr,
                         xe::be<uint32_t>* fromlen_ptr,
                         XWSAOVERLAPPED* overlapped_ptr,
                         uint32_t completion_routine,
                         uint32_t overlapped_guest_ptr) {
  if (!buffers || !flags_ptr || (from_ptr && !fromlen_ptr)) {
    SetLastWSAError(X_WSAError::X_WSA_INVALID_PARAMETER);
    return -1;
  }
  // On win32 we could pipe all this directly to WSARecvFrom.
  // We would however need find a way to call the completion callback without
  // relying on the caller to set the "alertable" flag to true when waiting. We
  // also need to do our own async handling anyway for Linux so we might as well
  // make the code paths the same to improve symmetry in behaviour.
  if (overlapped_ptr) {
    overlapped_ptr->offset_high |= WSAInfo::recvfrom_flag;
  }

  // Direct non-blocking recv. No async worker — the game polls us directly.
  // This avoids the async worker stealing packets from the game thread.
  WSARecvFromData receive_async_data = {};
  receive_async_data.buffers = buffers;
  receive_async_data.num_buffers = num_buffers;
  receive_async_data.flags = *flags_ptr;
  receive_async_data.from = from_ptr;
  receive_async_data.from_len = fromlen_ptr;
  XWSAOVERLAPPED probe_overlapped;
  std::memset(&probe_overlapped, 0, sizeof(probe_overlapped));
  receive_async_data.overlapped = &probe_overlapped;
  int ret = PollWSARecvFrom(false, receive_async_data);
  if (ret < 0) {
    auto wsa_error = probe_overlapped.internal_high.get();
    if (wsa_error == (uint32_t)X_WSAError::X_WSAEWOULDBLOCK) {
      // No data available — return 0 with 0 bytes so the game keeps polling.
      if (overlapped_ptr) {
        overlapped_ptr->internal = 0;
        overlapped_ptr->internal_high = 0;
      }
      if (num_bytes_recv_ptr) {
        *num_bytes_recv_ptr = 0;
      }
      SetLastWSAError(X_WSAError::X_WSAEWOULDBLOCK);
      return 0;
    }
    // Real error — propagate.
    SetLastWSAError((X_WSAError)wsa_error);
    return -1;
  }
  // Got data — copy probe results to real overlapped and return.
  if (overlapped_ptr) {
    overlapped_ptr->internal = probe_overlapped.internal;
    overlapped_ptr->internal_high = probe_overlapped.internal_high;
    overlapped_ptr->offset = probe_overlapped.offset;
    overlapped_ptr->offset_high |= WSAInfo::complete;
    MemoryBarrier();
    if (overlapped_ptr->event_handle) {
      xboxkrnl::xeNtSetEvent(overlapped_ptr->event_handle, nullptr);
    }
  }
  if (num_bytes_recv_ptr) {
    *num_bytes_recv_ptr = probe_overlapped.internal;
  }
  *flags_ptr = probe_overlapped.offset;
  return 0;
}
bool XSocket::WSAGetOverlappedResult(XWSAOVERLAPPED* overlapped_ptr,
                                     xe::be<uint32_t>* bytes_transferred,
                                     bool wait, xe::be<uint32_t>* flags_ptr) {
  if (!overlapped_ptr || !bytes_transferred || !flags_ptr) {
    SetLastWSAError(X_WSAError::X_WSA_INVALID_PARAMETER);
    return false;
  }
  if (overlapped_ptr->offset_high & WSAInfo::sendto_flag) {
    std::unique_lock lock(send_mutex_);
    if (!(overlapped_ptr->offset_high & WSAInfo::complete)) {
      if (wait) {
        send_cv_.wait(lock, [&] {
          return (overlapped_ptr->offset_high & WSAInfo::complete) != 0;
        });
      } else {
        SetLastWSAError(X_WSAError::X_WSA_IO_INCOMPLETE);
        return false;
      }
    }
    // Pair with MemoryBarrier() in PushWSASendTo before setting complete.
    MemoryBarrier();
    if (overlapped_ptr->internal_high != 0) {
      SetLastWSAError((X_WSAError)overlapped_ptr->internal_high.get());
      // Operation complete with error.
      return false;
    }
    *bytes_transferred = overlapped_ptr->internal;
    *flags_ptr = overlapped_ptr->offset;
  }
  if (overlapped_ptr->offset_high & WSAInfo::recvfrom_flag) {
    std::unique_lock lock(receive_mutex_);
    if (!(overlapped_ptr->offset_high & WSAInfo::complete)) {
      if (wait) {
        receive_cv_.wait(lock, [&] {
          return (overlapped_ptr->offset_high & WSAInfo::complete) != 0;
        });
      } else {
        SetLastWSAError(X_WSAError::X_WSA_IO_INCOMPLETE);
        return false;
      }
    }
    // Pair with MemoryBarrier() in PollWSARecvFrom before setting complete.
    MemoryBarrier();
    if (overlapped_ptr->internal_high != 0) {
      SetLastWSAError((X_WSAError)overlapped_ptr->internal_high.get());
      return false;
    }
    *bytes_transferred = overlapped_ptr->internal;
    *flags_ptr = overlapped_ptr->offset;
  }
  return true;
}
int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len,
              flags);
}
int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                    XSOCKADDR_IN* to, uint32_t to_len) {
  if (XLiveAPI::upnp_handler) {
    to->address_port =
        XLiveAPI::upnp_handler->GetMappedBindPort(to->address_port);
  }
  sockaddr addr = to->to_host();
  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? &addr : nullptr, to_len);
}
int XSocket::WSAEventSelect(uint64_t socket_handle, uint64_t event_handle,
                            uint32_t flags) {
  return ::WSAEventSelect(socket_handle, reinterpret_cast<HANDLE>(event_handle),
                          flags);
}
bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port,
                          const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;
  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);
  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);
  // TODO: Limit on number of incoming packets?
  return true;
}
X_STATUS XSocket::GetPeerName(XSOCKADDR_IN* buf, int* buf_len) {
  sockaddr addr = buf->to_host();
  sockaddr* sa = const_cast<sockaddr*>(&addr);
  int ret = getpeername(native_handle_, sa, (socklen_t*)buf_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
  buf->to_guest(sa);
  return X_STATUS_SUCCESS;
}
X_STATUS XSocket::GetSockName(XSOCKADDR_IN* buf, int* buf_len) {
  sockaddr addr = buf->to_host();
  sockaddr* sa = const_cast<sockaddr*>(&addr);
  int ret = getsockname(native_handle_, sa, (socklen_t*)buf_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
  buf->to_guest(sa);
  return X_STATUS_SUCCESS;
}
uint32_t XSocket::GetLastWSAError() const {
  // Prefer the stored error — WSAGetLastError() is thread-local and can be
  // cleared by any intervening Windows API call (mutex, alloc, etc.).
  uint32_t stored = last_wsa_error_.exchange(0);
  if (stored != 0) {
    return stored;
  }
#ifdef XE_PLATFORM_WIN32
  int host_err = WSAGetLastError();
#else
  int host_err = errno;
#endif
  // Map host OS error codes to Xbox 360 WSA error codes.
  switch (host_err) {
    case 0:
      return 0;
#ifdef XE_PLATFORM_WIN32
    case WSAEINTR:
      return (uint32_t)X_WSAError::X_WSAEINTR;
    case WSAEBADF:
      return (uint32_t)X_WSAError::X_WSAEBADF;
    case WSAEACCES:
      return (uint32_t)X_WSAError::X_WSAEACCES;
    case WSAEFAULT:
      return (uint32_t)X_WSAError::X_WSAEFAULT;
    case WSAEINVAL:
      return (uint32_t)X_WSAError::X_WSAEINVAL;
    case WSAEMFILE:
      return (uint32_t)X_WSAError::X_WSAEMFILE;
    case WSAEWOULDBLOCK:
      return (uint32_t)X_WSAError::X_WSAEWOULDBLOCK;
    case WSAEINPROGRESS:
      return (uint32_t)X_WSAError::X_WSAEINPROGRESS;
    case WSAEALREADY:
      return (uint32_t)X_WSAError::X_WSAEALREADY;
    case WSAENOTSOCK:
      return (uint32_t)X_WSAError::X_WSAENOTSOCK;
    case WSAEDESTADDRREQ:
      return (uint32_t)X_WSAError::X_WSAEDESTADDRREQ;
    case WSAEMSGSIZE:
      return (uint32_t)X_WSAError::X_WSAEMSGSIZE;
    case WSAEPROTOTYPE:
      return (uint32_t)X_WSAError::X_WSAEPROTOTYPE;
    case WSAENOPROTOOPT:
      return (uint32_t)X_WSAError::X_WSAENOPROTOOPT;
    case WSAEPROTONOSUPPORT:
      return (uint32_t)X_WSAError::X_WSAEPROTONOSUPPORT;
    case WSAESOCKTNOSUPPORT:
      return (uint32_t)X_WSAError::X_WSAESOCKTNOSUPPORT;
    case WSAEOPNOTSUPP:
      return (uint32_t)X_WSAError::X_WSAEOPNOTSUPP;
    case WSAEPFNOSUPPORT:
      return (uint32_t)X_WSAError::X_WSAEPFNOSUPPORT;
    case WSAEAFNOSUPPORT:
      return (uint32_t)X_WSAError::X_WSAEAFNOSUPPORT;
    case WSAEADDRINUSE:
      return (uint32_t)X_WSAError::X_WSAEADDRINUSE;
    case WSAEADDRNOTAVAIL:
      return (uint32_t)X_WSAError::X_WSAEADDRNOTAVAIL;
    case WSAENETDOWN:
      return (uint32_t)X_WSAError::X_WSAENETDOWN;
    case WSAENETUNREACH:
      return (uint32_t)X_WSAError::X_WSAENETUNREACH;
    case WSAENETRESET:
      return (uint32_t)X_WSAError::X_WSAENETRESET;
    case WSAECONNABORTED:
      return (uint32_t)X_WSAError::X_WSAECONNABORTED;
    case WSAECONNRESET:
      return (uint32_t)X_WSAError::X_WSAECONNRESET;
    case WSAENOBUFS:
      return (uint32_t)X_WSAError::X_WSAENOBUFS;
    case WSAEISCONN:
      return (uint32_t)X_WSAError::X_WSAEISCONN;
    case WSAENOTCONN:
      return (uint32_t)X_WSAError::X_WSAENOTCONN;
    case WSAESHUTDOWN:
      return (uint32_t)X_WSAError::X_WSAESHUTDOWN;
    case WSAETIMEDOUT:
      return (uint32_t)X_WSAError::X_WSAETIMEDOUT;
    case WSAECONNREFUSED:
      return (uint32_t)X_WSAError::X_WSAECONNREFUSED;
    case WSAEHOSTDOWN:
      return (uint32_t)X_WSAError::X_WSAEHOSTDOWN;
    case WSAEHOSTUNREACH:
      return (uint32_t)X_WSAError::X_WSAEHOSTUNREACH;
    case WSAEPROCLIM:
      return (uint32_t)X_WSAError::X_WSAEPROCLIM;
    case WSANOTINITIALISED:
      return (uint32_t)X_WSAError::X_WSANOTINITIALISED;
    case WSAEDISCON:
      return (uint32_t)X_WSAError::X_WSAEDISCON;
    case WSA_IO_PENDING:
      return (uint32_t)X_WSAError::X_WSA_IO_PENDING;
    case WSA_IO_INCOMPLETE:
      return (uint32_t)X_WSAError::X_WSA_IO_INCOMPLETE;
    case WSA_OPERATION_ABORTED:
      return (uint32_t)X_WSAError::X_WSA_OPERATION_ABORTED;
    case ERROR_INVALID_PARAMETER:
      return (uint32_t)X_WSAError::X_WSA_INVALID_PARAMETER;
#else
    case EACCES:
      return (uint32_t)X_WSAError::X_WSAEACCES;
    case EFAULT:
      return (uint32_t)X_WSAError::X_WSAEFAULT;
    case EINVAL:
      return (uint32_t)X_WSAError::X_WSAEINVAL;
    case EWOULDBLOCK:
      return (uint32_t)X_WSAError::X_WSAEWOULDBLOCK;
    case ENOTSOCK:
      return (uint32_t)X_WSAError::X_WSAENOTSOCK;
    case EMSGSIZE:
      return (uint32_t)X_WSAError::X_WSAEMSGSIZE;
    case ENETDOWN:
      return (uint32_t)X_WSAError::X_WSAENETDOWN;
    case EADDRINUSE:
      return (uint32_t)X_WSAError::X_WSAEADDRINUSE;
#endif
    default:
      XELOGW("XSocket::GetLastWSAError unmapped host error: {}", host_err);
      return (uint32_t)host_err;
  }
}
void XSocket::SetLastWSAError(X_WSAError error) const {
  last_wsa_error_ = (uint32_t)error;
#ifdef XE_PLATFORM_WIN32
  WSASetLastError((int)error);
#endif
  errno = (int)error;
}
}  // namespace kernel
}  // namespace xe