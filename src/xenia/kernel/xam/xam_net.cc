/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <mutex>
#include <random>
#include <unordered_map>

// clang-format off
// We want to include platform.h first to define NOMINMAX to prevent window.h
// from defining the macros.
#include "xenia/base/platform.h"
#include <curl/curl.h>
// clang-format on

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/net_utils.h"
#include "xenia/kernel/util/network_adapter_manager.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_net.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_error.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_modules.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xsocket.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

#ifdef XE_PLATFORM_WIN32
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

DECLARE_bool(logging);

DECLARE_bool(log_mask_ips);

DECLARE_int32(network_mode);

DECLARE_bool(bind_interface);

DECLARE_bool(nexiahub_transport);

enum XNET_QOS {
  LISTEN_ENABLE = 0x01,
  LISTEN_DISABLE = 0x02,
  LISTEN_SET_DATA = 0x04,
  LISTEN_SET_BITSPERSEC = 0x08,
  XLISTEN_RELEASE = 0x10
};

enum XNET_CONNECT {
  STATUS_IDLE = 0x00,
  XNET_CONNECT_STATUS_PENDING = 0x01,
  STATUS_CONNECTED = 0x02,
  STATUS_LOST = 0x03,
};

// Tracks the secure-link connection status per peer for XNetGetConnectStatus,
// instead of reporting CONNECTED unconditionally. Mirrors the XDK lifecycle: a
// peer is IDLE until we send to it (PENDING) and only CONNECTED once we have
// actually received a packet back from it. Keyed by the peer IPv4 in the same
// host-order form the title passes to XNetGetConnectStatus (the inaOnline that
// XNetXnAddrToInAddr produced). Titles that gate their session/transport setup
// on the IDLE->PENDING->CONNECTED progression then behave as they do on
// console.
static std::mutex xnet_connect_status_mutex_;
static std::unordered_map<uint32_t, uint32_t> xnet_connect_status_;

static void XNetUpdateConnectStatus(uint32_t peer_ip, uint32_t status) {
  if (!peer_ip) {
    return;
  }
  std::lock_guard<std::mutex> lock(xnet_connect_status_mutex_);
  uint32_t& current = xnet_connect_status_[peer_ip];
  // Only ever advance the state (IDLE -> PENDING -> CONNECTED).
  if (status > current) {
    current = status;
  }
}

// Host-order peer key matching the value titles pass to XNetGetConnectStatus.
static inline uint32_t XNetPeerKey(const in_addr& addr) {
  return xe::byte_swap(static_cast<uint32_t>(addr.s_addr));
}

enum XNET_STARTUP {
  BYPASS_SECURITY = 0x01,
  ALLOCATE_MAX_DGRAM_SOCKETS = 0x02,
  ALLOCATE_MAX_STREAM_SOCKETS = 0x04,
  DISABLE_PEER_ENCRYPTION = 0x08,
};

enum XNET_XNQOSINFO {
  COMPLETE = 0x01,
  TARGET_CONTACTED = 0x02,
  TARGET_DISABLED = 0x04,
  DATA_RECEIVED = 0x08,
  PARTIAL_COMPLETE = 0x10
};

// XNetGetBroadcastVersionStatus
enum VERSION {
  OLDER = 0x01,
  NEWER = 0x02,
};

namespace xe {
namespace kernel {
namespace xam {

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

struct XNDNS {
  xe::be<int32_t> status;
  xe::be<uint32_t> cina;
  in_addr aina[8];
};
static_assert_size(XNDNS, 0x28);

struct XNQOSINFO {
  uint8_t flags;
  uint8_t reserved;
  xe::be<uint16_t> probes_xmit;
  xe::be<uint16_t> probes_recv;
  xe::be<uint16_t> data_len;
  xe::be<uint32_t> data_ptr;
  xe::be<uint16_t> rtt_min_in_msecs;
  xe::be<uint16_t> rtt_med_in_msecs;
  xe::be<uint32_t> up_bits_per_sec;
  xe::be<uint32_t> down_bits_per_sec;
};
static_assert_size(XNQOSINFO, 0x18);

struct XNQOS {
  xe::be<uint32_t> count;
  xe::be<uint32_t> count_pending;
  XNQOSINFO info[1];
};
static_assert_size(XNQOS, 0x20);

struct X_WSADATA {
  xe::be<uint16_t> version;
  xe::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  xe::be<uint16_t> max_sockets;
  xe::be<uint16_t> max_udpdg;
  xe::be<uint32_t> vendor_info_ptr;
};
static_assert_size(X_WSADATA, 0x190);

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags = 0;
  uint8_t cfgSockMaxDgramSockets = 8;
  uint8_t cfgSockMaxStreamSockets = 32;
  uint8_t cfgSockDefaultRecvBufsizeInK = 16;
  uint8_t cfgSockDefaultSendBufsizeInK = 16;
  uint8_t cfgKeyRegMax = 8;
  uint8_t cfgSecRegMax = 32;
  uint8_t cfgQosDataLimitDiv4 = 64;
  uint8_t cfgQosProbeTimeoutInSeconds = 2;
  uint8_t cfgQosProbeRetries = 3;
  uint8_t cfgQosSrvMaxSimultaneousResponses = 8;
  uint8_t cfgQosPairWaitTimeInSeconds = 2;
};
static_assert_size(XNetStartupParams, 0xD);

struct XAUTH_SETTINGS {
  xe::be<uint32_t> SizeOfStruct;
  xe::be<uint32_t> Flags;
};

struct XNQOSLISTENSTATS {
  uint32_t size_of_struct;
  uint32_t requests_received_count;
  uint32_t probes_received_count;
  uint32_t slots_full_discards_count;
  uint32_t data_replies_sent_count;
  uint32_t data_reply_bytes_sent;
  uint32_t probe_replies_sent_count;
};
static_assert_size(XNQOSLISTENSTATS, 0x1C);

struct X_TIMEVAL {
  xe::be<long> tv_sec;
  xe::be<long> tv_usec;
};
static_assert_size(X_TIMEVAL, 0x8);

// Initialize sockaddr to its default state
static void InitializeSockaddr(XSOCKADDR_IN* sockaddr_ptr) {
  if (sockaddr_ptr) {
    std::memset(sockaddr_ptr, 0, sizeof(XSOCKADDR_IN));
    sockaddr_ptr->address_family = XSocket::AddressFamily::X_AF_INET;
  }
}

XNetStartupParams xnet_startup_params{};

void Update_XNetStartupParams(XNetStartupParams& dest,
                              const XNetStartupParams& src) {
  uint8_t* dest_ptr = reinterpret_cast<uint8_t*>(&dest);
  const uint8_t* src_ptr = reinterpret_cast<const uint8_t*>(&src);

  size_t size = sizeof(XNetStartupParams);

  for (size_t i = 0; i < size; i++) {
    if (src_ptr[i] != 0 && dest_ptr[i] != src_ptr[i]) {
      dest_ptr[i] = src_ptr[i];
    }
  }
}

dword_result_t NetDll_XNetStartup_entry(dword_t caller,
                                        pointer_t<XNetStartupParams> params) {
  if (initialized_xnet_) {
    return X_ERROR_SUCCESS;
  }

  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    Update_XNetStartupParams(xnet_startup_params, *params);

    switch (xnet_startup_params.cfgFlags) {
      case BYPASS_SECURITY:
        XELOGI("XNetStartup BYPASS_SECURITY");
        break;
      case ALLOCATE_MAX_DGRAM_SOCKETS:
        XELOGI("XNetStartup ALLOCATE_MAX_DGRAM_SOCKETS");
        break;
      case ALLOCATE_MAX_STREAM_SOCKETS:
        XELOGI("XNetStartup ALLOCATE_MAX_STREAM_SOCKETS");
        break;
      case DISABLE_PEER_ENCRYPTION:
        XELOGI("XNetStartup DISABLE_PEER_ENCRYPTION");
        break;
      default:
        break;
    }
  }

