/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NET_PLATFORM_SOCKET_H_
#define XENIA_KERNEL_NET_PLATFORM_SOCKET_H_

#include <cstdint>

#include "xenia/base/platform.h"

#ifdef XE_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WS2tcpip.h>
#include <WinSock2.h>
#pragma comment(lib, "Ws2_32.lib")
#elif XE_PLATFORM_LINUX || XE_PLATFORM_ANDROID
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace xe {
namespace kernel {
namespace net {

// Platform-specific socket handle type
#ifdef XE_PLATFORM_WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
constexpr int kSocketError = SOCKET_ERROR;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
constexpr int kSocketError = -1;
#endif

// Socket poll event flags (unified across platforms)
enum PollEvents : uint16_t {
  kPollIn = 0x0001,   // Data available to read
  kPollOut = 0x0002,  // Ready to write
  kPollErr = 0x0004,  // Error condition
  kPollHup = 0x0008,  // Hang up
};

// Poll result for a single socket
struct PollResult {
  NativeSocket socket;
  uint16_t revents;
};

// Initialize platform networking (WSAStartup on Windows, no-op elsewhere)
bool PlatformNetworkInit();

// Shutdown platform networking
void PlatformNetworkShutdown();

// Create a socket
NativeSocket SocketCreate(int af, int type, int protocol);

// Close a socket
int SocketClose(NativeSocket s);

// Bind socket to address
int SocketBind(NativeSocket s, const sockaddr* addr, int addrlen);

// Listen for connections
int SocketListen(NativeSocket s, int backlog);

// Accept incoming connection
NativeSocket SocketAccept(NativeSocket s, sockaddr* addr, int* addrlen);

// Connect to remote address
int SocketConnect(NativeSocket s, const sockaddr* addr, int addrlen);

// Send data (connected socket)
int SocketSend(NativeSocket s, const void* buf, int len, int flags);

// Send data to address (UDP)
int SocketSendTo(NativeSocket s, const void* buf, int len, int flags,
                 const sockaddr* to, int tolen);

// Receive data (connected socket)
int SocketRecv(NativeSocket s, void* buf, int len, int flags);

// Receive data with source address (UDP)
int SocketRecvFrom(NativeSocket s, void* buf, int len, int flags,
                   sockaddr* from, int* fromlen);

// Set socket option
int SocketSetOpt(NativeSocket s, int level, int optname, const void* optval,
                 int optlen);

// Get socket option
int SocketGetOpt(NativeSocket s, int level, int optname, void* optval,
                 int* optlen);

// Set socket blocking mode
int SocketSetBlocking(NativeSocket s, bool blocking);

// Poll multiple sockets for events
// Returns number of sockets with events, 0 on timeout, -1 on error
int SocketPoll(PollResult* results, int count, int timeout_ms);

// Get last socket error
int GetLastSocketError();

// Set last socket error
void SetLastSocketError(int error);

// Shutdown socket for reading/writing
int SocketShutdown(NativeSocket s, int how);

// Get local address of bound socket
int SocketGetSockName(NativeSocket s, sockaddr* addr, int* addrlen);

// Get remote address of connected socket
int SocketGetPeerName(NativeSocket s, sockaddr* addr, int* addrlen);

// Disable UDP CONNRESET behavior on Windows (no-op elsewhere)
void SocketDisableUdpConnReset(NativeSocket s);

}  // namespace net
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NET_PLATFORM_SOCKET_H_
