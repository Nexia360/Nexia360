/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_SOCKET_COMPAT_H_
#define XENIA_KERNEL_UTIL_SOCKET_COMPAT_H_

#ifdef XE_PLATFORM_WIN32
// Windows already has everything via winsock2.h / ws2tcpip.h
#else

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// Type aliases to match Windows names
typedef int SOCKET;
typedef unsigned long DWORD;
typedef long HRESULT;
typedef char CHAR;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define ioctlsocket ioctl
#define SD_BOTH SHUT_RDWR
// POLLRDNORM/POLLWRNORM already defined on Linux via poll.h

// Missing WSA error codes
#define WSAEMSGSIZE EMSGSIZE
#define WSAENETDOWN ENETDOWN
#define WSAEHOSTDOWN EHOSTDOWN
#define WSAEHOSTUNREACH EHOSTUNREACH
#define WSAECONNABORTED ECONNABORTED
#define WSAESHUTDOWN ESHUTDOWN

// Windows Sleep(ms) → Linux usleep(us) / nanosleep
#include <time.h>
inline void Sleep(unsigned long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

// MemoryBarrier
#define MemoryBarrier() __sync_synchronize()

// IN_ADDR is just in_addr on Linux
typedef struct in_addr IN_ADDR;

// WSABUF equivalent
struct WSABUF {
  unsigned long len;
  char* buf;
};

// WSA error codes mapped to POSIX
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAEINVAL EINVAL
#define WSAENOTSOCK ENOTSOCK
#define WSAENETRESET ENETRESET
#define WSAECONNRESET ECONNRESET
#define WSAETIMEDOUT ETIMEDOUT
#define WSAENOTCONN ENOTCONN
#define WSAECONNREFUSED ECONNREFUSED

inline int WSAGetLastError() { return errno; }

// InetPtonA / InetNtopA
#define InetPtonA(af, src, dst) inet_pton(af, src, dst)
inline const char* InetNtopA(int af, const void* src, char* dst, size_t size) {
  return inet_ntop(af, src, dst, static_cast<socklen_t>(size));
}

// WSAPOLLFD → pollfd
typedef struct pollfd WSAPOLLFD;
inline int WSAPoll(WSAPOLLFD* fds, unsigned long nfds, int timeout) {
  return poll(fds, nfds, timeout);
}

// S_un.S_addr → s_addr compatibility
// On Windows: in_addr.S_un.S_addr
// On Linux: in_addr.s_addr
// Provide a union wrapper so code using S_un.S_addr compiles on Linux.
// We can't redefine in_addr, so we fix it at each call site or use a macro.

// Helper to access the 32-bit address from an in_addr portably.
#define INADDR_VALUE(addr) ((addr).s_addr)

// S_un.S_un_b byte access — extract individual bytes from in_addr
#define INADDR_B1(addr) (((uint8_t*)&(addr).s_addr)[0])
#define INADDR_B2(addr) (((uint8_t*)&(addr).s_addr)[1])
#define INADDR_B3(addr) (((uint8_t*)&(addr).s_addr)[2])
#define INADDR_B4(addr) (((uint8_t*)&(addr).s_addr)[3])

#endif  // !XE_PLATFORM_WIN32

#endif  // XENIA_KERNEL_UTIL_SOCKET_COMPAT_H_