  initialized_xnet_ = true;

  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(kernel_state());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetStartup, kNetworking, kImplemented);

// https://github.com/jogolden/testdev/blob/master/xkelib/syssock.h#L46
dword_result_t NetDll_XNetStartupEx_entry(dword_t caller,
                                          pointer_t<XNetStartupParams> params,
                                          dword_t versionReq) {
  return NetDll_XNetStartup_entry(caller, params);
}
DECLARE_XAM_EXPORT1(NetDll_XNetStartupEx, kNetworking, kImplemented);

dword_result_t NetDll_XNetCleanup_entry(dword_t caller) {
  if (!initialized_xnet_) {
    return static_cast<uint32_t>(X_WSAError::X_WSANOTINITIALISED);
  }

  initialized_xnet_ = false;

  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetCleanup, kNetworking, kStub);

dword_result_t XNetLogonGetMachineID_entry(lpqword_t machine_id_ptr) {
  *machine_id_ptr = GetLocalMachineId(GetConsoleMacAddress());

  // if (XLiveAPI::GetInitState() != XLiveAPI::InitState::Success) {
  //   *machine_id_ptr = 0;
  //   return X_ERROR_LOGON_NOT_LOGGED_ON;
  // }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XNetLogonGetMachineID, kNetworking, kImplemented);

dword_result_t XNetLogonGetTitleID_entry(dword_t caller, lpvoid_t params) {
  return kernel_state()->title_id();
}
DECLARE_XAM_EXPORT1(XNetLogonGetTitleID, kNetworking, kImplemented);

dword_result_t XNetLogonGetServiceNetworkID_entry() {
  return 0x50524F44;  // 'PROD'
}
DECLARE_XAM_EXPORT1(XNetLogonGetServiceNetworkID, kNetworking, kImplemented);

dword_result_t NetDll_XnpLogonGetStatus_entry(
    dword_t caller, pointer_t<SGADDR> security_gateway_ptr, lpdword_t reason) {
  if (security_gateway_ptr) {
    security_gateway_ptr.Zero();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XnpLogonGetStatus, kNetworking, kStub);

// Alias XAuthGetToken
dword_result_t XamGetToken_entry(dword_t user_index, lpstring_t url_ptr,
                                 dword_t url_size, lpdword_t token_out_ptr,
                                 pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;

    const std::string url(url_ptr, url_size);

    XELOGI("XamGetToken: URL - {}", url);

    const uint32_t token_address =
        kernel_memory()->SystemHeapAlloc(sizeof(XAM_RELYING_PARTY_TOKEN));

    auto token_ptr =
        kernel_memory()->TranslateVirtual<XAM_RELYING_PARTY_TOKEN*>(
            token_address);

    *token_out_ptr = token_address;

    const std::string mock_token = "MOCK_XBOX_STS_TOKEN";
    const uint32_t mock_token_len = static_cast<uint32_t>(mock_token.size());

    const uint32_t token_data_addrress =
        kernel_memory()->SystemHeapAlloc(mock_token.size());

    uint8_t* token_data =
        kernel_memory()->TranslateVirtual<uint8_t*>(token_data_addrress);

    std::memcpy(token_data, mock_token.data(), mock_token_len);

    token_ptr->token_data_ptr = token_data_addrress;
    token_ptr->length = mock_token_len;

    // XamFreeToken (xam_info.cc) owns the release: it frees this block and,
    // because the bytes sit in their own allocation rather than inside it,
    // token_data_ptr as well.
    RegisterIssuedToken(token_address,
                        static_cast<uint32_t>(sizeof(XAM_RELYING_PARTY_TOKEN)));

    return X_ERROR_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamGetToken, kNetworking, kStub);

dword_result_t NetDll_XNetGetOpt_entry(dword_t caller, dword_t option_id,
                                       lpvoid_t buffer_ptr,
                                       lpdword_t buffer_size) {
  assert_true(caller == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return uint32_t(X_WSAError::X_WSAEMSGSIZE);
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      XELOGE("NetDll_XNetGetOpt: option {} unimplemented", option_id.value());
      return uint32_t(X_WSAError::X_WSAEINVAL);
  }
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetOpt, kNetworking, kSketchy);

dword_result_t NetDll_XNetRandom_entry(dword_t caller, lpvoid_t buffer_ptr,
                                       dword_t length) {
  uint8_t* buffer_data_ptr = buffer_ptr.as<uint8_t*>();

  if (buffer_data_ptr == nullptr || length == 0) {
    return X_ERROR_SUCCESS;
  }

  std::random_device rnd;
  std::mt19937_64 gen(rnd());
  std::uniform_int_distribution<int> dist(0,
                                          std::numeric_limits<uint8_t>::max());

  std::generate(buffer_data_ptr, buffer_data_ptr + length,
                [&]() { return static_cast<uint8_t>(dist(gen)); });

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetRandom, kNetworking, kImplemented);

dword_result_t NetDll_WSAStartup_entry(dword_t caller, word_t version,
                                       pointer_t<X_WSADATA> data_ptr) {
  // NetDll_WSAStartup is called multiple times?
  XELOGI("NetDll_WSAStartup");

  // TODO(benvanik): abstraction layer needed.
  int ret = 0;

#ifdef XE_PLATFORM_WIN32
  WSADATA wsaData = {};

  ret = WSAStartup(version, &wsaData);

  // 415607E1 provides version 0 which returns WSAVERNOTSUPPORTED on Windows.
  // However console does not support such error, instead it returns 0.
  if (ret == WSAVERNOTSUPPORTED) {
    ret = X_ERROR_SUCCESS;
  }

  if (ret != X_ERROR_SUCCESS) {
    assert_always();
    ret = X_ERROR_SUCCESS;
  }
#endif

  if (data_ptr) {
    data_ptr.Zero();

#ifdef XE_PLATFORM_WIN32
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
#else
    data_ptr->version = version.value();
    data_ptr->version_high = 0x0202;
#endif
  }

  // WSAStartup implicitly calls XNetStartup.
  if (!ret) {
    winsock_reference_count_++;
    initialized_xnet_ = true;
  }

  // DEBUG
  /*
  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(kernel_state());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_WSAStartup, kNetworking, kImplemented);

dword_result_t NetDll_WSAStartupEx_entry(dword_t caller, word_t version,
                                         pointer_t<X_WSADATA> data_ptr,
                                         dword_t versionReq) {
  return NetDll_WSAStartup_entry(caller, version, data_ptr);
}
DECLARE_XAM_EXPORT1(NetDll_WSAStartupEx, kNetworking, kImplemented);

dword_result_t NetDll_WSACleanup_entry(dword_t caller) {
  if (!winsock_reference_count_) {
    XThread::SetLastError(
        static_cast<uint32_t>(X_WSAError::X_WSANOTINITIALISED));
    return X_SOCKET_ERROR;
  }

  --winsock_reference_count_;

  if (!winsock_reference_count_) {
    // Cleanup Resources...
    // Close all XSockets...
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_WSACleanup, kNetworking, kStub);

// Instead of using dedicated storage for WSA error like on OS.
// Xbox shares space between normal error codes and WSA errors.
// This under the hood returns directly value received from RtlGetLastError.
dword_result_t NetDll_WSAGetLastError_entry() {
  uint32_t last_error = XThread::GetLastError();
  XELOGD("NetDll_WSAGetLastError: {}", last_error);
  return last_error;
}
DECLARE_XAM_EXPORT1(NetDll_WSAGetLastError, kNetworking, kImplemented);

#ifdef XE_PLATFORM_WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET 0x9800000C  // from mstcpip.h
#endif
// Turn off the "ICMP Port Unreachable => WSAECONNRESET on next recvfrom"
// behavior (needed for the overlapped UDP recv path).
static void DisableUdpConnReset(SOCKET s) {
  BOOL new_behavior = FALSE;
  DWORD bytes_returned = 0;
  (void)WSAIoctl(s, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior),
                 nullptr, 0, &bytes_returned, nullptr, nullptr);
}
#endif

dword_result_t NetDll_WSARecvFrom_entry(
    dword_t caller, dword_t socket_handle, pointer_t<XWSABUF> buffers,
    dword_t num_buffers, lpdword_t num_bytes_recv_ptr, lpdword_t flags_ptr,
    pointer_t<XSOCKADDR_IN> from_ptr, lpdword_t fromlen_ptr,
    pointer_t<XWSAOVERLAPPED> overlapped_ptr, lpvoid_t completion_routine_ptr) {
  InitializeSockaddr(from_ptr);

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

#ifdef XE_PLATFORM_WIN32
  // In online UDP modes, proactively disable UDP connreset on this socket.
  if (cvars::network_mode >= NETWORK_MODE::XBOXLIVE) {
    DisableUdpConnReset(socket->native_handle());
  }
#endif

  int ret = socket->WSARecvFrom(
      buffers, num_buffers, num_bytes_recv_ptr, flags_ptr, from_ptr,
      fromlen_ptr, overlapped_ptr, completion_routine_ptr.guest_address(),
      overlapped_ptr.guest_address());
  if (ret < 0) {
    const auto err = socket->GetLastWSAError();
    XThread::SetLastError(err);
    return -1;
  }

  XThread::SetLastError(0);

  // A successful call with 0 bytes just means "nothing queued yet" (the recv is
  // a direct non-blocking poll), so only treat an actual payload as proof the
  // peer is alive.
  const bool received_data =
      num_bytes_recv_ptr && static_cast<uint32_t>(*num_bytes_recv_ptr) > 0;

  if (from_ptr && received_data) {
    // Heard from this peer -> the secure link is live.
    XNetUpdateConnectStatus(XNetPeerKey(from_ptr->address_ip),
                            STATUS_CONNECTED);
  }

  if (!cvars::log_mask_ips && from_ptr && received_data) {
    XELOGI("NetDll_WSARecvFrom: Received {} bytes from: {}:{}({})",
           static_cast<uint32_t>(*num_bytes_recv_ptr),
           ip_to_string(from_ptr->address_ip), from_ptr->address_port.get(),
           socket->GetProtocolUPnPString());
  }

  return ret;
}
DECLARE_XAM_EXPORT2(NetDll_WSARecvFrom, kNetworking, kImplemented,
                    kHighFrequency);

dword_result_t NetDll_WSAGetOverlappedResult_entry(
    dword_t caller, dword_t socket_handle,
    pointer_t<XWSAOVERLAPPED> overlapped_ptr, lpdword_t bytes_transferred,
    dword_t wait, lpdword_t flags_ptr) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return 0;
  }

  bool ret = socket->WSAGetOverlappedResult(overlapped_ptr, bytes_transferred,
                                            wait, flags_ptr);
  if (!ret) {
    XThread::SetLastError(socket->GetLastWSAError());
  }
  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_WSAGetOverlappedResult, kNetworking, kImplemented);

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
dword_result_t NetDll_WSASendTo_entry(
    dword_t caller, dword_t socket_handle, pointer_t<XWSABUF> buffers,
    dword_t num_buffers, lpdword_t num_bytes_sent, dword_t flags,
    pointer_t<XSOCKADDR_IN> to_ptr, dword_t to_len,
    pointer_t<XWSAOVERLAPPED> overlapped, lpvoid_t completion_routine) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

#ifdef XE_PLATFORM_WIN32
  // In online UDP modes, proactively disable UDP connreset on this socket.
  if (cvars::network_mode >= NETWORK_MODE::XBOXLIVE) {
    DisableUdpConnReset(socket->native_handle());
  }
#endif

  // Real async scatter/gather send (overlapped + completion routine), instead
  // of combining the buffers and falling back to a blocking SendTo.
  int result = socket->WSASendTo(
      buffers, num_buffers, num_bytes_sent, flags, to_ptr, to_len, overlapped,
      completion_routine.guest_address(), overlapped.guest_address());

  if (result < 0) {
    const auto err = socket->GetLastWSAError();
    XThread::SetLastError(err);

    switch (err) {
      case (uint32_t)X_WSAError::X_WSAENOTSOCK:
      case (uint32_t)X_WSAError::X_WSA_INVALID_PARAMETER:
      case (uint32_t)X_WSAError::X_WSAENOTCONN:
        // Fatal -- return error to caller.
        XELOGD("NetDll_WSASendTo: FATAL sock={} err={:08X}",
               (uint32_t)socket_handle, err);
        return -1;
      default:
        // Non-fatal (would-block / pending) -- the guest retries later.
        XELOGD("NetDll_WSASendTo: non-fatal sock={} err={:08X}",
               (uint32_t)socket_handle, err);
        return 0;
    }
  }

  XThread::SetLastError(0);

  if (to_ptr) {
    // Sent to this peer -> connection at least pending until we hear back.
    XNetUpdateConnectStatus(XNetPeerKey(to_ptr->address_ip),
                            XNET_CONNECT_STATUS_PENDING);
  }

  if (result != -1 && to_ptr && !cvars::log_mask_ips) {
    XELOGI("NetDll_WSASendTo: Send {} bytes to: {}:{}({})", result,
           ip_to_string(to_ptr->address_ip), to_ptr->address_port.get(),
           socket->GetProtocolUPnPString());
  }

  // num_bytes_sent is filled by WSASendTo itself (it knows whether the send
  // completed inline or was queued for the overlapped path).

  return result;
}
DECLARE_XAM_EXPORT1(NetDll_WSASendTo, kNetworking, kImplemented);

dword_result_t NetDll_WSAWaitForMultipleEvents_entry(dword_t num_events,
                                                     lpdword_t events,
                                                     dword_t wait_all,
                                                     dword_t timeout,
                                                     dword_t alertable) {
  if (num_events > 64) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSA_INVALID_PARAMETER));
    return -1;
  }

  uint64_t timeout_wait = 0;
  const bool wait_all_ = !static_cast<bool>(wait_all);

  if (timeout != -1) {
    timeout_wait = -10000LL * static_cast<uint64_t>(timeout);
  }

  X_STATUS result = 0;

  while (true) {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(
        num_events, events, wait_all_, 1, alertable,
        timeout != -1 ? &timeout_wait : nullptr);

    if (XFAILED(result)) {
      uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
      XThread::SetLastError(error);
      return -1;
    }

    if (!alertable || result != X_STATUS_ALERTED) {
      return result;
    }
  }
}
DECLARE_XAM_EXPORT2(NetDll_WSAWaitForMultipleEvents, kNetworking, kImplemented,
                    kBlocking);

dword_result_t NetDll_WSACreateEvent_entry() {
  auto ev = object_ref<XEvent>(new XEvent(kernel_state()));
  ev->Initialize(true, false);
  return ev->handle();
}
DECLARE_XAM_EXPORT1(NetDll_WSACreateEvent, kNetworking, kImplemented);

dword_result_t NetDll_WSACloseEvent_entry(dword_t event_handle) {
  X_STATUS result = xboxkrnl::NtClose(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_WSACloseEvent, kNetworking, kImplemented);

dword_result_t NetDll_WSAResetEvent_entry(dword_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_WSAResetEvent, kNetworking, kImplemented);

dword_result_t NetDll_WSASetEvent_entry(dword_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_WSASetEvent, kNetworking, kImplemented);

// Sets the console IP address.
dword_result_t NetDll_XNetGetTitleXnAddr_entry(dword_t caller,
                                               pointer_t<XNADDR> XnAddr_ptr) {
  XnAddr_ptr.Zero();

  // 415607D1, 4D5307E6
  // XNetStartup, WSAStartup, WSAStartupEx were not called before
  // XNetGetTitleXnAddr.

  uint32_t status = 0;

  if (cvars::network_mode == NETWORK_MODE::OFFLINE) {
    status |=
        XNADDR_STATUS::XNADDR_ETHERNET | XNADDR_STATUS::XNADDR_TROUBLESHOOT;
  }

  if (cvars::network_mode != NETWORK_MODE::OFFLINE) {
    status |= XNADDR_STATUS::XNADDR_ETHERNET | XNADDR_STATUS::XNADDR_STATIC |
              XNADDR_STATUS::XNADDR_GATEWAY | XNADDR_STATUS::XNADDR_DNS;
  }

  if (cvars::network_mode >= NETWORK_MODE::XBOXLIVE) {
    status |= XNADDR_STATUS::XNADDR_ONLINE;
  }

  XLiveAPI::IpGetConsoleXnAddr(XnAddr_ptr);

  // TODO(gibbed): A proper mac address.
  // RakNet's 360 version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup.
  // The 360-specific code is scrubbed from the RakNet repo, but there's still
  // traces of what it's doing which match the game code.
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L382
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4527
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4467
  // "Mac address is a poor solution because you can't have multiple connections
  // from the same system"

  return status;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetTitleXnAddr, kNetworking, kImplemented);

dword_result_t NetDll_XNetGetDebugXnAddr_entry(dword_t caller,
                                               pointer_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNADDR_NONE causes caller to gracefully return.
  return XNADDR_STATUS::XNADDR_NONE;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetDebugXnAddr, kNetworking, kStub);

dword_result_t NetDll_XNetGetXnAddrPlatform_entry(dword_t caller,
                                                  pointer_t<XNADDR> addr_ptr,
                                                  lpdword_t platform_type) {
  // 58411457 filters session search based on platform type

  *platform_type = addr_ptr->abOnline.platform_type;

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetXnAddrPlatform, kNetworking, kStub);

dword_result_t NetDll_XNetXnAddrToMachineId_entry(dword_t caller,
                                                  pointer_t<XNADDR> addr_ptr,
                                                  lpqword_t id_ptr) {
  id_ptr.Zero();

  if (!addr_ptr->inaOnline.s_addr || !addr_ptr->wPortOnline) {
    return static_cast<uint32_t>(X_WSAError::X_WSAEINVAL);
  }

  const MacAddress mac = MacAddress(addr_ptr->abEnet);
  const uint64_t machine_id = GetMachineId(mac.to_uint64());

  *id_ptr = machine_id;

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetXnAddrToMachineId, kNetworking, kImplemented);

dword_result_t NetDll_XNetUnregisterInAddr_entry(dword_t caller, dword_t addr) {
  XELOGI("NetDll_XNetUnregisterInAddr({:08X})",
         cvars::log_mask_ips ? 0 : addr.value());

  // return static_cast<uint32_t>(X_WSAError::X_WSAEINVAL);

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetUnregisterInAddr, kNetworking, kStub);

dword_result_t NetDll_XNetConnect_entry(dword_t caller, dword_t addr) {
  XELOGD("XNetConnect({:08X})", cvars::log_mask_ips ? 0 : addr.value());

  // Connection requested: mark the peer pending. XNetGetConnectStatus reports
  // CONNECTED once we actually receive traffic from it.
  XNetUpdateConnectStatus(addr.value(), XNET_CONNECT_STATUS_PENDING);

  // 43430806, 43430821 and 5841124E fail to connect without sleep.
  xe::threading::Sleep(150ms);

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetConnect, kNetworking, kStub);

dword_result_t NetDll_XNetGetConnectStatus_entry(dword_t caller, dword_t addr) {
  const uint32_t peer_ip = addr.value();

  uint32_t status = STATUS_IDLE;

  // Loopback / self is always connected.
  if ((peer_ip >> 24) == 0x7F) {
    status = STATUS_CONNECTED;
  } else {
    std::lock_guard<std::mutex> lock(xnet_connect_status_mutex_);
    const auto it = xnet_connect_status_.find(peer_ip);
    if (it != xnet_connect_status_.end()) {
      status = it->second;
    }
  }

  XELOGD("XNetGetConnectStatus({:08X}) = {}", cvars::log_mask_ips ? 0 : peer_ip,
         status);

  return status;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetConnectStatus, kNetworking, kStub);

dword_result_t NetDll_XNetServerToInAddr_entry(dword_t caller,
                                               dword_t server_addr,
                                               dword_t service_id,
                                               pointer_t<in_addr> pina) {
  XELOGI("XNetServerToInAddr");

  if (kernel_state()->GetXboxLiveAPI()->GetInitState() ==
      XLiveAPI::InitState::Pending) {
    return static_cast<uint32_t>(X_WSAError::X_WSANOTINITIALISED);
  }

  if (!server_addr || !service_id) {
    return static_cast<uint32_t>(X_WSAError::X_WSAEINVAL);
  }

  pina->s_addr = htonl(server_addr);

  XELOGI("Server IP: {}", ip_to_string(*pina));

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetServerToInAddr, kNetworking, kImplemented);

dword_result_t NetDll_XNetInAddrToServer_entry(dword_t caller,
                                               dword_t server_addr,
                                               pointer_t<in_addr> pina) {
  XELOGI("XNetInAddrToServer");

  if (!server_addr) {
    return static_cast<uint32_t>(X_WSAError::X_WSAEINVAL);
  }

  pina->s_addr = htonl(server_addr);

  XELOGI("Server IP: {}", ip_to_string(*pina));

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetInAddrToServer, kNetworking, kSketchy);

dword_result_t NetDll_XNetTsAddrToInAddr_entry(dword_t caller,
                                               pointer_t<TSADDR> tsaddr_ptr,
                                               dword_t service_id,
                                               pointer_t<XNKID> xnkid_ptr,
                                               pointer_t<in_addr> ina_ptr) {
  XELOGI("XNetTsAddrToInAddr");

  if (!tsaddr_ptr || !service_id || !xnkid_ptr || !ina_ptr) {
    return static_cast<uint32_t>(X_WSAError::X_WSAEINVAL);
  }

  // Use XNKID to lookup security association?

  *ina_ptr = tsaddr_ptr->inaOnline;

  IsValidXNKID(xnkid_ptr->as_uintBE64());

  XELOGI("Server IP: {}, Service ID: {:08X}", ip_to_string(*ina_ptr),
         static_cast<uint32_t>(service_id));

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetTsAddrToInAddr, kNetworking, kSketchy);

dword_result_t NetDll_XNetInAddrToString_entry(dword_t caller, dword_t ina,
                                               lpstring_t string_out,
                                               dword_t string_size) {
  in_addr addr = in_addr{};
  addr.s_addr = ntohl(ina);

  strncpy(string_out, ip_to_string(addr).c_str(), string_size);

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetInAddrToString, kNetworking, kImplemented);

// This converts a XNet address to an in_addr. The in_addr is used for
// subsequent socket calls (like a handle to a XNet address)
dword_result_t NetDll_XNetXnAddrToInAddr_entry(dword_t caller,
                                               pointer_t<XNADDR> xn_addr,
                                               pointer_t<XNKID> xid,
                                               pointer_t<in_addr> in_addr) {
  in_addr.Zero();

  // The handle is the answer - it was minted from the peer's XUID when this
  // XnAddr was built. "Is this us?" compares XUIDs, not MACs: the MAC comes
  // from the config, so two instances sharing one present the same MAC.
  if (cvars::nexiahub_transport && xn_addr->inaOnline.s_addr) {
    const uint64_t peer_xuid =
        XLiveAPI::XuidForHandle(xn_addr->inaOnline.s_addr);

    if (peer_xuid && peer_xuid == XLiveAPI::local_online_xuid) {
      in_addr->s_addr = xe::byte_swap(LOOPBACK);
      return X_ERROR_SUCCESS;
    }

    in_addr->s_addr = xn_addr->inaOnline.s_addr;
    return X_ERROR_SUCCESS;
  }

  // 494707E4, 4E4D07D1
  if (GetConsoleMacAddress() == MacAddress(xn_addr->abEnet)) {
    XELOGI("Resolving XNetXnAddrToInAddr to LOOPBACK!");

    if (cvars::bind_interface) {
      const auto network_adapter =
          kernel_state()->emulator()->GetNetworkAdapterManager();

      in_addr->s_addr =
          network_adapter->GetSelectedAdapterLocalIP().sin_addr.s_addr;
    } else {
      in_addr->s_addr = xe::byte_swap(LOOPBACK);
    }

    return X_ERROR_SUCCESS;
  }

  if (cvars::network_mode == NETWORK_MODE::LAN) {
    in_addr->s_addr = xn_addr->ina.s_addr;
  }

  if (cvars::network_mode >= NETWORK_MODE::XBOXLIVE) {
    in_addr->s_addr = xn_addr->inaOnline.s_addr;
  }

  // Nothing to register with the relay: it routes on the addresses carried in
  // each envelope, so a peer needs no permission and no channel - only that
  // it is also on the transport.
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetXnAddrToInAddr, kNetworking, kSketchy);

dword_result_t NetDll_XNetInAddrToXnAddr_entry(dword_t caller, dword_t in_addr,
                                               pointer_t<XNADDR> xn_addr,
                                               pointer_t<XNKID> xid_ptr) {
  if (xn_addr) {
    memset(xn_addr, 0, sizeof(XNADDR));
  }

  if (xid_ptr) {
    memset(xid_ptr, 0, sizeof(XNKID));
  }

  if (in_addr == BROADCAST) {
    XELOGI("Resolving XnAddr via BROADCAST!");
  }

  if (in_addr == LOOPBACK) {
    XELOGI("Resolving XnAddr via LOOPBACK!");
  }

  bool bound_interface = false;

  // 4E4D07D1
  if (cvars::bind_interface) {
    const auto network_adapter =
        kernel_state()->emulator()->GetNetworkAdapterManager();

    const uint32_t interface_ip = xe::byte_swap(
        network_adapter->GetSelectedAdapterLocalIP().sin_addr.s_addr);

    if (in_addr == interface_ip) {
      bound_interface = true;
      XELOGI("Resolving XnAddr via Interface!");
    }
  }

  if (in_addr == LOOPBACK || bound_interface || in_addr == BROADCAST) {
    XLiveAPI::IpGetConsoleXnAddr(xn_addr);
    return X_ERROR_SUCCESS;
  } else {
    xn_addr->ina.s_addr = ntohl(in_addr);
    xn_addr->inaOnline.s_addr = ntohl(in_addr);
    if (XLiveAPI::server_supports_tag) {
      // Capable hub: the peer advertises its reachable port in its VDP tag
      // (client XUID:Port -> VDP). Use that instead of the local port, which is
      // wrong for a remote peer once ports differ. Falls back to the local port
      // only until the first tagged packet from this peer arrives.
      const auto port_it =
          XLiveAPI::packet_port_cache.find(xn_addr->inaOnline.s_addr);
      xn_addr->wPortOnline =
          (port_it != XLiveAPI::packet_port_cache.end())
              ? port_it->second
              : kernel_state()->GetXboxLiveAPI()->GetPlayerPort();
    } else {
      // Legacy hub: every console used the same fixed port, so the local port
      // matched the remote one.
      xn_addr->wPortOnline = kernel_state()->GetXboxLiveAPI()->GetPlayerPort();
    }
    xn_addr->abOnline.platform_type = PLATFORM_TYPE::Xbox360;
  }

  const uint64_t cached_session_id =
      kernel_state()->GetXboxLiveAPI()->GetSystemlinkID();

  // Find cached online IP?
  if (XLiveAPI::macAddressCache.find(xn_addr->inaOnline.s_addr) ==
      XLiveAPI::macAddressCache.end()) {
    // On the transport the address is a handle, not something the hub knows -
    // so look the player up by the XUID it stands for.
    const uint64_t handle_xuid =
        cvars::nexiahub_transport
            ? XLiveAPI::XuidForHandle(xn_addr->inaOnline.s_addr)
            : 0;

    const auto player =
        handle_xuid
            ? kernel_state()->GetXboxLiveAPI()->FindPlayerByXuid(handle_xuid)
            : kernel_state()->GetXboxLiveAPI()->FindPlayer(
                  ip_to_string(xn_addr->inaOnline));

    // Record peer identity + in-packet-tag capability, keyed on the UNIQUE XUID
    // (never the shared-able IP), from the version the peer advertised to the
    // hub. This is what lets the send path decide whether to tag this peer.
    if (player->XUID()) {
      const uint64_t peer_xuid = player->XUID().get();
      XLiveAPI::ip_to_xuid[xn_addr->inaOnline.s_addr] = peer_xuid;
      XLiveAPI::peer_supports_tag[peer_xuid] =
          player->ClientVersion() >= XLiveAPI::kNexiaNetProtocolVersion;

      // The handle the guest will address this peer by is minted from that
      // XUID, so a relayed send resolves the player without consulting any
      // address at all.
      XLiveAPI::RegisterXuidHandle(peer_xuid);
    }

    // FIXME
    if (!cached_session_id || EXPLICIT_XBOXLIVE_KEY) {
      IsValidXNKID(player->SessionID());

      if (player->SessionID()) {
        XLiveAPI::sessionIdCache[xn_addr->inaOnline.s_addr] =
            player->SessionID();
      }

      if (player->MacAddress()) {
        XLiveAPI::macAddressCache[xn_addr->inaOnline.s_addr] =
            player->MacAddress();
      }
    } else {
      // Remote mac missing for systemlink!
      // 415607E1 (CoD 3) checks for this!
      //
      // If we're connected to server then use it
      if (player->MacAddress()) {
        XLiveAPI::macAddressCache[xn_addr->inaOnline.s_addr] =
            player->MacAddress();
      }
    }
  }

  const uint64_t remote_mac =
      XLiveAPI::macAddressCache[xn_addr->inaOnline.s_addr];
  MacAddress mac = MacAddress(static_cast<uint64_t>(0));

  if (remote_mac) {
    mac = MacAddress(XLiveAPI::macAddressCache[xn_addr->inaOnline.s_addr]);
  }

  std::memcpy(xn_addr->abEnet, mac.raw(), MacAddress::MacAddressSize);

  if (xid_ptr != nullptr) {
    XNKID* sessionId_ptr = kernel_memory()->TranslateVirtual<XNKID*>(xid_ptr);
    xe::be<uint64_t> session_id = 0;

    // FIXME
    if (cached_session_id) {
      session_id = cached_session_id;
    } else {
      session_id = XLiveAPI::sessionIdCache[xn_addr->inaOnline.s_addr];
    }

    memcpy(sessionId_ptr, &session_id, sizeof(uint64_t));

    IsValidXNKID(sessionId_ptr->as_uintBE64());
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetInAddrToXnAddr, kNetworking, kImplemented);

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
dword_result_t NetDll_XNetSetSystemLinkPort_entry(dword_t caller, word_t port) {
  if (!xboxkrnl::XexCheckExecutablePrivilege(
          XEX_PRIVILEGE_CROSSPLATFORM_SYSTEM_LINK)) {
    XELOGW("Title not allowed to set System Link port!");
    return static_cast<uint32_t>(X_WSAError::X_WSAEACCES);
  }

  // XNET_SYSTEMLINK_PORT = port;

  XELOGI("XNetSetSystemLinkPort: {}", port.value());

  return static_cast<uint32_t>(X_WSAError::X_WSAEADDRINUSE);
}
DECLARE_XAM_EXPORT1(NetDll_XNetSetSystemLinkPort, kNetworking, kImplemented);

dword_result_t NetDll_XNetGetSystemLinkPort_entry(dword_t caller,
                                                  lpword_t port) {
  if (!xboxkrnl::XexCheckExecutablePrivilege(
          XEX_PRIVILEGE_CROSSPLATFORM_SYSTEM_LINK)) {
    return static_cast<uint32_t>(X_WSAError::X_WSAEACCES);
  }

  *port = XNET_SYSTEMLINK_PORT;

  XELOGI("XNetGetSystemLinkPort: {}", static_cast<uint16_t>(*port));

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetSystemLinkPort, kNetworking, kImplemented);

dword_result_t NetDll_XNetGetBroadcastVersionStatus_entry(dword_t caller,
                                                          dword_t reset) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetBroadcastVersionStatus, kNetworking, kStub);

dword_result_t NetDll_XNetGetEthernetLinkStatus_entry(dword_t caller) {
  if (cvars::network_mode == NETWORK_MODE::OFFLINE) {
    return ETHERNET_STATUS::ETHERNET_LINK_NONE;
  }

  return ETHERNET_STATUS::ETHERNET_LINK_ACTIVE |
         ETHERNET_STATUS::ETHERNET_LINK_100MBPS |
         ETHERNET_STATUS::ETHERNET_LINK_FULL_DUPLEX;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetEthernetLinkStatus, kNetworking,
                    kImplemented);

dword_result_t NetDll_XNetDnsLookup_entry(dword_t caller, lpstring_t host,
                                          dword_t event_handle,
                                          lpdword_t dns_ptr) {
  XELOGI("DNS Lookup: {}", host.value());

  if (!dns_ptr) {
    return static_cast<uint32_t>(X_WSAError::X_WSAEINVAL);
  }

  const uint32_t dns_address = kernel_memory()->SystemHeapAlloc(sizeof(XNDNS));
  XNDNS* dns = kernel_memory()->TranslateVirtual<XNDNS*>(dns_address);

  dns->status = static_cast<uint32_t>(X_WSAError::X_WSAEINPROGRESS);

  *dns_ptr = dns_address;

  auto run = [=](std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
      dns->status = X_ERROR_SUCCESS;
      xboxkrnl::xeNtSetEvent(event_handle, nullptr);
      return;
    }

    ADDRINFOA hints = {.ai_family = XSocket::X_AF_INET};
    PADDRINFOA addr_info = {};

    const int status = getaddrinfo(host, nullptr, &hints, &addr_info);

    if (status) {
      XELOGI("DNS Lookup: Failed");
      dns->status = XSocket::GetLastWSAErrorStatic();
      xboxkrnl::xeNtSetEvent(event_handle, nullptr);
      return;
    }

    XELOGI("DNS Lookup: Success");

    uint32_t address_index = 0;
    addrinfo* info = addr_info;

    while (info && address_index < std::size(dns->aina) &&
           !stop_token.stop_requested()) {
      dns->aina[address_index] = *reinterpret_cast<in_addr*>(info->ai_addr);
      info = addr_info->ai_next;
      address_index++;
    }

    dns->cina = address_index;
    dns->status = XSocket::GetLastWSAErrorStatic();

    xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  };

  std::jthread dns_lookup_thread(run);

  std::unique_lock lock(dns_lookup_mutex);
  dns_lookup_threads[dns_address] = dns_lookup_thread.get_stop_source();

  dns_lookup_thread.detach();

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetDnsLookup, kNetworking, kImplemented);

dword_result_t NetDll_XNetDnsRelease_entry(dword_t caller,
                                           pointer_t<XNDNS> dns_ptr) {
  if (!dns_ptr) {
    return static_cast<uint32_t>(X_WSAError::X_WSAEINVAL);
  }

  const uint32_t dns_address =
      kernel_state()->memory()->HostToGuestVirtual(std::to_address(dns_ptr));

  std::unique_lock lock(dns_lookup_mutex);

  if (!dns_lookup_threads.contains(dns_address)) {
    XELOGI("XNetDnsRelease: DNS already released {:08X}",
           dns_ptr.guest_address());
    return X_ERROR_SUCCESS;
  }

  dns_lookup_threads.at(dns_address).request_stop();

  kernel_memory()->SystemHeapFree(dns_ptr.guest_address());

  dns_lookup_threads.erase(dns_address);

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetDnsRelease, kNetworking, kImplemented);

dword_result_t NetDll_XNetQosServiceLookup_entry(dword_t caller, dword_t flags,
                                                 dword_t event_handle,
                                                 lpdword_t qos_ptr) {
  XELOGI("XNetQosServiceLookup({}, {:08X}, {:08X}, {:08X})", caller.value(),
         flags.value(), event_handle.value(), qos_ptr.guest_address());

  if (!qos_ptr) {
    return static_cast<uint32_t>(X_WSAError::X_WSAEINVAL);
  }

  const uint32_t qos_address = kernel_memory()->SystemHeapAlloc(sizeof(XNQOS));
  XNQOS* qos = kernel_memory()->TranslateVirtual<XNQOS*>(qos_address);

  *qos_ptr = qos_address;

  qos->count_pending = 1;

  auto run = [=](std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
      return;
    }

    if (!kernel_state()->xam_state()->user_tracker()->LoggedInToLive()) {
      qos->count_pending = 0;
      return;
    }

    qos->info[0].probes_xmit = 8;
    qos->info[0].probes_recv = 8;
    qos->info[0].data_len = 0;
    qos->info[0].data_ptr = 0;
    qos->info[0].rtt_min_in_msecs = 10;
    qos->info[0].rtt_med_in_msecs = 10;

    // 4541080F and 584109B7 expect high bit/sec
    qos->info[0].up_bits_per_sec = static_cast<uint32_t>(5_MiB);
    qos->info[0].down_bits_per_sec = static_cast<uint32_t>(5_MiB);

    qos->info[0].flags = XNET_XNQOSINFO::COMPLETE |
                         XNET_XNQOSINFO::PARTIAL_COMPLETE |
                         XNET_XNQOSINFO::TARGET_CONTACTED;

    qos->count_pending = 0;
    qos->count = 1;

    // If COMPLETE, TARGET_CONTACTED or PARTIAL_COMPLETE flag is set then set
    // event.
    xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  };

  std::jthread qos_lookup_thread(run);

  std::unique_lock lock(qos_lookup_mutex);
  qos_lookup_threads[qos_address] = qos_lookup_thread.get_stop_source();

  qos_lookup_thread.detach();

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosServiceLookup, kNetworking, kStub);

dword_result_t NetDll_XNetQosRelease_entry(dword_t caller,
                                           pointer_t<XNQOS> qos_ptr) {
  if (!qos_ptr) {
    return static_cast<uint32_t>(X_WSAError::X_WSAEINVAL);
  }

  const uint32_t qos_address =
      kernel_state()->memory()->HostToGuestVirtual(std::to_address(qos_ptr));

  std::unique_lock lock(qos_lookup_mutex);

  if (!qos_lookup_threads.contains(qos_address)) {
    XELOGI("XNetQosRelease: QoS already released {:08X}",
           qos_ptr.guest_address());
    return X_ERROR_SUCCESS;
  }

  qos_lookup_threads.at(qos_address).request_stop();

  for (uint32_t i = 0; i < qos_ptr->count; i++) {
    const XNQOSINFO& qos_info = qos_ptr->info[i];

    if (qos_info.data_ptr && (qos_info.flags & XNET_XNQOSINFO::DATA_RECEIVED)) {
      kernel_memory()->SystemHeapFree(qos_info.data_ptr);
    }
  }

  kernel_memory()->SystemHeapFree(qos_ptr.guest_address());

  qos_lookup_threads.erase(qos_address);

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosRelease, kNetworking, kStub);

// Create a socket and listen for incoming probes via player port and filter
// by session id
dword_result_t NetDll_XNetQosListen_entry(
    dword_t caller, pointer_t<XNKID> sessionId, pointer_t<uint32_t> data,
    dword_t data_size, dword_t bits_per_second, dword_t flags) {
  XELOGI("XNetQosListen({:08X}, {:016X}, {:016X}, {}, {:08X}, {:08X})",
         caller.value(), sessionId.host_address(), data.host_address(),
         data_size.value(), bits_per_second.value(), flags.value());

  if (flags & LISTEN_ENABLE) {
    XELOGI("XNetQosListen LISTEN_ENABLE");
  }

  if (flags & LISTEN_DISABLE) {
    XELOGI("XNetQosListen LISTEN_DISABLE");
  }

  if (flags & LISTEN_SET_BITSPERSEC) {
    XELOGI("XNetQosListen LISTEN_SET_BITSPERSEC");
  }

  if (flags & XLISTEN_RELEASE) {
    XELOGI("XNetQosListen XLISTEN_RELEASE");
  }

  if (data_size <= 0) {
    return X_ERROR_SUCCESS;
  }

  if (data_size > (uint32_t)(xnet_startup_params.cfgQosDataLimitDiv4 * 4)) {
    assert_always();
  }

  if (data == nullptr) {
    return X_ERROR_SUCCESS;
  }

  const uint64_t session_id = sessionId->as_uintBE64();

  IsValidXNKID(session_id);

  if (flags & LISTEN_SET_DATA) {
    std::vector<uint8_t> qos_buffer(data_size);
    memcpy(qos_buffer.data(), data, data_size);

    if (kernel_state()->GetXboxLiveAPI()->UpdateQoSCache(session_id,
                                                         qos_buffer)) {
      XELOGI("XNetQosListen LISTEN_SET_DATA");

      kernel_state()->GetXboxLiveAPI()->QoSPostAsync(session_id,
                                                     std::move(qos_buffer));
    }
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosListen, kNetworking, kSketchy);

dword_result_t NetDll_XNetQosLookup_entry(
    dword_t caller, dword_t num_remote_consoles,
    pointer_t<uint32_t> remote_addresses_array_ptrs,
    pointer_t<uint32_t> sessionId_array_ptrs,
    pointer_t<uint32_t> remote_keys_array_ptrs, dword_t num_gateways,
    pointer_t<in_addr> gateways_array, pointer_t<uint32_t> service_ids_array,
    dword_t probes_count, dword_t bits_per_second, dword_t flags,
    dword_t event_handle, lpdword_t qos_ptr) {
  if (!qos_ptr) {
    return static_cast<uint32_t>(X_WSAError::X_WSAEACCES);
  }

  auto session_ids = std::make_shared<std::vector<XNKID>>();
  auto remote_keys = std::make_shared<std::vector<XNKEY>>();
  auto remote_addresses = std::make_shared<std::vector<XNADDR>>();
  std::shared_ptr<std::vector<uint32_t>> service_ids;
  std::shared_ptr<std::vector<in_addr>> security_gateways;

  if (sessionId_array_ptrs) {
    const xe::be<uint32_t>* session_id_ptrs_ptr =
        kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(
            sessionId_array_ptrs);

    const auto session_ids_ptrs = std::vector<uint32_t>(
        session_id_ptrs_ptr, session_id_ptrs_ptr + num_remote_consoles);

    for (const auto& session_id_ptr : session_ids_ptrs) {
      const XNKID session_id =
          *kernel_memory()->TranslateVirtual<XNKID*>(session_id_ptr);

      session_ids->push_back(session_id);
    }
  }

  if (remote_keys_array_ptrs) {
    const xe::be<uint32_t>* remote_keys_ptrs_ptr =
        kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(
            remote_keys_array_ptrs);

    const auto remote_keys_ptrs = std::vector<uint32_t>(
        remote_keys_ptrs_ptr, remote_keys_ptrs_ptr + num_remote_consoles);

    for (const auto& remote_keys_ptr : remote_keys_ptrs) {
      const XNKEY remote_key =
          *kernel_memory()->TranslateVirtual<XNKEY*>(remote_keys_ptr);

      remote_keys->push_back(remote_key);
    }
  }

  if (remote_addresses_array_ptrs) {
    const xe::be<uint32_t>* remote_addresses_ptrs_ptr =
        kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(
            remote_addresses_array_ptrs);

    const auto remote_addresses_ptrs =
        std::vector<uint32_t>(remote_addresses_ptrs_ptr,
                              remote_addresses_ptrs_ptr + num_remote_consoles);

    for (const auto& remote_address_ptr : remote_addresses_ptrs) {
      const XNADDR remote_address =
          *kernel_memory()->TranslateVirtual<XNADDR*>(remote_address_ptr);

      remote_addresses->push_back(remote_address);
    }
  }

  if (num_gateways) {
    XELOGI("XNetQosLookup: Gateways & Service Ids");
  }

  if (service_ids_array) {
    const xe::be<uint32_t>* service_ids_ptr =
        kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(service_ids_array);

    service_ids = std::make_shared<std::vector<uint32_t>>(
        service_ids_ptr, service_ids_ptr + num_gateways);
  }

  if (gateways_array) {
    const xe::be<in_addr>* gateways_ptr =
        kernel_memory()->TranslateVirtual<xe::be<in_addr>*>(gateways_array);

    security_gateways = std::make_shared<std::vector<in_addr>>(
        gateways_ptr, gateways_ptr + num_gateways);
  }

  const uint32_t count = num_remote_consoles + num_gateways;

  const uint32_t size = sizeof(XNQOS) + (sizeof(XNQOSINFO) * count);
  const uint32_t qos_address = kernel_memory()->SystemHeapAlloc(size);
  XNQOS* qos = kernel_memory()->TranslateVirtual<XNQOS*>(qos_address);

  *qos_ptr = qos_address;

  // 415707D1 uses count_pending to determine completion relying on async
  // implementation.
  // Therefore it expects qos->count_pending != 0 on return, otherwise will
  // cause QoS lookup spam.
  qos->count_pending = count;

  auto run = [=, shared_session_ids = session_ids](std::stop_token stop_token) {
    for (uint32_t i = 0; i < count && !stop_token.stop_requested(); i++) {
      XNQOSINFO& qos_info = qos->info[i];

      response_data chunk = {.http_code = HTTP_STATUS_CODE::HTTP_NO_CONTENT};

      if (i < shared_session_ids->size()) {
        const uint64_t session_id = shared_session_ids->at(i).as_uintBE64();
        chunk = kernel_state()->GetXboxLiveAPI()->QoSGet(session_id);
      }

      if (chunk.http_code == HTTP_STATUS_CODE::HTTP_OK) {
        if (chunk.response && chunk.size) {
          uint32_t data_ptr = kernel_memory()->SystemHeapAlloc(
              static_cast<uint16_t>(chunk.size));
          uint8_t* data = kernel_memory()->TranslateVirtual<uint8_t*>(data_ptr);

          std::memcpy(data, chunk.response, chunk.size);

          qos_info.data_ptr = data_ptr;
          qos_info.data_len = static_cast<uint16_t>(chunk.size);
          qos_info.flags |= XNET_XNQOSINFO::DATA_RECEIVED;
        }
      }

      // 415607DD and 415607D4 expect probes count, otherwise spams lookup.
      qos_info.probes_xmit = probes_count.value();
      qos_info.probes_recv = probes_count.value();
      qos_info.rtt_min_in_msecs = 10;
      qos_info.rtt_med_in_msecs = 10;
      qos_info.up_bits_per_sec = static_cast<uint32_t>(5_MiB);
      qos_info.down_bits_per_sec = static_cast<uint32_t>(5_MiB);
      qos_info.flags |=
          XNET_XNQOSINFO::COMPLETE | XNET_XNQOSINFO::TARGET_CONTACTED;

      qos->count_pending =
          std::max(static_cast<int32_t>(qos->count_pending - 1), 0);
      qos->count++;
    }

    // If COMPLETE or TARGET_CONTACTED flag is then set event.
    if (qos->count > 0) {
      xboxkrnl::xeNtSetEvent(event_handle, nullptr);
    }
  };

  std::jthread qos_lookup_thread(run);

  std::unique_lock lock(qos_lookup_mutex);
  qos_lookup_threads[qos_address] = qos_lookup_thread.get_stop_source();

  // 5345081A expects QoS results immediately on return, assume this behavior is
  // expected due to probes count of 0.
  if (probes_count) {
    qos_lookup_thread.detach();
  } else {
    XELOGI("XNetQosLookup: Sync Lookup!");
    qos_lookup_thread.join();
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosLookup, kNetworking, kImplemented);

dword_result_t NetDll_XNetQosGetListenStats_entry(
    dword_t caller, pointer_t<XNKID> xnkid_ptr,
    pointer_t<XNQOSLISTENSTATS> qos_stats_ptr) {
  XELOGI("XNetQosGetListenStats({:08X}, {:08X}, {:08X})", caller.value(),
         xnkid_ptr.guest_address(), qos_stats_ptr.guest_address());

  if (qos_stats_ptr) {
    qos_stats_ptr->requests_received_count = 1;
    qos_stats_ptr->probes_received_count = 1;
    qos_stats_ptr->slots_full_discards_count = 1;
    qos_stats_ptr->data_replies_sent_count = 1;
    qos_stats_ptr->data_reply_bytes_sent = 1;
    qos_stats_ptr->probe_replies_sent_count = 1;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosGetListenStats, kNetworking, kImplemented);

dword_result_t XamGetServiceEndpoint_entry(
    lpstring_t service_name_ptr, lpstring_t service_endpoint_ptr,
    dword_t service_endpoint_len, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!service_name_ptr || !service_endpoint_ptr || !service_endpoint_len) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const std::string service_name = service_name_ptr.value();

  std::string service_endpoint = fmt::format(
      "{}{}", kernel_state()->GetXboxLiveAPI()->GetApiAddress(), service_name);
  const std::string fallback_service_endpoint =
      fmt::format("http://xbox.com/{}", service_name);

  CURLU* url = curl_url();
  CURLUcode rc = curl_url_set(url, CURLUPART_URL, service_endpoint.c_str(), 0);

  // Check if endpoint has scheme, protocol expected for XHttpCrackUrl.
  if (rc == CURLUE_BAD_SCHEME) {
    service_endpoint = fmt::format("http://{}", service_endpoint);
  }

  curl_url_cleanup(url);

  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;

    std::memset(service_endpoint_ptr, 0, service_endpoint_len);

    // 41560914 uses endpoint length of 64
    if (service_endpoint_len < service_endpoint.size() + 1) {
      if (service_endpoint_len < fallback_service_endpoint.size() + 1) {
        extended_error = X_ERROR_INSUFFICIENT_BUFFER;
        return X_ERROR_FUNCTION_FAILED;
      } else {
        xe::string_util::copy_truncating(service_endpoint_ptr,
                                         fallback_service_endpoint.c_str(),
                                         service_endpoint_len);
        XELOGI("XamGetServiceEndpoint: {}", fallback_service_endpoint.c_str());
      }
    } else {
      xe::string_util::copy_truncating(
          service_endpoint_ptr, service_endpoint.c_str(), service_endpoint_len);
      XELOGI("XamGetServiceEndpoint: {}", service_endpoint.c_str());
    }

    return X_ERROR_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    X_RESULT result = run(extended_error, length);

    return result == X_ERROR_SUCCESS ? result : extended_error;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamGetServiceEndpoint, kNetworking, kSketchy);

dword_result_t XampXAuthStartup_entry(pointer_t<XAUTH_SETTINGS> setttings) {
  if (setttings->SizeOfStruct != sizeof(XAUTH_SETTINGS)) {
    return 0x80158401;
  }

  if (!kernel_state()->xam_state()->user_tracker()->LoggedInToLive()) {
    return 0x80158406;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XampXAuthStartup, kNetworking, kStub);

// Returns whether insecure sockets are allowed e.g. SO_GRANTINSECURE
// 58411457
dword_result_t XampXAuthIsLocalSocketAllowed_entry() { return true; }
DECLARE_XAM_EXPORT1(XampXAuthIsLocalSocketAllowed, kNetworking, kStub);

void XampXAuthShutdown_entry(lpdword_t unkn) {
  *unkn = 1;

  // Causes a call to XampXAuthGetTitleBuffer
  // *unkn = 0;
}
DECLARE_XAM_EXPORT1(XampXAuthShutdown, kNetworking, kStub);

dword_result_t XampXAuthGetTitleBuffer_entry() {
  // pointer? - non-zero causes crash
  return 0;
}
DECLARE_XAM_EXPORT1(XampXAuthGetTitleBuffer, kNetworking, kStub);

dword_result_t NetDll_inet_addr_entry(lpstring_t addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

#pragma warning(push)
#pragma warning(    \
    disable : 4996, \
    justification : "Retain original functionality e.g. Input Notation")
  uint32_t addr = inet_addr(addr_ptr);
#pragma warning(pop)
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of
  // inet_addr In case of empty string it return 0 instead of -1
  if (addr == -1 && addr_ptr.value().empty()) {
    return 0;
  }

  return xe::byte_swap(addr);
}
DECLARE_XAM_EXPORT1(NetDll_inet_addr, kNetworking, kImplemented);

bool optEnable = true;
dword_result_t NetDll_socket_entry(dword_t caller, dword_t af, dword_t type,
                                   dword_t protocol) {
  auto socket = object_ref<XSocket>(new XSocket(kernel_state()));
  X_STATUS result = socket->Initialize(XSocket::AddressFamily((uint32_t)af),
                                       XSocket::Type((uint32_t)type),
                                       XSocket::Protocol((uint32_t)protocol));
  if (XFAILED(result)) {
    socket->ReleaseHandle();

    XThread::SetLastError(XSocket::GetLastWSAErrorStatic());
    XELOGE("NetDll_socket: failed with error {:08X}",
           XSocket::GetLastWSAErrorStatic());
    return -1;
  }

  // socket->SetOption(SOL_SOCKET, 0x5801, &optEnable, sizeof(BOOL));
  // if (type == SOCK_STREAM)
  //   socket->SetOption(SOL_SOCKET, 0x5802, &optEnable, sizeof(BOOL));

  return socket->handle();
}
DECLARE_XAM_EXPORT1(NetDll_socket, kNetworking, kImplemented);

dword_result_t NetDll_closesocket_entry(dword_t caller, dword_t socket_handle) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  const auto upnp = kernel_state()->emulator()->GetUPnP();

  if (upnp) {
    CleanupUPnPActions();

    if (socket->IsBound()) {
      auto remove_port = upnp->RemovePortAsync(socket->bound_port(),
                                               socket->GetProtocolUPnPString());
      upnp_actions_.push_back(std::move(remove_port));
    }
  }

  const int result = socket->Close();

  if (result == X_SOCKET_ERROR) {
    XThread::SetLastError(socket->GetLastWSAError());
  }

  // Release the handle either way. The guest called closesocket, so it will
  // never reference this handle again no matter what we return, and the
  // underlying descriptor is gone in every failure case that can reach here.
  // Keeping the handle alive on failure only strands the XSocket object and
  // its object-table entry until the title exits.
  socket->ReleaseHandle();

  return result;
}
DECLARE_XAM_EXPORT1(NetDll_closesocket, kNetworking, kImplemented);

int_result_t NetDll_shutdown_entry(dword_t caller, dword_t socket_handle,
                                   int_t how) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
    XThread::SetLastError(socket->GetLastWSAError());
  }
  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_shutdown, kNetworking, kImplemented);

dword_result_t NetDll_setsockopt_entry(dword_t caller, dword_t socket_handle,
                                       dword_t level, dword_t optname,
                                       lpvoid_t optval_ptr, dword_t optlen) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  auto ret = socket->SetOption(level, optname, optval_ptr, optlen);
  if (ret < 0) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_setsockopt, kNetworking, kImplemented);

dword_result_t NetDll_getsockopt_entry(dword_t caller, dword_t socket_handle,
                                       dword_t level, dword_t optname,
                                       lpvoid_t optval_ptr, lpdword_t optlen) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  uint32_t native_len = *optlen;
  X_STATUS status = socket->GetOption(level, optname, optval_ptr, &native_len);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_getsockopt, kNetworking, kImplemented);

dword_result_t NetDll_ioctlsocket_entry(dword_t caller, dword_t socket_handle,
                                        dword_t cmd, lpvoid_t arg_ptr) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr.as<uint32_t*>());
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    XELOGE("NetDll_ioctlsocket: failed with error {:08X}",
           socket->GetLastWSAError());
    return -1;
  }

  // TODO
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_ioctlsocket, kNetworking, kImplemented);

dword_result_t NetDll_bind_entry(dword_t caller, dword_t socket_handle,
                                 pointer_t<XSOCKADDR_IN> name,
                                 dword_t namelen) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  const auto network_adapter =
      kernel_state()->emulator()->GetNetworkAdapterManager();

  const std::string local_ip =
      network_adapter->GetSelectedAdapterLocalIPString();

  X_STATUS status = socket->Bind(name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    XELOGE("NetDll_bind: failed with error {:08X}", socket->GetLastWSAError());
    return -1;
  }

  const auto upnp = kernel_state()->emulator()->GetUPnP();

  uint16_t upnp_internal_port = name->address_port;

  if (upnp) {
    const uint16_t mapped_internal_port =
        upnp->GetMappedBindPort(name->address_port);

    upnp_internal_port = mapped_internal_port;

    // Support wildcard port. Prefer the port the socket ACTUALLY bound - the
    // hub port allocator may have moved it off the guest-requested port on a
    // same-machine collision.
    if (!upnp_internal_port || !mapped_internal_port) {
      upnp_internal_port = socket->bound_port()
                               ? socket->bound_port()
                               : static_cast<uint16_t>(name->address_port);
    }

    const std::string protocol = socket->GetProtocolUPnPString();

    // Track in BOTH paths so the port shows in the UPnP dialog / "Refresh
    // Ports" and is closed on exit, even when a mapping is added actively.
    upnp->TrackPort(upnp_internal_port, protocol);

    // Open the port SYNCHRONOUSLY, before returning to the guest. The title
    // announces this port to matchmaking immediately after bind, so a mapping
    // that is still in flight (or was never attempted) means peers are handed
    // an address the router does not forward. Can be called multiple times.
    const int32_t result =
        upnp->AddPort(local_ip, upnp_internal_port, protocol);

    // A stale IGD control URL answers 401. Re-discover once and retry, rather
    // than leaving the port closed for the rest of the session.
    if (result == HTTP_UNAUTHORIZED && !upnp_refreshed_unauthorized) {
      upnp_refreshed_unauthorized = true;

      XELOGW("UPnP unauthorized on bind - re-discovering IGD");

      const auto igd_desc = upnp->DiscoverValidIGD();

      if (igd_desc.has_value()) {
        upnp->LoadIGD(igd_desc.value());
        upnp->AddPort(local_ip, upnp_internal_port, protocol);
      }
    }
  }

  if (cvars::logging) {
    XELOGI("Bind port {}", upnp_internal_port);
  }

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_bind, kNetworking, kImplemented);

dword_result_t NetDll_connect_entry(dword_t caller, dword_t socket_handle,
                                    pointer_t<XSOCKADDR_IN> name,
                                    dword_t namelen) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  X_STATUS status = socket->Connect(name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }

  // The IDLE->PENDING->CONNECTED progression is otherwise only driven by the
  // UDP paths (WSASendTo / WSARecvFrom). A title whose secure-link traffic is
  // TCP therefore never sees CONNECTED: XNetConnect marks the peer PENDING and
  // nothing ever promotes it, so XNetGetConnectStatus polls PENDING forever and
  // the title eventually times out. Demonware/LSP backends are exactly this
  // case (Black Ops II reports "server is not available" after ~8s despite a
  // healthy TCP session). A completed TCP connect to the peer is proof the link
  // is live, so promote it here.
  XNetUpdateConnectStatus(XNetPeerKey(name->address_ip), STATUS_CONNECTED);

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_connect, kNetworking, kImplemented);

dword_result_t NetDll_listen_entry(dword_t caller, dword_t socket_handle,
                                   int_t backlog) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_listen, kNetworking, kImplemented);

dword_result_t NetDll_accept_entry(dword_t caller, dword_t socket_handle,
                                   pointer_t<XSOCKADDR_IN> addr_ptr,
                                   lpdword_t addrlen_ptr) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  int* name_len_host_ptr = nullptr;
  if (addrlen_ptr) {
    name_len_host_ptr = reinterpret_cast<int*>(addrlen_ptr.host_address());
  }
  auto new_socket = socket->Accept(addr_ptr, name_len_host_ptr);
  if (!new_socket) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  } else if (addr_ptr && !cvars::log_mask_ips) {
    XELOGI("NetDll_accept: {}:{}({})", ip_to_string(addr_ptr->address_ip),
           addr_ptr->address_port.get(), socket->GetProtocolUPnPString());
  }

  return new_socket->handle();
}
DECLARE_XAM_EXPORT1(NetDll_accept, kNetworking, kImplemented);

struct x_fd_set {
  xe::be<uint32_t> fd_count;
  xe::be<uint32_t> fd_array[X_FD_SETSIZE];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[X_FD_SETSIZE];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < X_FD_SETSIZE);

    count = guest_set->fd_count;
    for (uint32_t i = 0; i < count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        count = i;
        break;
      }

      // Convert from Xenia -> native
      auto socket =
          kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);

      if (!socket) {
        count = i;
        break;
      }

      sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const object_ref<XSocket>& socket = sockets[i];
      if (socket) {
        guest_set->fd_array[guest_set->fd_count++] = socket->handle();
      }
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < count; ++i) {
      const object_ref<XSocket>& socket = sockets[i];
      if (socket) {
        FD_SET(socket->native_handle(), native_set);
      }
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const object_ref<XSocket>& socket = sockets[i];
      if (socket) {
        if (FD_ISSET(socket->native_handle(), native_set)) {
          sockets[new_count++] = socket;
        }
      }
    }
    count = new_count;
  }
};

bool verify_x_fd_set(const x_fd_set* guest_set) {
  for (uint32_t i = 0; i < guest_set->fd_count; ++i) {
    auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
    if (socket_handle == -1) {
      break;
    }
    // Convert from Xenia -> native
    auto socket =
        kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
    if (!socket) {
      return false;
    }
  }
  return true;
}

int_result_t NetDll_select_entry(dword_t caller, dword_t nfds,
                                 pointer_t<x_fd_set> readfds,
                                 pointer_t<x_fd_set> writefds,
                                 pointer_t<x_fd_set> exceptfds,
                                 pointer_t<X_TIMEVAL> timeout_ptr) {
  host_set host_readfds = {0};
  fd_set native_readfds = {0};
  if (readfds) {
    if (!verify_x_fd_set(readfds)) {
      XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
      return -1;
    }

    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {0};
  fd_set native_writefds = {0};
  if (writefds) {
    if (!verify_x_fd_set(writefds)) {
      XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
      return -1;
    }

    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {0};
  fd_set native_exceptfds = {0};
  if (exceptfds) {
    if (!verify_x_fd_set(exceptfds)) {
      XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
      return -1;
    }

    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout = {};
  if (timeout_ptr) {
    timeout = {timeout_ptr->tv_sec, timeout_ptr->tv_usec};
    Clock::ScaleGuestDurationTimeval(
        reinterpret_cast<int32_t*>(&timeout.tv_sec),
        reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }

  const int handles_count =
      select(nfds, readfds ? &native_readfds : nullptr,
             writefds ? &native_writefds : nullptr,
             exceptfds ? &native_exceptfds : nullptr, timeout_in);

  if (handles_count == X_SOCKET_ERROR) {
    XThread::SetLastError(XSocket::GetLastWSAErrorStatic());
  }

  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // TEMPORARY PROBE - remove before release.
  //
  // Black Ops 1 (T5, 0x41560855) fastfile-of-the-day load state. Two lockups
  // and one crash landed in this path while the DW server logged a clean
  // transfer every time, so read the globals the loader branches on rather
  // than inferring the state from the wire.
  //
  // Addresses are from default_mp_tu11.xex:
  //   0x84185EE0  downloaded size   -- sub_824F0360 only applies if > 0
  //   0x84185FB0  download complete
  //   0x84185EB8  zone load issued   0x84185EB9  ffotd_settings.cfg exec'd
  //   0x82A086B0  DB ring base       0x82A086B4  bytes consumed
  //   0x82A08698  blocks available   0x82A086A4  read offset
  //   0x82A14720  stream pointer     0x82A14724  stream length
  //   0x826E9D24  stream count
  //
  // `window` is what sub_8226FCA0 computes at 0x8226FD8C and passes to the
  // Salsa20 transform AS A LENGTH: (ringBase - streamPtr) + 0x60000, selected
  // with a SIGNED compare and then consumed UNSIGNED. The crash dump showed it
  // arriving as 0xAE6FFE60 (~2.9 GB), which is how the cipher walked off the
  // end of the buffer into .idata. Reproduced in the interpreter: it goes
  // negative the moment streamPtr - ringBase reaches 0x60000. If `neg=1` shows
  // up here, that is the same bug on hardware; if the window stays small and
  // positive then the lockup is somewhere else and this rules the theory out.
  //
  // Scoped to the title so the addresses mean something, but it fires on EVERY
  // select() with no counter, no rate limit and no one-shot -- a throttled
  // probe cannot catch the moment the window goes bad.
  if (kernel_state()->title_id() == 0x41560855) {
    // TranslateVirtual does NOT validate - it just adds to the memory base, so
    // it returns a non-null host pointer for an address that was never
    // committed and dereferencing it faults. A `p ? *p : default` check is
    // worthless here; the read has to be gated on the page actually existing,
    // the way MmIsAddressValid does it. Reading these globals before the title
    // has allocated them is normal, so this must not be a crash.
    auto mapped = [](uint32_t va) -> bool {
      auto* heap = kernel_memory()->LookupHeap(va);
      return heap &&
             heap->QueryRangeAccess(va, va) != memory::PageAccess::kNoAccess;
    };
    auto t8 = [&mapped](uint32_t va) -> uint32_t {
      if (!mapped(va)) {
        return 0xFFu;
      }
      auto* p = kernel_memory()->TranslateVirtual<uint8_t*>(va);
      return p ? *p : 0xFFu;
    };
    auto t32 = [&mapped](uint32_t va) -> uint32_t {
      if (!mapped(va)) {
        return 0xFFFFFFFFu;
      }
      auto* p = kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(va);
      return p ? p->get() : 0xFFFFFFFFu;
    };
    const uint32_t dlsize = t32(0x84185EE0u);
    const uint32_t dldone = t8(0x84185FB0u);
    const uint32_t zoned = t8(0x84185EB8u);
    const uint32_t execd = t8(0x84185EB9u);
    const uint32_t ringbase = t32(0x82A086B0u);
    const uint32_t consumed = t32(0x82A086B4u);
    const uint32_t blocks = t32(0x82A08698u);
    const uint32_t offset = t32(0x82A086A4u);
    const uint32_t sptr = t32(0x82A14720u);
    const uint32_t slen = t32(0x82A14724u);
    const uint32_t nstream = t32(0x826E9D24u);
    const int32_t window = (int32_t)(ringbase - sptr) + 0x60000;

    // The two gates sub_824F0360 must pass before it will queue the ffotd
    // zone at all. First run showed zoned=0/execd=0 for the whole session with
    // dlsize already set, so the download is not the problem -- one of these is
    // saying "not yet".
    //   sub_82358FF8 = (wait(*(0x834C03DC), 0) == 0), i.e. is that object
    //     SIGNALLED. Log the handle; 0 means it was never created.
    //   sub_823EBB38 walks a loaded-zone list at 0x83A49F80 (count) /
    //     0x83A49F88 (name pointer) looking for code_post_gfx_mp, common_mp,
    //     dev_mp, patch_mp, InitGcm -- i.e. "are the base zones up".
    const uint32_t waith = t32(0x834C03DCu);
    const uint32_t zcnt = t32(0x83A49F80u);
    const uint32_t zptr = t32(0x83A49F88u);
    // zptr above is the FIRST entry of the loaded-zone list, not the zone
    // being streamed -- reading it as "what the DB is working on" is wrong.
    // The in-flight zone is the DB context's own name pointer, ctx+0x04
    // (ctx base 0x82A08680, the same block ring/consumed/off are read from).
    const uint32_t inflight = t32(0x82A08684u);
    // The DB queue state. Milestones proved the worker is asleep at
    // 0x82283A04 with hDbIdle clear, which is only possible if it woke,
    // found nothing queued and returned at 0x822838C4 WITHOUT signalling
    // idle. These three say it outright:
    //   pend == 0 while idleflg == 0  ->  exactly that stranding
    //   outst != 0                    ->  zones queued that never finished
    // NOTE the signed displacements. PowerPC lwz/stw take a SIGNED 16-bit D,
    // so `lis r11,0x82C2 / lwz r10,0xD074(r11)` is 0x82C20000 - 0x2F8C, not
    // 0x82C20000 + 0xD074. Reading them unsigned pointed at unrelated memory
    // and printed ASCII (outst came back 0x20202020, four spaces).
    const uint32_t pend = t32(0x82C1D074u);   // pendingCount  (0x822838BC)
    const uint32_t outst = t32(0x82A08614u);  // outstanding   (0x8228394C)
    const uint32_t idleflg =
        t32(0x834C0430u);                    // the idle flag DB_SetIdle* sets
    const uint32_t svfh = t32(0x834C044Cu);  // hSvFrame
    char zname[32] = {};
    if (zptr && mapped(zptr)) {
      for (uint32_t i = 0; i < sizeof(zname) - 1; i++) {
        if (!mapped(zptr + i)) {
          break;
        }
        char c = (char)t8(zptr + i);
        if (!c) {
          break;
        }
        zname[i] = c;
      }
    }
    char iname[32] = {};
    if (inflight && mapped(inflight)) {
      for (uint32_t i = 0; i < sizeof(iname) - 1; i++) {
        if (!mapped(inflight + i)) {
          break;
        }
        char c = (char)t8(inflight + i);
        if (!c) {
          break;
        }
        iname[i] = c;
      }
    }
    XELOGE(
        "[bo1-ffotd] dlsize={:08X} dldone={} zoned={} execd={} | ring={:08X} "
        "consumed={:08X} blocks={} off={:08X} | sptr={:08X} slen={:08X} "
        "nstream={} window={:08X} neg={} | waith={:08X} zcnt={:08X} "
        "zptr={:08X} zname='{}' | inflight='{}' pend={} outst={} idleflg={} "
        "svfh={:08X}",
        dlsize, dldone, zoned, execd, ringbase, consumed, blocks, offset, sptr,
        slen, nstream, (uint32_t)window, window < 0 ? 1 : 0, waith, zcnt, zptr,
        zname, iname, pend, outst, idleflg, svfh);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest
  // fd_sets?
  return handles_count;
}
DECLARE_XAM_EXPORT1(NetDll_select, kNetworking, kImplemented);

dword_result_t NetDll_recv_entry(dword_t caller, dword_t socket_handle,
                                 lpvoid_t buf_ptr, dword_t buf_len,
                                 dword_t flags) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  int ret = socket->Recv(buf_ptr, buf_len, flags);
  if (ret < 0) {
    XThread::SetLastError(socket->GetLastWSAError());
  } else if (ret >= 0 && !cvars::log_mask_ips) {
    XELOGI("NetDll_recv: Received {} bytes from: Any:{}({})", ret,
           socket->bound_port(), socket->GetProtocolUPnPString());
  }

  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_recv, kNetworking, kImplemented);

dword_result_t NetDll_recvfrom_entry(dword_t caller, dword_t socket_handle,
                                     lpvoid_t buf_ptr, dword_t buf_len,
                                     dword_t flags,
                                     pointer_t<XSOCKADDR_IN> from_ptr,
                                     lpdword_t fromlen_ptr) {
  // Fixed 415607D6, 4E4D07DC
  InitializeSockaddr(from_ptr);

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  socklen_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret = socket->RecvFrom(buf_ptr, buf_len, flags, from_ptr,
                             fromlen_ptr ? &native_fromlen : nullptr);
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
    XThread::SetLastError(socket->GetLastWSAError());
  } else if (ret >= 0 && from_ptr) {
    // Heard back from this peer -> the secure link is live.
    XNetUpdateConnectStatus(XNetPeerKey(from_ptr->address_ip),
                            STATUS_CONNECTED);

    if (!cvars::log_mask_ips) {
      XELOGI("NetDll_recvfrom: Received {} bytes from: {}:{}({})", ret,
             ip_to_string(from_ptr->address_ip), from_ptr->address_port.get(),
             socket->GetProtocolUPnPString());
    }
  }

  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_recvfrom, kNetworking, kImplemented);

dword_result_t NetDll_send_entry(dword_t caller, dword_t socket_handle,
                                 lpvoid_t buf_ptr, dword_t buf_len,
                                 dword_t flags) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  int ret = socket->Send(buf_ptr, buf_len, flags);
  if (ret < 0) {
    XThread::SetLastError(socket->GetLastWSAError());
  } else if (ret >= 0 && !cvars::log_mask_ips) {
    XELOGI("NetDll_send: Send {} bytes to: Any:{}({})", ret,
           socket->bound_port(), socket->GetProtocolUPnPString());
  }

  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_send, kNetworking, kImplemented);

dword_result_t NetDll_sendto_entry(dword_t caller, dword_t socket_handle,
                                   lpvoid_t buf_ptr, dword_t buf_len,
                                   dword_t flags,
                                   pointer_t<XSOCKADDR_IN> to_ptr,
                                   dword_t to_len) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  int ret = socket->SendTo(buf_ptr, buf_len, flags, to_ptr, to_len);
  if (ret < 0) {
    XThread::SetLastError(socket->GetLastWSAError());
  } else if (ret >= 0 && to_ptr) {
    // Sent to this peer -> connection at least pending until we hear back.
    XNetUpdateConnectStatus(XNetPeerKey(to_ptr->address_ip),
                            XNET_CONNECT_STATUS_PENDING);

    if (!cvars::log_mask_ips) {
      XELOGI("NetDll_sendto: Send {} bytes to: {}:{}({})", ret,
             ip_to_string(to_ptr->address_ip), to_ptr->address_port.get(),
             socket->GetProtocolUPnPString());
    }
  }

  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_sendto, kNetworking, kImplemented);

dword_result_t NetDll_WSAEventSelect_entry(dword_t caller,
                                           dword_t socket_handle,
                                           dword_t event_handle,
                                           dword_t flags) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
  if (!ev) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  int ret = socket->WSAEventSelect(socket->native_handle(), ev->native_handle(),
                                   flags);

  if (ret < 0) {
    XThread::SetLastError(socket->GetLastWSAError());
  }

  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_WSAEventSelect, kNetworking, kImplemented);

dword_result_t NetDll___WSAFDIsSet_entry(dword_t socket_handle,
                                         pointer_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count =
      std::min((uint32_t)fd_set->fd_count, uint32_t(X_FD_SETSIZE));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll___WSAFDIsSet, kNetworking, kImplemented);

void NetDll_WSASetLastError_entry(dword_t error_code) {
  XThread::SetLastError(error_code);
}
DECLARE_XAM_EXPORT1(NetDll_WSASetLastError, kNetworking, kImplemented);

dword_result_t NetDll_getpeername_entry(dword_t caller, dword_t socket_handle,
                                        pointer_t<XSOCKADDR_IN> addr_ptr,
                                        lpdword_t addrlen_ptr) {
  if (!addr_ptr) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAEFAULT));
    return -1;
  }

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  int native_len = *addrlen_ptr;
  X_STATUS status = socket->GetPeerName(addr_ptr, &native_len);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }

  *addrlen_ptr = native_len;
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_getpeername, kNetworking, kImplemented);

dword_result_t NetDll_getsockname_entry(dword_t caller, dword_t socket_handle,
                                        pointer_t<XSOCKADDR_IN> addr_ptr,
                                        lpdword_t addrlen_ptr) {
  InitializeSockaddr(addr_ptr);

  if (!addr_ptr) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAEFAULT));
    return -1;
  }

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  int native_len = *addrlen_ptr;
  X_STATUS status = socket->GetSockName(addr_ptr, &native_len);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }

  *addrlen_ptr = native_len;
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_getsockname, kNetworking, kImplemented);

dword_result_t NetDll_XNetCreateKey_entry(dword_t caller,
                                          pointer_t<XNKID> session_key,
                                          pointer_t<XNKEY> exchange_key) {
  const xe::be<uint64_t> xnkid = GenerateSessionId(XNKID_SYSTEM_LINK);
  memcpy(session_key->ab, &xnkid, sizeof(XNKID));

  GenerateIdentityExchangeKey(exchange_key);
  // memcpy(exchange_key, kernel_state()->title_lan_key(), sizeof(XNKEY));

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetCreateKey, kNetworking, kStub);

dword_result_t NetDll_XNetRegisterKey_entry(dword_t caller,
                                            pointer_t<XNKID> session_key,
                                            pointer_t<XNKEY> exchange_key) {
  if (IsSystemlink(session_key->as_uintBE64())) {
    XELOGI("XNetRegisterKey: Systemlink");
    kernel_state()->GetXboxLiveAPI()->SetSystemlinkID(
        session_key->as_uintBE64());
    return 0;
  }

  if (IsOnlinePeer(session_key->as_uintBE64())) {
    XELOGI("XNetRegisterKey: Xbox Live");
    EXPLICIT_XBOXLIVE_KEY = true;
    return 0;
  }

  if (IsServer(session_key->as_uintBE64())) {
    XELOGI("XNetRegisterKey: Server");
    return 0;
  }

  XELOGI(fmt::format("XNetRegisterKey: {:016X} (Unknown)",
                     session_key->as_uintBE64()));

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetRegisterKey, kNetworking, kStub);

dword_result_t NetDll_XNetUnregisterKey_entry(dword_t caller,
                                              pointer_t<XNKID> session_key) {
  const uint64_t systemlink_id =
      kernel_state()->GetXboxLiveAPI()->GetSystemlinkID();

  if (systemlink_id) {
    if (IsSystemlink(systemlink_id)) {
      XELOGI("XNetUnregisterKey: Systemlink");
    }
    kernel_state()->GetXboxLiveAPI()->SetSystemlinkID(0);
  }

  if (EXPLICIT_XBOXLIVE_KEY) {
    EXPLICIT_XBOXLIVE_KEY = false;

    XELOGI("XNetUnregisterKey: Xbox Live");
  }

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetUnregisterKey, kNetworking, kStub);

dword_result_t XamBackgroundDownloadSetMode_entry(dword_t mode) {
  download_mode_ = static_cast<X_BACKGROUND_DOWNLOAD_MODE>(mode.value());
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadSetMode, kMisc, kStub);

dword_result_t XamBackgroundDownloadGetMode_entry() {
  return static_cast<uint32_t>(download_mode_);
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadGetMode, kMisc, kStub);

dword_result_t XamBackgroundDownloadItemGetStatus_entry(
    dword_t user_index, pointer_t<XCONTENT_DATA_INTERNAL> content_ptr,
    dword_t validate_content, dword_t unkn1, lpdword_t unkn2_ptr,
    lpdword_t unkn3_ptr, lpdword_t unkn4_ptr) {
  // Set either unkn2_ptr or unkn3_ptr to 1 so function succeeds.
  *unkn2_ptr = 0;
  *unkn3_ptr = 0;
  *unkn4_ptr = 0;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadItemGetStatus, kMisc, kStub);

dword_result_t XamBackgroundDownloadItemGetHistoryStatus_entry(
    dword_t user_index, pointer_t<XCONTENT_DATA_INTERNAL> content_ptr,
    dword_t unkn1) {
  return X_ERROR_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadItemGetHistoryStatus, kMisc, kStub);

namespace {

constexpr uint32_t kKdcTimeoutBytes = 0x18;
constexpr uint32_t kKdcTimeoutCount = kKdcTimeoutBytes / sizeof(uint32_t);
constexpr uint32_t kKdcStoredCount = 4;
constexpr uint32_t kKdcTimeoutLimit = 0x63FF;
constexpr uint32_t kKdcTimeoutUnit = 100;
constexpr uint32_t kKdcInvalidParameter = 0xC00000EF;
constexpr uint32_t kNtFacilityBit = 0x10000000;

struct KdcTimeouts {
  uint32_t defaults[kKdcTimeoutCount];
  uint8_t stored[kKdcStoredCount];
};

std::mutex kdc_timeouts_mutex;
KdcTimeouts kerb_timeouts = {{4000, 6000, 8000, 8000, 8000, 8000}, {}};
KdcTimeouts macs_timeouts = {{4000, 4000, 4000, 4000, 4000, 4000}, {}};

uint32_t SetKdcTimeouts(KdcTimeouts& timeouts, const xe::be<uint32_t>* values,
                        uint32_t size) {
  if (size != kKdcTimeoutBytes || !values) {
    return kKdcInvalidParameter | kNtFacilityBit;
  }
  uint8_t packed[kKdcStoredCount];
  for (uint32_t i = 0; i < kKdcStoredCount; ++i) {
    const uint32_t value = values[i];
    if (value > kKdcTimeoutLimit) {
      return kKdcInvalidParameter | kNtFacilityBit;
    }
    packed[i] = static_cast<uint8_t>(value / kKdcTimeoutUnit);
  }
  std::lock_guard<std::mutex> lock(kdc_timeouts_mutex);
  for (uint32_t i = 0; i < kKdcStoredCount; ++i) {
    timeouts.stored[i] = packed[i];
  }
  return kNtFacilityBit;
}

uint32_t GetKdcTimeouts(const KdcTimeouts& timeouts, xe::be<uint32_t>* values,
                        uint32_t size) {
  if (size != kKdcTimeoutBytes || !values) {
    return kKdcInvalidParameter | kNtFacilityBit;
  }
  uint32_t result[kKdcTimeoutCount] = {};
  {
    std::lock_guard<std::mutex> lock(kdc_timeouts_mutex);
    for (uint32_t i = 0; i < kKdcStoredCount; ++i) {
      result[i] = uint32_t(timeouts.stored[i]) * kKdcTimeoutUnit;
    }
  }
  if (!result[0]) {
    for (uint32_t i = 0; i < kKdcTimeoutCount; ++i) {
      result[i] = timeouts.defaults[i];
    }
  }
  for (uint32_t i = 0; i < kKdcTimeoutCount; ++i) {
    values[i] = result[i];
  }
  return kNtFacilityBit;
}

}  // namespace

dword_result_t XamSetKerbTimeouts_entry(lpdword_t values, dword_t size) {
  return SetKdcTimeouts(kerb_timeouts, values, size);
}
DECLARE_XAM_EXPORT1(XamSetKerbTimeouts, kNetworking, kImplemented);

dword_result_t XamGetKerbTimeouts_entry(lpdword_t values, dword_t size) {
  return GetKdcTimeouts(kerb_timeouts, values, size);
}
DECLARE_XAM_EXPORT1(XamGetKerbTimeouts, kNetworking, kImplemented);

dword_result_t XamSetMacsTimeouts_entry(lpdword_t values, dword_t size) {
  return SetKdcTimeouts(macs_timeouts, values, size);
}
DECLARE_XAM_EXPORT1(XamSetMacsTimeouts, kNetworking, kImplemented);

dword_result_t XamGetMacsTimeouts_entry(lpdword_t values, dword_t size) {
  return GetKdcTimeouts(macs_timeouts, values, size);
}
DECLARE_XAM_EXPORT1(XamGetMacsTimeouts, kNetworking, kImplemented);

// Remove completed UPnP actions
void CleanupUPnPActions() {
  auto completed_actions =
      std::remove_if(upnp_actions_.begin(), upnp_actions_.end(),
                     [](const std::future<int32_t>& f) {
                       return f.wait_for(0s) == std::future_status::ready;
                     });

  upnp_actions_.erase(completed_actions, upnp_actions_.end());
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Net);
