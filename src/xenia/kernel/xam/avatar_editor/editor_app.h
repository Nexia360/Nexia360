/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_APP_H_
#define XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_APP_H_

#include <stddef.h>
#include <stdint.h>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xam/avatar_editor/avatar_editor.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

// The application object reaches the rest of .bss through addis/addi pairs, so
// these are the application's own fields at their real addresses, not
// unrelated globals.
constexpr uint32_t kMustVerifyUserAddress = 0x9231338C;
constexpr uint32_t kSigninHandoffPendingAddress = 0x92327334;
constexpr uint32_t kInitialScreenOpenedAddress = 0x92327335;
constexpr uint32_t kAssetPackageStateAddress = 0x9422A270;
constexpr uint32_t kManifestHistoryAddress = 0x942727A8;
constexpr uint32_t kRendererManagerAddress = 0x9426F1E0;
constexpr uint32_t kSigninWatcherAddress = 0x9428B358;
constexpr uint32_t kStartupSubsystemAddress = 0x9456F85C;
constexpr uint32_t kExitPushBlockerAddress = 0x94570288;
constexpr uint32_t kModalHandleAddress = 0x945706B8;
constexpr uint32_t kNuiTrackerPointerAddress = 0x945706F0;
constexpr uint32_t kIdleCountdownAddress = 0x94571590;
constexpr uint32_t kFlag94571598Address = 0x94571598;
constexpr uint32_t kFlag94571599Address = 0x94571599;
constexpr uint32_t kAvatarSystemReadyAddress = 0x945BFDA5;
constexpr uint32_t kNuiCursorAddress = 0x945C5CF0;

constexpr uint32_t kSigninWatcherSystemUiVisibleAddress =
    kSigninWatcherAddress + 4;
constexpr uint32_t kSigninWatcherUserConfirmedAddress =
    kSigninWatcherAddress + 6;
constexpr uint32_t kNuiCursorStateAddress = kNuiCursorAddress + 8;

constexpr uint32_t kManifestHistoryCommandFloatAddress =
    kManifestHistoryAddress + 4;
constexpr uint32_t kManifestHistoryCommandModeAddress =
    kManifestHistoryAddress + 0x2C;

constexpr uint32_t kLaunchMagicEpix = 0x45504958;
constexpr uint32_t kLaunchMagicCallerRelaunch = 0x43445831;
constexpr uint32_t kLaunchBlobBytes = 0x10C;

constexpr uint32_t kNavCommandBack = 1;
constexpr uint32_t kNavCommandCreateFirstAvatar = 3;
constexpr uint32_t kNavCommandExitRequested = 0x4C;
constexpr uint32_t kNavCommand4E = 0x4E;
constexpr uint32_t kNavCommand4F = 0x4F;
constexpr uint32_t kNavCommandNewContent = 0x53;

constexpr float kIdleCountdownResetSeconds = 3.0f;
constexpr float kIdleCountdownAfterCommandSeconds = 0.75f;
constexpr float kIdleCountdownAfterFlaggedCommandSeconds = 0.25f;
constexpr float kManifestHistoryCommandFloat = 1.2f;

constexpr int32_t kPending = static_cast<int32_t>(0x8000000A);

#pragma pack(push, 1)

struct EditorAppMemory {
  uint8_t is_started;
  uint8_t user_has_avatar;
  uint8_t field_002[2];
  xe::be<uint32_t> launch_magic;
  xe::be<uint32_t> launch_user_index;
  uint8_t field_00C[4];
  uint8_t launch_action_or_image_path[0x100];
  xe::be<uint32_t> signed_in_user;
  xe::be<uint32_t> performance_tier;
  xe::be<uint32_t> body_is_second_default;
  uint8_t field_11C[0x140 - 0x11C];
  // The item the collection-ready follow-up clears, as UTF-16BE.
  uint8_t pending_item_name[0x100];
  xe::be<int32_t> pending_item_index;
  xe::be<float> command_started_seconds;
  xe::be<uint64_t> counter_now;
  xe::be<uint64_t> counter_delta;
  xe::be<float> seconds_now;
  xe::be<float> delta_seconds;
  xe::be<float> counter_to_seconds;
  uint8_t field_264[4];
  uint8_t is_exiting;
  uint8_t field_269[3];
  xe::be<float> transition_seconds;
  xe::be<float> fade;
  uint8_t field_274;
  // Set once the exit command has been held long enough; until then the exit
  // push in sub_920C5458 is refused.
  uint8_t exit_push_allowed;
  uint8_t field_276[2];
  xe::be<int32_t> field_278;
  xe::be<float> exit_hold_seconds;
  uint8_t field_280[0x3BC - 0x280];
  uint8_t original_metadata[kManifestBytes];
  uint8_t edited_manifest[kManifestBytes];
  uint8_t displayed_manifest[kManifestBytes];
  uint8_t default_manifest_first_body[kManifestBytes];
  uint8_t default_manifest_second_body[kManifestBytes];
  xe::be<uint32_t> edited_avatar_slot;
  xe::be<uint32_t> displayed_avatar_slot;
  uint8_t field_174C[0x1770 - 0x174C];
  uint8_t collection_build_started;
};

