# Xenia Network Service

Decoupled network I/O layer for Xenia's Xbox 360 emulation.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      Main / Xbox Thread                          │
│  (XSocket methods delegate to NetworkService)                    │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│                      NetworkService                              │
│                                                                  │
│  CreateSocket/Bind/Close ──────► Control Queue ──► RX Thread    │
│  (synchronous RPC)               (processed there)               │
│                                                                  │
│  Send/SendTo ──────────────────► TX Queue ───────► TX Thread    │
│  (fire and forget)               (drains queue)                  │
│                                                                  │
│  TryRecv/WaitRecv ◄────────────── Per-Socket RX Queue ◄── RX    │
│  (poll or block)                  (filled by RX thread)          │
└─────────────────────────────────────────────────────────────────┘
```

## Components

### platform_socket.h / .cc
Platform abstraction layer for socket operations. Handles Windows vs Linux/Android differences.

### threadsafe_queue.h
Simple mutex-protected queue using standard library primitives. Matches Xenia's existing patterns.

### network_messages.h
Message types for communication between threads:
- `TxMessage`: Send/SendTo requests
- `RxMessage`: Received data
- `ControlMessage`: Socket lifecycle operations
- `NetworkResponse`: Response from control operations

### network_service.h / .cc
Main service class with:
- **TX Thread**: Processes send queue, fire-and-forget semantics
- **RX Thread**: Polls sockets, fills per-socket receive queues, handles control messages
- **Synchronous control operations**: Socket creation, binding, options, etc.

## Usage

```cpp
#include "xenia/kernel/net/network_service.h"

using namespace xe::kernel::net;

// Initialize
NetworkService service;
service.Initialize();

// Create socket
int error;
SocketId sock = service.CreateSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &error);

// Bind
NetAddress addr;
addr.family = AF_INET;
addr.port = htons(3074);
addr.addr = INADDR_ANY;
service.Bind(sock, addr);

// Send (fire and forget)
uint8_t data[] = {1, 2, 3, 4};
NetAddress dest;
dest.family = AF_INET;
dest.port = htons(3074);
dest.addr = inet_addr("192.168.1.100");
service.SendTo(sock, data, sizeof(data), dest);

// Receive (non-blocking)
RxMessage rx;
if (service.TryRecv(sock, rx)) {
    // Process rx.buffer, rx.from
}

// Or blocking with timeout
if (service.WaitRecv(sock, rx, std::chrono::milliseconds(1000))) {
    // Got data
}

// Cleanup
service.CloseSocket(sock);
service.Shutdown();
```

## Integration with XSocket

Replace XSocket's direct socket calls with NetworkService calls:

```cpp
// Old (in xsocket.cc):
native_handle_ = socket(af, type, proto);

// New:
auto* net_service = GetNetworkService();
socket_id_ = net_service->CreateSocket(af, type, proto);
native_handle_ = net_service->GetNativeHandle(socket_id_);
```

## Blocking Receive Simulation

For Xbox calls that expect blocking (recv without FIONBIO):

```cpp
// In XSocket::Recv() or XSocket::RecvFrom():
RxMessage rx;
if (net_service->WaitRecv(socket_id_, rx, std::chrono::milliseconds(timeout))) {
    // Copy data to guest buffer
    std::memcpy(buf, rx.buffer.Data(), rx.buffer.Size());
    return rx.buffer.Size();
}
return -1;  // WSAEWOULDBLOCK
```

## Thread Safety

- TX Queue: Main thread pushes, TX thread pops
- RX Queues: RX thread pushes, main thread pops (per-socket)
- Control Queue: Main thread pushes, RX thread processes
- Socket Map: Protected by mutex, accessed from all threads

## Platform Support

- **Windows**: Uses WSAPoll, WSAStartup/Cleanup, ioctlsocket
- **Linux/Android**: Uses poll, fcntl, standard POSIX sockets
- Unified error handling via GetLastSocketError/SetLastSocketError

## Files

```
src/xenia/kernel/net/
├── platform_socket.h      # Platform abstraction header
├── platform_socket.cc     # Platform abstraction implementation
├── threadsafe_queue.h     # Thread-safe queue template
├── network_messages.h     # Message type definitions
├── network_service.h      # NetworkService header
├── network_service.cc     # NetworkService implementation
└── README.md              # This file
```

## TODO

- [ ] Integrate with Xenia logging (XELOGI, etc.)
- [ ] Add Xbox error code translation
- [ ] WSASendTo/WSARecvFrom overlapped support
- [ ] TCP connection handling refinements
- [ ] UPnP integration (port mapping)
- [ ] Performance tuning (batch sizes, poll timeouts)
