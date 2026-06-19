/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XSOCKET_H_
#define XENIA_KERNEL_XSOCKET_H_

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <queue>
#include <string>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xobject.h"

#ifdef XE_PLATFORM_WIN32
// clang-format off
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include "xenia/base/platform_win.h"
#include <WS2tcpip.h>
#include <WinSock2.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace xe {
namespace kernel {
enum class X_WSAError : uint32_t {
  // Xbox 360 WSA error codes.
  // NOTE: The SDK header winsockx.h says WSA_IO_PENDING == WSAEWOULDBLOCK,
  // but the REAL XAM kernel uses 0x3E5 (997 = Windows ERROR_IO_PENDING).
  // Confirmed by decompiling sub_81746610/sub_81746900 in xam.xex.
  X_WSA_INVALID_PARAMETER = 0x2726,  // == WSAEINVAL
  X_WSA_OPERATION_ABORTED = 0x2714,  // == WSAEINTR
  X_WSA_IO_INCOMPLETE = 0x3E5,       // 997 — same as IO_PENDING in XAM
  X_WSA_IO_PENDING = 0x3E5,          // 997 — real XAM value, NOT 0x2733
  // WSABASEERR + N (from Xbox 360 SDK winsockx.h)
  X_WSAEINTR = 0x2714,            // 10004
  X_WSAEBADF = 0x2719,            // 10009
  X_WSAEACCES = 0x271D,           // 10013
  X_WSAEFAULT = 0x271E,           // 10014
  X_WSAEINVAL = 0x2726,           // 10022
  X_WSAEMFILE = 0x2728,           // 10024
  X_WSAEWOULDBLOCK = 0x2733,      // 10035
  X_WSAEINPROGRESS = 0x2734,      // 10036
  X_WSAEALREADY = 0x2735,         // 10037
  X_WSAENOTSOCK = 0x2736,         // 10038
  X_WSAEDESTADDRREQ = 0x2737,     // 10039
  X_WSAEMSGSIZE = 0x2738,         // 10040
  X_WSAEPROTOTYPE = 0x2739,       // 10041
  X_WSAENOPROTOOPT = 0x273A,      // 10042
  X_WSAEPROTONOSUPPORT = 0x273B,  // 10043
  X_WSAESOCKTNOSUPPORT = 0x273C,  // 10044
  X_WSAEOPNOTSUPP = 0x273D,       // 10045
  X_WSAEPFNOSUPPORT = 0x273E,     // 10046
  X_WSAEAFNOSUPPORT = 0x273F,     // 10047
  X_WSAEADDRINUSE = 0x2740,       // 10048
  X_WSAEADDRNOTAVAIL = 0x2741,    // 10049
  X_WSAENETDOWN = 0x2742,         // 10050
  X_WSAENETUNREACH = 0x2743,      // 10051
  X_WSAENETRESET = 0x2744,        // 10052
  X_WSAECONNABORTED = 0x2745,     // 10053
  X_WSAECONNRESET = 0x2746,       // 10054
  X_WSAENOBUFS = 0x2747,          // 10055
  X_WSAEISCONN = 0x2748,          // 10056
  X_WSAENOTCONN = 0x2749,         // 10057
  X_WSAESHUTDOWN = 0x274A,        // 10058
  X_WSAETOOMANYREFS = 0x274B,     // 10059
  X_WSAETIMEDOUT = 0x274C,        // 10060
  X_WSAECONNREFUSED = 0x274D,     // 10061
  X_WSAELOOP = 0x274E,            // 10062
  X_WSAENAMETOOLONG = 0x274F,     // 10063
  X_WSAEHOSTDOWN = 0x2750,        // 10064
  X_WSAEHOSTUNREACH = 0x2751,     // 10065
  X_WSAENOTEMPTY = 0x2752,        // 10066
  X_WSAEPROCLIM = 0x2753,         // 10067
  X_WSAEUSERS = 0x2754,           // 10068
  X_WSAEDQUOT = 0x2755,           // 10069
  X_WSAESTALE = 0x2756,           // 10070
  X_WSAEREMOTE = 0x2757,          // 10071
  X_WSASYSNOTREADY = 0x276B,      // 10091
  X_WSAVERNOTSUPPORTED = 0x276C,  // 10092
  X_WSANOTINITIALISED = 0x276D,   // 10093
  X_WSAEDISCON = 0x2775,          // 10101
  X_WSANO_DATA = 0x2AFC,          // 11004
};

struct XSOCKADDR {
  xe::be<uint16_t> address_family;
  char sa_data[14];
};
static_assert_size(XSOCKADDR, 0x10);

struct XSOCKADDR_IN {
  xe::be<uint16_t> address_family;
  xe::be<uint16_t> address_port;
  in_addr address_ip;
  char sa_zero[8];

  const sockaddr to_host() const {
    sockaddr sa = {};
    std::memcpy(&sa, this, sizeof(sockaddr));

    sa.sa_family = xe::byte_swap(sa.sa_family);
    // port is already in correct endianness
    return sa;
  }

  void to_guest(const sockaddr* host) {
    std::memcpy(this, host, sizeof(sockaddr));
    address_family = host->sa_family;
  }
};

struct XWSABUF {
  xe::be<uint32_t> len;
  xe::be<uint32_t> buf_ptr;
};
static_assert_size(XWSABUF, 0x8);

struct XWSAOVERLAPPED {
  xe::be<uint32_t> internal;
  xe::be<uint32_t> internal_high;
  xe::be<uint32_t> offset;
  xe::be<uint32_t> offset_high;
  xe::be<uint32_t> event_handle;
};
static_assert_size(XWSAOVERLAPPED, 0x14);

class XSocket : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Socket;

