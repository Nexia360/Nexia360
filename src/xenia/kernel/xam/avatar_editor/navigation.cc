/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_editor/navigation.h"

#include <ctype.h>
#include <string.h>

#include "xenia/base/clock.h"
#include "xenia/kernel/xam/avatar_editor/asset_tile.h"
#include "xenia/kernel/xam/avatar_editor/editor_app.h"
#include "xenia/kernel/xam/avatar_editor/scene.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

namespace {

// sub_920AD580's two words, and the duration the deeplink path hands it from
// the float at 0x92001DA0.
constexpr uint32_t kScreenTransitionModeAddress = 0x923125C8;
constexpr uint32_t kScreenTransitionSecondsAddress = 0x923125CC;
constexpr uint32_t kScreenTransitionDeeplink = 1;
constexpr float kScreenTransitionSeconds = 0.01f;

void StoreFloat(const Guest& guest, uint32_t address, float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  guest.Store32(address, bits);
}

bool ClassIsTile(uint32_t class_id) {
  return class_id == kAssetTileVtable || class_id == kColourButtonTileVtable ||
         class_id == kSwatchTileVtable || class_id == kMultiColourTileVtable;
}

// sub_920C7730 and sub_920C78A8, over the table asset_tile.cc writes: a
// presence byte then the manifest, one entry per screen on the stack.
uint32_t PendingEditAtDepth(const Guest& guest, int32_t depth) {
  if (depth < 0) {
    return 0;
  }
  const uint32_t entry = kPendingEditTableAddress + kPendingEditTableOffset +
                         static_cast<uint32_t>(depth) * kPendingEditStride;
  return guest.Load8(entry) ? entry + 1 : 0;
}

bool EqualsIgnoreCase(const char* left, const char* right) {
  while (*left && *right) {
    if (tolower(static_cast<unsigned char>(*left)) !=
        tolower(static_cast<unsigned char>(*right))) {
      return false;
    }
    ++left;
    ++right;
  }
  return *left == *right;
}

bool StartsWithIgnoreCase(const char* text, const char* prefix) {
  while (*prefix) {
    if (tolower(static_cast<unsigned char>(*text)) !=
        tolower(static_cast<unsigned char>(*prefix))) {
      return false;
    }
    ++text;
    ++prefix;
  }
  return true;
}

void WriteScreenName(NavigationScreen* screen, const char* name) {
  size_t length = 0;
  if (name) {
    while (name[length] && length + 1 < sizeof(screen->name)) {
      screen->name[length] = name[length];
      ++length;
    }
  }
  screen->name[length] = 0;
}

// The original widens with L"%hs" in one arm and wcscpy_s in the other; both
// end up as wide characters in the same field.
void WriteScreenBackgroundImage(NavigationScreen* screen, const char* path) {
  size_t length = 0;
  while (path[length] && length + 1 < kBackgroundImageChars) {
    screen->background_image[length] =
        static_cast<uint16_t>(static_cast<unsigned char>(path[length]));
    ++length;
  }
  screen->background_image[length] = 0;
}

struct ActionRoute {
  const char* action;
  uint32_t command;
  const char* screen_name;
};

constexpr ActionRoute kActionRoutes[] = {
    {"creator", kCommandCreator, nullptr},
    {"closet", kCommandCloset, nullptr},
    {"award", kCommandAwards, nullptr},
    {"photo", kCommandPhoto, nullptr},
    {"marketplace", kCommandStorefront, kStorefrontScreenName},
    {"storelist:storefront", kCommandStorefront, kStorefrontScreenName},
};

constexpr const char* kMarketplacePrefixes[] = {"storelist", "store", "title",
                                                "items"};

}  // namespace

/* sub_920EC380 */
bool ActionIsMarketplaceLink(const char* action) {
  for (const char* prefix : kMarketplacePrefixes) {
    if (StartsWithIgnoreCase(action, prefix)) {
      return true;
    }
  }
  return false;
}

/* sub_920FB458 plus the two ids sub_920ED2F0 tests by name */
bool CommandLeavesEditor(uint32_t command) {
  const int32_t id = static_cast<int32_t>(command);
  return id == static_cast<int32_t>(kCommandExit) ||
         id == static_cast<int32_t>(kCommandExitAlt) || (id >= 89 && id <= 90);
}

