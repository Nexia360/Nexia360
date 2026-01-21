// src/xenia/kernel/xam/nexiahub_transport.h
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace xe {
namespace kernel {
namespace xam {

struct NexiaEndpoint {
  std::string host;
  uint16_t port = 0;
};

class NexiaHubTransport {
 public:
  NexiaHubTransport() = default;

  // Configure rendezvous / stun (stun unused for the TCP tunnel path).
  bool Configure(const std::string& rendezvous_host, uint16_t rendezvous_port,
                 const std::string& stun_host, uint16_t stun_port);

  // Legacy helpers (still used by some call sites).
  bool GetObservedAddr(std::string& out_host, uint16_t& out_port);
  bool Rendezvous(const std::string& room_key,
                  std::string& other_host, uint16_t& other_port);
  bool RegisterSelf(const std::string& room_key, uint16_t udp_port);

  // NEW (preferred): TCP tunnel to relay (proxy). Both peers connect out.
  // 5-arg version with explicit room id + role.
  bool ConnectTcpTunnel(const std::string& relay_host,
                        uint16_t relay_port,
                        const std::string& room_id,
                        const char* role,
                        uint16_t udp_port_hint /*0 for sender*/);

  // BACK-COMPAT OVERLOAD (matches xam_net.cc 4-arg call):
  // Interprets args as (relay_host, relay_port, role, udp_port_hint)
  // and uses default room_id "xenia".
  bool ConnectTcpTunnel(const std::string& relay_host,
                        uint16_t relay_port,
                        const char* role,
                        uint16_t udp_port_hint);

  // Send a UDP datagram payload over the tunnel (length-prefixed).
  // Returns bytes sent on success, -1 on error.
  int TunnelSend(const void* data, size_t size);

  // Receive a UDP datagram payload from the tunnel (length-prefixed).
  // - host_out/port_out are optional (relay may omit; we leave empty/0).
  // - nonblock=true: return 0 if no data available (no error).
  // On success: returns 1 and sets nread to payload bytes copied.
  // On EOF: returns 0. On error: returns -1.
  int TunnelRecv(std::string& host_out, uint16_t& port_out,
                 void* buf, size_t buf_cap, size_t& nread, bool nonblock);

  // Utility: check RFC1918 / CGNAT etc. (IPv4, network byte order).
  static bool IsPrivateIPv4(uint32_t be_addr);

 private:
  // Simple TCP helpers used internally.
  static bool TcpPost(const char* host, uint16_t port,
                      const char* path, const std::string& body,
                      std::string& resp_out);
  static bool TcpConnect(const std::string& host, uint16_t port, int& out_fd);

  // Length-prefixed (big-endian u32) I/O on a blocking socket.
  static bool WriteAll(int fd, const void* data, size_t sz);
  static bool ReadAll(int fd, void* data, size_t sz);
  static bool ReadU32BE(int fd, uint32_t& out);
  static bool WriteU32BE(int fd, uint32_t v);

 private:
  NexiaEndpoint rendezvous_{};
  NexiaEndpoint stun_{};

  std::string observed_host_;
  uint16_t observed_port_ = 0;

  // Tunnel state
  std::mutex tunnel_mu_;
  int tunnel_sock_ = -1;          // OS socket handle
  std::string tunnel_room_;       // for debug
  std::string relay_host_cached_;
  uint16_t relay_port_cached_ = 0;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe
