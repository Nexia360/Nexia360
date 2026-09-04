/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/nexia_transport.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

DECLARE_bool(nexiahub_transport_tcp_fallback);

#ifdef XE_PLATFORM_LINUX
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

namespace xe {
namespace kernel {

namespace {

#ifdef XE_PLATFORM_WIN32
using native_socket = SOCKET;
using socklen_type = int;
#else
using native_socket = int;
using socklen_type = socklen_t;
#endif

void WriteU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value >> 8);
  out[1] = static_cast<uint8_t>(value & 0xFF);
}

void WriteU64(uint8_t* out, uint64_t value) {
  for (int i = 0; i < 8; i++) {
    out[i] = static_cast<uint8_t>((value >> (56 - i * 8)) & 0xFF);
  }
}

uint16_t ReadU16(const uint8_t* in) {
  return static_cast<uint16_t>((in[0] << 8) | in[1]);
}

uint64_t ReadU64(const uint8_t* in) {
  uint64_t value = 0;
  for (int i = 0; i < 8; i++) {
    value = (value << 8) | in[i];
  }
  return value;
}

uint64_t NowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

NexiaTransport::~NexiaTransport() { Stop(); }

void NexiaTransport::Configure(const std::string& host, uint16_t port,
                               uint64_t xuid) {
  std::lock_guard lock(mutex_);

  if (host != host_ || port != port_) {
    server_resolved_ = false;
  }

  host_ = host;
  port_ = port ? port : kNexiaTransportPort;
  xuid_ = xuid;
}

bool NexiaTransport::is_configured() const {
  std::lock_guard lock(mutex_);
  return !host_.empty() && xuid_ != 0;
}

bool NexiaTransport::ResolveServer() {
  if (server_resolved_) {
    return true;
  }
  if (host_.empty()) {
    return false;
  }

  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* result = nullptr;
  const std::string port_str = std::to_string(port_);
  if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result) != 0 ||
      !result) {
    XELOGE("NexiaTransport: could not resolve {}", host_);
    return false;
  }

  std::memcpy(&server_addr_, result->ai_addr, sizeof(sockaddr_in));
  freeaddrinfo(result);
  server_resolved_ = true;
  return true;
}

bool NexiaTransport::OpenSocket() {
  if (socket_ != ~uintptr_t(0)) {
    return true;
  }

  const native_socket fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == static_cast<native_socket>(-1)) {
    XELOGE("NexiaTransport: could not create socket");
    return false;
  }

  // Game traffic is latency-critical and small; Nagle would hold a frame back
  // waiting for more to coalesce with.
  int nodelay = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

  // Enlarge the outbound (SO_SNDBUF) and inbound (SO_RCVBUF) buffers so a burst
  // of relayed frames does not stall the writer or overflow the reader. Set
  // before connect so the sizes apply to the connection's window. Best-effort:
  // the OS may clamp to its max, which is fine.
  const int bufsize = kSocketBufferBytes;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufsize),
             sizeof(bufsize));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufsize),
             sizeof(bufsize));

  if (connect(fd, reinterpret_cast<const sockaddr*>(&server_addr_),
              sizeof(server_addr_)) < 0) {
    XELOGE("NexiaTransport: could not connect to {}:{}", host_, port_);
#ifdef XE_PLATFORM_WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    return false;
  }

  socket_ = static_cast<uintptr_t>(fd);
  return true;
}

