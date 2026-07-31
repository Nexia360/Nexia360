/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_NEXIA_TRANSPORT_H_
#define XENIA_KERNEL_UTIL_NEXIA_TRANSPORT_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/platform.h"

#ifdef XE_PLATFORM_WIN32
#include <WS2tcpip.h>
#elif XE_PLATFORM_LINUX
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

namespace xe {
namespace kernel {

// Nexia Hub Transport client.
//
// One long-lived TCP connection to the relay carries every flow. Frame header
// is 22 bytes, big-endian:
//
//   0   8   SXUID     source XUID
//   8   2   SPORT     source port
//   10  8   DXUID     destination XUID - this is what routes
//   18  2   DPORT     destination port
//   20  2   LEN       payload length, actual bytes following
//   22  ..  Payload
//
// There are no IP addresses on the wire. A player is identified by XUID; the
// ports are carried so each end can present the peer to the guest on the port
// it expects. An address cannot identify a player - two instances behind one
// public IP share it, and both bind the same low guest port.
//
// LEN also frames the stream: TCP has no datagram boundaries, so the reader
// consumes exactly header + LEN per frame.
//
// Registration is the first frame after connect: our XUID with a zero DXUID and
// zero LEN. The same frame is the keepalive, sent after 60s idle. The socket
// stays open for the session.

// Port the relay listens on, alongside the hub's HTTP API on the same host.
// Matches NEXIA_TRANSPORT_PORT server-side.
constexpr uint16_t kNexiaTransportPort = 36001;

class NexiaTransport {
 public:
  static constexpr size_t kHeaderSize = 22;

  // Largest payload LEN can describe. Guest datagrams are far below this.
  static constexpr size_t kMaxPayload = 0xFFFF;

  // Idle time before a keepalive frame goes out.
  static constexpr uint32_t kKeepaliveIdleMs = 60'000;

  // OS socket buffer sizes for the relay stream. The kernel default (~64 KB) is
  // small for a burst of game frames; a larger window lets the writer keep
  // filling without blocking on a full send buffer, and the reader absorb a
  // burst before recv would need to drop.
  static constexpr int kSocketBufferBytes = 256 * 1024;

  // Per-recv chunk. Sized to drain a full receive buffer in few syscalls.
  static constexpr size_t kRecvChunkBytes = 16 * 1024;

  struct Datagram {
    // Who sent it. The guest is handed an address derived from this, because
    // the XUID is the only identity the wire carries.
    uint64_t xuid = 0;
    uint16_t port_be = 0;
    std::vector<uint8_t> data;
  };

  NexiaTransport() = default;
  ~NexiaTransport();

  // host/port of the relay; xuid identifies us. There is no sign-in and no
  // address registration - the XUID is the identity, for the whole session.
  void Configure(const std::string& host, uint16_t port, uint64_t xuid);

  bool is_configured() const;

  // Connects, registers and starts the reader. Safe to call more than once.
  bool Start();
  void Stop();

  bool is_ready() const { return ready_.load(); }

  // Frames and sends. dest_xuid is what the relay routes on; a zero one cannot
  // be routed and is refused here rather than at the relay.
  bool SendTo(uint64_t dest_xuid, uint16_t source_port_be,
              uint16_t dest_port_be, const uint8_t* data, size_t size);

  // A socket claims the port it answers on, so an arriving frame goes to the
  // one it was addressed to instead of whichever happens to read first. Claims
  // are counted: two sockets may share a port, and the last one out drops it.
  void ClaimPort(uint16_t port_be);
  void ReleasePort(uint16_t port_be);

  // Pops the next datagram addressed to this port. Non-blocking.
  bool PopDatagram(uint16_t port_be, Datagram* out);

  // Whether anything is waiting for this port. Relayed sockets poll this
  // instead of the host socket, which never receives in relay mode.
  bool HasDatagram(uint16_t port_be);

 private:
  void PumpMain();
  void KeepaliveMain();

  // Our XUID with no destination and no payload. Registers us on connect and
  // keeps the connection alive when the title has nothing to say.
  bool SendRegistration();

  bool ResolveServer();
  bool OpenSocket();
  void CloseSocket();

  // Queues a whole frame for the writer thread. Never touches the socket, so a
  // guest thread calling sendto can never block on TCP flow control.
  bool EnqueueFrame(std::vector<uint8_t> frame);

  // Drains send_queue_ to the socket, looping over partial writes - TCP is
  // free to accept less than asked, and a short write would corrupt framing.
  void WriterMain();

  void BuildHeader(uint8_t* out, uint16_t source_port_be, uint64_t dest_xuid,
                   uint16_t dest_port_be, uint16_t payload_len) const;

  // Consumes as many whole frames as the buffer holds, leaving any partial
  // one in place for the next read.
  void ConsumeFrames();

  mutable std::mutex mutex_;

  std::string host_;
  uint16_t port_ = kNexiaTransportPort;
  uint64_t xuid_ = 0;

  sockaddr_in server_addr_ = {};
  bool server_resolved_ = false;

  uintptr_t socket_ = ~uintptr_t(0);

  std::atomic<bool> ready_ = {false};
  std::atomic<bool> running_ = {false};

  // Milliseconds (steady) of the last frame we sent, for the idle keepalive.
  std::atomic<uint64_t> last_send_ms_ = {0};

  // Unparsed stream bytes. TCP delivers a byte stream, so a frame can arrive
  // split across reads or several frames can arrive in one.
  std::vector<uint8_t> stream_;

  // Destination port (network order) -> what is waiting for it. Queues exist
  // only while a socket claims the port: creating one on arrival would let a
  // stray frame allocate a queue nothing ever drains, and would swallow the
  // packet silently instead of saying where it went.
  struct PortQueue {
    uint32_t claims = 0;
    std::deque<Datagram> queue;
  };

  // Per port, not global: one chatty flow must not starve another, and a title
  // that stops draining its QoS socket should not cost the game socket its
  // buffer.
  static constexpr size_t kMaxQueueDepth = 5120;

  std::map<uint16_t, PortQueue> port_queues_;
  std::mutex recv_mutex_;

  // Frames waiting for the writer. Bounded, because an unbounded queue in front
  // of a stalled socket just moves the failure somewhere less visible.
  static constexpr size_t kMaxSendQueue = 10240;

  std::deque<std::vector<uint8_t>> send_queue_;
  std::mutex send_mutex_;
  std::condition_variable send_cv_;

  std::thread pump_thread_;
  std::thread keepalive_thread_;
  std::thread writer_thread_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_NEXIA_TRANSPORT_H_
