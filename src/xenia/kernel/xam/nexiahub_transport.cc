// src/xenia/kernel/xam/nexiahub_transport.cc
#include "nexiahub_transport.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace xe {
namespace kernel {
namespace xam {

//------------------------------------------------------------------------------
// Internal helpers
//------------------------------------------------------------------------------

static inline void closesock_int(int s) {
#ifdef _WIN32
  if (s >= 0) closesocket(s);
#else
  if (s >= 0) ::close(s);
#endif
}
static inline int bytes_available(int fd) {
#ifdef _WIN32
  u_long n = 0;
  if (ioctlsocket(fd, FIONREAD, &n) != 0) return -1;
  return (int)n;
#else
  int n = 0;
  if (ioctl(fd, FIONREAD, &n) != 0) return -1;
  return n;
#endif
}
bool NexiaHubTransport::WriteAll(int fd, const void* data, size_t sz) {
  const char* p = static_cast<const char*>(data);
  size_t left = sz;
  while (left) {
#ifdef _WIN32
    int n = ::send(fd, p, (int)left, 0);
#else
    ssize_t n = ::send(fd, p, left, 0);
#endif
    if (n <= 0) return false;
    p += n;
    left -= (size_t)n;
  }
  return true;
}

bool NexiaHubTransport::ReadAll(int fd, void* data, size_t sz) {
  char* p = static_cast<char*>(data);
  size_t left = sz;
  while (left) {
#ifdef _WIN32
    int n = ::recv(fd, p, (int)left, 0);
#else
    ssize_t n = ::recv(fd, p, left, 0);
#endif
    if (n <= 0) return false;
    p += n;
    left -= (size_t)n;
  }
  return true;
}

static inline uint32_t ToBE32(uint32_t v) { return htonl(v); }
static inline uint32_t FromBE32(uint32_t v) { return ntohl(v); }

bool NexiaHubTransport::WriteU32BE(int fd, uint32_t v) {
  uint32_t be = ToBE32(v);
  return WriteAll(fd, &be, sizeof(be));
}

bool NexiaHubTransport::ReadU32BE(int fd, uint32_t& out) {
  uint32_t be = 0;
  if (!ReadAll(fd, &be, sizeof(be))) return false;
  out = FromBE32(be);
  return true;
}

bool NexiaHubTransport::TcpConnect(const std::string& host, uint16_t port,
                                   int& out_fd) {
#ifdef _WIN32
  static bool wsa_ok = []() {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  (void)wsa_ok;
#endif
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  char pbuf[16];
  std::snprintf(pbuf, sizeof(pbuf), "%u", (unsigned)port);
  if (getaddrinfo(host.c_str(), pbuf, &hints, &res) != 0 || !res) return false;

  int s = -1;
  for (addrinfo* rp = res; rp; rp = rp->ai_next) {
    s = (int)::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (s < 0) continue;

    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

    if (::connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;

    closesock_int(s);
    s = -1;
  }
  freeaddrinfo(res);
  if (s < 0) return false;

  out_fd = s;
  return true;
}

bool NexiaHubTransport::TcpPost(const char* host, uint16_t port,
                                const char* path, const std::string& body,
                                std::string& resp_out) {
  int s = -1;
  if (!TcpConnect(host, port, s)) return false;

  char hosthdr[256];
  std::snprintf(hosthdr, sizeof(hosthdr), "%s:%u", host, (unsigned)port);
  char clen[32];
  std::snprintf(clen, sizeof(clen), "%u", (unsigned)body.size());

  std::string req;
  req.reserve(256 + body.size());
  req += "POST ";
  req += path;
  req += " HTTP/1.1\r\nHost: ";
  req += hosthdr;
  req +=
      "\r\nConnection: close\r\nContent-Type: "
      "application/json\r\nContent-Length: ";
  req += clen;
  req += "\r\n\r\n";
  req += body;

  if (!WriteAll(s, req.data(), req.size())) {
    closesock_int(s);
    return false;
  }

  // Read everything
  std::string resp;
  char buf[4096];
  for (;;) {
#ifdef _WIN32
    int n = ::recv(s, buf, (int)sizeof(buf), 0);
#else
    ssize_t n = ::recv(s, buf, sizeof(buf), 0);
#endif
    if (n <= 0) break;
    resp.append(buf, buf + n);
  }
  closesock_int(s);

  if (resp.rfind("HTTP/1.1 200", 0) != 0 &&
      resp.rfind("HTTP/1.0 200", 0) != 0) {
    return false;
  }
  resp_out.swap(resp);
  return true;
}

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

bool NexiaHubTransport::Configure(const std::string& rendezvous_host,
                                  uint16_t rendezvous_port,
                                  const std::string& stun_host,
                                  uint16_t stun_port) {
  rendezvous_.host =
      rendezvous_host.empty() ? "107.155.85.242" : rendezvous_host;
  rendezvous_.port = rendezvous_port ? rendezvous_port : 8088;
  stun_.host = stun_host.empty() ? rendezvous_.host : stun_host;
  stun_.port = stun_port ? stun_port : 3478;
  return true;
}

bool NexiaHubTransport::GetObservedAddr(std::string& out_host,
                                        uint16_t& out_port) {
  // If your server has /whoami, wire it here. For now, return
  // cached/placeholder.
  if (!observed_host_.empty() && observed_port_) {
    out_host = observed_host_;
    out_port = observed_port_;
    return true;
  }
  out_host = "0.0.0.0";
  out_port = 0;
  return true;
}

bool NexiaHubTransport::Rendezvous(const std::string& room_key,
                                   std::string& other_host,
                                   uint16_t& other_port) {
  // Legacy rendezvous contract: POST /rendezvous {"room_id": "..."}
  std::string body = std::string("{\"room_id\":\"") + room_key + "\"}";
  std::string resp;
  if (!TcpPost(rendezvous_.host.c_str(), rendezvous_.port, "/rendezvous", body,
               resp)) {
    return false;
  }
  size_t hend = resp.find("\r\n\r\n");
  if (hend == std::string::npos) return false;
  std::string json = resp.substr(hend + 4);

  // Expect ..."addr":["HOST",PORT]...
  size_t p = json.find("\"addr\"");
  if (p == std::string::npos) return false;
  p = json.find('[', p);
  if (p == std::string::npos) return false;
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
  if (p >= json.size() || json[p] != '\"') return false;
  ++p;

  size_t q = json.find('\"', p);
  if (q == std::string::npos) return false;
  other_host = json.substr(p, q - p);

  size_t r = json.find(',', q + 1);
  if (r == std::string::npos) return false;
  ++r;
  while (r < json.size() && (json[r] == ' ' || json[r] == '\t')) ++r;
  if (r >= json.size()) return false;

  other_port =
      static_cast<uint16_t>(std::strtoul(json.c_str() + r, nullptr, 10));
  return true;
}

bool NexiaHubTransport::RegisterSelf(const std::string& room_key,
                                     uint16_t udp_port) {
  // Optional registration kept for compatibility.
  std::string body;
  body.reserve(64 + room_key.size());
  body += "{\"room_id\":\"";
  body += room_key;
  body += "\",\"udp_port\":";
  body += std::to_string((unsigned)udp_port);
  body += "}";

  std::string resp;
  if (!TcpPost(rendezvous_.host.c_str(), rendezvous_.port, "/rendezvous", body,
               resp)) {
    return false;
  }
  return true;
}

bool NexiaHubTransport::ConnectTcpTunnel(const std::string& relay_host,
                                         uint16_t relay_port,
                                         const std::string& room_id,
                                         const char* role,
                                         uint16_t udp_port_hint) {
  int s = -1;
  if (!TcpConnect(relay_host, relay_port, s)) return false;

  // Handshake JSON (newline-terminated)
  std::string hello =
      std::string("{\"room_id\":\"") + room_id + "\",\"role\":\"";
  hello += (role ? role : "sender");
  hello += "\"";
  if (udp_port_hint) {
    hello += ",\"udp_port\":";
    hello += std::to_string((unsigned)udp_port_hint);
  }
  hello += "}\n";

  // Send handshake (blocking just for this tiny write)
  if (!WriteAll(s, hello.data(), hello.size())) {
    closesock_int(s);
    return false;
  }

  // Make the tunnel socket non-blocking and low-latency
#ifdef _WIN32
  {
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
  }
  {
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
  }
  // Short timeouts (best-effort; non-blocking anyway)
  {
    DWORD ms = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
  }
#else
  {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);
  }
  {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  }
  {
    timeval tv{0, 100 * 1000};  // 100ms
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
#endif

  // Cache tunnel socket
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    if (tunnel_sock_ >= 0) closesock_int(tunnel_sock_);
    tunnel_sock_ = s;
    tunnel_room_ = room_id;
    relay_host_cached_ = relay_host;
    relay_port_cached_ = relay_port;
  }
  return true;
}

// 4-arg back-compat overload: (host, port, role, udp_port_hint) -> default room
// "xenia".
bool NexiaHubTransport::ConnectTcpTunnel(const std::string& relay_host,
                                         uint16_t relay_port, const char* role,
                                         uint16_t udp_port_hint) {
  return ConnectTcpTunnel(relay_host, relay_port, /*room_id*/ "xenia", role,
                          udp_port_hint);
}

int NexiaHubTransport::TunnelSend(const void* data, size_t size) {
  std::lock_guard<std::mutex> lock(tunnel_mu_);
  if (tunnel_sock_ < 0 || !data || size == 0) return -1;

  // Try a cheap space check first
  int avail = bytes_available(tunnel_sock_);
  (void)avail;  // not reliable for write space; still proceed

  // Best-effort: write length and payload; if the OS send returns <=0, treat as
  // “try again”
  uint32_t be_len = htonl((uint32_t)size);
#ifdef _WIN32
  int n1 = ::send(tunnel_sock_, reinterpret_cast<const char*>(&be_len), 4, 0);
#else
  ssize_t n1 = ::send(tunnel_sock_, &be_len, 4, 0);
#endif
  if (n1 != 4) return 0;  // would block / partial; caller can retry later

#ifdef _WIN32
  int n2 =
      ::send(tunnel_sock_, reinterpret_cast<const char*>(data), (int)size, 0);
#else
  ssize_t n2 = ::send(tunnel_sock_, data, size, 0);
#endif
  if (n2 <= 0) return 0;  // would block; caller retries later

  return (int)n2;
}

int NexiaHubTransport::TunnelRecv(std::string& host_out, uint16_t& port_out,
                                  void* buf, size_t buf_cap, size_t& nread,
                                  bool nonblock) {
  host_out.clear();
  port_out = 0;
  nread = 0;

  int s;
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    s = tunnel_sock_;
  }
  if (s < 0) return -1;

  // Non-blocking: only proceed if we have enough bytes buffered for a full
  // frame. Frame format: [u32_be length] [payload...]
  int avail = bytes_available(s);
  if (avail == 0) {
    return nonblock ? 0 : 0;  // don’t block here; caller can poll again
  }
  if (avail < 0) {
    return -1;  // socket error
  }
  if (avail < 4) {
    return nonblock ? 0 : 0;  // not enough to read the length yet
  }

  // Peek the 4-byte length without consuming (so we can ensure full frame
  // present)
#ifdef _WIN32
  uint32_t be_len = 0;
  // MSG_PEEK is supported on Windows too.
  int pn = ::recv(s, reinterpret_cast<char*>(&be_len), 4, MSG_PEEK);
  if (pn <= 0) return 0;  // closed or temp no data
#else
  uint32_t be_len = 0;
  ssize_t pn = ::recv(s, &be_len, 4, MSG_PEEK);
  if (pn <= 0) return 0;
#endif
  if (pn < 4) {
    return nonblock ? 0 : 0;  // not a full length yet
  }
  uint32_t want = ntohl(be_len);
  if (want == 0) {
    // Consume the 4-byte header, return empty datagram
    uint32_t drop;
    if (!ReadAll(s, &drop, 4)) return 0;
    nread = 0;
    return 1;
  }

  // Do we have the full frame buffered? (length + payload)
  // We already know at least 4 are present; check payload too.
  avail = bytes_available(s);
  if (avail < 4 + (int)want) {
    // Not all bytes have arrived yet — don’t block.
    return nonblock ? 0 : 0;
  }

  // Now actually consume: first the length header, then the payload (bounded by
  // buf_cap)
  uint32_t drop_len = 0;
  if (!ReadAll(s, &drop_len, 4)) return -1;

  size_t to_copy = (size_t)want;
  if (to_copy > buf_cap) to_copy = buf_cap;

  if (!ReadAll(s, buf, to_copy)) return -1;

  // If payload is larger than our buffer, drain the remainder.
  size_t left = (size_t)want - to_copy;
  if (left) {
    char drain[1024];
    while (left) {
#ifdef _WIN32
      int n = ::recv(s, drain, (int)std::min(left, sizeof(drain)), 0);
#else
      ssize_t n = ::recv(s, drain, std::min(left, sizeof(drain)), 0);
#endif
      if (n <= 0) return -1;
      left -= (size_t)n;
    }
  }

  nread = to_copy;
  return 1;
}

bool NexiaHubTransport::IsPrivateIPv4(uint32_t be_addr) {
  uint32_t host = ntohl(be_addr);
  uint8_t a = (host >> 24) & 0xFF;
  uint8_t b = (host >> 16) & 0xFF;

  if (a == 10) return true;
  if (a == 172 && (b >= 16 && b <= 31)) return true;
  if (a == 192 && b == 168) return true;
  if (a == 100 && (b >= 64 && b <= 127)) return true;  // CGNAT
  if (a == 169 && b == 254) return true;               // link-local
  return false;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