bool NexiaTransport::TryStartUdp() {
  const native_socket fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == static_cast<native_socket>(-1)) {
    return false;
  }

  // Deliberately left at the OS defaults, unlike the TCP path. A large socket
  // buffer only lets stale datagrams pile up: by the time a deep queue drains,
  // its contents describe a world that has moved on, and the delay is
  // indistinguishable from lag. Dropping early is the better failure.

  // Connecting a datagram socket fixes the peer, which is what lets send/recv
  // be used and makes the OS surface an unreachable port as an error instead of
  // silence. The same socket sends and receives: the outbound datagram is what
  // opens the NAT mapping the relay's replies come back through, so a second
  // socket for reading would never hear anything.
  if (connect(fd, reinterpret_cast<const sockaddr*>(&server_addr_),
              sizeof(server_addr_)) < 0) {
#ifdef XE_PLATFORM_WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    return false;
  }

  // Carries the same fragment header as everything else on this socket - the
  // relay has one parser, so registration cannot be an exception to it.
  uint8_t hello[kUdpFragHeaderBytes + kHeaderSize] = {};
  WriteU16(hello, 0);
  hello[2] = 0;
  hello[3] = 1;
  BuildHeader(hello + kUdpFragHeaderBytes, 0, 0, 0, 0);

  for (uint32_t attempt = 0; attempt < kUdpRegisterAttempts; ++attempt) {
    if (send(fd, reinterpret_cast<const char*>(hello), sizeof(hello), 0) !=
        static_cast<int>(sizeof(hello))) {
      break;
    }

    timeval tv = {};
    tv.tv_sec = kUdpRegisterTimeoutMs / 1000;
    tv.tv_usec = (kUdpRegisterTimeoutMs % 1000) * 1000;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    if (select(static_cast<int>(fd) + 1, &rfds, nullptr, nullptr, &tv) <= 0) {
      // Lost, or nothing listening. Registration is idempotent, so it is simply
      // sent again.
      continue;
    }

    uint8_t ack[kUdpDatagramBytes] = {};
    const int received = recv(fd, reinterpret_cast<char*>(ack), sizeof(ack), 0);
    if (received < static_cast<int>(kUdpFragHeaderBytes + kHeaderSize)) {
      continue;
    }

    // The relay answers as itself: source XUID zero, addressed to us, empty.
    // Anything else is stray traffic on the port rather than an ack.
    const uint8_t* body = ack + kUdpFragHeaderBytes;
    if (ReadU64(body) != 0 || ReadU64(body + 10) != xuid_) {
      continue;
    }

    // Non-blocking from here on so the reader can drain the socket dry on each
    // wake instead of taking one datagram per select. Done after the handshake,
    // which wants the blocking recv above.
#ifdef XE_PLATFORM_WIN32
    u_long nonblocking = 1;
    ioctlsocket(fd, FIONBIO, &nonblocking);
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif

    socket_ = static_cast<uintptr_t>(fd);
    mode_.store(Mode::kUdp);
    // The ack just heard is the first proof of life; without seeding this the
    // silence check would measure from zero and rebuild a healthy link.
    last_recv_ms_.store(NowMs());
    XELOGI("NexiaTransport: registered over UDP with {}:{}", host_, port_);
    return true;
  }

#ifdef XE_PLATFORM_WIN32
  closesocket(fd);
#else
  close(fd);
#endif
  return false;
}

void NexiaTransport::ReestablishUdp() {
  XELOGE(
      "NexiaTransport: nothing from the relay in {}ms - rebuilding the UDP "
      "link",
      NowMs() - last_recv_ms_.load());

  std::lock_guard lock(mutex_);

  // Closed first: the reader and writer read socket_ each pass, so they park
  // on the invalid handle while this runs rather than using a dead one.
  CloseSocket();

  // Anything half-assembled belonged to the old socket.
  udp_pending_.clear();

  if (TryStartUdp()) {
    last_recv_ms_.store(NowMs());
    return;
  }

  // Left closed deliberately. The next heartbeat tries again, so a relay that
  // is briefly away is picked back up without the guest seeing a teardown.
  XELOGE("NexiaTransport: could not re-register over UDP - will retry");
}

void NexiaTransport::CloseSocket() {
  if (socket_ == ~uintptr_t(0)) {
    return;
  }
  const native_socket fd = static_cast<native_socket>(socket_);
#ifdef XE_PLATFORM_WIN32
  closesocket(fd);
#else
  close(fd);
#endif
  socket_ = ~uintptr_t(0);
}

void NexiaTransport::BuildHeader(uint8_t* out, uint16_t source_port_be,
                                 uint64_t dest_xuid, uint16_t dest_port_be,
                                 uint16_t payload_len) const {
  // Ports arrive in network order, which is already big-endian - so they are
  // copied rather than swapped again.
  WriteU64(out, xuid_);
  std::memcpy(out + 8, &source_port_be, 2);
  WriteU64(out + 10, dest_xuid);
  std::memcpy(out + 18, &dest_port_be, 2);
  WriteU16(out + 20, payload_len);
}