/* sub_920FD2D8 */
// The original returns a global object's +0x10; the base of that load is still
// unread, and the only use - the once-a-second refusal retry - needs nothing
// more than a monotonic millisecond count.
uint32_t MillisecondsNow() { return xe::Clock::QueryGuestUptimeMillis(); }

uint32_t Navigation::ScreenAddress(int32_t level) const {
  return kNavigationAddress + offsetof(NavigationState, screens) +
         level * sizeof(NavigationScreen);
}

NavigationScreen* Navigation::Screen(int32_t level) const {
  return &state()->screens[level];
}

/* sub_920EBEB0 */
void Navigation::Reset() {
  auto* nav = state();
  nav->depth = -1;
  nav->overlay_command = 0;
  nav->overlay_offset = -1;
  nav->background_back_slot = 0;
  nav->background_front_slot = 1;
  nav->current_scene = 0;
  nav->installed_scene = 0;
  nav->active_scene = 0;
}

/* sub_920EBF88 */
uint32_t Navigation::CurrentCommand() const {
  const auto* nav = state();
  if (nav->overlay_command != 0) {
    return nav->overlay_command;
  }
  const int32_t depth = nav->depth;
  if (depth <= -1) {
    return 0;
  }
  return nav->screens[depth].command;
}

/* sub_920EBFC8 */
uint32_t Navigation::ParentCommand() const {
  const auto* nav = state();
  const int32_t depth = nav->depth;
  if (depth <= 0) {
    return 0;
  }
  return nav->screens[depth - 1].command;
}

/* sub_920EBFF0 */
uint32_t Navigation::CommandAt(int32_t level) const {
  const auto* nav = state();
  // Level 0 answers 0: the bounds are the original's, and the root screen is
  // deliberately outside them.
  if (level <= 0 || level > static_cast<int32_t>(nav->depth)) {
    return 0;
  }
  return nav->screens[level].command;
}

/* sub_920EC178 */
bool Navigation::StackContains(uint32_t command) const {
  const auto* nav = state();
  const int32_t depth = nav->depth;
  for (int32_t level = 0; level <= depth; ++level) {
    if (nav->screens[level].command == command) {
      return true;
    }
  }
  return false;
}

/* sub_920EC100 */
uint32_t Navigation::OverlayCommand() const { return state()->overlay_command; }

/* sub_920EC110 */
uint32_t Navigation::CurrentScene() const { return state()->current_scene; }

/* sub_920EC768 */
int32_t Navigation::RestoreIndex() const {
  const auto* nav = state();
  if (nav->overlay_command != 0) {
    return 0;
  }
  const int32_t depth = nav->depth;
  if (depth <= -1) {
    return 0;
  }
  return nav->screens[depth].index;
}

/* sub_920EC720 */
int32_t Navigation::RestoreOffset() const {
  const auto* nav = state();
  if (nav->overlay_command != 0) {
    return nav->overlay_offset;
  }
  const int32_t depth = nav->depth;
  if (depth <= -1) {
    return -1;
  }
  return nav->screens[depth].offset;
}

/* sub_920ECCB0 */
void Navigation::RecordOffset(int32_t offset) {
  auto* nav = state();
  if (nav->overlay_command != 0) {
    nav->overlay_offset = offset;
  } else if (static_cast<int32_t>(nav->depth) > -1) {
    nav->screens[nav->depth].offset = offset;
  }

  const uint32_t installed = nav->installed_scene;
  if (installed && SceneChildSlot(installed, offset)) {
    SetActiveScene(installed);
  }
}

