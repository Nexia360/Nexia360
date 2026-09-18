/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_editor/editor_app.h"

#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/avatar_editor/asset_tile.h"
#include "xenia/kernel/xam/xam_avatar_assets.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xna/xna_avatar.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

namespace {

// 0x92005AE0: how long a command has to have been up before the collection's
// ready frame will follow it up.
constexpr float kCollectionReadyDelaySeconds = 2.0f;
// 0x92000F94: the exit command's hold, and the countdown value it pins while
// the hold runs.
constexpr float kExitHoldSeconds = 1.0f;

constexpr uint32_t kFlag942569ACAddress = 0x942569AC;
constexpr uint32_t kExitPushBlockedValue = 0x3E5;

// The asset package's state word, and the value sub_920BDE08 puts there when
// the edited avatar cannot be written back.
constexpr uint32_t kAssetPackageStateLimit = 11;
constexpr uint32_t kAssetPackageStateSaveRefused = 0xB;

// 0x9428B380, the object sub_920C7400 pumps: an 8 here says the editor lost
// the user it started with.
constexpr uint32_t kSigninLostSignalAddress = 0x9428B380;
constexpr uint32_t kSigninLostSignal = 8;

constexpr uint32_t kSigninWatcherXuidAddress = kSigninWatcherAddress + 8;

// 0x945BE530: where the exit reports whether the avatar was edited.
constexpr uint32_t kEditResultAddress = 0x945BE530;

constexpr uint32_t kManifestHistoryCursorAddress =
    kManifestHistoryAddress + 4 + 0x30;
constexpr uint32_t kManifestHistoryEntriesOffset = 0x49;
constexpr uint32_t kManifestHistoryEntryBytes = kManifestBytes + 1;
constexpr uint32_t kManifestHistorySlotCount = 0x54;

// The application's default camera block, seeded from .rdata at construction.
constexpr uint32_t kDefaultCameraOffset = 0x390;
// 0x92006BE0: field of view, aspect, near plane, far plane.
constexpr float kDefaultCameraProjection[4] = {0.78539819f, 1.77777779f, 0.01f,
                                               200.0f};

// 0x92000BB0, the body the second default manifest is built from.
constexpr uint8_t kSecondDefaultBodyAssetId[kAssetIdBytes] = {
    0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02,
    0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0};
constexpr uint32_t kManifestBodyAssetIdOffset = 0x120;

constexpr uint32_t kNavCommandKeepsCameraFirst = 33;
constexpr uint32_t kNavCommandKeepsCameraLast = 35;

// XAvatarInitialize's second argument: the editor renders left-handed.
constexpr uint32_t kAvatarCoordinateSystem = 1;

constexpr int32_t kWireBodyFemale = 0;
constexpr int32_t kWireBodyMale = 1;
constexpr int32_t kWireBodyAny = -1;

float LoadFloat(const Guest& guest, uint32_t address) {
  const uint32_t bits = guest.Load32(address);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

void StoreFloat(const Guest& guest, uint32_t address, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  guest.Store32(address, bits);
}

float Clamp01(float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  return value > 1.0f ? 1.0f : value;
}

void NarrowLaunchActionName(const uint8_t* wide_name, char* out_name,
                            size_t capacity) {
  size_t written = 0;
  while (written + 1 < capacity) {
    const uint16_t character = static_cast<uint16_t>(
        (wide_name[written * 2] << 8) | wide_name[written * 2 + 1]);
    if (!character) {
      break;
    }
    out_name[written] = character < 0x100 ? static_cast<char>(character) : '?';
    ++written;
  }
  out_name[written] = '\0';
}

uint32_t ManifestHistoryEntry(uint32_t slot) {
  return kManifestHistoryAddress + kManifestHistoryEntriesOffset +
         slot * kManifestHistoryEntryBytes;
}

// sub_920C7588
void ResetManifestHistory(const Guest& guest) {
  for (uint32_t slot = 0; slot < kManifestHistorySlotCount; ++slot) {
    guest.Store8(ManifestHistoryEntry(slot), 0);
  }
  guest.Store32(kManifestHistoryCursorAddress, uint32_t(-1));
  guest.Store32(kManifestHistoryCommandModeAddress, 0);
}

// sub_920C43A0, the .rdata camera at 0x92006BE0
void SeedDefaultCamera(const Guest& guest) {
  for (uint32_t i = 0; i < 4; ++i) {
    StoreFloat(guest, kApplicationAddress + kDefaultCameraOffset + i * 4,
               kDefaultCameraProjection[i]);
  }
}

XamModule* LoaderModule() {
  auto* kernel = KernelState::shared();
  if (!kernel) {
    return nullptr;
  }
  auto module = kernel->GetKernelModule<XamModule>("xam.xex");
  return module ? module.get() : nullptr;
}

// XamLoaderGetLaunchData
void ReadLaunchBlob(const Guest& guest, uint32_t blob_address) {
  uint8_t* blob = guest.At<uint8_t>(blob_address);
  if (!blob) {
    return;
  }
  memset(blob, 0, kLaunchBlobBytes);

  XamModule* loader = LoaderModule();
  std::vector<uint8_t> data;
  if (loader) {
    data = loader->loader_data().launch_data;
  }
  if (data.empty()) {
    // Opened without a title asking for an item, which is the case the real
    // launcher covers with an empty item string.
    data = BuildAvatarEditorLaunchData(std::u16string());
  }
  memcpy(blob, data.data(),
         std::min<size_t>(data.size(), size_t(kLaunchBlobBytes)));
}

// XamLoaderSetLaunchData
void StoreLaunchBlobForCaller(const Guest& guest, uint32_t blob_address) {
  const uint8_t* blob = guest.At<uint8_t>(blob_address);
  XamModule* loader = LoaderModule();
  if (!blob || !loader) {
    return;
  }
  loader->loader_data().launch_data.assign(blob, blob + kLaunchBlobBytes);
}

// XAvatarInitialize at 0x92141990, which calls XamAvatarInitialize. Nothing
// the slots do works until the byte at 0x945BFDA5 is set here.
bool InitializeAvatarSystem(const Guest& guest) {
  SetAvatarCoordinateSystem(kAvatarCoordinateSystem);
  if (!xe::kernel::xna::XnaAvatarCatalog()) {
    XELOGE(
        "avatar_editor: the Avatar update is not installed, so nothing can be "
        "rendered or enumerated");
    return false;
  }
  guest.Store8(kAvatarSystemReadyAddress, 1);
  return true;
}

// sub_920C47C0, Avatars::ManifestReaderWriter
void BuildDefaultManifests(const Guest& guest, uint32_t first_body_manifest,
                           uint32_t second_body_manifest) {
  const auto first = xe::kernel::xna::XnaAvatarRandomManifest(kWireBodyMale);
  const auto second = xe::kernel::xna::XnaAvatarRandomManifest(kWireBodyFemale);
  uint8_t* first_out = guest.At<uint8_t>(first_body_manifest);
  uint8_t* second_out = guest.At<uint8_t>(second_body_manifest);
  if (first_out) {
    memcpy(first_out, first.data(), kManifestBytes);
  }
  if (second_out) {
    memcpy(second_out, second.data(), kManifestBytes);
  }
}

// XAvatarGetMetadataLocalUser
bool FetchLocalUserAvatarMetadata(const Guest& guest, uint32_t user_index,
                                  uint32_t metadata_address) {
  uint8_t* out = guest.At<uint8_t>(metadata_address);
  auto* xam_state =
      KernelState::shared() ? KernelState::shared()->xam_state() : nullptr;
  if (!out || !xam_state) {
    return false;
  }
  UserProfile* profile = xam_state->GetUserProfile(user_index);
  if (!profile) {
    return false;
  }

  const auto manifest =
      xe::kernel::xna::XnaAvatarManifestForXuid(profile->xuid());
  memcpy(out, manifest.data(), kManifestBytes);

  // A profile with no avatar still hands back a manifest; an all-zero one is
  // how that reads, and it is the answer this returns false for.
  for (uint32_t i = 0; i < kManifestBytes; ++i) {
    if (manifest[i]) {
      return true;
    }
  }
  return false;
}

// XAvatarGetMetadataRandom(3, 1, metadata, 0)
bool FetchRandomAvatarMetadata(const Guest& guest, uint32_t metadata_address) {
  uint8_t* out = guest.At<uint8_t>(metadata_address);
  if (!out) {
    return false;
  }
  const auto manifest = xe::kernel::xna::XnaAvatarRandomManifest(kWireBodyAny);
  memcpy(out, manifest.data(), kManifestBytes);
  return true;
}

// sub_920C26D8, against the body component id at 0x92000BB0
bool BodyComponentMatchesSecondDefault(const uint8_t* manifest) {
  return memcmp(manifest + kManifestBodyAssetIdOffset,
                kSecondDefaultBodyAssetId, kAssetIdBytes) == 0;
}

// sub_920F0E80 on the watcher at 0x9428B358
void PumpSigninWatcher(const Guest& guest, uint32_t user_index) {
  auto* xam_state =
      KernelState::shared() ? KernelState::shared()->xam_state() : nullptr;

  uint64_t xuid = 0;
  if (xam_state && xam_state->IsUserSignedIn(user_index)) {
    UserProfile* profile = xam_state->GetUserProfile(user_index);
    xuid = profile ? profile->xuid() : 0;
  }

  auto* recorded = guest.At<xe::be<uint64_t>>(kSigninWatcherXuidAddress);
  if (recorded && !uint64_t(*recorded)) {
    *recorded = xuid;
  }

  const bool same_user = recorded && xuid && uint64_t(*recorded) == xuid;
  guest.Store8(kSigninWatcherUserConfirmedAddress, same_user ? 1 : 0);
  guest.Store8(kSigninWatcherSystemUiVisibleAddress,
               xam_state && xam_state->IsUIActive() ? 1 : 0);
}

// XamUserGetSigninState
bool UserIsSignedIn(uint32_t user_index) {
  auto* xam_state =
      KernelState::shared() ? KernelState::shared()->xam_state() : nullptr;
  return xam_state && xam_state->IsUserSignedIn(user_index);
}

// sub_920C7D00
void SnapshotManifestHistory(const Guest& guest, uint32_t command) {
  guest.Store32(kManifestHistoryCursorAddress, uint32_t(-1));
  if (command >= kManifestHistorySlotCount) {
    return;
  }

  const uint32_t entry = ManifestHistoryEntry(command);
  guest.Store8(entry, 0);

  // The original asks the command's scene whether it edits the avatar; a
  // command with no scene registered has nothing to ask and nothing to record.
  if (!guest.Load32(kSceneRegistryAddress + command * 4)) {
    return;
  }

  const uint8_t* edited = guest.At<uint8_t>(
      kApplicationAddress + offsetof(EditorAppMemory, edited_manifest));
  uint8_t* stored = guest.At<uint8_t>(entry + 1);
  if (!edited || !stored) {
    return;
  }
  memcpy(stored, edited, kManifestBytes);
  guest.Store8(entry, 1);
}

// sub_920C4038
void HandleExitCommand(const Guest& guest, uint32_t user_index) {
  // A signed-in profile goes on to the upload arm, which needs a Live
  // connection this port does not carry; without one the original drops the
  // asset package into its refused state instead.
  if (UserIsSignedIn(user_index)) {
    return;
  }
  guest.Store32(kAssetPackageStateAddress, kAssetPackageStateSaveRefused);
  guest.Store8(kFlag942569ACAddress, 0);
}

// sub_920C34A0
void ReportSigninLoss(const Guest& guest) {
  if (guest.Load8(kSigninWatcherUserConfirmedAddress) ||
      guest.Load8(kSigninWatcherSystemUiVisibleAddress)) {
    return;
  }
  if (guest.Load8(kApplicationAddress +
                  offsetof(EditorAppMemory, is_exiting))) {
    return;
  }
  guest.Store32(kSigninLostSignalAddress, kSigninLostSignal);
}

// sub_920C3678
bool NavigationAdvanceIsBlocked(const Guest& guest, uint32_t command) {
  return LoadFloat(guest, kIdleCountdownAddress) > 0.0f ||
         command == kNavCommandExitRequested || command == kNavCommandBack;
}

// sub_920BE978 on the asset package state at 0x9422A270
uint32_t AssetPackageState(const Guest& guest) {
  return guest.Load32(kAssetPackageStateAddress);
}

// sub_920C3918, the report to 0x945BE530
void ReportEditResult(const Guest& guest, bool manifest_changed) {
  guest.Store32(kEditResultAddress, manifest_changed ? 1 : 0);
}

}  // namespace

namespace {

AvatarComponentInfo* ManifestComponentAt(const Guest& guest,
                                         uint32_t manifest_address,
                                         uint32_t index) {
  uint8_t* manifest = guest.At<uint8_t>(manifest_address);
  if (!manifest || index >= kManifestComponentCount) {
    return nullptr;
  }
  return reinterpret_cast<AvatarComponentInfo*>(
      manifest + kManifestComponentsOffset + index * kManifestEntryBytes);
}

}  // namespace

// sub_920C7C40, the manifest a tile writes while it is being browsed
void ApplyManifestSnapshot(const Guest& guest, const uint8_t* manifest) {
  uint8_t* edited = guest.At<uint8_t>(
      kApplicationAddress + offsetof(EditorAppMemory, edited_manifest));
  if (!edited || !manifest) {
    return;
  }
  memcpy(edited, manifest, kManifestBytes);
}

// sub_920C31A0 through the application
void CommitManifestSnapshot(const Guest& guest, const uint8_t* manifest,
                            uint32_t flags) {
  ApplyManifestSnapshot(guest, manifest);
  EditorApp(guest.memory(), nullptr)
      .PushManifestToRenderer(
          kApplicationAddress + offsetof(EditorAppMemory, edited_manifest),
          flags != 0);
}

// sub_920C8B78: every entry of that type becomes an unworn slot.
void RemoveManifestComponents(const Guest& guest, uint32_t manifest_address,
                              uint16_t component_type) {
  for (uint32_t i = 0; i < kManifestComponentCount; ++i) {
    AvatarComponentInfo* entry =
        ManifestComponentAt(guest, manifest_address, i);
    if (entry && static_cast<uint16_t>(entry->type) == component_type) {
      memset(entry, 0, sizeof(*entry));
    }
  }
}

// sub_920C8C58, through the one writer. A caller holding nothing but a
// _XAVATAR_COMPONENT_INFO has only its sixteen-bit type to route on, which is
// the whole kind for everything the component array can hold.
void SetManifestComponentInfo(const Guest& guest, uint32_t manifest_address,
                              uint32_t component_info_address) {
  const auto* info = guest.At<AvatarComponentInfo>(component_info_address);
  if (!info) {
    return;
  }
  WriteManifestItem(guest, manifest_address, *info,
                    static_cast<uint16_t>(info->type));
}

// sub_9221B6B8
uint32_t NuiCursorState(const Guest& guest) {
  return guest.Load32(kNuiCursorStateAddress);
}

EditorApp::EditorApp(Memory* memory, EditorSections* sections)
    : guest_(memory), sections_(sections) {}

bool EditorApp::is_started() const { return app()->is_started != 0; }

float EditorApp::delta_seconds() const { return app()->delta_seconds; }

uint32_t EditorApp::signed_in_user() const { return app()->signed_in_user; }

AvatarRenderSlot EditorApp::edited_avatar_slot() const {
  return AvatarRenderSlot(guest_, app()->edited_avatar_slot);
}

AvatarRenderSlot EditorApp::displayed_avatar_slot() const {
  return AvatarRenderSlot(guest_, app()->displayed_avatar_slot);
}

// sub_920C43A0
void EditorApp::Construct() {
  EditorAppMemory* app = this->app();
  memset(app, 0, sizeof(*app));

  ResetManifestHistory(guest_);
  SeedDefaultCamera(guest_);

  app->edited_avatar_slot = AllocateAvatarRenderSlot(
      guest_, kApplicationAddress + offsetof(EditorAppMemory, edited_manifest));
  app->displayed_avatar_slot = AllocateAvatarRenderSlot(
      guest_,
      kApplicationAddress + offsetof(EditorAppMemory, displayed_manifest));

  const uint32_t blob_address =
      kApplicationAddress + offsetof(EditorAppMemory, launch_magic);
  ReadLaunchBlob(guest_, blob_address);

  const uint32_t magic = app->launch_magic;
  if (magic == kLaunchMagicEpix) {
    app->signed_in_user = app->launch_user_index;
  } else if (magic == kLaunchMagicCallerRelaunch) {
    app->signed_in_user = 0;
  } else {
    memset(&app->launch_magic, 0, kLaunchBlobBytes);
  }
}

// sub_920C47C0
void EditorApp::Start() {
  EditorAppMemory* app = this->app();

  // The original takes the D3D device out of the graphics context at
  // 0x92326DF4 and hands it to XAvatarInitialize. Nothing fills that pointer
  // here - the host presenter draws what the editor holds - so reading it
  // would abort start-up before it ever set is_started.
  if (!InitializeAvatarSystem(guest_)) {
    return;
  }

  sections_->PopulateSceneRegistry();
  BuildDefaultManifests(
      guest_,
      kApplicationAddress +
          offsetof(EditorAppMemory, default_manifest_first_body),
      kApplicationAddress +
          offsetof(EditorAppMemory, default_manifest_second_body));

  const uint32_t original_metadata =
      kApplicationAddress + offsetof(EditorAppMemory, original_metadata);
  app->user_has_avatar = FetchLocalUserAvatarMetadata(
                             guest_, app->signed_in_user, original_metadata)
                             ? 1
                             : 0;
  if (app->user_has_avatar) {
    AdoptManifest(original_metadata);
    PushManifestToRenderer(
        kApplicationAddress + offsetof(EditorAppMemory, edited_manifest),
        false);
  }

  // Every failure above returns before this store, which leaves the tick inert
  // rather than degraded.
  app->is_started = 1;
}

// sub_920C6F00
void EditorApp::Tick() {
  EditorAppMemory* app = this->app();

  UpdateFrameClock();
  UpdateFade();
  PumpSigninWatcher(guest_, app->signed_in_user);

  if (!app->is_started || !sections_) {
    return;
  }

  RunStartupHandoff();
  StartComponentCollectionBuildWhenReady();

  sections_->TickNavigation();
  sections_->TickComponentCollection();
  PumpAvatarRenderSlots();

  if (sections_->IsComponentCollectionReady()) {
    OnCollectionReady();
  }

  const uint32_t command = sections_->CurrentNavigationCommand();
  ReportSigninLoss(guest_);
  TickExit();
  RunExitHold(command);
  RunIdleCountdown();

  if (!NuiCursorState(guest_) && !NavigationAdvanceIsBlocked(guest_, command) &&
      !app->is_exiting) {
    sections_->AdvanceNavigationAfterInput();
  }

  RunNuiCursorInput();
}

// sub_920C2488
void EditorApp::UpdateFrameClock() {
  EditorAppMemory* app = this->app();

  const uint64_t counter = Clock::QueryGuestTickCount();
  const uint64_t previous = app->counter_now;
  const uint64_t delta = previous ? counter - previous : 0;
  const float seconds_per_tick =
      1.0f / static_cast<float>(Clock::guest_tick_frequency());

  app->counter_now = counter;
  app->counter_delta = delta;
  app->counter_to_seconds = seconds_per_tick;
  app->delta_seconds = static_cast<float>(delta) * seconds_per_tick;
  app->seconds_now = float(app->seconds_now) + float(app->delta_seconds);
}

// sub_920C2F00
void EditorApp::UpdateFade() {
  EditorAppMemory* app = this->app();

  if (app->is_exiting) {
    float remaining =
        float(app->transition_seconds) - float(app->delta_seconds);
    if (remaining < 0.0f) {
      remaining = 0.0f;
    }
    app->transition_seconds = remaining;
    app->fade = Clamp01(1.0f - remaining);
    return;
  }

  app->fade = Clamp01(float(app->fade) - float(app->delta_seconds));
}

// sub_920C6F00, the three-frame start-up hand-off
void EditorApp::RunStartupHandoff() {
  EditorAppMemory* app = this->app();

  if (guest_.Load8(kMustVerifyUserAddress)) {
    if (UserIsSignedIn(app->signed_in_user) &&
        guest_.Load8(kSigninWatcherUserConfirmedAddress) &&
        !guest_.Load8(kSigninWatcherSystemUiVisibleAddress)) {
      AdoptRandomAvatarIfNone();
      guest_.Store8(kMustVerifyUserAddress, 0);
      guest_.Store8(kSigninHandoffPendingAddress, 1);
    }
    return;
  }

  if (guest_.Load8(kSigninHandoffPendingAddress)) {
    if (guest_.Load32(kModalHandleAddress) == 0) {
      guest_.Store8(kSigninHandoffPendingAddress, 0);
    }
    return;
  }

  if (!guest_.Load8(kInitialScreenOpenedAddress)) {
    OpenInitialScreen();
    guest_.Store8(kInitialScreenOpenedAddress, 1);
  }
}

// sub_920C6F00, the build gate
void EditorApp::StartComponentCollectionBuildWhenReady() {
  EditorAppMemory* app = this->app();

  if (app->collection_build_started) {
    return;
  }
  if (!guest_.Load8(kInitialScreenOpenedAddress)) {
    return;
  }
  if (!edited_avatar_slot().is_ready() && !displayed_avatar_slot().is_ready()) {
    return;
  }

  app->collection_build_started = 1;
  sections_->StartComponentCollectionBuild();
}

// sub_920C6F00, the countdown at 0x94571590
void EditorApp::RunIdleCountdown() {
  const float remaining =
      LoadFloat(guest_, kIdleCountdownAddress) - delta_seconds();
  StoreFloat(guest_, kIdleCountdownAddress,
             remaining < 0.0f ? 0.0f : remaining);
}

// sub_920C6F00, the NUI cursor arm. Nexia has no Kinect, so the tracker
// answers 0x83010001 - one of the three codes the original tolerates, after
// which it leaves the cursor and the idle countdown exactly as they were.
void EditorApp::RunNuiCursorInput() {}

// sub_920C5458
void EditorApp::TickExit() {
  EditorAppMemory* app = this->app();

  const uint32_t command = sections_->CurrentNavigationCommand();
  if (command == kNavCommandExitRequested &&
      guest_.Load32(kModalHandleAddress) == 0) {
    guest_.Store8(kFlag94571599Address, 1);
    HandleExitCommand(guest_, app->signed_in_user);
  }

  if (guest_.Load8(kFlag94571599Address) && app->exit_push_allowed &&
      guest_.Load32(kExitPushBlockerAddress) != kExitPushBlockedValue &&
      float(app->transition_seconds) <= 0.0f) {
    sections_->PostNavigationCommand(kNavCommandBack);
  }

  if (app->is_exiting && float(app->transition_seconds) <= 0.0f) {
    LeaveEditor();
  }
}

// sub_920C2F78
void EditorApp::RunExitHold(uint32_t command) {
  EditorAppMemory* app = this->app();

  if (command != kNavCommandExitRequested || app->exit_push_allowed) {
    return;
  }

  const float held = float(app->exit_hold_seconds) + float(app->delta_seconds);
  app->exit_hold_seconds = held;
  StoreFloat(guest_, kIdleCountdownAddress, kExitHoldSeconds);
  if (held > kExitHoldSeconds) {
    // Once the hold completes the original asks the save dialog whether it is
    // still up and only latches when it is not; this port never raises one.
    app->exit_push_allowed = 1;
  }
}

// sub_920C3918
void EditorApp::LeaveEditor() {
  EditorAppMemory* app = this->app();

  ReportEditResult(guest_, memcmp(app->original_metadata, app->edited_manifest,
                                  kManifestBytes) != 0);

  if (app->launch_magic == kLaunchMagicCallerRelaunch &&
      app->launch_action_or_image_path[0] != 0) {
    StoreLaunchBlobForCaller(
        guest_, kApplicationAddress + offsetof(EditorAppMemory, launch_magic));
  }

  // The original launches its way out, either back into the caller named in
  // the blob or into the dashboard. Here the editor is an overlay over a title
  // that never stopped running, so leaving is going inert and letting the host
  // take the session down.
  app->is_started = 0;
}

// sub_920C6370
void EditorApp::OnCollectionReady() {
  EditorAppMemory* app = this->app();

  const uint32_t command = sections_->CurrentNavigationCommand();
  const uint32_t package_state = AssetPackageState(guest_);
  bool follow_up = false;

  if (command == kNavCommandNewContent) {
    follow_up = package_state != 0;
  } else if (command == kNavCommand4E) {
    const float since_command =
        float(app->seconds_now) - float(app->command_started_seconds);
    follow_up = package_state < kAssetPackageStateLimit &&
                since_command > kCollectionReadyDelaySeconds &&
                !guest_.Load8(kFlag942569ACAddress);
  } else if (command == kNavCommand4F) {
    follow_up = package_state < kAssetPackageStateLimit;
  }

  if (follow_up) {
    RunCollectionReadyFollowUp(command);
  }
}

// sub_920C36E8
void EditorApp::RunCollectionReadyFollowUp(uint32_t command) {
  EditorAppMemory* app = this->app();

  if (!command) {
    return;
  }

  // The original also resets the overlay's position, which belongs to the
  // navigation section.
  memset(app->pending_item_name, 0, sizeof(app->pending_item_name));
  app->pending_item_index = 0;
  app->command_started_seconds = 0.0f;
}

// sub_920C4208
void EditorApp::AdoptRandomAvatarIfNone() {
  EditorAppMemory* app = this->app();

  if (!app->user_has_avatar) {
    // The random avatar lands in original_metadata so that leaving without an
    // edit still reports "unchanged" against what the editor started from.
    const uint32_t metadata_address =
        kApplicationAddress + offsetof(EditorAppMemory, original_metadata);
    if (FetchRandomAvatarMetadata(guest_, metadata_address)) {
      AdoptManifest(metadata_address);
      PushManifestToRenderer(
          kApplicationAddress + offsetof(EditorAppMemory, edited_manifest),
          false);
    }
  }
}

// sub_920C2DB8
void EditorApp::OpenInitialScreen() {
  EditorAppMemory* app = this->app();

  if (!app->user_has_avatar) {
    sections_->PostNavigationCommand(kNavCommandCreateFirstAvatar);
    return;
  }

  if (app->launch_magic != kLaunchMagicEpix) {
    return;
  }

  char action_name[0x80];
  NarrowLaunchActionName(app->launch_action_or_image_path, action_name,
                         sizeof(action_name));
  sections_->DispatchNavigationActionName(action_name);
}

// sub_920C26D8
void EditorApp::AdoptManifest(uint32_t metadata_address) {
  EditorAppMemory* app = this->app();

  const uint8_t* metadata = guest_.At<uint8_t>(metadata_address);
  if (!metadata) {
    return;
  }

  memcpy(app->edited_manifest, metadata, kManifestBytes);
  app->body_is_second_default =
      BodyComponentMatchesSecondDefault(app->edited_manifest) ? 1 : 0;
}

// sub_920C31A0
void EditorApp::PushManifestToRenderer(uint32_t manifest_address, bool force) {
  EditorAppMemory* app = this->app();

  const uint8_t* manifest = guest_.At<uint8_t>(manifest_address);
  if (!manifest) {
    return;
  }

  AvatarRenderSlot edited = edited_avatar_slot();
  if (!force &&
      memcmp(manifest, app->displayed_manifest, kManifestBytes) == 0) {
    edited.BindManifest(manifest_address);
    return;
  }

  if (edited.is_ready()) {
    displayed_avatar_slot().CopyFrom(edited);
  }
  edited.StoreManifest(manifest_address);
  QueueAvatarRendererRebuild(guest_, edited);
}

// sub_920C3758
void EditorApp::NotifyCommand(uint32_t command) {
  const bool command_keeps_camera = command >= kNavCommandKeepsCameraFirst &&
                                    command <= kNavCommandKeepsCameraLast &&
                                    !NuiCursorState(guest_);
  if (!command_keeps_camera) {
    StoreFloat(guest_, kManifestHistoryCommandFloatAddress,
               kManifestHistoryCommandFloat);
  }

  SnapshotManifestHistory(guest_, command);
  guest_.Store32(kManifestHistoryCommandModeAddress, 2);

  StoreFloat(guest_, kIdleCountdownAddress,
             guest_.Load8(kFlag94571599Address)
                 ? kIdleCountdownAfterFlaggedCommandSeconds
                 : kIdleCountdownAfterCommandSeconds);
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