bool NexiaTransport::EnqueueFrame(std::vector<uint8_t> frame) {
  // Dropped buffers go back to the pool, but not while send_mutex_ is held -
  // taking the pool lock underneath it would nest the two for no reason.
  std::vector<std::vector<uint8_t>> recycle;

  {
    std::lock_guard lock(send_mutex_);

    if (is_udp()) {
      // Keep the newest, discard the oldest. Refusing the new frame instead
      // would leave a full queue of stale ones draining forever while every
      // current frame was thrown away - permanently behind, and never able to
      // recover.
      size_t dropped = 0;
      while (send_queue_.size() >= kUdpMaxSendQueue) {
        // A whole frame at a time, never part of one. Its pieces sit
        // contiguously and share a FRAG_ID, and shedding some of them would
        // guarantee the frame could never reassemble while still paying to
        // send the survivors.
        const uint16_t head_id = ReadU16(send_queue_.front().data());
        do {
          recycle.push_back(std::move(send_queue_.front()));
          send_queue_.pop_front();
          ++dropped;
        } while (!send_queue_.empty() &&
                 ReadU16(send_queue_.front().data()) == head_id);
      }
      if (dropped) {
        XELOGD("NexiaTransport: backlog - dropped {} stale datagram(s)",
               dropped);
      }
    } else if (send_queue_.size() >= kMaxSendQueue) {
      // The socket is not draining. The caller is a guest thread and must not
      // be parked on TCP flow control, so refuse rather than block.
      XELOGE("NexiaTransport: send queue full ({}) - frame dropped",
             send_queue_.size());
      return false;
    }

    send_queue_.push_back(std::move(frame));
  }

  for (auto& buffer : recycle) {
    ReturnBuffer(std::move(buffer));
  }

  send_cv_.notify_one();
  return true;
}

void NexiaTransport::WriterMain() {
  xe::threading::set_name("Nexia Transport TX");

  const native_socket fd = static_cast<native_socket>(socket_);

  while (running_.load()) {
    std::vector<uint8_t> frame;
    {
      std::unique_lock lock(send_mutex_);
      send_cv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
        return !send_queue_.empty() || !running_.load();
      });

      if (send_queue_.empty()) {
        continue;
      }

      frame = std::move(send_queue_.front());
      send_queue_.pop_front();
    }

    if (is_udp()) {
      // Already wire-ready: one send, no copy, and the buffer goes back to the
      // pool so the steady state does not touch the allocator at all.
      SendUdpDatagram(socket_, frame);
      last_send_ms_.store(NowMs());
      ReturnBuffer(std::move(frame));
      continue;
    }

    // Loop over partial writes: TCP may accept less than asked, and a short
    // write would splice this frame into the next one.
    size_t offset = 0;
    while (offset < frame.size() && running_.load()) {
      const int chunk =
          send(fd, reinterpret_cast<const char*>(frame.data() + offset),
               static_cast<int>(frame.size() - offset), 0);
      if (chunk <= 0) {
        XELOGE("NexiaTransport: write failed - relay connection lost");
        ready_.store(false);
        return;
      }
      offset += static_cast<size_t>(chunk);
    }

    last_send_ms_.store(NowMs());
  }
}

