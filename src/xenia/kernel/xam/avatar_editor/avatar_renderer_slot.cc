/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string.h>

#include "xenia/base/logging.h"
#include "xenia/kernel/xam/avatar_editor/editor_app.h"
#include "xenia/kernel/xam/xam_avatar_assets.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

namespace {

constexpr uint32_t kAllManifestComponentsMask = 0x1FFF;
static_assert((1u << kManifestComponentCount) - 1 == kAllManifestComponentsMask,
              "");

#pragma pack(push, 1)
// Avatars::Renderable_c, cut down to what the slot drives: the two buffers
// XamAvatarGetAssets filled, the mip-map pass's result, and the animation
// clock the slot advances every frame.
struct RenderableMemory {
  xe::be<uint32_t> assets;
  xe::be<uint32_t> gpu_resources;
  xe::be<int32_t> load_status;
  xe::be<float> animation_seconds;
};
#pragma pack(pop)

// XAvatarInitialize sets this; XAvatarCreateRendererEx refuses with
// 0x8000FFFF while it is clear.
bool AvatarSystemIsReady(const Guest& guest) {
  return guest.Load8(kAvatarSystemReadyAddress) != 0;
}

// sub_920F7940 -> XAvatarCreateRendererEx (0x92141860) ->
// Avatars::Renderable_c::Create -> XamAvatarGetAssets
int32_t CreateRenderableThroughXam(const Guest& guest,
                                   const AvatarRenderSlotMemory& slot,
                                   uint32_t* renderable_out) {
  *renderable_out = 0;

  const uint32_t renderable =
      guest.memory()->SystemHeapAlloc(sizeof(RenderableMemory));
  const uint32_t assets =
      renderable ? guest.memory()->SystemHeapAlloc(kAvatarResultBufferSize) : 0;
  const uint32_t gpu_resources =
      assets ? guest.memory()->SystemHeapAlloc(kAvatarGpuBufferSize) : 0;
  if (!gpu_resources) {
    if (assets) {
      guest.memory()->SystemHeapFree(assets);
    }
    if (renderable) {
      guest.memory()->SystemHeapFree(renderable);
    }
    return -1;
  }

  const X_RESULT result = BuildAvatarAssets(
      slot.metadata, kManifestBytes, slot.create_flags, assets, gpu_resources);
  if (result != X_ERROR_SUCCESS) {
    guest.memory()->SystemHeapFree(gpu_resources);
    guest.memory()->SystemHeapFree(assets);
    guest.memory()->SystemHeapFree(renderable);
    return -1;
  }

  RenderableMemory* object = guest.At<RenderableMemory>(renderable);
  object->assets = assets;
  object->gpu_resources = gpu_resources;
  object->load_status = kPending;
  object->animation_seconds = 0.0f;

  *renderable_out = renderable;
  return 0;
}

// Avatars::Renderable_c::UpdateLoading (0x92141398) ->
// XGetOverlappedExtendedError and XamAvatarGenerateMipMaps
int32_t UpdateRenderableLoadingThroughXam(const Guest& guest,
                                          uint32_t renderable) {
  RenderableMemory* object = guest.At<RenderableMemory>(renderable);
  if (!object) {
    return -1;
  }
  if (object->load_status != kPending) {
    return object->load_status;
  }
  // XamAvatarGenerateMipMaps writes the mips into the same GPU buffer the
  // assets already occupy, so there is nothing to poll after it.
  object->load_status = 0;
  return 0;
}

// Renderable_c vtable + 0x0C
void AdvanceRenderable(const Guest& guest, uint32_t renderable, float seconds) {
  RenderableMemory* object = guest.At<RenderableMemory>(renderable);
  if (object) {
    object->animation_seconds = float(object->animation_seconds) + seconds;
  }
}

// sub_920F8350
void HandOverRenderable(const Guest& guest, uint32_t from_slot,
                        uint32_t to_slot) {
  AvatarRenderSlotMemory* from = guest.At<AvatarRenderSlotMemory>(from_slot);
  AvatarRenderSlotMemory* to = guest.At<AvatarRenderSlotMemory>(to_slot);
  if (!from || !to) {
    return;
  }
  if (to->renderable) {
    RenderableMemory* old = guest.At<RenderableMemory>(to->renderable);
    if (old) {
      guest.memory()->SystemHeapFree(old->gpu_resources);
      guest.memory()->SystemHeapFree(old->assets);
    }
    guest.memory()->SystemHeapFree(to->renderable);
  }
  to->renderable = from->renderable;
  to->state = from->state;
  from->renderable = 0;
  from->state = static_cast<uint32_t>(AvatarRenderSlotState::kNoRenderable);
}

}  // namespace