/* sub_920ED138 */
void Navigation::SetSelection(int32_t index, int32_t offset) {
  auto* nav = state();
  const int32_t depth = nav->depth;
  if (depth <= -1) {
    return;
  }

  // The index is parked in the offset field so that the -1 arm below reads it
  // back through RestoreOffset; the settled offset overwrites it at the end.
  nav->screens[depth].offset = index;

  const uint32_t scene = nav->current_scene;
  if (scene) {
    SetSceneIndex(scene, index);
    SceneChildAt(scene, index);
  }

  int32_t settled = offset;
  if (offset == -1) {
    settled = RestoreOffset();
    if (scene) {
      DetachActiveScene(scene);
    }
  }

  if (nav->overlay_command != 0) {
    nav->overlay_offset = settled;
  } else {
    nav->screens[depth].offset = settled;
  }

  CompleteInstall(scene, settled);
  if (guest_.Load8(kApplicationAddress + kAppDestroyActiveSceneOffset)) {
    DestroyActiveScene();
  }
}

/* sub_920EC0C0 */
void Navigation::SetField90F5(uint8_t value) { state()->field_90F5 = value; }

/* sub_920EDDD0 */
uint32_t Navigation::LookupScene(int32_t command) const {
  if (command < 0 || command >= static_cast<int32_t>(kSceneRegistrySize)) {
    return 0;
  }
  return guest_.Load32(kSceneRegistryAddress + command * 4);
}

// A count that was a nonzero multiple of 256 reads as zero here; the mask is
// the original's and nothing bounds the count.
bool Navigation::SceneHasChildren(uint32_t scene) {
  return (SceneChildCount(scene) & 0xFF) != 0;
}

/* sub_920EC2E8 */
void Navigation::ClampSelection(uint32_t scene, int32_t* index,
                                int32_t* offset) {
  if (!scene) {
    *index = 0;
    *offset = 0;
    return;
  }

  const int32_t count = SceneItemCount(scene);
  if (count <= 0) {
    return;  // a scene with no items leaves both values untouched
  }

  if (*index < 0) {
    *index = 0;
    *offset = 0;
  }

  const int32_t last = count - 1;
  if (*index > last) {
    *index = last;
    *offset = 0;
  }
}

/* sub_920EC810 */
bool Navigation::SeedPosition(uint32_t scene, int32_t* index, int32_t* offset) {
  if (!scene) {
    return false;
  }

  auto* nav = state();
  int32_t start = 0;
  if (nav->overlay_command == 0 && static_cast<int32_t>(nav->depth) > -1) {
    start = nav->screens[nav->depth].index;
  }
  *index = start;
  *offset = RestoreOffset();

  SceneSeedPosition(scene, index, offset);
  ClampSelection(scene, index, offset);
  return true;
}

void Navigation::AddRefScene(uint32_t scene) {
  const uint32_t field = scene + kSceneRefcountOffset;
  guest_.Store32(field, guest_.Load32(field) + 1);
}

/* sub_920A9430 */
void Navigation::ReleaseScene(uint32_t scene) {
  if (!scene) {
    return;  // the original decrements first and faults only on the host
  }
  const uint32_t field = scene + kSceneRefcountOffset;
  const int32_t remaining = static_cast<int32_t>(guest_.Load32(field)) - 1;
  guest_.Store32(field, static_cast<uint32_t>(remaining));
  if (remaining == 0) {
    DestroyScene(scene);
  }
}

/* sub_920EC8B8 */
void Navigation::DestroyActiveScene() {
  auto* nav = state();
  const uint32_t active = nav->active_scene;
  if (!active) {
    return;
  }
  DetachActiveScene(active);
  if (nav->active_scene) {
    ReleaseScene(nav->active_scene);
    nav->active_scene = 0;
  }
}

/* sub_920ECA78 */
void Navigation::SetActiveScene(uint32_t scene) {
  DestroyActiveScene();
  if (!scene) {
    return;
  }
  auto* nav = state();
  nav->active_scene = scene;
  AddRefScene(scene);
  SetSceneIndex(nav->active_scene, 0);
}

/* sub_920ECAF8 */
void Navigation::DestroyInstalledScene() {
  auto* nav = state();
  nav->field_88CC = 1;
  DestroyActiveScene();

  if (!nav->installed_scene) {
    return;
  }

  const int32_t count = SceneChildCount(nav->installed_scene);
  for (int32_t i = 0; i < count; ++i) {
    // The slot is re-read every turn because detaching a child can replace it.
    const uint32_t child = SceneChildSlot(nav->installed_scene, i);
    if (child) {
      DetachScene(child);
    }
  }

  DetachScene(nav->installed_scene);

  if (nav->installed_scene) {
    ReleaseScene(nav->installed_scene);
    nav->installed_scene = 0;
  }
}