bool NexiaTransport::SendTo(uint64_t dest_xuid, uint16_t source_port_be,
                            uint16_t dest_port_be, const uint8_t* data,
                            size_t size) {
  if (!ready_.load() || !data || !size) {
    return false;
  }

  if (!dest_xuid) {
    // Nothing to route on.
    return false;
  }

  if (size > kMaxPayload) {
    XELOGE("NexiaTransport: payload {} exceeds LEN field - dropped", size);
    return false;
  }

  if (is_udp()) {
    // Built wire-ready here, fragment header and all, so the writer hands the
    // buffer straight to send() - the payload is touched exactly once on its
    // way out. Splitting happens here too, so each queue entry is one datagram
    // and the writer has no idea fragments exist.
    const size_t count =
        (kHeaderSize + size + kUdpFragPayloadBytes - 1) / kUdpFragPayloadBytes;
    if (!count || count > kUdpMaxFragments) {
      XELOGE("NexiaTransport: {} byte payload needs {} datagrams - dropped",
             size, count);
      return false;
    }

    uint8_t header[kHeaderSize];
    {
      std::lock_guard lock(mutex_);
      BuildHeader(header, source_port_be, dest_xuid, dest_port_be,
                  static_cast<uint16_t>(size));
    }

    const uint16_t frag_id = next_frag_id_++;
    bool queued = false;

    for (size_t index = 0; index < count; ++index) {
      const size_t offset = index * kUdpFragPayloadBytes;
      const size_t chunk =
          std::min(kUdpFragPayloadBytes, kHeaderSize + size - offset);

      std::vector<uint8_t> datagram = TakeBuffer();
      datagram.resize(kUdpFragHeaderBytes + chunk);

      WriteU16(datagram.data(), frag_id);
      datagram[2] = static_cast<uint8_t>(index);
      datagram[3] = static_cast<uint8_t>(count);

      // The frame is the 22 byte header followed by the payload, and a slice
      // can span the join - so each side is copied straight from where it
      // already lives rather than assembling the whole frame somewhere first.
      uint8_t* out = datagram.data() + kUdpFragHeaderBytes;
      if (offset < kHeaderSize) {
        const size_t from_header = std::min(chunk, kHeaderSize - offset);
        std::memcpy(out, header + offset, from_header);
        out += from_header;
        if (from_header < chunk) {
          std::memcpy(out, data, chunk - from_header);
        }
      } else {
        std::memcpy(out, data + (offset - kHeaderSize), chunk);
      }

      queued |= EnqueueFrame(std::move(datagram));
    }

    return queued;
  }

  std::vector<uint8_t> frame(kHeaderSize + size);
  {
    std::lock_guard lock(mutex_);
    BuildHeader(frame.data(), source_port_be, dest_xuid, dest_port_be,
                static_cast<uint16_t>(size));
  }
  std::memcpy(frame.data() + kHeaderSize, data, size);

  // Handed to the writer whole, so frames can never interleave on the stream
  // and a guest thread never waits on the socket.
  const bool sent = EnqueueFrame(std::move(frame));

  return sent;
}

bool NexiaTransport::SendRegistration() {
  // Our XUID, no destination, no payload. Registers us on connect, and is the
  // keepalive thereafter.
  const bool udp = is_udp();
  const size_t prefix = udp ? kUdpFragHeaderBytes : 0;

  std::vector<uint8_t> frame = TakeBuffer();
  frame.assign(prefix + kHeaderSize, 0);

  if (udp) {
    // Everything on this socket carries a fragment header, registration
    // included - the relay has one parser and no exceptions to it.
    WriteU16(frame.data(), next_frag_id_++);
    frame[2] = 0;
    frame[3] = 1;
  }
  {
    std::lock_guard lock(mutex_);
    BuildHeader(frame.data() + prefix, 0, 0, 0, 0);
  }
  return EnqueueFrame(std::move(frame));
}

void NexiaTransport::ClaimPort(uint16_t port_be) {
  std::lock_guard lock(recv_mutex_);
  port_queues_[port_be].claims++;
}

void NexiaTransport::ReleasePort(uint16_t port_be) {
  std::lock_guard lock(recv_mutex_);

  auto it = port_queues_.find(port_be);
  if (it == port_queues_.end()) {
    return;
  }

  // Counted, because two sockets may share a port - erasing on the first close
  // would take the other one's queue with it.
  if (it->second.claims > 1) {
    it->second.claims--;
    return;
  }

  port_queues_.erase(it);
}

bool NexiaTransport::HasDatagram(uint16_t port_be) {
  std::lock_guard lock(recv_mutex_);
  auto it = port_queues_.find(port_be);
  return it != port_queues_.end() && !it->second.queue.empty();
}

