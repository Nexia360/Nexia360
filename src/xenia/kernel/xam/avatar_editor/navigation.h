/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_EDITOR_NAVIGATION_H_
#define XENIA_KERNEL_XAM_AVATAR_EDITOR_NAVIGATION_H_

#include <stddef.h>
#include <stdint.h>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xam/avatar_editor/avatar_editor.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

constexpr uint32_t kSceneRegistrySize = 100;
constexpr uint32_t kLiveManifestAddress = 0x92327AE4;
constexpr uint32_t kPendingEditTableAddress = 0x942727A8;

constexpr uint32_t kSceneRefcountOffset = 0x04;
constexpr uint32_t kAppEmptiesActiveSceneOffset = 0xA258;
constexpr uint32_t kAppDestroyActiveSceneOffset = 0xA25A;

constexpr uint32_t kCommandRootScreen = 2;
constexpr uint32_t kCommandExit = 3;
constexpr uint32_t kCommandExitAlt = 91;
constexpr uint32_t kCommandRefusalScreen = 85;
constexpr uint32_t kCommandCreator = 4;
constexpr uint32_t kCommandCloset = 22;
constexpr uint32_t kCommandAwards = 16;
constexpr uint32_t kCommandPhoto = 50;
constexpr uint32_t kCommandStorefront = 67;
constexpr uint32_t kCommandMarketplaceList = 68;

constexpr const char* kRootBackgroundImage = "background.jpg";
constexpr const char* kStorefrontScreenName = "storelist:storefront";

constexpr uint32_t kScreenNameBytes = 0x80;
constexpr uint32_t kBackgroundImageChars = 0x400;
constexpr uint32_t kScreenStackLimit = 16;

constexpr uint32_t kRefusalRetryIntervalMs = 1000;

#pragma pack(push, 1)

// One screen on the stack, at nav+4 + level * 0x88C.
struct NavigationScreen {
  xe::be<uint32_t> command;
  char name[kScreenNameBytes];
  xe::be<uint16_t> background_image[kBackgroundImageChars];
  xe::be<int32_t> index;
  xe::be<int32_t> offset;
};
static_assert(sizeof(NavigationScreen) == 0x88C, "screen stride");
static_assert(offsetof(NavigationScreen, name) == 0x04, "screen name");
static_assert(offsetof(NavigationScreen, background_image) == 0x84,
              "screen background image");
static_assert(offsetof(NavigationScreen, index) == 0x884, "screen index");
static_assert(offsetof(NavigationScreen, offset) == 0x888, "screen offset");

// The whole navigation object, at 0x92328AB4.
struct NavigationState {
  xe::be<int32_t> depth;
  NavigationScreen screens[kScreenStackLimit];
  xe::be<uint32_t> overlay_command;
  xe::be<int32_t> overlay_offset;
  xe::be<uint32_t> field_88CC;
  xe::be<uint32_t> background_widgets[2];
  xe::be<int32_t> background_back_slot;
  xe::be<int32_t> background_front_slot;
  xe::be<uint16_t> background_image_shown[kBackgroundImageChars];
  uint8_t background_widget_ready;
  uint8_t field_90E1[3];
  xe::be<uint32_t> current_scene;
  xe::be<uint32_t> installed_scene;
  xe::be<uint32_t> active_scene;
  xe::be<uint32_t> last_refusal_time_ms;
  uint8_t navigating;
  uint8_t field_90F5;
};
static_assert(offsetof(NavigationState, screens) == 0x0004, "screen stack");
static_assert(offsetof(NavigationState, overlay_command) == 0x88C4, "overlay");
static_assert(offsetof(NavigationState, overlay_offset) == 0x88C8,
              "overlay offset");
static_assert(offsetof(NavigationState, field_88CC) == 0x88CC, "field_88CC");
static_assert(offsetof(NavigationState, background_widgets) == 0x88D0,
              "background widgets");
static_assert(offsetof(NavigationState, background_back_slot) == 0x88D8,
              "background back slot");
static_assert(offsetof(NavigationState, background_front_slot) == 0x88DC,
              "background front slot");
static_assert(offsetof(NavigationState, background_image_shown) == 0x88E0,
              "background image shown");
static_assert(offsetof(NavigationState, background_widget_ready) == 0x90E0,
              "background widget ready");
static_assert(offsetof(NavigationState, current_scene) == 0x90E4, "current");
static_assert(offsetof(NavigationState, installed_scene) == 0x90E8,
              "installed");
static_assert(offsetof(NavigationState, active_scene) == 0x90EC, "active");
static_assert(offsetof(NavigationState, last_refusal_time_ms) == 0x90F0,
              "last refusal time");
static_assert(offsetof(NavigationState, navigating) == 0x90F4, "navigating");
static_assert(offsetof(NavigationState, field_90F5) == 0x90F5, "field_90F5");