/* sub_920ECBC0 */
void Navigation::SetInstalledScene(uint32_t scene) {
  DestroyInstalledScene();
  if (!scene) {
    return;
  }
  auto* nav = state();
  nav->installed_scene = scene;
  AddRefScene(scene);
  AttachScene(nav->installed_scene);
}

/* sub_920ECC40 */
void Navigation::DestroyCurrentScene() {
  DestroyInstalledScene();

  auto* nav = state();
  if (!nav->current_scene) {
    return;
  }
  DetachScene(nav->current_scene);

  if (nav->current_scene) {
    ReleaseScene(nav->current_scene);
    nav->current_scene = 0;
  }
}

/* sub_920ECE28 */
void Navigation::SetCurrentScene(uint32_t scene) {
  auto* nav = state();
  nav->navigating = 1;  // lowered by the caller, not here
  DestroyCurrentScene();
  if (!scene) {
    return;
  }
  nav->current_scene = scene;
  AddRefScene(scene);
  AttachScene(nav->current_scene);
}

/* sub_920ECD48 */
void Navigation::CompleteInstall(uint32_t scene, int32_t offset) {
  if (!scene) {
    return;
  }

  SetInstalledScene(scene);

  auto* nav = state();
  const int32_t count = SceneChildCount(nav->installed_scene);
  for (int32_t i = 0; i < count; ++i) {
    const uint32_t child = SceneChildSlot(nav->installed_scene, i);
    if (child) {
      AttachScene(child);
    }
  }

  if (CompleteInstallEmptiesActiveScene()) {
    DestroyActiveScene();
    return;
  }

  // The original fetches a child by the OFFSET and then makes the installed
  // scene - not that child - active.
  if (SceneChildSlot(nav->installed_scene, offset)) {
    SetActiveScene(nav->installed_scene);
  }
}

/* sub_920ED280 */
void Navigation::InstallSceneAt(uint32_t scene, int32_t index, int32_t offset) {
  if (!scene) {
    return;
  }
  SetCurrentScene(scene);
  SetSceneIndex(scene, index);
  SceneChildAt(scene, index);
  CompleteInstall(scene, offset);
}

/* sub_920ED2F0, tail at LABEL_920ED554 */
void Navigation::InstallRefusalScreen(int32_t index, int32_t offset) {
  InstallSceneAt(LookupScene(kCommandRefusalScreen), index, offset);
  state()->last_refusal_time_ms = MillisecondsNow();
}

/* sub_920ED2F0 */
void Navigation::PushCommand(uint32_t command, const char* name,
                             const char* background_image, int32_t index,
                             int32_t offset) {
  auto* nav = state();
  const int32_t depth = static_cast<int32_t>(nav->depth) + 1;
  if (depth >= static_cast<int32_t>(kScreenStackLimit)) {
    return;
  }

  nav->depth = depth;
  NavigationScreen* screen = Screen(depth);
  screen->command = command;
  WriteScreenName(screen, name);
  if (background_image && background_image[0]) {
    WriteScreenBackgroundImage(screen, background_image);
  } else if (depth > 0) {
    memcpy(screen->background_image, Screen(depth - 1)->background_image,
           sizeof(screen->background_image));
  } else {
    screen->background_image[0] = 0;
  }
  screen->index = 0;
  screen->offset = 0;

  SetBackgroundImage(ScreenAddress(depth) +
                     offsetof(NavigationScreen, background_image));
  NotifyCommand(command);

  const uint32_t scene = LookupScene(static_cast<int32_t>(command));
  const bool caller_chose_position = index != -1 && offset != -1;

  if (caller_chose_position) {
    ClampSelection(scene, &index, &offset);
    InstallSceneAt(scene, index, offset);
  } else if (scene && SceneHasChildren(scene)) {
    SetCurrentScene(scene);
    if (!SeedPosition(scene, &index, &offset)) {
      index = 0;
      offset = 0;
    }
    SetSceneIndex(scene, index);
    SceneChildAt(scene, index);
    CompleteInstall(scene, offset);
  } else if (!scene && CommandLeavesEditor(command)) {
    nav->navigating = 1;
    DestroyCurrentScene();
    nav->navigating = 0;
    nav->field_90F5 = 0;
  } else {
    InstallRefusalScreen(0, 0);
  }

  screen->index = index;
  screen->offset = offset;

  if (scene) {
    DetachActiveScene(scene);
  }
}

