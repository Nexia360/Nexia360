/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "src/xenia/kernel/xsocket.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "xenia/base/platform.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"

// Network thread priority setting
DEFINE_int32(network_priority, 3,
             "Network thread priority setting: 0 - Leave Alone, 1 - Below "
             "Normal, 2 - Above Normal, 3 - High",
             "Live");

DECLARE_bool(bind_interface);

using namespace std::chrono_literals;

namespace xe {
namespace kernel {

namespace {
// TEMPORARILY DISABLED. Flip either back to true to re-enable; every code path
// below is intact and gated on these two switches only.
//
//  * kEnableVdpIdentityTag - append/strip the 18-byte XUID:Port trailer on VDP
//    game traffic (the "3rd buffer"). Off = packets go out plain, exactly as
//    upstream sends them, and nothing is stripped on receive.
//  * kEnableHubPortRemap   - hub-coordinated bind port (AllocateHostPort) and
//    the PortMap neighbour walk on EADDRINUSE. Off = the guest's requested port
//    is bound as-is and the advertised player port is never moved.
constexpr bool kEnableVdpIdentityTag = false;
constexpr bool kEnableHubPortRemap = false;

// Nexia in-packet identity trailer for VDP game traffic:
//   [8 x 0x00][be64 sender XUID][be16 sender port]
// Appended after the VDP voice data on send (to peers confirmed capable), and
// stripped on receive. The sender tells the peer who it is AND which port to
// reach it on (the peer can't derive the port from the shared public IP). The
// sentinel + XUID range check keeps it distinguishable; untagged packets
// (old/real peers) pass through untouched.
constexpr uint32_t kVdpTagSize = 18;

bool IsPlausibleXuid(uint64_t x) {
  const uint16_t hi = uint16_t(x >> 48);
  return x != 0 && (hi == 0x0009 || (hi & 0xF000) == 0xE000);
}

// Build the tag (local XUID + local reachable port) into out (kVdpTagSize).
void BuildVdpTag(uint64_t xuid, uint16_t port, uint8_t* out) {
  std::memset(out, 0, 8);
  xe::be<uint64_t> be_xuid = xuid;
  std::memcpy(out + 8, &be_xuid, sizeof(be_xuid));
  xe::be<uint16_t> be_port = port;
  std::memcpy(out + 16, &be_port, sizeof(be_port));
}

// Validate the gathered tail. On a valid tag, fills *xuid/*port and returns
// true; otherwise returns false.
bool ParseVdpTag(const uint8_t* tail, uint64_t* xuid, uint16_t* port) {
  for (int i = 0; i < 8; i++) {
    if (tail[i] != 0) {
      return false;
    }
  }
  xe::be<uint64_t> be_xuid;
  std::memcpy(&be_xuid, tail + 8, sizeof(be_xuid));
  if (!IsPlausibleXuid(be_xuid)) {
    return false;
  }
  xe::be<uint16_t> be_port;
  std::memcpy(&be_port, tail + 16, sizeof(be_port));
  *xuid = be_xuid;
  *port = be_port;
  return true;
}

// Copy the last kVdpTagSize logical bytes of a scattered datagram into out16,
// crossing native scatter-buffer boundaries (the tag can straddle two buffers).
// seg_ptr(i)/seg_len(i) describe the native segments in fill order; `total` is
// bytes actually received. No copy of the payload — only the 16-byte tail.
template <typename PtrFn, typename LenFn>
bool GatherVdpTail(PtrFn seg_ptr, LenFn seg_len, uint32_t num_segs,
                   size_t total, uint8_t* out16) {
  if (total < kVdpTagSize) {
    return false;
  }
  const size_t start = total - kVdpTagSize;
  size_t seen = 0, produced = 0;
  for (uint32_t i = 0; i < num_segs && produced < kVdpTagSize; i++) {
    const size_t fill = std::min<size_t>(seg_len(i), total - seen);
    if (seen + fill > start) {
      const size_t from = start > seen ? start - seen : 0;
      const uint8_t* p = seg_ptr(i);
      for (size_t j = from; j < fill && produced < kVdpTagSize; j++) {
        out16[produced++] = p[j];
      }
    }
    seen += fill;
    if (seen >= total) {
      break;
    }
  }
  return produced == kVdpTagSize;
}

// One scatter/gather segment for the async send path, mapped onto the native
// type. sendmsg() with an iovec array gathers exactly like WSASendTo() with a
// WSABUF array - one datagram, identical bytes on the wire - so the on-wire
// format (including the VDP identity trailer) is the same on both platforms.
#if XE_PLATFORM_WIN32
using XeSendSeg = WSABUF;
#else
using XeSendSeg = iovec;
#endif

inline void SetSendSeg(XeSendSeg& seg, void* data, size_t length) {
#if XE_PLATFORM_WIN32
  seg.buf = reinterpret_cast<CHAR*>(data);
  seg.len = static_cast<ULONG>(length);
#else
  seg.iov_base = data;
  seg.iov_len = length;
#endif
}
}  // namespace

// Translate socket options to native
// Note:
// SO_DONTLINGER = ~SO_LINGER
// SO_EXCLUSIVEADDRUSE = ~SO_REUSEADDR
// TODO: Check SO_DONTLINGER and SO_EXCLUSIVEADDRUSE usage on linux
const std::map<uint32_t, uint32_t> supported_socket_options = {
    {0x0004, SO_REUSEADDR}, {0x0020, SO_BROADCAST}, {0x0080, SO_LINGER},
    {0x1001, SO_SNDBUF},    {0x1002, SO_RCVBUF},    {0x1005, SO_SNDTIMEO},
    {0x1006, SO_RCVTIMEO},  {~0x0080, ~SO_LINGER},  {~0x0004, ~SO_REUSEADDR}};

// Translate socket TCP options to native
const std::map<uint32_t, uint32_t> supported_tcp_options = {
    {0x0001, TCP_NODELAY}};

// Translate socket levels to native
const std::map<uint32_t, uint32_t> supported_levels = {{0xFFFF, SOL_SOCKET},
                                                       {0x6, IPPROTO_TCP}};

// Translate ioctl commands to native
const std::map<uint32_t, uint32_t> supported_controls = {
    {0x8004667E, FIONBIO}, {0x4004667F, FIONREAD}};

XSocket::XSocket(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  if (!socket_closed_) {
    Close();
  }
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;
  vdp_ = false;

  if (!type) {
    if (proto == X_IPPROTO_UDP || proto == X_IPPROTO_VDP) {
      type_ = X_SOCK_DGRAM;
    } else if (proto == X_IPPROTO_TCP) {
      type_ = X_SOCK_STREAM;
    }
  }

  if (!proto) {
    if (type_ == X_SOCK_DGRAM) {
      proto_ = X_IPPROTO_UDP;
    } else if (type_ == X_SOCK_STREAM) {
      proto_ = X_IPPROTO_TCP;
    }
  } else if (proto == X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto_ = X_IPPROTO_UDP;
    vdp_ = true;
  }

  if (!type && !proto) {
    type_ = X_SOCK_STREAM;
    proto_ = X_IPPROTO_TCP;
  }

  native_handle_ = socket(af, type_, proto_);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

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

X_STATUS XSocket::Close() {
  std::unique_lock lock(receive_mutex_);
  if (active_overlapped_ &&
      !(active_overlapped_->offset_high & WSAInfo::complete)) {
    active_overlapped_->offset_high |= WSAInfo::closed;
  }
  lock.unlock();

  std::unique_lock socket_lock(receive_socket_mutex_);
#if XE_PLATFORM_WIN32
  int ret = closesocket(native_handle_);
#else
  int ret = close(native_handle_);
#endif
  socket_lock.unlock();

  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  socket_closed_ = true;

  return X_STATUS_SUCCESS;
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

int XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                       uint32_t optlen) {
  if (level == 0xFFFF && (optname == SO_MARKINSECURE || optname == SO_PRIVATE ||
                          optname == SO_GRANTINSECURE)) {
    // Disable socket encryption
    secure_ = false;
    return X_ERROR_SUCCESS;
  }

  int native_level = level;

  assert_false(!supported_levels.contains(level));

  if (supported_levels.contains(level)) {
    native_level = supported_levels.at(level);
  }

  int native_optname = optname;

  if (level == 0xFFFF) {
    assert_false(!supported_socket_options.contains(optname));

    if (supported_socket_options.contains(optname)) {
      native_optname = supported_socket_options.at(optname);
    }
  }

  if (level == IPPROTO_TCP) {
    assert_false(!supported_tcp_options.contains(optname));

    if (supported_tcp_options.contains(optname)) {
      native_optname = supported_tcp_options.at(optname);
    }
  }

  void* proper_ptr =
      GetOptValueWithProperEndianness(optval_ptr, optname, optlen);

  int ret = setsockopt(native_handle_, native_level, native_optname,
                       static_cast<const char*>(proper_ptr), optlen);

  // Cheezy way to check if we created some additional allocation.
  if (optval_ptr != proper_ptr) {
    free(proper_ptr);
  }

  if (ret < 0) {
    // TODO: WSAGetLastError()
    XELOGE("XSocket::SetOption: failed with error {:08X}", GetLastWSAError());
    return -1;
  }

  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_ERROR_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint32_t* arg_ptr) {
#ifdef XE_PLATFORM_WIN32
  const u_long initial_param = xe::load_and_swap<uint32_t>(arg_ptr);
  u_long param = initial_param;

  int ret = ioctlsocket(native_handle_, cmd, &param);

  // Parameter was written to therefore byte swap output
  if (initial_param != param) {
    xe::store_and_swap(arg_ptr, static_cast<uint32_t>(param));
  }

  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
#else
  int native_cmd = cmd;

  assert_false(!supported_controls.contains(cmd));

  if (supported_controls.contains(cmd)) {
    native_cmd = supported_controls.at(cmd);
  }

  int ret = ioctl(native_handle_, native_cmd, arg_ptr);

  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
#endif
}

X_STATUS XSocket::Connect(const XSOCKADDR_IN* name, int name_len) {
  XSOCKADDR_IN sa_in = *name;

  const auto upnp = kernel_state()->emulator()->GetUPnP();

  if (upnp) {
    sa_in.address_port = upnp->GetMappedConnectPort(name->address_port);
  }

  sockaddr addr = sa_in.to_host();

  int ret = connect(native_handle_, &addr, name_len);

  // Implicit Bind
  bound_port_ = sa_in.address_port;
  bound_ = true;

  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(const XSOCKADDR_IN* name, int name_len) {
  XSOCKADDR_IN sa_in = *name;

  const auto upnp = kernel_state()->emulator()->GetUPnP();

  if (upnp) {
    sa_in.address_port = upnp->GetMappedBindPort(name->address_port);
  }

  // Hub-coordinated allocation for netplay (VDP) host sockets: reserve a free
  // (public-IP, port) with the hub BEFORE binding, so two hosts behind one
  // public IP get distinct ports (neither the local bind nor UPnP can
  // coordinate across separate machines). Bind that port, map the game's
  // requested port onto it, and advertise it.
  auto* xlive_api = kernel_state()->GetXboxLiveAPI();
  if (kEnableHubPortRemap && vdp_ && xlive_api &&
      xlive_api->IsConnectedToServer()) {
    const uint16_t guest_port = name->address_port;
    const uint16_t hub_port = xlive_api->AllocateHostPort();
    sa_in.address_port = hub_port;
    if (upnp) {
      upnp->AddMappedBindPort(guest_port, hub_port);
    }
    if (guest_port == xlive_api->GetLocalPlayerPort()) {
      xlive_api->SetPlayerPort(hub_port);
    }
  }

  sockaddr addr = sa_in.to_host();

  // Force socket to bind to the IP of the selected interface
  const bool force_interface = cvars::bind_interface;
  in_addr forced_interface_addr = {};

  if (force_interface) {
    sockaddr_in* addr_in = reinterpret_cast<sockaddr_in*>(&addr);

    const auto network_adapter =
        kernel_state()->emulator()->GetNetworkAdapterManager();

    forced_interface_addr =
        network_adapter->GetSelectedAdapterLocalIP().sin_addr;

    // Title wants to bind to and interface but is it our bound interface?
    if (name->address_ip.s_addr) {
      assert_true(name->address_ip.s_addr == forced_interface_addr.s_addr);
    }

    addr_in->sin_addr = forced_interface_addr;
  } else {
    // Check if title tried to bind an interface
    assert_zero(name->address_ip.s_addr);
  }

  int ret = bind(native_handle_, &addr, name_len);

  // PortMap: the port is already taken by a sibling instance on this host (two
  // consoles, one PC). Walk to a free neighbor port, bind it, remap UPnP to it,
  // and — if this is the primary online (player) socket — advertise the
  // neighbor so remote peers reach us there. The guest transparently gets a
  // working port instead of a bind failure.
  if (kEnableHubPortRemap && ret < 0 && upnp &&
      GetLastWSAError() == (uint32_t)X_WSAError::X_WSAEADDRINUSE) {
    const uint16_t base = sa_in.address_port;        // port we tried to bind
    const uint16_t guest_port = name->address_port;  // guest's requested port
    for (uint16_t off = 1; off <= 32 && ret < 0; off++) {
      const uint16_t cand = base + off;
      sa_in.address_port = cand;
      addr = sa_in.to_host();
      if (force_interface) {
        reinterpret_cast<sockaddr_in*>(&addr)->sin_addr = forced_interface_addr;
      }
      ret = bind(native_handle_, &addr, name_len);
      if (ret >= 0) {
        upnp->AddMappedBindPort(guest_port, cand);
        upnp->TrackPort(cand, GetProtocolUPnPString());
        if (xlive_api && guest_port == xlive_api->GetLocalPlayerPort()) {
          xlive_api->SetPlayerPort(cand);
        }
        XELOGI("PortMap: bind port {} in use; bound neighbor {}", base, cand);
      }
    }
  }

  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_port_ = sa_in.address_port;

  if (!bound_port_) {
    bound_port_ = GetImplicitlyBoundPort();
  }

  bound_ = true;

  return X_STATUS_SUCCESS;
}

uint16_t XSocket::GetImplicitlyBoundPort() const {
  sockaddr_in sock_name = {};
  int sock_name_len = sizeof(sockaddr);

  if (!getsockname(native_handle_, reinterpret_cast<sockaddr*>(&sock_name),
                   &sock_name_len)) {
    return xe::byte_swap(sock_name.sin_port);
  }

  assert_always();
  return 0;
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
  socklen_t addrlen = 0;
  const bool is_name_and_name_len_available = name && name_len;

  if (is_name_and_name_len_available) {
    addrlen = xe::byte_swap(*name_len);
  }

  const uint64_t socket_handle = accept(native_handle_, name ? &sa : nullptr,
                                        name_len ? &addrlen : nullptr);
  if (socket_handle == -1) {
    return nullptr;
  }

  if (is_name_and_name_len_available) {
    name->to_guest(&sa);
    *name_len = xe::byte_swap(addrlen);
  }

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, socket_handle));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;
  socket->vdp_ = vdp_;

  sockaddr_in sock_name = {};
  int sock_name_len = sizeof(sockaddr);

  // Implicit Bind
  socket->bound_port_ = bound_port_ = GetImplicitlyBoundPort();
  socket->bound_ = true;

  return socket;
}

int XSocket::Shutdown(int how) { return shutdown(native_handle_, how); }

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                      XSOCKADDR_IN* from, socklen_t* from_len) {
  sockaddr sa = {};

  if (from) {
    sa = from->to_host();
  }

  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len,
                     flags, from ? &sa : nullptr, from_len);

