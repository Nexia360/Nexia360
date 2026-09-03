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

  // UDP needs a far shorter one. A NAT keeps a UDP mapping for as little as
  // 20-30s, and once it lapses the relay's return path is silently gone - the
  // client keeps sending and simply stops hearing anything back.
  static constexpr uint32_t kUdpKeepaliveIdleMs = 15'000;

  // Every UDP datagram describes itself, big-endian:
  //
  //   0  2  FRAG_ID     per-sender, one value per logical frame
  //   2  1  FRAG_INDEX  which piece this is, zero based
  //   3  1  FRAG_COUNT  how many pieces the frame was split into
  //   4  .. that slice of the frame
  //
  // LEN frames a TCP stream because every byte sent arrives, in order. UDP
  // guarantees neither, so a stream reader loses its place permanently the
  // first time a datagram goes missing and reads payload bytes as a header.
  // Carrying the position in each datagram means a lost one costs only its own
  // frame - the next still says exactly where it belongs.
  static constexpr size_t kUdpFragHeaderBytes = 4;

  // Total datagram size, fragment header included. Titles fill their packets
  // to the link MTU, so this has to leave room for a full one plus both
  // headers or nearly every packet splits into a large piece and a stub -
  // doubling the packet rate and making each frame twice as likely to be lost.
  // 1400 + 8 UDP + 20 IP stays inside even a 1492 byte PPPoE path, so the IP
  // layer still never fragments underneath us.
  static constexpr size_t kUdpDatagramBytes = 1400;
  static constexpr size_t kUdpFragPayloadBytes =
      kUdpDatagramBytes - kUdpFragHeaderBytes;

  // A frame can be split into at most this many pieces - FRAG_COUNT is a byte,
  // and the largest frame LEN can describe needs far fewer.
  static constexpr uint32_t kUdpMaxFragments = 255;

  // How long an incomplete frame waits for its missing pieces before it is
  // given up on. Only its own frame is lost; nothing after it is affected.
  static constexpr uint64_t kUdpReassemblyTimeoutMs = 1200;

  // A datagram socket has no completed connect to prove the relay is there, so
  // registration is answered and retried until it is.
  static constexpr uint32_t kUdpRegisterAttempts = 4;
  static constexpr uint32_t kUdpRegisterTimeoutMs = 400;

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
    // The bytes as they came off the socket, headers included, with the guest
    // payload starting at payload_offset. Kept whole rather than trimmed so
    // the receive path never copies it out of the buffer it arrived in.
    std::vector<uint8_t> data;
    size_t payload_offset = 0;

    const uint8_t* payload() const { return data.data() + payload_offset; }
    size_t payload_size() const { return data.size() - payload_offset; }
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

  // Which link carried the session. UDP is preferred and TCP is the fallback
  // for when it cannot be established at all - a datagram relay avoids the
  // head-of-line blocking a stream imposes, where one delayed frame holds up
  // every unrelated flow behind it.
  enum class Mode { kNone, kUdp, kTcp };
  Mode mode() const { return mode_.load(); }
  bool is_udp() const { return mode_.load() == Mode::kUdp; }

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

  // Buffer reuse, shared with the socket layer. Allocating per packet is the
  // real cost on this path - a memcpy of a few hundred bytes is nothing next
  // to hitting the allocator thousands of times a second from several threads
  // at once. Recycling only works if whoever takes one hands it back, so both
  // halves are callable from outside.
  std::vector<uint8_t> TakeBuffer();
  void ReturnBuffer(std::vector<uint8_t> buffer);

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

  // Opens the datagram socket and registers over it, retrying until the relay
  // answers. False means UDP is unusable here - blocked, black-holed, or the
  // relay is older than this protocol - and the caller falls back to TCP.
  bool TryStartUdp();

  // Replaces a UDP link that has gone silent with a fresh socket and
  // registration, in place - the reader and writer pick the new one up.
  void ReestablishUdp();

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

  // Sends a datagram that is already wire-ready, headers and all.
  bool SendUdpDatagram(uintptr_t fd, const std::vector<uint8_t>& datagram);

  std::vector<std::vector<uint8_t>> buffer_pool_;
  std::mutex buffer_pool_mutex_;
  static constexpr size_t kBufferPoolMax = 512;

  // Takes one datagram, reassembles if needed, and delivers any frame it
  // completes. Discards pieces of frames whose rest never arrived.
  void ConsumeUdpDatagram(const uint8_t* data, size_t size);

  // Hands a fully assembled frame to the port queues.
  void DeliverFrame(const uint8_t* frame, size_t size);

  mutable std::mutex mutex_;

  std::string host_;
  uint16_t port_ = kNexiaTransportPort;
  uint64_t xuid_ = 0;

  sockaddr_in server_addr_ = {};
  bool server_resolved_ = false;

  uintptr_t socket_ = ~uintptr_t(0);

  std::atomic<bool> ready_ = {false};
  std::atomic<bool> running_ = {false};
  std::atomic<Mode> mode_ = {Mode::kNone};

  // Milliseconds (steady) of the last frame we sent, for the idle keepalive.
  std::atomic<uint64_t> last_send_ms_ = {0};

  // Milliseconds (steady) of the last thing heard from the relay. The link is
  // only known to be alive by hearing back, so this is what silence is measured
  // against.
  std::atomic<uint64_t> last_recv_ms_ = {0};

  // A dead UDP link cannot announce itself: there is no connection to close,
  // so without this the client sends into the void indefinitely. Silence for
  // this long means the socket is re-created and registration redone.
  static constexpr uint64_t kUdpLinkSilenceMs = 45'000;

  // Unparsed stream bytes. TCP delivers a byte stream, so a frame can arrive
  // split across reads or several frames can arrive in one.
  std::vector<uint8_t> stream_;

  // Frames arriving in pieces, keyed by FRAG_ID. Only ever holds an entry for
  // a frame that genuinely needed splitting; a single-piece one is delivered
  // without touching this.
  struct PendingFrame {
    std::vector<std::vector<uint8_t>> parts;
    // Held separately from parts: a piece can legitimately be empty, so an
    // empty slot cannot stand in for "not arrived" without a duplicate of it
    // decrementing the counter twice and underflowing it.
    std::vector<bool> have;
    uint32_t remaining = 0;
    uint64_t first_seen_ms = 0;
  };
  std::map<uint16_t, PendingFrame> udp_pending_;

  // Labels the pieces of one frame so the far side can place them.
  uint16_t next_frag_id_ = 0;

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

  // The inbound equivalent of kUdpMaxSendQueue, and for the same reason: a
  // deep receive queue only hands the guest datagrams that were already stale
  // when they got there. The TCP depth above is far too generous for a
  // real-time flow.
  static constexpr size_t kUdpMaxQueueDepth = 128;

  std::map<uint16_t, PortQueue> port_queues_;
  std::mutex recv_mutex_;

  // Frames waiting for the writer. Bounded, because an unbounded queue in front
  // of a stalled socket just moves the failure somewhere less visible.
  static constexpr size_t kMaxSendQueue = 10240;

  // At most this many frames in flight on UDP, and the newest are the ones
  // kept. A deep queue in front of a real-time flow is itself the problem: by
  // the time a backlog drains its contents describe a world that has moved on,
  // and the game has already resent whatever mattered.
  static constexpr size_t kUdpMaxSendQueue = 128;

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