#pragma pack(pop)

// The editor's screen stack and everything that moves it: a posted command id
// in, a scene installed over a background out. All of its state is the
// console's, in guest memory at 0x92328AB4.
//
// A screen can be on the stack or off it. An off-stack screen - an overlay - is
// current without having been pushed, so every accessor answers for the overlay
// when one is up and for the top of the stack otherwise.
struct SceneOps;
class SceneTree;

class Navigation {
 public:
  explicit Navigation(Memory* memory) : guest_(memory) {}

  const Guest& guest() const { return guest_; }
  NavigationState* state() const {
    return guest_.At<NavigationState>(kNavigationAddress);
  }

  void Reset();

  uint32_t CurrentCommand() const;
  uint32_t ParentCommand() const;
  uint32_t CommandAt(int32_t level) const;
  bool StackContains(uint32_t command) const;
  uint32_t OverlayCommand() const;
  uint32_t CurrentScene() const;
  int32_t RestoreIndex() const;
  int32_t RestoreOffset() const;

  void RecordOffset(int32_t offset);
  void SetSelection(int32_t index, int32_t offset);
  void SetField90F5(uint8_t value);

  void PushCommand(uint32_t command, const char* name,
                   const char* background_image, int32_t index, int32_t offset);
  void ResetStackAndPush(uint32_t command, int32_t index, int32_t offset);
  void ResetStackToRoot();
  void ResetStackToRootHighlighting(uint32_t target_command);
  void ReplaceTopScreen(uint32_t command, const char* name,
                        const char* background_image, int32_t index,
                        int32_t offset);

  void PopScreen(uint32_t reason);
  void PopToCommand(uint32_t target_command, uint32_t reason, bool confirm);
  void PopUntilAScreenStaysUp();

  void ShowOverlayScreen(uint32_t command, bool reset_position);

  void GoToAction(const char* action);

  void ActivateFocusedItem(uint32_t scene);

  void Tick();
  void PresentPendingBackground();
  void SetBackgroundImage(uint32_t path);

  uint32_t LookupScene(int32_t command) const;
  void InstallSceneAt(uint32_t scene, int32_t index, int32_t offset);
  void SetCurrentScene(uint32_t scene);
  void CompleteInstall(uint32_t scene, int32_t offset);
  void DestroyCurrentScene();

 private:
  uint32_t ScreenAddress(int32_t level) const;
  NavigationScreen* Screen(int32_t level) const;

  bool SceneHasChildren(uint32_t scene);
  bool SeedPosition(uint32_t scene, int32_t* index, int32_t* offset);
  void ClampSelection(uint32_t scene, int32_t* index, int32_t* offset);
  void InstallRefusalScreen(int32_t index, int32_t offset);

  void SetInstalledScene(uint32_t scene);
  void SetActiveScene(uint32_t scene);
  void DestroyInstalledScene();
  void DestroyActiveScene();
  void AddRefScene(uint32_t scene);
  void ReleaseScene(uint32_t scene);

  // The scene classes, the application object and the pending-edit table are
  // other sections' work; these are the points where navigation reaches them.
  void DestroyScene(uint32_t scene);
  void AttachScene(uint32_t scene);
  void DetachScene(uint32_t scene);
  void DetachActiveScene(uint32_t scene);
  void SetSceneIndex(uint32_t scene, int32_t index);
  int32_t SceneChildCount(uint32_t scene);
  int32_t SceneItemCount(uint32_t scene);
  uint32_t SceneChildAt(uint32_t scene, int32_t index);
  uint32_t SceneChildSlot(uint32_t scene, int32_t index);
  void SceneSeedPosition(uint32_t scene, int32_t* index, int32_t* offset);
  bool SceneWantsToClose(uint32_t scene);
  uint32_t SceneFocusedItem(uint32_t scene);
  bool SceneChildMatchesCommand(uint32_t scene, int32_t child,
                                uint32_t command);
  void UpdateCurrentScene(uint32_t scene);
  static const SceneOps* SceneOpsOf(SceneTree& tree, uint32_t scene);

  void NotifyCommand(uint32_t command);
  void NotifyScreenPopped(uint32_t command, uint32_t reason);
  bool CompleteInstallEmptiesActiveScene() const;

  void CommitPendingEditsOfTopScreen();

  void StartScreenTransition();
  void ReleaseBackgroundWidget(uint32_t widget);
  uint32_t BuildRemoteBackgroundWidget(uint32_t path, int32_t slot);
  uint32_t BuildLocalBackgroundWidget(uint32_t path, int32_t slot);
  bool BackgroundWidgetIsReady(uint32_t widget);

  Guest guest_;
};

bool ActionIsMarketplaceLink(const char* action);
bool CommandLeavesEditor(uint32_t command);
uint32_t MillisecondsNow();

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_EDITOR_NAVIGATION_H_