/* sub_920ED978 */
void Navigation::ResetStackAndPush(uint32_t command, int32_t index,
                                   int32_t offset) {
  state()->depth = -1;
  PushCommand(command, nullptr, kRootBackgroundImage, index, offset);
}

/* sub_920EDB00 */
void Navigation::ResetStackToRoot() {
  state()->depth = -1;
  PushCommand(kCommandRootScreen, nullptr, kRootBackgroundImage, -1, -1);
}

/* sub_920EDB28 */
void Navigation::ResetStackToRootHighlighting(uint32_t target_command) {
  const uint32_t root_scene = LookupScene(kCommandRootScreen);
  SetSceneIndex(root_scene, 0);

  int32_t found = -1;
  const int32_t count = SceneChildCount(root_scene);
  for (int32_t child = 0; child < count; ++child) {
    if (SceneChildMatchesCommand(root_scene, child, target_command)) {
      found = 0;
      break;
    }
  }

  // The search picks which arm of PushCommand runs - found means a named
  // position, so the scene's own seeding is skipped - not where the cursor
  // lands.
  state()->depth = -1;
  PushCommand(kCommandRootScreen, nullptr, kRootBackgroundImage, 0, found);
}

/* sub_920ED7B8 */
void Navigation::ReplaceTopScreen(uint32_t command, const char* name,
                                  const char* background_image, int32_t index,
                                  int32_t offset) {
  auto* nav = state();
  if (static_cast<int32_t>(nav->depth) > 0) {
    nav->depth = static_cast<int32_t>(nav->depth) - 1;
    DestroyCurrentScene();
  }
  PushCommand(command, name, background_image, index, offset);
}

/* sub_920ED5D8 */
void Navigation::PopScreen(uint32_t reason) {
  auto* nav = state();
  const int32_t depth = nav->depth;
  if (depth <= 0) {
    return;  // depth 0 is one screen, and the last screen never pops
  }

  nav->depth = depth - 1;
  SetBackgroundImage(ScreenAddress(depth - 1) +
                     offsetof(NavigationScreen, background_image));
  NotifyScreenPopped(CurrentCommand(), reason);

  const uint32_t command = CurrentCommand();
  const uint32_t scene = LookupScene(static_cast<int32_t>(command));

  if (scene && SceneHasChildren(scene)) {
    InstallSceneAt(scene, RestoreIndex(), RestoreOffset());
  } else if (!scene && CommandLeavesEditor(command)) {
    nav->navigating = 1;
    DestroyCurrentScene();
    nav->navigating = 0;
    nav->field_90F5 = 0;
  } else {
    InstallRefusalScreen(RestoreIndex(), RestoreOffset());
  }
}

/* sub_920EDC20 */
void Navigation::PopToCommand(uint32_t target_command, uint32_t reason,
                              bool confirm) {
  while (static_cast<int32_t>(state()->depth) > 0) {
    if (CurrentCommand() == target_command) {
      break;
    }
    if (confirm) {
      CommitPendingEditsOfTopScreen();
    }
    PopScreen(reason);
  }

  if (CurrentCommand() != target_command) {
    ResetStackToRoot();
  }
}

/* the back arm of sub_920DBCA0 */
void Navigation::PopUntilAScreenStaysUp() {
  while (static_cast<int32_t>(state()->depth) > 0) {
    PopScreen(2);
    const uint32_t current = CurrentScene();
    if (!current || !SceneWantsToClose(current)) {
      break;
    }
  }
  SetField90F5(1);
}