  // TCP ignores from and from_len.
  // 555307EE expects port even with TCP, include IP anyway.
  // Verified on console.
  if (proto_ == X_IPPROTO_TCP) {
    socklen_t peer_addar_len = sizeof(sockaddr);
    getpeername(native_handle_, &sa, &peer_addar_len);
  }

  if (from) {
    from->to_guest(&sa);
  }

  // Strip a Nexia VDP identity tag if present (single flat buffer). Untagged
  // packets fall through unchanged.
  if (kEnableVdpIdentityTag && vdp_ && ret >= (int)kVdpTagSize) {
    uint64_t xuid;
    uint16_t tag_port;
    if (ParseVdpTag(buf + ret - kVdpTagSize, &xuid, &tag_port)) {
      auto* sin = reinterpret_cast<sockaddr_in*>(&sa);
      XLiveAPI::CachePacketXuid(uint32_t(sin->sin_addr.s_addr), tag_port, xuid);
      ret -= (int)kVdpTagSize;  // guest never sees the trailer
      // Present the sender at its ADVERTISED port so the guest matches it to
      // the peer's XNADDR (the raw UDP source port differs under port mapping).
      if (from) {
        from->address_port = tag_port;
      }
    }
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
#if XE_PLATFORM_WIN32
  WSAPOLLFD fds = {};
#else
  pollfd fds = {};
#endif
  fds.fd = native_handle_;
  fds.events = POLLOUT;
  uint32_t bytes_sent = 0;
  uint32_t flags = send_async_data.flags;
  XeSendSeg* buffers = nullptr;
  sockaddr addr = {};
  if (send_async_data.to) {
    addr = send_async_data.to->to_host();
  }
  int ret;
  // Identity-tag decision up front, before any `goto threadexit`, so its
  // initialization is never skipped. `tag` is filled just before the send.
  const bool do_tag = kEnableVdpIdentityTag && vdp_ && send_async_data.to &&
                      XLiveAPI::local_online_xuid &&
                      XLiveAPI::PeerSupportsTag(
                          uint32_t(send_async_data.to->address_ip.s_addr));
  uint8_t tag[kVdpTagSize];
  const uint32_t send_count = send_async_data.num_buffers + (do_tag ? 1 : 0);
  do {
#if XE_PLATFORM_WIN32
    ret = WSAPoll(&fds, 1, wait ? 1000 : 0);
#else
    ret = poll(&fds, 1, wait ? 1000 : 0);
#endif
    if (send_async_data.overlapped->offset_high & WSAInfo::closed) {
      send_async_data.overlapped->internal_high =
          (uint32_t)X_WSAError::X_WSA_OPERATION_ABORTED;
      ret = -1;
      goto threadexit;
    }
  } while (ret == 0 && wait);
  if (ret < 0) {
    // Normalize through the shared host->Xbox mapping so the classification
    // below is identical on Windows and POSIX.
    const uint32_t poll_err = GetLastWSAErrorStatic();
    if (poll_err == (uint32_t)X_WSAError::X_WSAENOTSOCK ||
        poll_err == (uint32_t)X_WSAError::X_WSAEINVAL) {
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
  // Append the identity tag as one extra scatter segment (no payload copy) for
  // VDP peers confirmed capable (do_tag decided above the poll loop).
  buffers = new XeSendSeg[send_count];
  for (uint32_t i = 0; i < send_async_data.num_buffers; i++) {
    SetSendSeg(buffers[i],
               kernel_state()->memory()->TranslateVirtual(
                   send_async_data.buffers[i].buf_ptr),
               send_async_data.buffers[i].len);
  }
  if (do_tag) {
    BuildVdpTag(XLiveAPI::local_online_xuid,
                kernel_state()->GetXboxLiveAPI()->GetPlayerPort(), tag);
    SetSendSeg(buffers[send_async_data.num_buffers], tag, kVdpTagSize);
  }
  for (int send_retry = 0; send_retry < 5; send_retry++) {
#if XE_PLATFORM_WIN32
    DWORD win_bytes_sent = 0;
    ret = ::WSASendTo(
        native_handle_, buffers, send_count, &win_bytes_sent,
        send_async_data.flags, send_async_data.to ? &addr : nullptr,
        send_async_data.to ? send_async_data.to_len : 0, nullptr, nullptr);
    if (ret >= 0) {
      bytes_sent = win_bytes_sent;
      break;
    }
#else
    // sendmsg() gathers the same segments into the same single datagram.
    msghdr msg = {};
    msg.msg_name = send_async_data.to ? &addr : nullptr;
    msg.msg_namelen = send_async_data.to ? send_async_data.to_len : 0;
    msg.msg_iov = buffers;
    msg.msg_iovlen = send_count;
    const ssize_t sent = ::sendmsg(native_handle_, &msg,
                                   static_cast<int>(send_async_data.flags));
    if (sent >= 0) {
      bytes_sent = static_cast<uint32_t>(sent);
      ret = 0;
      break;
    }
    ret = -1;
#endif
    const uint32_t host_err = GetLastWSAErrorStatic();
    // Retryable errors: reestablish socket and retry up to 5 times.
    if (host_err == (uint32_t)X_WSAError::X_WSAENETRESET ||
        host_err == (uint32_t)X_WSAError::X_WSAECONNRESET ||
        host_err == (uint32_t)X_WSAError::X_WSAETIMEDOUT) {
      XELOGW("WSASendTo retry {}/5 after error {}", send_retry + 1, host_err);
      // Reestablish the socket.
      {
        std::lock_guard lock(send_socket_mutex_);
        const uint64_t old = native_handle_;
        // vdp_ sockets are UDP underneath; proto_ already reflects that.
        const auto fresh = socket(af_, type_, proto_);
        if (fresh != -1) {
#if XE_PLATFORM_WIN32
          closesocket(old);
#else
          close(static_cast<int>(old));
#endif
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
      xe::threading::Sleep(std::chrono::milliseconds(10 * (send_retry + 1)));
      continue;
    }
    // Non-retryable errors. Compared against the Xbox codes so the behaviour is
    // identical on Windows and POSIX.
    switch ((X_WSAError)host_err) {
      case X_WSAError::X_WSAEWOULDBLOCK:
        send_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSAEWOULDBLOCK;
        break;
      case X_WSAError::X_WSAENOTSOCK:
      case X_WSAError::X_WSAEINVAL:
        send_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSA_OPERATION_ABORTED;
        delete[] buffers;
        buffers = nullptr;
        goto threadexit;
      case X_WSAError::X_WSAEMSGSIZE:
        send_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSAEMSGSIZE;
        break;
      case X_WSAError::X_WSAENETDOWN:
        send_async_data.overlapped->internal_high =
            (uint32_t)X_WSAError::X_WSAENETDOWN;
        break;
      case X_WSAError::X_WSAEHOSTDOWN:
      case X_WSAError::X_WSAEHOSTUNREACH:
      case X_WSAError::X_WSAECONNABORTED:
      case X_WSAError::X_WSAENOTCONN:
      case X_WSAError::X_WSAESHUTDOWN:
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
    send_async_data.overlapped->internal =
        bytes_sent - (do_tag && bytes_sent >= kVdpTagSize ? kVdpTagSize : 0);
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
  // Publish every write made above before the completion flag becomes visible
  // (paired with the acquire fence in WSAGetOverlappedResult).
  std::atomic_thread_fence(std::memory_order_release);
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

struct WSARecvFromData {
  XWSABUF* buffers;
  uint32_t num_buffers;
  uint32_t flags;
  XSOCKADDR_IN* from;
  xe::be<uint32_t>* from_len;
  XWSAOVERLAPPED* overlapped;
  uint32_t completion_routine;    // guest function pointer for APC callback
  uint32_t overlapped_guest_ptr;  // guest address of overlapped struct
  object_ref<XThread> calling_thread;  // thread to enqueue APC to
};

int XSocket::PollWSARecvFrom(bool wait, WSARecvFromData receive_async_data) {
  receive_async_data.overlapped->internal_high = 0;

  struct pollfd fds[1];
  fds->fd = native_handle_;
  fds->events = POLLIN;

  uint32_t bytes_received = 0;
  uint32_t flags = receive_async_data.flags;
  // Same scatter segment type as the send path (WSABUF / iovec).
  auto buffers = new XeSendSeg[receive_async_data.num_buffers];

  int ret;
  do {
#ifdef XE_PLATFORM_WIN32
    ret = WSAPoll(fds, 1, wait ? 1000 : 0);
#else
    ret = poll(fds, 1, wait ? 1000 : 0);
#endif

    if (receive_async_data.overlapped->offset_high & WSAInfo::closed) {
      receive_async_data.overlapped->internal_high =
          (uint32_t)X_WSAError::X_WSA_OPERATION_ABORTED;
      ret = -1;
      goto threadexit;
    }
  } while (ret == 0 && wait);

  if (ret < 0) {
    receive_async_data.overlapped->internal_high = GetLastWSAErrorStatic();
    XELOGE("XSocket receive thread failed polling with error {}",
           static_cast<uint32_t>(receive_async_data.overlapped->internal_high));
    goto threadexit;
  } else if (ret == 0) {
    receive_async_data.overlapped->internal_high =
        (uint32_t)X_WSAError::X_WSAEWOULDBLOCK;
    ret = -1;
    goto threadexit;
  }

#ifdef XE_PLATFORM_WIN32
  for (auto i = 0u; i < receive_async_data.num_buffers; i++) {
    buffers[i].len = receive_async_data.buffers[i].len;
    buffers[i].buf =
        reinterpret_cast<CHAR*>(kernel_state()->memory()->TranslateVirtual(
            receive_async_data.buffers[i].buf_ptr));
  }

  {
    std::unique_lock socket_lock(receive_socket_mutex_);

    sockaddr* sa = nullptr;
    if (receive_async_data.from) {
      sockaddr addr = receive_async_data.from->to_host();
      sa = const_cast<sockaddr*>(&addr);
    }

    // WSARecvFrom wants LPDWORD (unsigned long*), not uint32_t*.
    DWORD win_bytes_received = 0;
    DWORD win_flags = flags;
    ret = ::WSARecvFrom(native_handle_, buffers, receive_async_data.num_buffers,
                        &win_bytes_received, &win_flags, sa,
                        (LPINT)receive_async_data.from_len, nullptr, nullptr);
    bytes_received = win_bytes_received;
    flags = win_flags;
    if (ret < 0) {
      receive_async_data.overlapped->internal_high = GetLastWSAError();
    } else {
      receive_async_data.overlapped->internal = bytes_received;
    }
    receive_async_data.from->to_guest(sa);
    socket_lock.unlock();
  }

  receive_async_data.overlapped->offset = flags;
#else
  for (auto i = 0u; i < receive_async_data.num_buffers; i++) {
    buffers[i].iov_len = receive_async_data.buffers[i].len;
    buffers[i].iov_base = kernel_state()->memory()->TranslateVirtual(
        receive_async_data.buffers[i].buf_ptr);
  }

  msghdr msg;
  std::memset(&msg, 0, sizeof(msg));
  msg.msg_name = &n_from;
  msg.msg_namelen = n_from_len;
  msg.msg_iov = buffers;
  msg.msg_iovlen = receive_async_data.num_buffers;

  {
    std::unique_lock socket_lock(receive_socket_mutex_);
    ret = recvmsg(native_handle_, &msg, receive_async_data.flags);
    if (ret < 0) {
      receive_async_data.overlapped->internal_high = GetLastWSAError();
    } else {
      receive_async_data.overlapped->internal = ret;
    }
    socket_lock.unlock();
  }

  flags = 0;
  // MSG_PARTIAL Doesn't exist on linux?
  if (msg.msg_flags & MSG_TRUNC) {
    flags |= MSG_PARTIAL;
  }
  if (msg.msg_flags & MSG_OOB) {
    flags |= MSG_OOB;
  }
  receive_async_data.overlapped->offset = flags;

  if (ret >= 0) {
    SetLastWSAError((X_WSAError)0);
    ret = 0;
  }
#endif
  delete[] buffers;

threadexit:
  std::unique_lock lock(receive_mutex_);
  if (wait) {
    delete[] receive_async_data.buffers;
  }

  receive_async_data.overlapped->offset_high |= WSAInfo::complete;

  if (wait && receive_async_data.overlapped->event_handle) {
    xboxkrnl::xeNtSetEvent(receive_async_data.overlapped->event_handle,
                           nullptr);
  }

  // Fire completion routine APC if one was provided. A title that uses a
  // completion routine instead of waiting on the event gets no notification
  // otherwise, and waits forever.
  if (wait && receive_async_data.completion_routine &&
      receive_async_data.calling_thread) {
    receive_async_data.calling_thread->EnqueueApc(
        receive_async_data.completion_routine,
        receive_async_data.overlapped->internal_high,  // dwError
        receive_async_data.overlapped->internal,       // cbTransferred
        receive_async_data.overlapped_guest_ptr);      // lpOverlapped
  }

  receive_cv_.notify_all();
  lock.unlock();

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
    std::atomic_thread_fence(std::memory_order_release);
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

  // A send and a receive overlap are distinguished by the operation bit, so
  // each waits on its own condition variable.
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

    // Pair with the release fence in PushWSASendTo before it set complete.
    std::atomic_thread_fence(std::memory_order_acquire);

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

    std::atomic_thread_fence(std::memory_order_acquire);

    if (overlapped_ptr->internal_high != 0) {
      SetLastWSAError((X_WSAError)overlapped_ptr->internal_high.get());
      active_overlapped_ = nullptr;
      return false;
    }

    *bytes_transferred = overlapped_ptr->internal;
    *flags_ptr = overlapped_ptr->offset;

    active_overlapped_ = nullptr;
  }

  return true;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len,
              flags);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                    XSOCKADDR_IN* to, uint32_t to_len) {
  const auto upnp = kernel_state()->emulator()->GetUPnP();

  if (upnp) {
    to->address_port = upnp->GetMappedBindPort(to->address_port);
  }

  sockaddr addr = to->to_host();

  // Ensure the bound interface can route to the loopback interface/itself
  if (cvars::bind_interface) {
    sockaddr_in* addr_in = reinterpret_cast<sockaddr_in*>(&addr);

    if (addr_in->sin_addr.s_addr == xe::byte_swap(LOOPBACK)) {
      const auto network_adapter =
          kernel_state()->emulator()->GetNetworkAdapterManager();

      addr_in->sin_addr = network_adapter->GetSelectedAdapterLocalIP().sin_addr;
    }
  }

  int ret;

  // Append the local player's identity tag only to a peer positively confirmed
  // (hub-capable + advertised version) to strip it. Everything else is plain.
  if (kEnableVdpIdentityTag && vdp_ && to && XLiveAPI::local_online_xuid &&
      XLiveAPI::PeerSupportsTag(uint32_t(to->address_ip.s_addr))) {
    std::vector<uint8_t> tagged;
    tagged.reserve(buf_len + kVdpTagSize);
    tagged.assign(buf, buf + buf_len);
    uint8_t tag[kVdpTagSize];
    BuildVdpTag(XLiveAPI::local_online_xuid,
                kernel_state()->GetXboxLiveAPI()->GetPlayerPort(), tag);
    tagged.insert(tagged.end(), tag, tag + kVdpTagSize);

    const int sent =
        sendto(native_handle_, reinterpret_cast<char*>(tagged.data()),
               int(tagged.size()), flags, &addr, to_len);
    // Report the guest's own byte count - it must never see the trailer.
    ret = sent < 0 ? sent : int(buf_len);
  } else {
    ret = sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                 to ? &addr : nullptr, to_len);
  }

  // Implicit Bind
  if (!bound_port_) {
    bound_port_ = GetImplicitlyBoundPort();
    bound_ = true;
  }

  return ret;
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

X_STATUS XSocket::GetPeerName(XSOCKADDR_IN* name, int* name_len) {
  sockaddr addr = name->to_host();

  int ret = getpeername(native_handle_, &addr, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  name->to_guest(&addr);
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::GetSockName(XSOCKADDR_IN* name, int* name_len) {
  sockaddr addr = name->to_host();

  int ret = getsockname(native_handle_, &addr, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  name->to_guest(&addr);
  return X_STATUS_SUCCESS;
}

uint32_t XSocket::GetLastWSAError() const {
  // Prefer the stored error — WSAGetLastError() is thread-local and can be
  // cleared by any intervening Windows API call (mutex, alloc, etc.).
  uint32_t stored = last_wsa_error_.exchange(0);
  if (stored != 0) {
    return stored;
  }
  return GetLastWSAErrorStatic();
}
// Socketless translation of the current host WSA/errno value, used where there
// is no XSocket instance (e.g. Canary's DNS lookup). Shares the exact mapping
// with the per-socket GetLastWSAError() above.
uint32_t XSocket::GetLastWSAErrorStatic() {
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
    // The async send path classifies retryable vs fatal failures off these, so
    // they must map on POSIX too (otherwise they fall through to the raw errno
    // and no send is ever retried).
    case EINTR:
      return (uint32_t)X_WSAError::X_WSAEINTR;
    case EINPROGRESS:
      return (uint32_t)X_WSAError::X_WSAEINPROGRESS;
    case ENETRESET:
      return (uint32_t)X_WSAError::X_WSAENETRESET;
    case ECONNRESET:
      return (uint32_t)X_WSAError::X_WSAECONNRESET;
    case ECONNABORTED:
      return (uint32_t)X_WSAError::X_WSAECONNABORTED;
    case ETIMEDOUT:
      return (uint32_t)X_WSAError::X_WSAETIMEDOUT;
    case ECONNREFUSED:
      return (uint32_t)X_WSAError::X_WSAECONNREFUSED;
    case EHOSTDOWN:
      return (uint32_t)X_WSAError::X_WSAEHOSTDOWN;
    case EHOSTUNREACH:
      return (uint32_t)X_WSAError::X_WSAEHOSTUNREACH;
    case ENETUNREACH:
      return (uint32_t)X_WSAError::X_WSAENETUNREACH;
    case ENOTCONN:
      return (uint32_t)X_WSAError::X_WSAENOTCONN;
    case ESHUTDOWN:
      return (uint32_t)X_WSAError::X_WSAESHUTDOWN;
    case ENOBUFS:
      return (uint32_t)X_WSAError::X_WSAENOBUFS;
    case EISCONN:
      return (uint32_t)X_WSAError::X_WSAEISCONN;
    case EDESTADDRREQ:
      return (uint32_t)X_WSAError::X_WSAEDESTADDRREQ;
    case EAFNOSUPPORT:
      return (uint32_t)X_WSAError::X_WSAEAFNOSUPPORT;
    case EOPNOTSUPP:
      return (uint32_t)X_WSAError::X_WSAEOPNOTSUPP;
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
