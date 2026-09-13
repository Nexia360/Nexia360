/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_guest_heap.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {
namespace xna {

namespace {

// One 16 MB region, so that a descriptor's low 24 bits identify it uniquely and
// the top byte is a constant this file can restore. See the header.
// A HANDLE IS AN INDEX, NOT AN ADDRESS.
//
// The obvious reading of GraphicsResource::pComPtr being a System.UInt32 is
// that a handle is a console pointer, and it was - but the packet stream puts a
// hard ceiling on it. AddSetStreamSource builds its header as
// `pComPtr | MaskPacketType(SetStreamSource)` with NO mask on the handle, so
// anything in the top byte lands in the packet type: an arena at FF800000 made
// SetStreamSource arrive as type 255 and desynced the whole buffer. Other
// senders mask defensively, this one does not, so 24 bits is the real contract.
//
// So the handle is a one-based index into a table of descriptors that lives in
// guest memory. The descriptors and their payloads are still guest-side, which
// is the point of the exercise - the GPU can read them where they are - while
// the handle stays inside the 24 bits the command stream allows.
constexpr uint32_t kDescriptorStride = 64;
constexpr uint32_t kMaxResources = 65536;
constexpr uint32_t kArenaSize = kDescriptorStride * kMaxResources;
constexpr uint32_t kHandleMask = 0x00FFFFFFu;

std::mutex heap_mutex;
// Retired payload ranges, kept committed and reused. See the header.
struct PayloadBlock {
  uint32_t address;
  uint32_t size;
};
std::mutex payload_mutex;
std::vector<PayloadBlock> payload_pool;
std::unordered_map<uint32_t, uint32_t> payload_live;
uint32_t arena_base = 0;
// Next index never yet handed out. Index 0 is skipped so that a zeroed field is
// never mistaken for a live resource.
uint32_t next_index = 1;
std::vector<uint32_t> free_indices;

Memory* GuestMemory() {
  auto* state = kernel_state();
  return state ? state->memory() : nullptr;
}

}  // namespace

void XnaGuestRangeWritten(uint32_t guest_address, uint32_t length) {
  if (!guest_address || !length) {
    return;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  // The shared memory registers a physical memory invalidation callback and
  // relies on it to know a range is dirty. A guest store raises it through the
  // page protection; a memcpy from here does not, so it is raised by hand.
  // Without this the GPU keeps whatever it uploaded first - which for a buffer
  // filled after creation is zeroes - while every dump of the same address
  // reads the bytes the title actually wrote.
  auto global_lock = xe::global_critical_region::AcquireDirect();
  memory->TriggerPhysicalMemoryCallbacks(std::move(global_lock), guest_address,
                                         length, true, false);
}

void XnaGuestRangeRead(uint32_t guest_address, uint32_t length) {
  if (!guest_address || !length) {
    return;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  memory->MaterializeResolveReadWatches(guest_address, length);
}

uint32_t XnaGuestPayloadAlloc(uint32_t size) {
  if (!size) {
    return 0;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return 0;
  }
  {
    std::lock_guard<std::mutex> lock(payload_mutex);
    // Smallest block that will hold it, so a 4 MB render target is not handed
    // out to satisfy a 64 byte vertex buffer.
    size_t best = payload_pool.size();
    for (size_t i = 0; i < payload_pool.size(); ++i) {
      if (payload_pool[i].size < size) {
        continue;
      }
      if (best == payload_pool.size() ||
          payload_pool[i].size < payload_pool[best].size) {
        best = i;
      }
    }
    if (best != payload_pool.size()) {
      const PayloadBlock block = payload_pool[best];
      payload_pool[best] = payload_pool.back();
      payload_pool.pop_back();
      payload_live[block.address] = block.size;
      // SystemHeapAlloc hands back zeroed memory and callers rely on it, so a
      // recycled block has to look the same.
      memory->Zero(block.address, block.size);
      return block.address;
    }
  }
  // 4KB, because a texture fetch constant keeps the base address shifted right
  // by twelve and cannot express anything finer. The physical heap pages at
  // this size anyway, so every block in the pool satisfies it.
  const uint32_t address =
      memory->SystemHeapAlloc(size, 4096, kSystemHeapPhysical);
  if (address) {
    std::lock_guard<std::mutex> lock(payload_mutex);
    payload_live[address] = size;
  }
  return address;
}

void XnaGuestPayloadFree(uint32_t address) {
  if (!address) {
    return;
  }
  std::lock_guard<std::mutex> lock(payload_mutex);
  auto found = payload_live.find(address);
  if (found == payload_live.end()) {
    return;
  }
  payload_pool.push_back({address, found->second});
  payload_live.erase(found);
}

bool XnaGuestHeapEnsure() {
  std::lock_guard<std::mutex> lock(heap_mutex);
  if (arena_base) {
    return true;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return false;
  }
  // Physical, because the payloads these descriptors point at have to be
  // visible to the GPU. Where it lands no longer matters: the handle is an
  // index, so the arena is free to sit anywhere guest memory allows.
  const uint32_t base =
      memory->SystemHeapAlloc(kArenaSize, 4096, kSystemHeapPhysical);
  if (!base) {
    XELOGE("[xna] could not reserve the guest arena for D3D resources");
    return false;
  }
  arena_base = base;
  XELOGI("[xna] D3D resources live in guest memory at {:08X}-{:08X}, {} slots",
         base, base + kArenaSize, kMaxResources);
  return true;
}

uint32_t XnaGuestResourceCreate(uint32_t type) {
  if (!XnaGuestHeapEnsure()) {
    return 0;
  }
  uint32_t index = 0;
  {
    std::lock_guard<std::mutex> lock(heap_mutex);
    if (!free_indices.empty()) {
      index = free_indices.back();
      free_indices.pop_back();
    } else if (next_index < kMaxResources) {
      index = next_index++;
    } else {
      XELOGE("[xna] the guest resource table is full at {} slots",
             kMaxResources);
      return 0;
    }
  }
  auto* resource = XnaGuestResourceLookup(index);
  if (!resource) {
    return 0;
  }
  std::memset(resource, 0, sizeof(XnaGuestResource));
  resource->type = type;
  return index;
}

XnaGuestResource* XnaGuestResourceLookup(uint32_t handle) {
  // Masked, because senders differ: some write `pComPtr & 0x00FFFFFF` and some
  // OR the handle in raw. Masking is right for both, since a handle never
  // exceeds 24 bits by construction.
  handle &= kHandleMask;
  if (!arena_base || !handle || handle >= kMaxResources) {
    return nullptr;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return nullptr;
  }
  return memory->TranslateVirtual<XnaGuestResource*>(
      arena_base + handle * kDescriptorStride);
}

bool XnaGuestResourceResize(uint32_t handle, uint32_t size) {
  auto* resource = XnaGuestResourceLookup(handle);
  if (!resource) {
    return false;
  }
  if (resource->size >= size) {
    return true;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return false;
  }
  // Payloads come from the recycling pool: they are never named by a packet, so
  // they carry no 24-bit constraint and can be any size, and the pool keeps
  // every range committed for the life of the process.
  const uint32_t address = XnaGuestPayloadAlloc(size);
  if (!address) {
    XELOGE("[xna] could not allocate {} guest bytes for a D3D resource", size);
    return false;
  }
  auto* data = memory->TranslateVirtual<uint8_t*>(address);
  std::memset(data, 0, size);
  if (resource->data && resource->size) {
    // Grown, not replaced: XNA sizes some buffers by their first write, so the
    // contents of the old allocation are still the buffer's contents.
    std::memcpy(data, memory->TranslateVirtual<uint8_t*>(resource->data),
                resource->size);
    XnaGuestPayloadFree(resource->data);
  }
  resource->data = address;
  resource->size = size;
  return true;
}

uint8_t* XnaGuestResourceData(uint32_t handle) {
  auto* resource = XnaGuestResourceLookup(handle);
  if (!resource || !resource->data) {
    return nullptr;
  }
  auto* memory = GuestMemory();
  return memory ? memory->TranslateVirtual<uint8_t*>(resource->data) : nullptr;
}

void XnaGuestResourceDestroy(uint32_t handle) {
  auto* resource = XnaGuestResourceLookup(handle);
  if (!resource) {
    return;
  }
  if (resource->data) {
    XnaGuestPayloadFree(resource->data);
  }
  std::memset(resource, 0, sizeof(XnaGuestResource));
  std::lock_guard<std::mutex> lock(heap_mutex);
  free_indices.push_back(handle & kHandleMask);
}

// Senders disagree about masking - AddSetStreamSource ORs the handle in raw
// while SendResourceSetPacket masks it first - so the packet type byte may or
// may not still be sitting on top of the handle. Masking is correct for both,
// because a handle is an index that never reaches 24 bits.
uint32_t XnaGuestResolveHandle(uint32_t packed_handle) {
  return packed_handle & kHandleMask;
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