bool NexiaTransport::PopDatagram(uint16_t port_be, Datagram* out) {
  std::lock_guard lock(recv_mutex_);

  auto it = port_queues_.find(port_be);
  if (it == port_queues_.end() || it->second.queue.empty()) {
    return false;
  }

  *out = std::move(it->second.queue.front());
  it->second.queue.pop_front();
  return true;
}

void NexiaTransport::DeliverFrame(const uint8_t* frame, size_t size) {
  if (size < kHeaderSize) {
    return;
  }

  const uint16_t payload_len = ReadU16(frame + 20);
  if (kHeaderSize + payload_len != size) {
    return;
  }
  if (!payload_len) {
    // A registration/keepalive echoed back, or a frame with nothing in it.
    return;
  }

  const uint64_t src_xuid = ReadU64(frame);
  uint16_t src_port_be = 0;
  uint16_t dest_port_be = 0;
  std::memcpy(&src_port_be, frame + 8, 2);
  std::memcpy(&dest_port_be, frame + 18, 2);

  Datagram datagram;
  datagram.xuid = src_xuid;
  datagram.port_be = src_port_be;
  datagram.data = TakeBuffer();
  datagram.data.assign(frame, frame + kHeaderSize + payload_len);
  datagram.payload_offset = kHeaderSize;

  std::lock_guard lock(recv_mutex_);

  // DPORT decides which socket this belongs to. Delivering to whoever reads
  // first would hand a title's game packets to its QoS socket.
  auto it = port_queues_.find(dest_port_be);
  if (it == port_queues_.end()) {
    return;
  }

  if (is_udp()) {
    // Newest kept, oldest discarded - a guest that stops draining must not be
    // handed a backlog of datagrams describing a world that has moved on.
    while (it->second.queue.size() >= kUdpMaxQueueDepth) {
      it->second.queue.pop_front();
    }
    it->second.queue.push_back(std::move(datagram));
    return;
  }

  if (it->second.queue.size() < kMaxQueueDepth) {
    it->second.queue.push_back(std::move(datagram));
  }
}

std::vector<uint8_t> NexiaTransport::TakeBuffer() {
  std::lock_guard lock(buffer_pool_mutex_);
  if (buffer_pool_.empty()) {
    return {};
  }
  std::vector<uint8_t> buffer = std::move(buffer_pool_.back());
  buffer_pool_.pop_back();
  buffer.clear();
  return buffer;
}

void NexiaTransport::ReturnBuffer(std::vector<uint8_t> buffer) {
  // Only worth keeping if it already has capacity; an empty one is no cheaper
  // to reuse than to allocate.
  if (buffer.capacity() < kUdpDatagramBytes) {
    return;
  }
  std::lock_guard lock(buffer_pool_mutex_);
  if (buffer_pool_.size() >= kBufferPoolMax) {
    return;
  }
  buffer_pool_.push_back(std::move(buffer));
}

bool NexiaTransport::SendUdpDatagram(uintptr_t fd,
                                     const std::vector<uint8_t>& datagram) {
  const native_socket sock = static_cast<native_socket>(fd);

  const int sent = send(sock, reinterpret_cast<const char*>(datagram.data()),
                        static_cast<int>(datagram.size()), 0);
  if (sent > 0) {
    return true;
  }

#ifdef XE_PLATFORM_WIN32
  const int err = WSAGetLastError();
  const bool would_block = err == WSAEWOULDBLOCK;
#else
  const int err = errno;
  const bool would_block = err == EWOULDBLOCK || err == EAGAIN;
#endif

  // A full send buffer is backpressure, not a fault. Holding the datagram
  // would only deepen a queue whose contents are already stale by the time it
  // drained, so it is dropped - the guest protocol expects loss and resends.
  // Logging it per packet would drown the log during a burst.
  if (would_block) {
    return false;
  }

  // Anything else is real and has to be visible, or packets vanish with
  // nothing said.
  XELOGE("NexiaTransport: UDP send of {} bytes failed - error {}",
         datagram.size(), err);
  return false;
}

