/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_XUID_HANDLES_H_
#define XENIA_KERNEL_UTIL_XUID_HANDLES_H_

#include <cstdint>

namespace xe {
namespace kernel {

// XUID <-> in_addr handle mapping for NexiaHub Transport.
//
// XNetXnAddrToInAddr hands the title an in_addr that is a handle to an XnAddr,
// not a routable address, so on the transport we mint one per player from their
// XUID. The relay routes on XUID, so resolving a handle back to its XUID is the
// whole of address resolution.
//
// A handle is 32 bits from a 64-bit XUID, so it cannot be inverted - only
// looked up. Handles also travel between consoles inside the game's own
// protocol, so a joining player must resolve handles it never minted: register
// every XUID we learn from the hub, not just the ones we are about to talk to.
//
// Kept apart from XLiveAPI so the JSON parsers can register XUIDs without an
// include cycle.

// 10.x.y.z from the XUID's low three bytes, avoiding .0 and .255. Network
// order.
uint32_t XuidToHandle(uint64_t xuid);

void RegisterXuidHandle(uint64_t xuid);

// 0 when the handle was never registered.
uint64_t XuidForHandle(uint32_t handle);

size_t RegisteredHandleCount();

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_XUID_HANDLES_H_
