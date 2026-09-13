/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_GUEST_HEAP_H_
#define XENIA_KERNEL_XNA_XNA_GUEST_HEAP_H_

#include <cstdint>

namespace xe {
namespace kernel {
namespace xna {

// Where a hosted title's D3D resources live: GUEST memory, not the host heap.
//
// WHY THIS EXISTS. The console runtime is written for a console. Every handle
// it holds - GraphicsDevice::pComPtr, GraphicsResource::pComPtr - is declared
// System.UInt32, because on hardware it IS a 32-bit pointer into the console's
// address space. Backing those with host-side objects and a counter works right
// up until the data has to be drawn: Nexia's command processor, texture cache
// and vertex fetch all address GUEST memory, so a vertex buffer sitting in a
// std::vector has to be copied across on every single draw. Allocating it in
// guest memory in the first place removes that copy and the parallel resource
// system that would otherwise have to exist beside the real one.
//
// A HANDLE IS A 24-BIT INDEX, NOT AN ADDRESS. The console runtime declares
// pComPtr as System.UInt32 and on hardware it was a pointer, but the command
// stream caps what it can be: AddSetStreamSource builds its header as
// `pComPtr | MaskPacketType(SetStreamSource)` with NO mask on the handle, so
// any bits in the top byte land in the packet type. An arena at FF800000 made
// SetStreamSource decode as type 255 and desynced the buffer after Clear. Some
// senders mask defensively; this one does not, so 24 bits is the contract.
//
// So handles index a table of descriptors that lives in guest memory, and the
// descriptor carries the guest address of the payload. Both stay GPU-visible,
// which is the whole point, while the handle stays inside 24 bits.
//
// Payloads are NOT in the descriptor table. They are never named by a packet,
// only reached through a descriptor, so they come from the ordinary physical
// heap where they can be as large as the title needs.
struct XnaGuestResource {
  enum Type : uint32_t {
    kNone = 0,
    kVertexBuffer = 1,
    kIndexBuffer = 2,
    kTexture = 3,
    kDeclaration = 4,
  };

  uint32_t type;
  uint32_t data;  // guest address of the payload, or 0
  uint32_t size;  // payload bytes
  uint32_t stride;
  uint32_t usage;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t element_size;
};

// TELLS THE GPU THAT GUEST MEMORY CHANGED UNDER IT.
//
// Everything a hosted title "uploads" is a memcpy from emulator code straight
// into guest memory. That is invisible to the shared memory, which learns about
// writes through the physical memory invalidation callbacks a GUEST store
// raises - so it keeps serving whatever it uploaded the first time, and the GPU
// reads stale bytes while a dump of the same address reads the new ones. Every
// write into a resource has to say so.
void XnaGuestRangeWritten(uint32_t guest_address, uint32_t length);

// The mirror image, and just as necessary. A resolve does NOT write guest
// memory - it keeps the pixels in a GPU readback buffer and arms a one-shot
// watch, and the bytes materialize only on the first GUEST access. Reading the
// destination with TranslateVirtual from here is not one, so it returns zeroes
// however well the resolve worked. Anything on this side that wants to LOOK at
// resolved pixels has to force the contract first.
void XnaGuestRangeRead(uint32_t guest_address, uint32_t length);

// Reserves the arena on first use. False if guest memory is not up yet, which
// means a title called in before the emulator finished starting.
bool XnaGuestHeapEnsure();

// THE WORKSPACE IS A PARTITION, NOT A HEAP THAT UNMAPS.
//
// An XNA title on the console was a user-mode process holding a fixed slice of
// RAM. Nothing unmapped a page under it: releasing a resource returned its
// bytes to the title's own allocator and that was all. Nexia's SystemHeapFree
// DECOMMITS, and the GPU keeps referring to what it was given - the render
// target cache holds a resolve destination by physical address, and the texture
// cache holds base addresses - so a render target that the title disposed and
// recreated left the GPU pointing at pages that now read as no-access. Every
// resolve to that address then failed for the rest of the run, which is a black
// screen and a stall per frame, and the address never became valid again.
//
// So payloads come from here: allocated from guest physical memory once and
// afterwards recycled by size, never handed back. Memory is bounded by peak
// use rather than by churn, which is what a fixed partition gives you.
uint32_t XnaGuestPayloadAlloc(uint32_t size);
void XnaGuestPayloadFree(uint32_t address);

// Allocates a zeroed descriptor and returns its GUEST ADDRESS, which is the
// handle handed back to managed code as pComPtr. Zero on failure.
uint32_t XnaGuestResourceCreate(uint32_t type);

// Releases a descriptor and any payload it owns.
void XnaGuestResourceDestroy(uint32_t handle);

// The descriptor at a full guest address, or null. Host pointer into guest
// memory - valid until the resource is destroyed.
XnaGuestResource* XnaGuestResourceLookup(uint32_t handle);

// Gives a resource a payload of `size` bytes in guest physical memory,
// preserving what the old one held when it is grown. False if it could not be
// allocated.
bool XnaGuestResourceResize(uint32_t handle, uint32_t size);

// A host pointer to a resource's payload, or null.
uint8_t* XnaGuestResourceData(uint32_t handle);

// Strips a packet type byte that a sender left sitting on top of a handle.
// Senders disagree about masking, and masking is right for both, since a
// handle is an index that never reaches 24 bits.
uint32_t XnaGuestResolveHandle(uint32_t packed_handle);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_GUEST_HEAP_H_