// sub_920F8B30 / sub_920F81F0 over an allocation from sub_920FC228
uint32_t AllocateAvatarRenderSlot(const Guest& guest,
                                  uint32_t manifest_address) {
  const uint32_t address =
      guest.memory()->SystemHeapAlloc(kAvatarRenderSlotBytes);
  if (!address) {
    return 0;
  }

  AvatarRenderSlotMemory* slot = guest.At<AvatarRenderSlotMemory>(address);
  memset(slot, 0, kAvatarRenderSlotBytes);
  slot->create_flags = kAllManifestComponentsMask;
  slot->update_scale = 1.0f;
  slot->manifest_source = manifest_address;
  slot->state = static_cast<uint32_t>(AvatarRenderSlotState::kNoRenderable);
  return address;
}

// sub_920F90D0 on the renderer manager at 0x9426F1E0. The manager exists to
// serialise creations that were overlapped; ours finishes inside BeginCreate,
// so the queue has nothing to hold.
void QueueAvatarRendererRebuild(const Guest& guest, AvatarRenderSlot& slot) {
  (void)guest;
  slot.BeginCreate();
}

AvatarRenderSlot::AvatarRenderSlot(const Guest& guest, uint32_t address)
    : guest_(guest),
      address_(address),
      slot_(guest.At<AvatarRenderSlotMemory>(address)) {}

uint32_t AvatarRenderSlot::manifest_source() const {
  return slot_ ? static_cast<uint32_t>(slot_->manifest_source) : 0;
}

AvatarRenderSlotState AvatarRenderSlot::state() const {
  if (!slot_) {
    return AvatarRenderSlotState::kNoRenderable;
  }
  return static_cast<AvatarRenderSlotState>(
      static_cast<uint32_t>(slot_->state));
}

void AvatarRenderSlot::BindManifest(uint32_t manifest_address) {
  if (!slot_) {
    return;
  }
  slot_->manifest_source = manifest_address;
}

// sub_920F7B18
void AvatarRenderSlot::StoreManifest(uint32_t manifest_address) {
  const uint8_t* manifest = guest_.At<uint8_t>(manifest_address);
  if (!slot_ || !manifest) {
    return;
  }
  memcpy(slot_->metadata, manifest, kManifestBytes);
  slot_->manifest_source = manifest_address;
}

void AvatarRenderSlot::CopyFrom(const AvatarRenderSlot& other) {
  if (!slot_ || !other.slot_) {
    return;
  }
  memcpy(slot_->metadata, other.slot_->metadata, kManifestBytes);
  slot_->renderer_kind = other.slot_->renderer_kind;
  slot_->create_flags = other.slot_->create_flags;
  HandOverRenderable(guest_, other.address_, address_);
}

// sub_920F7940
void AvatarRenderSlot::BeginCreate() {
  if (!slot_ || slot_->renderable) {
    return;
  }
  if (!AvatarSystemIsReady(guest_)) {
    return;
  }

  uint32_t renderable = 0;
  const int32_t status =
      CreateRenderableThroughXam(guest_, *slot_, &renderable);
  slot_->renderable = renderable;
  slot_->state =
      static_cast<uint32_t>(status >= 0 ? AvatarRenderSlotState::kLoading
                                        : AvatarRenderSlotState::kFailed);
}

// sub_920F7A20
void AvatarRenderSlot::PollCreate(float delta_seconds) {
  if (!slot_ || !slot_->renderable) {
    return;
  }

  AdvanceRenderable(guest_, slot_->renderable,
                    float(slot_->update_scale) * delta_seconds);
  if (state() != AvatarRenderSlotState::kLoading) {
    return;
  }

  const int32_t status =
      UpdateRenderableLoadingThroughXam(guest_, slot_->renderable);
  if (status >= 0) {
    slot_->state = static_cast<uint32_t>(AvatarRenderSlotState::kReady);
  } else if (status != kPending) {
    // kFailed is terminal: no code in the image puts the slot back to work.
    slot_->state = static_cast<uint32_t>(AvatarRenderSlotState::kFailed);
  }
}

// sub_920F5890 -> sub_920F5430
void EditorApp::PumpAvatarRenderSlots() {
  const float delta = delta_seconds();

  AvatarRenderSlot edited = edited_avatar_slot();
  edited.BeginCreate();
  edited.PollCreate(delta);

  AvatarRenderSlot displayed = displayed_avatar_slot();
  displayed.BeginCreate();
  displayed.PollCreate(delta);
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