struct AvatarRenderSlotMemory {
  xe::be<uint32_t> vtable;
  xe::be<uint32_t> reference_count;
  xe::be<uint32_t> renderable;
  uint8_t metadata[kManifestBytes];
  xe::be<uint16_t> renderer_kind;
  uint8_t field_3F6[2];
  xe::be<uint32_t> create_flags;
  xe::be<float> update_scale;
  xe::be<uint32_t> state;
  xe::be<uint32_t> manifest_source;
};

#pragma pack(pop)

static_assert(offsetof(EditorAppMemory, is_started) == 0x000, "");
static_assert(offsetof(EditorAppMemory, user_has_avatar) == 0x001, "");
static_assert(offsetof(EditorAppMemory, launch_magic) == 0x004, "");
static_assert(offsetof(EditorAppMemory, launch_user_index) == 0x008, "");
static_assert(offsetof(EditorAppMemory, launch_action_or_image_path) == 0x010,
              "");
static_assert(offsetof(EditorAppMemory, signed_in_user) == 0x110, "");
static_assert(offsetof(EditorAppMemory, performance_tier) == 0x114, "");
static_assert(offsetof(EditorAppMemory, body_is_second_default) == 0x118, "");
static_assert(offsetof(EditorAppMemory, command_started_seconds) == 0x244, "");
static_assert(offsetof(EditorAppMemory, counter_now) == 0x248, "");
static_assert(offsetof(EditorAppMemory, counter_delta) == 0x250, "");
static_assert(offsetof(EditorAppMemory, seconds_now) == 0x258, "");
static_assert(offsetof(EditorAppMemory, delta_seconds) == 0x25C, "");
static_assert(offsetof(EditorAppMemory, counter_to_seconds) == 0x260, "");
static_assert(offsetof(EditorAppMemory, is_exiting) == 0x268, "");
static_assert(offsetof(EditorAppMemory, transition_seconds) == 0x26C, "");
static_assert(offsetof(EditorAppMemory, fade) == 0x270, "");
static_assert(offsetof(EditorAppMemory, field_274) == 0x274, "");
static_assert(offsetof(EditorAppMemory, exit_push_allowed) == 0x275, "");
static_assert(offsetof(EditorAppMemory, exit_hold_seconds) == 0x27C, "");
static_assert(offsetof(EditorAppMemory, pending_item_name) == 0x140, "");
static_assert(offsetof(EditorAppMemory, pending_item_index) == 0x240, "");
static_assert(offsetof(EditorAppMemory, original_metadata) == 0x3BC, "");
static_assert(offsetof(EditorAppMemory, edited_manifest) == 0x7A4, "");
static_assert(offsetof(EditorAppMemory, displayed_manifest) == 0xB8C, "");
static_assert(offsetof(EditorAppMemory, default_manifest_first_body) == 0xF74,
              "");
static_assert(offsetof(EditorAppMemory, default_manifest_second_body) == 0x135C,
              "");
static_assert(offsetof(EditorAppMemory, edited_avatar_slot) == 0x1744, "");
static_assert(offsetof(EditorAppMemory, displayed_avatar_slot) == 0x1748, "");
static_assert(offsetof(EditorAppMemory, collection_build_started) == 0x1770,
              "");
static_assert(kApplicationAddress +
                      offsetof(EditorAppMemory, edited_manifest) ==
                  0x92327AE4,
              "");
static_assert(kApplicationAddress +
                      offsetof(EditorAppMemory, collection_build_started) ==
                  0x92328AB0,
              "");
static_assert(kApplicationAddress + 0x1774 == kNavigationAddress, "");