/* sub_920ED818 */
void Navigation::ShowOverlayScreen(uint32_t command, bool reset_position) {
  auto* nav = state();
  nav->overlay_offset = -1;
  nav->overlay_command = command;

  const uint32_t scene = LookupScene(static_cast<int32_t>(CurrentCommand()));

  int32_t index = -1;
  int32_t offset = -1;
  if (reset_position) {
    SetSelection(-1, -1);
    RecordOffset(-1);
  } else {
    // The index comes out of the offset field, which is where SetSelection
    // leaves it.
    index = nav->overlay_command == 0 && static_cast<int32_t>(nav->depth) > -1
                ? static_cast<int32_t>(nav->screens[nav->depth].offset)
                : 0;
    offset = RestoreOffset();
  }
  InstallSceneAt(scene, index, offset);
}

/* sub_920ED998 */
void Navigation::GoToAction(const char* action) {
  state()->depth = -1;
  PushCommand(kCommandRootScreen, nullptr, kRootBackgroundImage, -1, -1);
  StartScreenTransition();

  for (const ActionRoute& route : kActionRoutes) {
    if (EqualsIgnoreCase(action, route.action)) {
      PushCommand(route.command, route.screen_name, nullptr, -1, -1);
      return;
    }
  }

  if (!ActionIsMarketplaceLink(action)) {
    return;  // an unrecognised deeplink leaves only the root screen up
  }

  PushCommand(kCommandMarketplaceList, kStorefrontScreenName, nullptr, -1, -1);
  PushCommand(kCommandStorefront, action, nullptr, -1, -1);
}

/* sub_920DBCA0, the press asset_tile.cc owns as AssetTile::Activate */
void Navigation::ActivateFocusedItem(uint32_t scene) {
  const uint32_t item = SceneFocusedItem(scene);
  if (!item) {
    PopUntilAScreenStaysUp();
    return;
  }
  AssetTile(guest_, item).Activate();
}

/* sub_920ECEB8 */
void Navigation::Tick() {
  PresentPendingBackground();

  auto* nav = state();
  if (nav->current_scene) {
    UpdateCurrentScene(nav->current_scene);
  }

  const uint32_t scene = LookupScene(static_cast<int32_t>(CurrentCommand()));
  const int32_t depth = nav->depth;
  const bool position_known = nav->overlay_command != 0 ||
                              (depth > -1 && nav->screens[depth].index != -1 &&
                               nav->screens[depth].offset != -1);
  if (position_known && nav->current_scene == scene) {
    return;
  }
  if (!scene || !SceneChildCount(scene)) {
    return;
  }
  if (MillisecondsNow() - nav->last_refusal_time_ms <=
      kRefusalRetryIntervalMs) {
    return;
  }

  int32_t index = 0;
  int32_t offset = 0;
  SetCurrentScene(scene);
  if (!SeedPosition(scene, &index, &offset)) {
    index = 0;
    offset = 0;
  }
  SetSceneIndex(scene, index);
  SceneChildAt(scene, index);
  CompleteInstall(scene, offset);
  if (depth > -1) {
    nav->screens[depth].index = index;
    nav->screens[depth].offset = offset;
  }
}

const SceneOps* Navigation::SceneOpsOf(SceneTree& tree, uint32_t scene) {
  return scene ? SceneOpsForClass(tree.ClassIdOf(scene)) : nullptr;
}

/* sub_920D98F8 / sub_920E6570, through the scene's class */
void Navigation::DestroyScene(uint32_t scene) {
  SceneTree tree(guest_.memory());
  tree.Destroy(scene, 1);
}

void Navigation::AttachScene(uint32_t scene) {
  SceneTree tree(guest_.memory());
  tree.Attach(scene);
}

void Navigation::DetachScene(uint32_t scene) {
  SceneTree tree(guest_.memory());
  tree.Detach(scene);
}

void Navigation::DetachActiveScene(uint32_t scene) {
  SceneTree tree(guest_.memory());
  if (const SceneOps* ops = SceneOpsOf(tree, scene)) {
    ops->slot_10(tree, scene);
  }
}

void Navigation::SetSceneIndex(uint32_t scene, int32_t index) {
  SceneTree tree(guest_.memory());
  tree.ScrollTo(scene, index);
}

int32_t Navigation::SceneChildCount(uint32_t scene) {
  SceneTree tree(guest_.memory());
  return tree.EnumerableCount(scene);
}