void NexiaTransport::ConsumeUdpDatagram(const uint8_t* data, size_t size) {
  if (size < kUdpFragHeaderBytes) {
    return;
  }

  const uint16_t frag_id = ReadU16(data);
  const uint8_t index = data[2];
  const uint8_t count = data[3];
  const uint8_t* body = data + kUdpFragHeaderBytes;
  const size_t body_size = size - kUdpFragHeaderBytes;

  if (!count || index >= count) {
    return;
  }

  const uint64_t now = NowMs();

  // Pieces of frames whose rest never turned up. Swept on every datagram, not
  // only on a multi-piece one, or an entry could sit here indefinitely once
  // traffic went back to fitting in single datagrams. Only the frame that lost
  // a piece is affected - never the ones after it.
  for (auto it = udp_pending_.begin(); it != udp_pending_.end();) {
    if (now - it->second.first_seen_ms >= kUdpReassemblyTimeoutMs) {
      XELOGE("NexiaTransport: frame {} incomplete after {}ms - pieces dropped",
             it->first, now - it->second.first_seen_ms);
      it = udp_pending_.erase(it);
    } else {
      ++it;
    }
  }

  // The overwhelming majority: a frame that fitted in one datagram, delivered
  // without keeping any state for it.
  if (count == 1) {
    DeliverFrame(body, body_size);
    return;
  }

  auto& pending = udp_pending_[frag_id];
  if (pending.have.empty()) {
    pending.parts.resize(count);
    pending.have.assign(count, false);
    pending.remaining = count;
    pending.first_seen_ms = now;
  }

  if (pending.have.size() != count) {
    // FRAG_ID reused while the previous frame was still incomplete.
    udp_pending_.erase(frag_id);
    return;
  }

  if (pending.have[index]) {
    return;  // duplicate
  }

  pending.parts[index].assign(body, body + body_size);
  pending.have[index] = true;
  if (--pending.remaining) {
    return;
  }

  std::vector<uint8_t> frame;
  for (const auto& part : pending.parts) {
    frame.insert(frame.end(), part.begin(), part.end());
  }
  udp_pending_.erase(frag_id);

  DeliverFrame(frame.data(), frame.size());
}

void NexiaTransport::ConsumeFrames() {
  size_t offset = 0;

  while (stream_.size() - offset >= kHeaderSize) {
    const uint8_t* header = stream_.data() + offset;
    const uint16_t payload_len = ReadU16(header + 20);

    if (stream_.size() - offset < kHeaderSize + payload_len) {
      // Frame is still arriving. Leave it for the next read.
      break;
    }

    DeliverFrame(header, kHeaderSize + payload_len);
    offset += kHeaderSize + payload_len;
  }

  if (offset) {
    stream_.erase(stream_.begin(), stream_.begin() + offset);
  }
}

