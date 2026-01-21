/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NET_NETWORK_SERVICE_H_
#define XENIA_KERNEL_NET_NETWORK_SERVICE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xenia/kernel/net/network_messages.h"
#include "xenia/kernel/net/platform_socket.h"
#include "xenia/kernel/net/threadsafe_queue.h"

namespace xe {
namespace kernel {
namespace net {

// Internal socket state tracked by NetworkService
struct SocketState {
  SocketId id = kInvalidSocketId;
  NativeSocket native_handle = kInvalidSocket;
  int address_family = 0;
  int socket_type = 0;
  int protocol = 0;
  bool bound = false;
  uint16_t bound_port = 0;
  bool connected = false;
  bool listening = false;
  bool is_udp = false;
  bool closed = false;

  // Per-socket RX queue for received data (pointer to avoid copy issues)
  std::unique_ptr<ThreadSafeQueue<RxMessage>> rx_queue;

  SocketState() : rx_queue(std::make_unique<ThreadSafeQueue<RxMessage>>()) {}

  // Copy constructor - copies state but creates fresh rx_queue
  SocketState(const SocketState& other)
      : id(other.id),
        native_handle(other.native_handle),
        address_family(other.address_family),
        socket_type(other.socket_type),
        protocol(other.protocol),
        bound(other.bound),
        bound_port(other.bound_port),
        connected(other.connected),
        listening(other.listening),
        is_udp(other.is_udp),
        closed(other.closed),
        rx_queue(std::make_unique<ThreadSafeQueue<RxMessage>>()) {}

  // Copy assignment - copies state but creates fresh rx_queue
  SocketState& operator=(const SocketState& other) {
    if (this != &other) {
      id = other.id;
      native_handle = other.native_handle;
      address_family = other.address_family;
      socket_type = other.socket_type;
      protocol = other.protocol;
      bound = other.bound;
      bound_port = other.bound_port;
      connected = other.connected;
      listening = other.listening;
      is_udp = other.is_udp;
      closed = other.closed;
      rx_queue = std::make_unique<ThreadSafeQueue<RxMessage>>();
    }
    return *this;
  }
};

// NetworkService: Decoupled network I/O from main emulator thread
//
// Architecture:
//   - TX Thread: Drains TX queue, sends packets (fire-and-forget)
//   - RX Thread: Polls registered sockets, enqueues received data
//   - Control ops: Synchronous RPC (socket lifecycle)
//
// Thread safety:
//   - TX queue: Main thread pushes, TX thread pops
//   - RX queues: RX thread pushes, main thread pops (per-socket)
//   - Control queue: Main thread pushes, processed on RX thread
//   - Socket map: Protected by sockets_mutex_, accessed from multiple threads
//
class NetworkService {
 public:
  NetworkService();
  ~NetworkService();

  // Non-copyable, non-movable
  NetworkService(const NetworkService&) = delete;
  NetworkService& operator=(const NetworkService&) = delete;

  // Initialize the service (starts TX/RX threads)
  bool Initialize();

  // Shutdown the service (stops threads, closes all sockets)
  void Shutdown();

  // Check if service is running
  bool IsRunning() const { return running_.load(); }

  // ==========================================================================
  // Control Operations (Synchronous - blocks until complete)
  // ==========================================================================

  // Create a new socket
  // Returns socket ID, or kInvalidSocketId on error
  SocketId CreateSocket(int af, int type, int protocol,
                        int* error_out = nullptr);

  // Close a socket
  bool CloseSocket(SocketId id, int* error_out = nullptr);

  // Bind socket to address
  bool Bind(SocketId id, const NetAddress& addr, int* error_out = nullptr);

  // Listen for connections
  bool Listen(SocketId id, int backlog, int* error_out = nullptr);

  // Connect to remote address
  bool Connect(SocketId id, const NetAddress& addr, int* error_out = nullptr);