static_assert(offsetof(AvatarRenderSlotMemory, vtable) == 0x000, "");
static_assert(offsetof(AvatarRenderSlotMemory, reference_count) == 0x004, "");
static_assert(offsetof(AvatarRenderSlotMemory, renderable) == 0x008, "");
static_assert(offsetof(AvatarRenderSlotMemory, metadata) == 0x00C, "");
static_assert(offsetof(AvatarRenderSlotMemory, renderer_kind) == 0x3F4, "");
static_assert(offsetof(AvatarRenderSlotMemory, create_flags) == 0x3F8, "");
static_assert(offsetof(AvatarRenderSlotMemory, update_scale) == 0x3FC, "");
static_assert(offsetof(AvatarRenderSlotMemory, state) == 0x400, "");
static_assert(offsetof(AvatarRenderSlotMemory, manifest_source) == 0x404, "");
static_assert(sizeof(AvatarRenderSlotMemory) == 0x408, "");

constexpr uint32_t kAvatarRenderSlotBytes = 0x464;

enum class AvatarRenderSlotState : uint32_t {
  kNoRenderable = 0,
  kLoading = 1,
  kReady = 2,
  kFailed = 3,
};

// One of the editor's two avatar renderers. It owns a copy of the manifest it
// was built from and drives the creation machine whose terminal failure state
// is kFailed: nothing in the image ever clears it.
class AvatarRenderSlot {
 public:
  AvatarRenderSlot(const Guest& guest, uint32_t address);

  bool exists() const { return slot_ != nullptr; }
  uint32_t address() const { return address_; }
  uint32_t manifest_source() const;

  AvatarRenderSlotState state() const;
  bool is_ready() const { return state() == AvatarRenderSlotState::kReady; }

  void BindManifest(uint32_t manifest_address);
  void StoreManifest(uint32_t manifest_address);
  void CopyFrom(const AvatarRenderSlot& other);

  void BeginCreate();
  void PollCreate(float delta_seconds);

 private:
  Guest guest_;
  uint32_t address_;
  AvatarRenderSlotMemory* slot_;
};

// sub_920F8B30 / sub_920F81F0: a 0x464-byte slot bound to one of the
// application's manifest buffers.
uint32_t AllocateAvatarRenderSlot(const Guest& guest,
                                  uint32_t manifest_address);

// sub_920F90D0: hands the slot to the renderer manager at 0x9426F1E0, which
// starts the next pending creation.
void QueueAvatarRendererRebuild(const Guest& guest, AvatarRenderSlot& slot);

// Implemented by the other sections of the editor. The application calls into
// them; it never reaches into their objects itself.
class EditorSections {
 public:
  virtual ~EditorSections() = default;

  virtual uint32_t CurrentNavigationCommand() = 0;
  virtual void PostNavigationCommand(uint32_t command) = 0;
  virtual void TickNavigation() = 0;
  virtual void AdvanceNavigationAfterInput() = 0;
  virtual void DispatchNavigationActionName(const char* action_name) = 0;

  virtual void PopulateSceneRegistry() = 0;

  virtual void StartComponentCollectionBuild() = 0;
  virtual void TickComponentCollection() = 0;
  virtual bool IsComponentCollectionReady() = 0;
};

class EditorApp {
 public:
  EditorApp(Memory* memory, EditorSections* sections);

  void Construct();
  void Start();
  void Tick();

  void NotifyCommand(uint32_t command);
  void AdoptManifest(uint32_t metadata_address);
  void PushManifestToRenderer(uint32_t manifest_address, bool force);
  void AdoptRandomAvatarIfNone();
  void OpenInitialScreen();

  bool is_started() const;
  float delta_seconds() const;
  uint32_t signed_in_user() const;

  AvatarRenderSlot edited_avatar_slot() const;
  AvatarRenderSlot displayed_avatar_slot() const;
  void PumpAvatarRenderSlots();

  const Guest& guest() const { return guest_; }

 private:
  EditorAppMemory* app() const {
    return guest_.At<EditorAppMemory>(kApplicationAddress);
  }

  void UpdateFrameClock();
  void UpdateFade();
  void RunStartupHandoff();
  void StartComponentCollectionBuildWhenReady();
  void RunIdleCountdown();
  void RunNuiCursorInput();
  void TickExit();
  void RunExitHold(uint32_t command);
  void LeaveEditor();
  void OnCollectionReady();
  void RunCollectionReadyFollowUp(uint32_t command);

  Guest guest_;
  // Null only for the manifest helpers, which never reach a section.
  EditorSections* sections_;
};

// sub_9221B6B8. Non-zero while the Kinect cursor has a target; the tick
// compares it either side of an update to notice the target changing.
uint32_t NuiCursorState(const Guest& guest);

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_APP_H_