void NexiaTransport::PumpMain() {
  xe::threading::set_name("Nexia Transport");

  uint8_t buffer[kRecvChunkBytes];

  while (running_.load()) {
    // Read every pass rather than cached: a UDP link that goes silent is
    // rebuilt underneath this loop, and a cached handle would keep polling the
    // closed one forever.
    native_socket fd;
    {
      std::lock_guard lock(mutex_);
      fd = static_cast<native_socket>(socket_);
    }
    if (fd == static_cast<native_socket>(~uintptr_t(0))) {
      xe::threading::Sleep(std::chrono::milliseconds(100));
      continue;
    }

    timeval tv = {};
    tv.tv_sec = 0;
    tv.tv_usec = 100 * 1000;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    if (select(static_cast<int>(fd) + 1, &rfds, nullptr, nullptr, &tv) <= 0) {
      continue;
    }

    if (is_udp()) {
      // Drained until empty, not one per wake: a recv returns a single
      // datagram, so one per select would need a syscall pair per packet and
      // fall behind a busy flow. The backlog then arrives in bursts and the
      // socket buffer starts overflowing. The socket is non-blocking, so this
      // ends when there is nothing left.
      for (;;) {
        const int received =
            recv(fd, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (received <= 0) {
          break;
        }
        // Anything at all proves the relay is still answering, so silence is
        // measured from here.
        last_recv_ms_.store(NowMs());
        // Each datagram says where it belongs, so there is no stream to keep
        // in step and a lost one costs only its own frame.
        ConsumeUdpDatagram(buffer, static_cast<size_t>(received));
      }
      continue;
    }

    const int received =
        recv(fd, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);

    if (received <= 0) {
      // Orderly close or a dead connection. Either way this socket is done -
      // report it rather than spinning on a broken descriptor.
      if (running_.load()) {
        XELOGE("NexiaTransport: relay connection lost");
        ready_.store(false);
      }
      return;
    }

    stream_.insert(stream_.end(), buffer, buffer + received);
    ConsumeFrames();
  }
}

void NexiaTransport::KeepaliveMain() {
  xe::threading::set_name("Nexia Transport KA");

  while (running_.load()) {
    xe::threading::Sleep(std::chrono::milliseconds(500));
    if (!running_.load()) {
      break;
    }

    if (!is_udp()) {
      // Only when genuinely idle: on a stream, game traffic already proves the
      // connection is alive and a fixed-interval ping would be pure noise.
      if (NowMs() - last_send_ms_.load() >= kKeepaliveIdleMs) {
        SendRegistration();
      }
      continue;
    }

    const uint64_t now = NowMs();

    // Unconditional on UDP, not only when idle. The relay answers every
    // registration, so this is the only thing that produces a reply on demand
    // - without it a session that is busy sending but hearing nothing back
    // cannot tell a healthy link from a dead one. It also keeps the NAT
    // mapping open, which lapses long before a TCP one would.
    if (now - last_send_ms_.load() >= kUdpKeepaliveIdleMs) {
      SendRegistration();
    }

    // Nothing heard back for several heartbeats. There is no connection to
    // close and no error to observe, so the link is presumed dead and rebuilt.
    if (last_recv_ms_.load() &&
        now - last_recv_ms_.load() >= kUdpLinkSilenceMs) {
      ReestablishUdp();
    }
  }
}

bool NexiaTransport::Start() {
  if (running_.load()) {
    return true;
  }

  {
    std::lock_guard lock(mutex_);
    if (host_.empty() || !xuid_) {
      return false;
    }
    if (!ResolveServer()) {
      return false;
    }
    // UDP first; TCP only when it cannot be established.
    if (!TryStartUdp()) {
      if (!cvars::nexiahub_transport_tcp_fallback) {
        XELOGE(
            "NexiaTransport: UDP registration with {}:{} did not answer and "
            "the TCP fallback is disabled - transport unavailable",
            host_, port_);
        return false;
      }
      XELOGI(
          "NexiaTransport: UDP registration with {}:{} did not answer - "
          "falling back to TCP",
          host_, port_);
      if (!OpenSocket()) {
        return false;
      }
      mode_.store(Mode::kTcp);
    }
  }

  running_.store(true);
  ready_.store(true);

  // Writer first: registration goes through the same queue as everything else,
  // so with no writer running it would sit there unsent.
  writer_thread_ = std::thread(&NexiaTransport::WriterMain, this);
  pump_thread_ = std::thread(&NexiaTransport::PumpMain, this);
  keepalive_thread_ = std::thread(&NexiaTransport::KeepaliveMain, this);

  // Registers us before any game traffic, so a peer can reach us as soon as it
  // looks us up rather than only after we have spoken.
  if (!SendRegistration()) {
    XELOGE("NexiaTransport: could not register with relay at {}:{}", host_,
           port_);
    Stop();
    return false;
  }

  return true;
}

void NexiaTransport::Stop() {
  if (!running_.exchange(false)) {
    CloseSocket();
    return;
  }

  // The writer parks on the condition variable, so it has to be woken or the
  // join below waits out its full timeout.
  send_cv_.notify_all();

  if (pump_thread_.joinable()) {
    pump_thread_.join();
  }
  if (keepalive_thread_.joinable()) {
    keepalive_thread_.join();
  }
  if (writer_thread_.joinable()) {
    writer_thread_.join();
  }

  ready_.store(false);
  mode_.store(Mode::kNone);
  CloseSocket();
}

}  // namespace kernel
}  // namespace xe
