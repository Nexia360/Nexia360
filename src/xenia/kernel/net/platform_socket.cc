/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/net/platform_socket.h"

#include <vector>

namespace xe {
namespace kernel {
namespace net {

bool PlatformNetworkInit() {
#ifdef XE_PLATFORM_WIN32
  WSADATA wsa_data;
  int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
  return result == 0;
#else
  return true;
#endif
}

void PlatformNetworkShutdown() {
#ifdef XE_PLATFORM_WIN32
  WSACleanup();
#endif
}

NativeSocket SocketCreate(int af, int type, int protocol) {
  return ::socket(af, type, protocol);
}

int SocketClose(NativeSocket s) {
#ifdef XE_PLATFORM_WIN32
  return ::closesocket(s);
#else
  return ::close(s);
#endif
}

int SocketBind(NativeSocket s, const sockaddr* addr, int addrlen) {
  return ::bind(s, addr, addrlen);
}

int SocketListen(NativeSocket s, int backlog) {
  return ::listen(s, backlog);
}

NativeSocket SocketAccept(NativeSocket s, sockaddr* addr, int* addrlen) {
#ifdef XE_PLATFORM_WIN32
  return ::accept(s, addr, addrlen);
#else
  socklen_t len = addrlen ? *addrlen : 0;
  NativeSocket result = ::accept(s, addr, addrlen ? &len : nullptr);
  if (addrlen) *addrlen = len;
  return result;
#endif
}

int SocketConnect(NativeSocket s, const sockaddr* addr, int addrlen) {
  return ::connect(s, addr, addrlen);
}

int SocketSend(NativeSocket s, const void* buf, int len, int flags) {
  return ::send(s, static_cast<const char*>(buf), len, flags);
}

int SocketSendTo(NativeSocket s, const void* buf, int len, int flags,
                 const sockaddr* to, int tolen) {
  return ::sendto(s, static_cast<const char*>(buf), len, flags, to, tolen);
}

int SocketRecv(NativeSocket s, void* buf, int len, int flags) {
  return ::recv(s, static_cast<char*>(buf), len, flags);
}

int SocketRecvFrom(NativeSocket s, void* buf, int len, int flags,
                   sockaddr* from, int* fromlen) {
#ifdef XE_PLATFORM_WIN32
  return ::recvfrom(s, static_cast<char*>(buf), len, flags, from, fromlen);
#else
  socklen_t flen = fromlen ? *fromlen : 0;
  int result = ::recvfrom(s, static_cast<char*>(buf), len, flags, from,
                          fromlen ? &flen : nullptr);
  if (fromlen) *fromlen = flen;
  return result;
#endif
}

int SocketSetOpt(NativeSocket s, int level, int optname, const void* optval,
                 int optlen) {
  return ::setsockopt(s, level, optname, static_cast<const char*>(optval),
                      optlen);
}

int SocketGetOpt(NativeSocket s, int level, int optname, void* optval,
                 int* optlen) {
#ifdef XE_PLATFORM_WIN32
  return ::getsockopt(s, level, optname, static_cast<char*>(optval), optlen);
#else
  socklen_t len = optlen ? *optlen : 0;
  int result =
      ::getsockopt(s, level, optname, optval, optlen ? &len : nullptr);
  if (optlen) *optlen = len;
  return result;
#endif
}

int SocketSetBlocking(NativeSocket s, bool blocking) {
#ifdef XE_PLATFORM_WIN32
  u_long mode = blocking ? 0 : 1;
  return ::ioctlsocket(s, FIONBIO, &mode);
#else
  int flags = ::fcntl(s, F_GETFL, 0);
  if (flags == -1) return -1;
  if (blocking) {
    flags &= ~O_NONBLOCK;
  } else {
    flags |= O_NONBLOCK;
  }
  return ::fcntl(s, F_SETFL, flags);
#endif
}

int SocketPoll(PollResult* results, int count, int timeout_ms) {
  if (count <= 0 || !results) return 0;

#ifdef XE_PLATFORM_WIN32
  // Use WSAPoll on Windows
  std::vector<WSAPOLLFD> fds(count);
  for (int i = 0; i < count; ++i) {
    fds[i].fd = results[i].socket;
    fds[i].events = 0;
    if (results[i].revents & kPollIn) fds[i].events |= POLLIN;
    if (results[i].revents & kPollOut) fds[i].events |= POLLOUT;
    fds[i].revents = 0;
  }

  int ret = WSAPoll(fds.data(), count, timeout_ms);

  for (int i = 0; i < count; ++i) {
    results[i].revents = 0;
    if (fds[i].revents & POLLIN) results[i].revents |= kPollIn;
    if (fds[i].revents & POLLOUT) results[i].revents |= kPollOut;
    if (fds[i].revents & POLLERR) results[i].revents |= kPollErr;
    if (fds[i].revents & POLLHUP) results[i].revents |= kPollHup;
  }

  return ret;
#else
  // Use poll on Linux/Android
  std::vector<pollfd> fds(count);
  for (int i = 0; i < count; ++i) {
    fds[i].fd = results[i].socket;
    fds[i].events = 0;
    if (results[i].revents & kPollIn) fds[i].events |= POLLIN;
    if (results[i].revents & kPollOut) fds[i].events |= POLLOUT;
    fds[i].revents = 0;
  }

  int ret = ::poll(fds.data(), count, timeout_ms);

  for (int i = 0; i < count; ++i) {
    results[i].revents = 0;
    if (fds[i].revents & POLLIN) results[i].revents |= kPollIn;
    if (fds[i].revents & POLLOUT) results[i].revents |= kPollOut;
    if (fds[i].revents & POLLERR) results[i].revents |= kPollErr;
    if (fds[i].revents & POLLHUP) results[i].revents |= kPollHup;
  }

  return ret;
#endif
}

int GetLastSocketError() {
#ifdef XE_PLATFORM_WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

void SetLastSocketError(int error) {
#ifdef XE_PLATFORM_WIN32
  WSASetLastError(error);
#else
  errno = error;
#endif
}

int SocketShutdown(NativeSocket s, int how) {
  return ::shutdown(s, how);
}

int SocketGetSockName(NativeSocket s, sockaddr* addr, int* addrlen) {
#ifdef XE_PLATFORM_WIN32
  return ::getsockname(s, addr, addrlen);
#else
  socklen_t len = addrlen ? *addrlen : 0;
  int result = ::getsockname(s, addr, addrlen ? &len : nullptr);
  if (addrlen) *addrlen = len;
  return result;
#endif
}

int SocketGetPeerName(NativeSocket s, sockaddr* addr, int* addrlen) {
#ifdef XE_PLATFORM_WIN32
  return ::getpeername(s, addr, addrlen);
#else
  socklen_t len = addrlen ? *addrlen : 0;
  int result = ::getpeername(s, addr, addrlen ? &len : nullptr);
  if (addrlen) *addrlen = len;
  return result;
#endif
}

void SocketDisableUdpConnReset(NativeSocket s) {
#ifdef XE_PLATFORM_WIN32
  // SIO_UDP_CONNRESET = 0x9800000C
  // Disable "ICMP Port Unreachable -> WSAECONNRESET on next recvfrom" behavior
  BOOL new_behavior = FALSE;
  DWORD bytes_returned = 0;
  WSAIoctl(s, 0x9800000C, &new_behavior, sizeof(new_behavior), nullptr, 0,
           &bytes_returned, nullptr, nullptr);
#else
  // No equivalent on Linux - UDP doesn't have this behavior
  (void)s;
#endif
}

}  // namespace net
}  // namespace kernel
}  // namespace xe