int32_t Navigation::SceneItemCount(uint32_t scene) {
  SceneTree tree(guest_.memory());
  return tree.GroupCount(scene);
}

uint32_t Navigation::SceneChildAt(uint32_t scene, int32_t index) {
  SceneTree tree(guest_.memory());
  return tree.GroupAt(scene, index);
}

uint32_t Navigation::SceneChildSlot(uint32_t scene, int32_t index) {
  SceneTree tree(guest_.memory());
  return tree.EnumerableAt(scene, index);
}

void Navigation::SceneSeedPosition(uint32_t scene, int32_t* index,
                                   int32_t* offset) {
  SceneTree tree(guest_.memory());
  tree.SeedPosition(scene, index, offset);
}

bool Navigation::SceneWantsToClose(uint32_t scene) {
  SceneTree tree(guest_.memory());
  const SceneOps* ops = SceneOpsOf(tree, scene);
  return ops && ops->answer_34(tree, scene) != 0;
}

// The focused item is the one the screen remembers, taken off a grid as an
// item and off every other scene as a child. A child that is itself a scene is
// not an item, and answering 0 for it is what sends the press down the back
// arm.
uint32_t Navigation::SceneFocusedItem(uint32_t scene) {
  if (!scene) {
    return 0;
  }
  SceneTree tree(guest_.memory());
  const int32_t index = RestoreIndex();

  uint32_t item = 0;
  if (tree.ItemCount(scene) > 0) {
    item = tree.AcquireItem(scene, index);
  } else {
    item = tree.ChildAt(scene, index);
  }
  return ClassIsTile(tree.ClassIdOf(item)) ? item : 0;
}

bool Navigation::SceneChildMatchesCommand(uint32_t scene, int32_t child,
                                          uint32_t command) {
  SceneTree tree(guest_.memory());
  const uint32_t node = tree.EnumerableAt(scene, child);
  const SceneOps* ops = SceneOpsOf(tree, node);
  return ops && static_cast<uint32_t>(ops->answer_40(tree, node)) == command;
}

// sub_920EC918 with sub_920EC420 inlined: the current scene settles, then every
// group it has materialised is scrolled back to its place in the window. The
// original also walks each group's tiles, which reads nothing here.
void Navigation::UpdateCurrentScene(uint32_t scene) {
  SceneTree tree(guest_.memory());
  const SceneOps* ops = SceneOpsOf(tree, scene);
  if (!ops) {
    return;
  }
  ops->slot_10(tree, scene);

  const int32_t materialised = tree.MaterialisedCount(scene);
  for (int32_t i = 0; i < materialised; ++i) {
    const uint32_t group = tree.MaterialisedAt(scene, i);
    if (group) {
      tree.ScrollTo(group, i);
    }
  }
}

/* sub_920C3758 */
void Navigation::NotifyCommand(uint32_t command) {
  EditorApp(guest_.memory(), nullptr).NotifyCommand(command);
}

/* sub_920C3820 */
// The pop-side twin of sub_920C3758: the same camera reset, history float and
// history mode. The reason only picks which of the two history entries the
// application keeps, and nothing in this port writes that history yet.
void Navigation::NotifyScreenPopped(uint32_t command, uint32_t reason) {
  (void)reason;
  NotifyCommand(command);
}

bool Navigation::CompleteInstallEmptiesActiveScene() const {
  return guest_.Load8(kApplicationAddress + kAppEmptiesActiveSceneOffset) != 0;
}

/* sub_920C7730 then sub_920C7E30 */
void Navigation::CommitPendingEditsOfTopScreen() {
  const uint32_t pending = PendingEditAtDepth(guest_, state()->depth);
  if (!pending) {
    return;
  }
  memcpy(guest_.At<uint8_t>(kLiveManifestAddress), guest_.At<uint8_t>(pending),
         kManifestBytes);
  EditorApp(guest_.memory(), nullptr)
      .PushManifestToRenderer(kLiveManifestAddress, false);
}

/* sub_920AD580 */
void Navigation::StartScreenTransition() {
  guest_.Store32(kScreenTransitionModeAddress, kScreenTransitionDeeplink);
  StoreFloat(guest_, kScreenTransitionSecondsAddress, kScreenTransitionSeconds);
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
