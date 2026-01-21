/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NET_NETWORK_MESSAGES_H_
#define XENIA_KERNEL_NET_NETWORK_MESSAGES_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "xenia/kernel/net/platform_socket.h"

namespace xe {
namespace kernel {
namespace net {

// Internal socket ID (not the native handle, not the Xbox handle)
using SocketId = uint32_t;
constexpr SocketId kInvalidSocketId = 0;

// Forward declare
struct NetworkResponse;

// Callback type for async operations that need responses
using ResponseCallback = std::function<void(const NetworkResponse&)>;

// Message types for the network service
enum class NetMessageType : uint8_t {
  // === TX Operations (fire and forget) ===
  Send,      // Send on connected socket
  SendTo,    // Send to specific address (UDP)

  // === RX Operations (results go to per-socket RX queue) ===
  // These are internal - RX thread handles them automatically

  // === Control Operations (synchronous RPC) ===
  CreateSocket,
  CloseSocket,
  Bind,
  Listen,
  Connect,
  Accept,
  SetOption,
  GetOption,
  SetBlocking,
  Shutdown,
  GetSockName,
  GetPeerName,

  // === Lifecycle ===
  RegisterSocket,    // Register existing native socket with service
  UnregisterSocket,  // Remove socket from service tracking
  ServiceShutdown,   // Shutdown the entire service
};

// Address structure (matches Xbox SOCKADDR_IN layout in concept)
struct NetAddress {
  uint16_t family = 0;      // AF_INET = 2
  uint16_t port = 0;        // Network byte order
  uint32_t addr = 0;        // Network byte order (IPv4)
  uint8_t zero[8] = {0};    // Padding

  void Clear() {
    family = 0;
    port = 0;
    addr = 0;
    std::memset(zero, 0, sizeof(zero));
  }

  sockaddr_in ToNative() const {
    sockaddr_in sa = {};
    sa.sin_family = family;
    sa.sin_port = port;
    sa.sin_addr.s_addr = addr;
    return sa;
  }

  void FromNative(const sockaddr_in& sa) {
    family = sa.sin_family;
    port = sa.sin_port;
    addr = sa.sin_addr.s_addr;
  }
};

// Buffer for send/recv operations
struct NetBuffer {
  std::vector<uint8_t> data;

  NetBuffer() = default;
  NetBuffer(const uint8_t* ptr, size_t len) : data(ptr, ptr + len) {}
  NetBuffer(size_t len) : data(len) {}

  uint8_t* Data() { return data.data(); }
  const uint8_t* Data() const { return data.data(); }
  size_t Size() const { return data.size(); }
  void Resize(size_t len) { data.resize(len); }
};

// Response from control operations
struct NetworkResponse {
  int32_t result = 0;       // 0 = success, -1 = error
  int32_t error_code = 0;   // Platform error code
  SocketId socket_id = kInvalidSocketId;  // For CreateSocket
  NetAddress address;       // For GetSockName/GetPeerName
  NetBuffer buffer;         // For GetOption
  uint32_t bytes_transferred = 0;
};

// TX Message: Send data
struct TxMessage {
  SocketId socket_id;
  NetBuffer buffer;
  NetAddress to;          // For SendTo; ignored for Send
  uint32_t flags = 0;
  bool has_destination;   // true = SendTo, false = Send

  static TxMessage MakeSend(SocketId id, const uint8_t* data, size_t len,
                            uint32_t flags = 0) {
    TxMessage msg;
    msg.socket_id = id;
    msg.buffer = NetBuffer(data, len);
    msg.flags = flags;
    msg.has_destination = false;
    return msg;
  }

  static TxMessage MakeSendTo(SocketId id, const uint8_t* data, size_t len,
                              const NetAddress& to, uint32_t flags = 0) {
    TxMessage msg;
    msg.socket_id = id;
    msg.buffer = NetBuffer(data, len);
    msg.to = to;
    msg.flags = flags;
    msg.has_destination = true;
    return msg;
  }
};

// RX Message: Received data (from RX thread to main thread)
struct RxMessage {
  SocketId socket_id;
  NetBuffer buffer;
  NetAddress from;        // Source address (for RecvFrom)
  uint32_t flags = 0;
  int32_t error_code = 0; // Non-zero if error occurred

  bool HasError() const { return error_code != 0; }
};

// Control message for synchronous operations
struct ControlMessage {
  NetMessageType type;
  SocketId socket_id = kInvalidSocketId;

  // Parameters (union-like, depends on type)
  struct {
    int af;
    int sock_type;
    int protocol;
  } create;

  struct {
    NetAddress address;
  } bind;

  struct {
    int backlog;
  } listen;

  struct {
    NetAddress address;
  } connect;

  struct {
    int level;
    int optname;
    NetBuffer optval;
  } sockopt;

  struct {
    bool blocking;
  } set_blocking;

  struct {
    int how;
  } shutdown;

  struct {
    NativeSocket native_handle;
    int af;
    int sock_type;
    int protocol;
  } register_socket;

  // Response callback (called from network thread)
  ResponseCallback callback;

  // Factory methods
  static ControlMessage MakeCreateSocket(int af, int type, int protocol,
                                         ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::CreateSocket;
    msg.create.af = af;
    msg.create.sock_type = type;
    msg.create.protocol = protocol;
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeCloseSocket(SocketId id, ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::CloseSocket;
    msg.socket_id = id;
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeBind(SocketId id, const NetAddress& addr,
                                 ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::Bind;
    msg.socket_id = id;
    msg.bind.address = addr;
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeListen(SocketId id, int backlog,
                                   ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::Listen;
    msg.socket_id = id;
    msg.listen.backlog = backlog;
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeConnect(SocketId id, const NetAddress& addr,
                                    ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::Connect;
    msg.socket_id = id;
    msg.connect.address = addr;
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeSetOption(SocketId id, int level, int optname,
                                      const void* optval, int optlen,
                                      ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::SetOption;
    msg.socket_id = id;
    msg.sockopt.level = level;
    msg.sockopt.optname = optname;
    if (optval && optlen > 0) {
      msg.sockopt.optval = NetBuffer(static_cast<const uint8_t*>(optval), optlen);
    }
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeGetOption(SocketId id, int level, int optname,
                                      int optlen, ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::GetOption;
    msg.socket_id = id;
    msg.sockopt.level = level;
    msg.sockopt.optname = optname;
    msg.sockopt.optval = NetBuffer(optlen);  // Pre-allocate buffer
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeSetBlocking(SocketId id, bool blocking,
                                        ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::SetBlocking;
    msg.socket_id = id;
    msg.set_blocking.blocking = blocking;
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeShutdown(SocketId id, int how, ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::Shutdown;
    msg.socket_id = id;
    msg.shutdown.how = how;
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeGetSockName(SocketId id, ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::GetSockName;
    msg.socket_id = id;
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeGetPeerName(SocketId id, ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::GetPeerName;
    msg.socket_id = id;
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeRegisterSocket(NativeSocket native, int af,
                                           int type, int protocol,
                                           ResponseCallback cb) {
    ControlMessage msg;
    msg.type = NetMessageType::RegisterSocket;
    msg.register_socket.native_handle = native;
    msg.register_socket.af = af;
    msg.register_socket.sock_type = type;
    msg.register_socket.protocol = protocol;
    msg.callback = std::move(cb);
    return msg;
  }

  static ControlMessage MakeServiceShutdown() {
    ControlMessage msg;
    msg.type = NetMessageType::ServiceShutdown;
    return msg;
  }
};

}  // namespace net
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NET_NETWORK_MESSAGES_H_
