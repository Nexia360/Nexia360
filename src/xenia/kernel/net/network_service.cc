/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/net/network_service.h"

#include <algorithm>
#include <cassert>

// TODO: Replace with xenia/base/logging.h when integrating
#ifndef XELOGI
#define XELOGI(...) (void)0
#define XELOGW(...) (void)0
#define XELOGE(...) (void)0
#endif

namespace xe {
namespace kernel {
namespace net {

// Global singleton
static NetworkService* g_network_service = nullptr;

NetworkService* GetNetworkService() {
  return g_network_service;
}

NetworkService::NetworkService() = default;

NetworkService::~NetworkService() {
  Shutdown();
}

bool NetworkService::Initialize() {
  if (initialized_.load()) {
    return true;
  }

  // Initialize platform networking
  if (!PlatformNetworkInit()) {
    XELOGE("NetworkService: Failed to initialize platform networking");
    return false;
  }

  running_.store(true);

  // Start TX thread
  tx_thread_ = std::thread(&NetworkService::TxThreadMain, this);

  // Start RX thread
  rx_thread_ = std::thread(&NetworkService::RxThreadMain, this);

  initialized_.store(true);
  g_network_service = this;

  XELOGI("NetworkService: Initialized");
  return true;
}

void NetworkService::Shutdown() {
  if (!initialized_.load()) {
    return;
  }

  XELOGI("NetworkService: Shutting down...");

  running_.store(false);

  // Wake up queues
  tx_queue_.Shutdown();
  control_queue_.Shutdown();

  // Wait for threads
  if (tx_thread_.joinable()) {
    tx_thread_.join();
  }
  if (rx_thread_.joinable()) {
    rx_thread_.join();
  }

  // Close all sockets
  {
    std::lock_guard<std::mutex> lock(sockets_mutex_);
    for (auto& pair : sockets_) {
      if (pair.second && pair.second->native_handle != kInvalidSocket) {
        SocketClose(pair.second->native_handle);
      }
    }
    sockets_.clear();
  }

  PlatformNetworkShutdown();

  initialized_.store(false);
  g_network_service = nullptr;

  XELOGI("NetworkService: Shutdown complete");
}

// =============================================================================
// Control Operations
// =============================================================================

NetworkResponse NetworkService::ExecuteControl(ControlMessage msg) {
  std::promise<NetworkResponse> promise;
  std::future<NetworkResponse> future = promise.get_future();

  msg.callback = [&promise](const NetworkResponse& response) {
    promise.set_value(response);
  };

  control_queue_.Push(std::move(msg));

  return future.get();
}

SocketId NetworkService::CreateSocket(int af, int type, int protocol,
                                       int* error_out) {
  auto response = ExecuteControl(
      ControlMessage::MakeCreateSocket(af, type, protocol, nullptr));

  if (error_out) *error_out = response.error_code;
  return response.result == 0 ? response.socket_id : kInvalidSocketId;
}

bool NetworkService::CloseSocket(SocketId id, int* error_out) {
  auto response = ExecuteControl(ControlMessage::MakeCloseSocket(id, nullptr));
  if (error_out) *error_out = response.error_code;
  return response.result == 0;
}

bool NetworkService::Bind(SocketId id, const NetAddress& addr, int* error_out) {
  auto response = ExecuteControl(ControlMessage::MakeBind(id, addr, nullptr));
  if (error_out) *error_out = response.error_code;
  return response.result == 0;
}

bool NetworkService::Listen(SocketId id, int backlog, int* error_out) {
  auto response =
      ExecuteControl(ControlMessage::MakeListen(id, backlog, nullptr));
  if (error_out) *error_out = response.error_code;
  return response.result == 0;
}

bool NetworkService::Connect(SocketId id, const NetAddress& addr,
                             int* error_out) {
  auto response =
      ExecuteControl(ControlMessage::MakeConnect(id, addr, nullptr));
  if (error_out) *error_out = response.error_code;
  return response.result == 0;
}

SocketId NetworkService::Accept(SocketId id, NetAddress* addr_out,
                                int* error_out) {
  // Accept is special - we need to do it directly since it creates a new socket
  auto state = GetSocket(id);
  if (!state || state->native_handle == kInvalidSocket) {
    if (error_out) *error_out = GetLastSocketError();
    return kInvalidSocketId;
  }

  sockaddr_in sa = {};
  int sa_len = sizeof(sa);

  NativeSocket new_native =
      SocketAccept(state->native_handle, reinterpret_cast<sockaddr*>(&sa),
                   &sa_len);

  if (new_native == kInvalidSocket) {
    if (error_out) *error_out = GetLastSocketError();
    return kInvalidSocketId;
  }

  // Register the new socket
  SocketId new_id =
      RegisterSocket(new_native, state->address_family, state->socket_type,
                     state->protocol, error_out);

  if (new_id != kInvalidSocketId) {
    auto new_state = GetSocket(new_id);
    if (new_state) {
      new_state->connected = true;
    }
  }

  if (addr_out) {
    addr_out->FromNative(sa);
  }

  return new_id;
}

bool NetworkService::SetOption(SocketId id, int level, int optname,
                               const void* optval, int optlen, int* error_out) {
  auto response = ExecuteControl(
      ControlMessage::MakeSetOption(id, level, optname, optval, optlen, nullptr));
  if (error_out) *error_out = response.error_code;
  return response.result == 0;
}

bool NetworkService::GetOption(SocketId id, int level, int optname,
                               void* optval, int* optlen, int* error_out) {
  auto response = ExecuteControl(
      ControlMessage::MakeGetOption(id, level, optname, *optlen, nullptr));

  if (response.result == 0 && optval && optlen) {
    size_t copy_len = std::min(static_cast<size_t>(*optlen), response.buffer.Size());
    std::memcpy(optval, response.buffer.Data(), copy_len);
    *optlen = static_cast<int>(copy_len);
  }

  if (error_out) *error_out = response.error_code;
  return response.result == 0;
}

bool NetworkService::SetBlocking(SocketId id, bool blocking, int* error_out) {
  auto response =
      ExecuteControl(ControlMessage::MakeSetBlocking(id, blocking, nullptr));
  if (error_out) *error_out = response.error_code;
  return response.result == 0;
}

bool NetworkService::ShutdownSocket(SocketId id, int how, int* error_out) {
  auto response =
      ExecuteControl(ControlMessage::MakeShutdown(id, how, nullptr));
  if (error_out) *error_out = response.error_code;
  return response.result == 0;
}

bool NetworkService::GetSockName(SocketId id, NetAddress* addr_out,
                                 int* error_out) {
  auto response =
      ExecuteControl(ControlMessage::MakeGetSockName(id, nullptr));
  if (addr_out && response.result == 0) {
    *addr_out = response.address;
  }
  if (error_out) *error_out = response.error_code;
  return response.result == 0;
}

bool NetworkService::GetPeerName(SocketId id, NetAddress* addr_out,
                                 int* error_out) {
  auto response =
      ExecuteControl(ControlMessage::MakeGetPeerName(id, nullptr));
  if (addr_out && response.result == 0) {
    *addr_out = response.address;
  }
  if (error_out) *error_out = response.error_code;
  return response.result == 0;
}

SocketId NetworkService::RegisterSocket(NativeSocket native, int af, int type,
                                         int protocol, int* error_out) {
  auto response = ExecuteControl(
      ControlMessage::MakeRegisterSocket(native, af, type, protocol, nullptr));
  if (error_out) *error_out = response.error_code;
  return response.result == 0 ? response.socket_id : kInvalidSocketId;
}

// =============================================================================
// TX Operations
// =============================================================================

void NetworkService::Send(SocketId id, const uint8_t* data, size_t len,
                          uint32_t flags) {
  if (!running_.load()) return;
  tx_queue_.Push(TxMessage::MakeSend(id, data, len, flags));
}

void NetworkService::SendTo(SocketId id, const uint8_t* data, size_t len,
                            const NetAddress& to, uint32_t flags) {
  if (!running_.load()) return;
  tx_queue_.Push(TxMessage::MakeSendTo(id, data, len, to, flags));
}

// =============================================================================
// RX Operations
// =============================================================================

bool NetworkService::TryRecv(SocketId id, RxMessage& msg_out) {
  auto state = GetSocket(id);
  if (!state) return false;
  return state->rx_queue->TryPop(msg_out);
}

bool NetworkService::WaitRecv(SocketId id, RxMessage& msg_out,
                              std::chrono::milliseconds timeout) {
  auto state = GetSocket(id);
  if (!state) return false;
  return state->rx_queue->WaitPop(msg_out, timeout);
}

bool NetworkService::HasPendingData(SocketId id) {
  auto state = GetSocket(id);
  if (!state) return false;
  return !state->rx_queue->Empty();
}

// =============================================================================
// Utility
// =============================================================================

NativeSocket NetworkService::GetNativeHandle(SocketId id) {
  auto state = GetSocket(id);
  return state ? state->native_handle : kInvalidSocket;
}

bool NetworkService::GetSocketState(SocketId id, SocketState* state_out) {
  auto state = GetSocket(id);
  if (!state) return false;
  if (state_out) {
    // Copy fields individually (SocketState is non-copyable due to unique_ptr)
    state_out->id = state->id;
    state_out->native_handle = state->native_handle;
    state_out->address_family = state->address_family;
    state_out->socket_type = state->socket_type;
    state_out->protocol = state->protocol;
    state_out->bound = state->bound;
    state_out->bound_port = state->bound_port;
    state_out->connected = state->connected;
    state_out->listening = state->listening;
    state_out->is_udp = state->is_udp;
    state_out->closed = state->closed;
    // Note: rx_queue is not copied - state_out gets its own empty queue
  }
  return true;
}

// =============================================================================
// Internal
// =============================================================================

std::shared_ptr<SocketState> NetworkService::GetSocket(SocketId id) {
  std::lock_guard<std::mutex> lock(sockets_mutex_);
  auto it = sockets_.find(id);
  return (it != sockets_.end()) ? it->second : nullptr;
}

SocketId NetworkService::AddSocket(std::shared_ptr<SocketState> state) {
  std::lock_guard<std::mutex> lock(sockets_mutex_);
  SocketId id = NextSocketId();
  state->id = id;
  sockets_[id] = state;
  return id;
}

void NetworkService::RemoveSocket(SocketId id) {
  std::lock_guard<std::mutex> lock(sockets_mutex_);
  sockets_.erase(id);
}

SocketId NetworkService::NextSocketId() {
  return next_socket_id_.fetch_add(1);
}

// =============================================================================
// TX Thread
// =============================================================================

void NetworkService::TxThreadMain() {
  XELOGI("NetworkService: TX thread started");

  std::vector<TxMessage> batch;
  batch.reserve(kMaxTxBatch);

  while (running_.load()) {
    // Wait for messages
    TxMessage msg;
    if (tx_queue_.WaitPop(msg, std::chrono::milliseconds(100))) {
      ProcessTxMessage(msg);

      // Process any additional queued messages
      batch.clear();
      tx_queue_.TryPopBulk(batch, kMaxTxBatch - 1);
      for (auto& m : batch) {
        ProcessTxMessage(m);
      }
    }
  }

  XELOGI("NetworkService: TX thread stopped");
}

void NetworkService::ProcessTxMessage(const TxMessage& msg) {
  auto state = GetSocket(msg.socket_id);
  if (!state || state->native_handle == kInvalidSocket || state->closed) {
    return;  // Fire and forget - silently drop
  }

  int result;
  if (msg.has_destination) {
    sockaddr_in sa = msg.to.ToNative();
    result = SocketSendTo(state->native_handle, msg.buffer.Data(),
                          static_cast<int>(msg.buffer.Size()), msg.flags,
                          reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
  } else {
    result = SocketSend(state->native_handle, msg.buffer.Data(),
                        static_cast<int>(msg.buffer.Size()), msg.flags);
  }

  // Fire and forget - ignore errors
  (void)result;
}

// =============================================================================
// RX Thread
// =============================================================================

void NetworkService::RxThreadMain() {
  XELOGI("NetworkService: RX thread started");

  while (running_.load()) {
    // Process control messages first
    ControlMessage ctrl_msg;
    while (control_queue_.TryPop(ctrl_msg)) {
      if (ctrl_msg.type == NetMessageType::ServiceShutdown) {
        XELOGI("NetworkService: RX thread received shutdown");
        return;
      }
      ProcessControlMessage(ctrl_msg);
    }

    // Poll sockets for incoming data
    PollSockets();
  }

  XELOGI("NetworkService: RX thread stopped");
}

void NetworkService::ProcessControlMessage(ControlMessage& msg) {
  NetworkResponse response;
  response.result = 0;
  response.error_code = 0;

  switch (msg.type) {
    case NetMessageType::CreateSocket: {
      NativeSocket native =
          SocketCreate(msg.create.af, msg.create.sock_type, msg.create.protocol);

      if (native == kInvalidSocket) {
        response.result = -1;
        response.error_code = GetLastSocketError();
      } else {
        auto state = std::make_shared<SocketState>();
        state->native_handle = native;
        state->address_family = msg.create.af;
        state->socket_type = msg.create.sock_type;
        state->protocol = msg.create.protocol;
        state->is_udp = (msg.create.sock_type == SOCK_DGRAM);

        // Disable UDP CONNRESET on Windows
        if (state->is_udp) {
          SocketDisableUdpConnReset(native);
        }

        response.socket_id = AddSocket(state);
      }
      break;
    }

    case NetMessageType::CloseSocket: {
      auto state = GetSocket(msg.socket_id);
      if (!state) {
        response.result = -1;
        response.error_code = GetLastSocketError();
      } else {
        state->closed = true;
        state->rx_queue->Shutdown();

        if (SocketClose(state->native_handle) != 0) {
          response.result = -1;
          response.error_code = GetLastSocketError();
        }
        state->native_handle = kInvalidSocket;
        RemoveSocket(msg.socket_id);
      }
      break;
    }

    case NetMessageType::Bind: {
      auto state = GetSocket(msg.socket_id);
      if (!state || state->native_handle == kInvalidSocket) {
        response.result = -1;
        response.error_code = GetLastSocketError();
      } else {
        sockaddr_in sa = msg.bind.address.ToNative();
        if (SocketBind(state->native_handle, reinterpret_cast<sockaddr*>(&sa),
                       sizeof(sa)) != 0) {
          response.result = -1;
          response.error_code = GetLastSocketError();
        } else {
          state->bound = true;
          state->bound_port = ntohs(sa.sin_port);

          // If bound to port 0, get actual port
          if (state->bound_port == 0) {
            int len = sizeof(sa);
            if (SocketGetSockName(state->native_handle,
                                  reinterpret_cast<sockaddr*>(&sa), &len) == 0) {
              state->bound_port = ntohs(sa.sin_port);
            }
          }
        }
      }
      break;
    }

    case NetMessageType::Listen: {
      auto state = GetSocket(msg.socket_id);
      if (!state || state->native_handle == kInvalidSocket) {
        response.result = -1;
        response.error_code = GetLastSocketError();
      } else {
        if (SocketListen(state->native_handle, msg.listen.backlog) != 0) {
          response.result = -1;
          response.error_code = GetLastSocketError();
        } else {
          state->listening = true;
        }
      }
      break;
    }

    case NetMessageType::Connect: {
      auto state = GetSocket(msg.socket_id);
      if (!state || state->native_handle == kInvalidSocket) {
        response.result = -1;
        response.error_code = GetLastSocketError();
      } else {
        sockaddr_in sa = msg.connect.address.ToNative();
        if (SocketConnect(state->native_handle,
                          reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
          response.result = -1;
          response.error_code = GetLastSocketError();
        } else {
          state->connected = true;
        }
      }
      break;
    }

    case NetMessageType::SetOption: {
      auto state = GetSocket(msg.socket_id);
      if (!state || state->native_handle == kInvalidSocket) {
        response.result = -1;
        response.error_code = GetLastSocketError();
      } else {
        if (SocketSetOpt(state->native_handle, msg.sockopt.level,
                         msg.sockopt.optname, msg.sockopt.optval.Data(),
                         static_cast<int>(msg.sockopt.optval.Size())) != 0) {
          response.result = -1;
          response.error_code = GetLastSocketError();
        }
      }
      break;
    }

    case NetMessageType::GetOption: {
      auto state = GetSocket(msg.socket_id);
      if (!state || state->native_handle == kInvalidSocket) {
        response.result = -1;
        response.error_code = GetLastSocketError();
      } else {
        int len = static_cast<int>(msg.sockopt.optval.Size());
        if (SocketGetOpt(state->native_handle, msg.sockopt.level,
                         msg.sockopt.optname, msg.sockopt.optval.Data(),
                         &len) != 0) {
          response.result = -1;
          response.error_code = GetLastSocketError();
        } else {
          response.buffer = std::move(msg.sockopt.optval);
          response.buffer.Resize(len);
        }
      }
      break;
    }

    case NetMessageType::SetBlocking: {
      auto state = GetSocket(msg.socket_id);
      if (!state || state->native_handle == kInvalidSocket) {
        response.result = -1;
        response.error_code = GetLastSocketError();
      } else {
        if (SocketSetBlocking(state->native_handle, msg.set_blocking.blocking) !=
            0) {
          response.result = -1;
          response.error_code = GetLastSocketError();
        }
      }
      break;
    }

    case NetMessageType::Shutdown: {
      auto state = GetSocket(msg.socket_id);
      if (!state || state->native_handle == kInvalidSocket) {
        response.result = -1;
        response.error_code = GetLastSocketError();
      } else {
        if (SocketShutdown(state->native_handle, msg.shutdown.how) != 0) {
          response.result = -1;
          response.error_code = GetLastSocketError();
        }
      }
      break;
    }

    case NetMessageType::GetSockName: {
      auto state = GetSocket(msg.socket_id);
      if (!state || state->native_handle == kInvalidSocket) {
        response.result = -1;
        response.error_code = GetLastSocketError();
      } else {
        sockaddr_in sa = {};
        int len = sizeof(sa);
        if (SocketGetSockName(state->native_handle,
                              reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
          response.result = -1;
          response.error_code = GetLastSocketError();
        } else {
          response.address.FromNative(sa);
        }
      }
      break;
    }

    case NetMessageType::GetPeerName: {
      auto state = GetSocket(msg.socket_id);
      if (!state || state->native_handle == kInvalidSocket) {
        response.result = -1;
        response.error_code = GetLastSocketError();
      } else {
        sockaddr_in sa = {};
        int len = sizeof(sa);
        if (SocketGetPeerName(state->native_handle,
                              reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
          response.result = -1;
          response.error_code = GetLastSocketError();
        } else {
          response.address.FromNative(sa);
        }
      }
      break;
    }

    case NetMessageType::RegisterSocket: {
      auto state = std::make_shared<SocketState>();
      state->native_handle = msg.register_socket.native_handle;
      state->address_family = msg.register_socket.af;
      state->socket_type = msg.register_socket.sock_type;
      state->protocol = msg.register_socket.protocol;
      state->is_udp = (msg.register_socket.sock_type == SOCK_DGRAM);

      if (state->is_udp) {
        SocketDisableUdpConnReset(state->native_handle);
      }

      response.socket_id = AddSocket(state);
      break;
    }

    default:
      response.result = -1;
      break;
  }

  // Invoke callback with response
  if (msg.callback) {
    msg.callback(response);
  }
}

void NetworkService::PollSockets() {
  // Gather all active sockets for polling
  std::vector<std::pair<SocketId, std::shared_ptr<SocketState>>> sockets_to_poll;
  {
    std::lock_guard<std::mutex> lock(sockets_mutex_);
    for (auto& pair : sockets_) {
      if (pair.second && pair.second->native_handle != kInvalidSocket &&
          !pair.second->closed && !pair.second->listening) {
        sockets_to_poll.push_back(pair);
      }
    }
  }

  if (sockets_to_poll.empty()) {
    // Nothing to poll, just sleep briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollTimeoutMs));
    return;
  }

  // Build poll array
  std::vector<PollResult> poll_results(sockets_to_poll.size());
  for (size_t i = 0; i < sockets_to_poll.size(); ++i) {
    poll_results[i].socket = sockets_to_poll[i].second->native_handle;
    poll_results[i].revents = kPollIn;  // We want to poll for readable
  }

  // Poll
  int ready = SocketPoll(poll_results.data(), static_cast<int>(poll_results.size()),
                         kPollTimeoutMs);

  if (ready <= 0) {
    return;  // Timeout or error
  }

  // Process readable sockets
  for (size_t i = 0; i < poll_results.size(); ++i) {
    if (!(poll_results[i].revents & kPollIn)) {
      continue;
    }

    auto& state = sockets_to_poll[i].second;
    if (state->closed) continue;

    // Receive data
    uint8_t buffer[65536];  // Max UDP packet size
    sockaddr_in from = {};
    int from_len = sizeof(from);

    int received = SocketRecvFrom(
        state->native_handle, buffer, sizeof(buffer), 0,
        reinterpret_cast<sockaddr*>(&from), &from_len);

    if (received > 0) {
      RxMessage rx_msg;
      rx_msg.socket_id = sockets_to_poll[i].first;
      rx_msg.buffer = NetBuffer(buffer, received);
      rx_msg.from.FromNative(from);
      rx_msg.error_code = 0;

      state->rx_queue->Push(std::move(rx_msg));
    } else if (received < 0) {
      int err = GetLastSocketError();

      // Ignore WOULDBLOCK/EAGAIN - not a real error
#ifdef XE_PLATFORM_WIN32
      if (err != WSAEWOULDBLOCK) {
#else
      if (err != EAGAIN && err != EWOULDBLOCK) {
#endif
        RxMessage rx_msg;
        rx_msg.socket_id = sockets_to_poll[i].first;
        rx_msg.error_code = err;
        state->rx_queue->Push(std::move(rx_msg));
      }
    }
    // received == 0 means connection closed for TCP
  }
}

}  // namespace net
}  // namespace kernel
}  // namespace xe