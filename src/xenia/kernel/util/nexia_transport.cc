/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/nexia_transport.h"

#include <chrono>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

#ifdef XE_PLATFORM_LINUX
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
  {
    std::lock_guard lock(send_mutex_);

    if (send_queue_.size() >= kMaxSendQueue) {
      // The socket is not draining. The caller is a guest thread and must not
      // be parked on TCP flow control, so refuse rather than block.
      XELOGE("NexiaTransport: send queue full ({}) - frame dropped",
             send_queue_.size());
      return false;
    }

    send_queue_.push_back(std::move(frame));
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
  std::vector<uint8_t> frame(kHeaderSize, 0);
  {
    std::lock_guard lock(mutex_);
    BuildHeader(frame.data(), 0, 0, 0, 0);
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

void NexiaTransport::ConsumeFrames() {
  size_t offset = 0;

  while (stream_.size() - offset >= kHeaderSize) {
    const uint8_t* header = stream_.data() + offset;
    const uint16_t payload_len = ReadU16(header + 20);

    if (stream_.size() - offset < kHeaderSize + payload_len) {
      // Frame is still arriving. Leave it for the next read.
      break;
    }

    const uint64_t src_xuid = ReadU64(header);
    uint16_t src_port_be = 0;
    uint16_t dest_port_be = 0;
    std::memcpy(&src_port_be, header + 8, 2);
    std::memcpy(&dest_port_be, header + 18, 2);

    const uint8_t* payload = header + kHeaderSize;
    offset += kHeaderSize + payload_len;

    if (!payload_len) {
      // A registration/keepalive echoed back, or a frame with nothing in it.
      continue;
    }

    Datagram datagram;
    datagram.xuid = src_xuid;
    datagram.port_be = src_port_be;
    datagram.data.assign(payload, payload + payload_len);

    {
      std::lock_guard lock(recv_mutex_);

      // DPORT decides which socket this belongs to. Delivering to whoever reads
      // first would hand a title's game packets to its QoS socket.
      auto it = port_queues_.find(dest_port_be);
      if (it != port_queues_.end() &&
          it->second.queue.size() < kMaxQueueDepth) {
        it->second.queue.push_back(std::move(datagram));
      }
    }
  }

  if (offset) {
    stream_.erase(stream_.begin(), stream_.begin() + offset);
  }
}

void NexiaTransport::PumpMain() {
  xe::threading::set_name("Nexia Transport");

  const native_socket fd = static_cast<native_socket>(socket_);
  uint8_t buffer[kRecvChunkBytes];

  while (running_.load()) {
    timeval tv = {};
    tv.tv_sec = 0;
    tv.tv_usec = 100 * 1000;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    if (select(static_cast<int>(fd) + 1, &rfds, nullptr, nullptr, &tv) <= 0) {
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

    // Only when genuinely idle: game traffic already proves the connection is
    // alive, so a fixed-interval ping would be pure noise.
    if (NowMs() - last_send_ms_.load() >= kKeepaliveIdleMs) {
      SendRegistration();
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
    if (!ResolveServer() || !OpenSocket()) {
      return false;
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
  CloseSocket();
}

}  // namespace kernel
}  // namespace xe