  enum AddressFamily {
    X_AF_INET = 2,
  };

  enum Type {
    X_SOCK_STREAM = 1,
    X_SOCK_DGRAM = 2,
  };

  enum Protocol {
    X_IPPROTO_TCP = 6,
    X_IPPROTO_UDP = 17,

    // LIVE Voice and Data Protocol
    // https://blog.csdn.net/baozi3026/article/details/4277227
    // Format: [cbGameData][GameData(encrypted)][VoiceData(unencrypted)]
    X_IPPROTO_VDP = 254,
  };

  enum WSAInfo {
    sendto_flag = 1,
    recvfrom_flag = 2,
    complete = 4,
    closed = 8,
  };

  XSocket(KernelState* kernel_state);
  ~XSocket();

  uint64_t native_handle() const { return native_handle_; }
  uint16_t bound_port() const { return bound_port_; }

  // Re-added for Canary's xam_net.cc (UPnP mapping + bind logging), which the
  // Nexia header dropped. Backed by proto_/bound_ that Nexia still keeps.
  Protocol protocol() const { return proto_; }
  bool IsBound() const { return bound_; }
  std::string GetProtocolUPnPString() const {
    if (proto_ == X_IPPROTO_UDP || proto_ == X_IPPROTO_VDP) {
      return "UDP";
    } else if (proto_ == X_IPPROTO_TCP) {
      return "TCP";
    } else {
      return "UDP";
    }
  }

  X_STATUS Initialize(AddressFamily af, Type type, Protocol proto);
  X_STATUS Close();

  X_STATUS GetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                     uint32_t* optlen);
  X_STATUS SetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                     uint32_t optlen);
  X_STATUS IOControl(uint32_t cmd, uint8_t* arg_ptr);

  X_STATUS Connect(const XSOCKADDR_IN* name, int name_len);
  X_STATUS Bind(const XSOCKADDR_IN* name, int name_len);
  X_STATUS Listen(int backlog);
  X_STATUS GetPeerName(XSOCKADDR_IN* name, int* name_len);
  X_STATUS GetSockName(XSOCKADDR_IN* buf, int* buf_len);
  object_ref<XSocket> Accept(XSOCKADDR_IN* name, int* name_len);
  int Shutdown(int how);

  int Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags);
  int Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags);

  int RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags,
               XSOCKADDR_IN* from, uint32_t* from_len);
  int SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, XSOCKADDR_IN* to,
             uint32_t to_len);

  int WSAEventSelect(uint64_t socket_handle, uint64_t event_handle,
                     uint32_t flags);

  int WSASendTo(XWSABUF* buffers, uint32_t num_buffers,
                xe::be<uint32_t>* num_bytes_sent_ptr, uint32_t flags,
                XSOCKADDR_IN* to_ptr, uint32_t to_len,
                XWSAOVERLAPPED* overlapped_ptr, uint32_t completion_routine = 0,
                uint32_t overlapped_guest_ptr = 0);

  int WSARecvFrom(XWSABUF* buffers, uint32_t num_buffers,
                  xe::be<uint32_t>* num_bytes_recv_ptr,
                  xe::be<uint32_t>* flags_ptr, XSOCKADDR_IN* from_ptr,
                  xe::be<uint32_t>* fromlen_ptr, XWSAOVERLAPPED* overlapped_ptr,
                  uint32_t completion_routine = 0,
                  uint32_t overlapped_guest_ptr = 0);
  bool WSAGetOverlappedResult(XWSAOVERLAPPED* overlapped_ptr,
                              xe::be<uint32_t>* bytes_transferred, bool wait,
                              xe::be<uint32_t>* flags_ptr);

  uint32_t GetLastWSAError() const;
  // Socketless variant for callers without an XSocket instance (e.g. DNS).
  static uint32_t GetLastWSAErrorStatic();

  struct packet {
    // These values are in network byte order.
    xe::be<uint16_t> src_port;
    xe::be<uint32_t> src_ip;

    uint16_t data_len;
    uint8_t data[1];
  };

  // Queue a packet into our internal buffer.
  bool QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf,
                   size_t len);

 private:
  XSocket(KernelState* kernel_state, uint64_t native_handle);
  uint64_t native_handle_ = -1;

  AddressFamily af_;    // Address family
  Type type_;           // Type (DGRAM/Stream/etc)
  Protocol proto_;      // Protocol (TCP/UDP/etc)
  bool secure_ = true;  // Secure socket (encryption enabled)

  bool bound_ = false;  // Explicitly bound to an IP address?

  // Special exception for port!
  // port is always stored in NBO (Network byte order).
  // which is basically BE.
  xe::be<uint16_t> bound_port_ = 0;

  bool broadcast_socket_ = false;

  std::unique_ptr<xe::threading::Event> event_;
  std::mutex incoming_packet_mutex_;
  std::queue<uint8_t*> incoming_packets_;

  std::vector<std::future<int>> send_tasks_;
  std::mutex send_mutex_;
  std::condition_variable send_cv_;
  std::mutex send_socket_mutex_;

  std::vector<std::future<int>> receive_tasks_;
  std::mutex receive_mutex_;
  std::condition_variable receive_cv_;
  std::mutex receive_socket_mutex_;

  void CleanupCompletedTasks(std::vector<std::future<int>>& tasks);

  int PushWSASendTo(bool wait, struct WSASendToData send_async_data);

  int PollWSARecvFrom(bool wait, struct WSARecvFromData data);

  void SetLastWSAError(X_WSAError) const;
  mutable std::atomic<uint32_t> last_wsa_error_{0};
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XSOCKET_H_