  // Accept incoming connection
  // Returns new socket ID, or kInvalidSocketId if no pending connection
  SocketId Accept(SocketId id, NetAddress* addr_out = nullptr,
                  int* error_out = nullptr);

  // Set socket option
  bool SetOption(SocketId id, int level, int optname, const void* optval,
                 int optlen, int* error_out = nullptr);

  // Get socket option
  bool GetOption(SocketId id, int level, int optname, void* optval, int* optlen,
                 int* error_out = nullptr);

  // Set socket blocking mode
  bool SetBlocking(SocketId id, bool blocking, int* error_out = nullptr);

  // Shutdown socket
  bool ShutdownSocket(SocketId id, int how, int* error_out = nullptr);

  // Get local address
  bool GetSockName(SocketId id, NetAddress* addr_out, int* error_out = nullptr);

  // Get peer address
  bool GetPeerName(SocketId id, NetAddress* addr_out, int* error_out = nullptr);

  // Register an existing native socket with the service
  // Used for sockets created outside NetworkService (e.g., from Accept)
  SocketId RegisterSocket(NativeSocket native, int af, int type, int protocol,
                          int* error_out = nullptr);

  // ==========================================================================
  // TX Operations (Fire and Forget - returns immediately)
  // ==========================================================================

  // Send data on connected socket
  void Send(SocketId id, const uint8_t* data, size_t len, uint32_t flags = 0);

  // Send data to specific address (UDP)
  void SendTo(SocketId id, const uint8_t* data, size_t len,
              const NetAddress& to, uint32_t flags = 0);

  // ==========================================================================
  // RX Operations (Non-blocking poll or blocking wait)
  // ==========================================================================

  // Try to receive data without blocking
  // Returns true if data was received
  bool TryRecv(SocketId id, RxMessage& msg_out);

  // Wait for data with timeout
  // Returns true if data was received, false on timeout
  bool WaitRecv(SocketId id, RxMessage& msg_out,
                std::chrono::milliseconds timeout);

  // Check if socket has pending received data
  bool HasPendingData(SocketId id);

  // ==========================================================================
  // Utility
  // ==========================================================================

  // Get native handle for socket (for use with external APIs)
  NativeSocket GetNativeHandle(SocketId id);

  // Get socket state (for debugging)
  bool GetSocketState(SocketId id, SocketState* state_out);

 private:
  // TX thread main loop
  void TxThreadMain();

  // RX thread main loop
  void RxThreadMain();

  // Process a single TX message
  void ProcessTxMessage(const TxMessage& msg);

  // Process a single control message
  void ProcessControlMessage(ControlMessage& msg);

  // Poll all registered sockets for incoming data
  void PollSockets();

  // Get socket by ID (thread-safe)
  std::shared_ptr<SocketState> GetSocket(SocketId id);

  // Add socket to tracking (thread-safe)
  SocketId AddSocket(std::shared_ptr<SocketState> state);

  // Remove socket from tracking (thread-safe)
  void RemoveSocket(SocketId id);

  // Generate next socket ID
  SocketId NextSocketId();

  // Execute control operation synchronously
  NetworkResponse ExecuteControl(ControlMessage msg);

 private:
  std::atomic<bool> running_{false};
  std::atomic<bool> initialized_{false};

  // Socket tracking
  std::mutex sockets_mutex_;
  std::map<SocketId, std::shared_ptr<SocketState>> sockets_;
  std::atomic<SocketId> next_socket_id_{1};

  // TX thread and queue
  std::thread tx_thread_;
  ThreadSafeQueue<TxMessage> tx_queue_;

  // RX thread and control queue
  std::thread rx_thread_;
  ThreadSafeQueue<ControlMessage> control_queue_;

  // Poll timeout for RX thread (ms)
  static constexpr int kPollTimeoutMs = 100;

  // Max TX messages to process per iteration
  static constexpr size_t kMaxTxBatch = 64;
};

// Global singleton access (matches Xenia patterns)
NetworkService* GetNetworkService();

}  // namespace net
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NET_NETWORK_SERVICE_H_
