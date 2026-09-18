/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_editor/editor_input.h"

#include <ctype.h>
#include <optional>

#include <cstring>

#include "xenia/kernel/xam/avatar_editor/asset_tile.h"
#include "xenia/kernel/xam/avatar_editor/component_collection.h"
#include "xenia/kernel/xam/avatar_editor/editor_asset_record.h"
#include "xenia/kernel/xam/avatar_editor/editor_session.h"
#include "xenia/kernel/xam/avatar_editor/navigation.h"
#include "xenia/kernel/xam/avatar_editor/scene.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

namespace {

// The reason code the original hands PopScreen when the back button is what
// closed the screen.
constexpr uint32_t kBackPressed = 2;

// A list scrolls one item at a time through a window the height of the grid's.
constexpr int32_t kVisibleRows = kComponentListVisibleGroups;

bool SameAction(const char* left, const char* right) {
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

// The actions GoToAction routes to a screen of their own.
constexpr const char* kNamedActions[] = {"creator", "closet", "award", "photo",
                                         "marketplace"};

bool IsNamedAction(const char* action) {
  for (const char* named : kNamedActions) {
    if (SameAction(action, named)) {
      return true;
    }
  }
  return false;
}

int32_t Clamp(int32_t value, int32_t low, int32_t high) {
  if (value < low) {
    return low;
  }
  return value > high ? high : value;
}

}  // namespace

EditorInput* EditorInput::ForCurrentSession() {
  static std::optional<EditorInput> bound;
  EditorSession* session = EditorSession::Current();
  if (!session) {
    bound.reset();
    return nullptr;
  }
  if (!bound || bound->session_ != session) {
    bound.emplace(session);
  }
  return &bound.value();
}

// A grid counts its items; everything else is a list of children, and a scene
// that answers neither has nothing to move through.
int32_t EditorInput::ItemCountOf(uint32_t scene) const {
  if (!scene) {
    return 0;
  }
  SceneTree& scenes = session_->scenes();
  const int32_t items = scenes.ItemCount(scene);
  if (items > 0) {
    return items;
  }
  const int32_t children = scenes.EnumerableCount(scene);
  if (children > 0) {
    return children;
  }
  const SceneObject* object = scenes.Object(scene);
  return object ? static_cast<int32_t>(object->child_count) : 0;
}

EditorFocus EditorInput::Focus() const {
  Navigation& navigation = session_->navigation();

  EditorFocus focus;
  focus.command = navigation.CurrentCommand();
  focus.scene = navigation.CurrentScene();
  focus.item_count = ItemCountOf(focus.scene);
  focus.is_grid = SceneCategoryForCommand(focus.command) != kNoConstant;
  focus.columns = focus.is_grid ? static_cast<int32_t>(kGridItemsPerGroup) : 1;

  // A screen the editor builds as a bare scene node reports one child however
  // many buttons it draws, and the clamp below would pin the focus to the
  // first one forever. What the screen declared wins.
  if (declared_scene_ == focus.scene && declared_count_ > focus.item_count) {
    focus.item_count = declared_count_;
    focus.columns = declared_columns_;
  }

  // A screen that does not know how many items it has cannot bound the index
  // either. Clamping to zero there pulled the focus back to the first item
  // every time it was read, so nothing could ever appear to move.
  const int32_t remembered = navigation.RestoreIndex();
  focus.focused_index = focus.item_count > 0
                            ? Clamp(remembered, 0, focus.item_count - 1)
                            : (remembered > 0 ? remembered : 0);

  const int32_t offset = navigation.RestoreOffset();
  focus.scroll_offset = offset > 0 ? offset : 0;
  return focus;
}

bool EditorInput::AtRootScreen() const {
  return session_->navigation().state()->depth <= 0;
}

// The window follows the focused row only far enough to keep it on screen, so
// a move inside the window leaves the screen still.
int32_t EditorInput::ScrollOffsetFor(int32_t index, int32_t columns,
                                     int32_t item_count,
                                     int32_t current_offset) {
  const int32_t row = index / columns;
  const int32_t last_row = (item_count - 1) / columns;
  int32_t offset = Clamp(current_offset, 0, last_row);
  if (row < offset) {
    offset = row;
  } else if (row >= offset + kVisibleRows) {
    offset = row - kVisibleRows + 1;
  }
  return offset;
}

bool EditorInput::MoveFocus(int32_t dx, int32_t dy) {
  const EditorFocus focus = Focus();
  if (focus.item_count <= 0) {
    return false;
  }

  const int32_t columns = focus.columns;
  const int32_t last = focus.item_count - 1;
  const int32_t row = focus.focused_index / columns;
  const int32_t column = focus.focused_index % columns;

  // A row is as wide as the grid except the last one, which is as wide as the
  // items left over.
  const int32_t columns_in_row =
      Clamp(focus.item_count - row * columns, 1, columns);
  const int32_t new_column = Clamp(column + dx, 0, columns_in_row - 1);
  const int32_t new_row = Clamp(row + dy, 0, last / columns);
  const int32_t index = Clamp(new_row * columns + new_column, 0, last);
  if (index == focus.focused_index) {
    return false;
  }

  RememberFocus(index, ScrollOffsetFor(index, columns, focus.item_count,
                                       focus.scroll_offset));
  return true;
}

void EditorInput::DeclareItemCount(int32_t item_count, int32_t columns) {
  declared_scene_ = session_->navigation().CurrentScene();
  declared_count_ = item_count;
  declared_columns_ = columns > 0 ? columns : 1;
}

bool EditorInput::SetFocusIndex(int32_t index, int32_t item_count,
                                int32_t columns) {
  if (item_count <= 0) {
    return false;
  }
  if (columns <= 0) {
    columns = 1;
  }
  // Declare before reading back, or Focus() clamps against the scene's own
  // count and the move is undone the moment it is made.
  DeclareItemCount(item_count, columns);
  const int32_t wanted = Clamp(index, 0, item_count - 1);
  const EditorFocus focus = Focus();
  if (wanted == focus.focused_index) {
    return false;
  }
  RememberFocus(wanted, ScrollOffsetFor(wanted, columns, item_count,
                                        focus.scroll_offset));
  return true;
}

// SetSelection re-attaches the newly selected child and records the offset; the
// index belongs to the screen, which is where the next install reads it from.
void EditorInput::RememberFocus(int32_t index, int32_t offset) {
  Navigation& navigation = session_->navigation();
  navigation.SetSelection(index, offset);

  NavigationState* nav = navigation.state();
  if (nav->overlay_command == 0 && static_cast<int32_t>(nav->depth) > -1) {
    nav->screens[nav->depth].index = index;
  }
}

bool EditorInput::Activate() {
  Navigation& navigation = session_->navigation();
  const uint32_t scene = navigation.CurrentScene();
  if (!scene) {
    return false;
  }
  navigation.ActivateFocusedItem(scene);
  return true;
}

bool EditorInput::OpenCommand(uint32_t command) {
  if (!command) {
    return false;
  }
  // The position is stated rather than left for the push to seed, because a
  // grid is built as an empty shell and fills from the component collection
  // afterwards. Leaving it unstated makes the push demand children the scene
  // does not have yet and install the refusal screen instead, which then
  // retries and drops back a screen every second.
  session_->navigation().PushCommand(command, nullptr, nullptr, 0, 0);
  return true;
}

// The grid press: dress the avatar in what is focused. The original reaches
// this through the tile object the scene holds; the index the UI moved is the
// same item, so it goes straight to the record.
bool EditorInput::WearItemAt(int32_t index) {
  if (index < 0) {
    return false;
  }
  const int32_t category =
      SceneCategoryForCommand(session_->navigation().CurrentCommand());
  if (category == kNoConstant) {
    return false;
  }
  ComponentCollection& components = session_->components();
  const EditorAssetRecord* record =
      components.BucketEntryAt(uint32_t(category), uint32_t(index));
  if (!record) {
    return false;
  }

  AvatarComponentInfo info = {};
  std::memcpy(info.asset_id, record->asset_id, kAssetIdBytes);
  const uint32_t kind = AssetKindOfRecord(record);
  info.type = uint16_t(kind);
  info.padding_012 = 0;
  const xe::be<uint32_t>* colours = EditorColourGroupCount(record) > 0
                                        ? EditorColourGroupAt(record, 0)
                                        : nullptr;
  if (colours) {
    std::memcpy(info.colours, colours, sizeof(info.colours));
  }
  WriteManifestItem(components.guest(), kLiveManifestAddress, info, kind);
  return true;
}

bool EditorInput::Back() {
  if (AtRootScreen()) {
    return false;
  }
  session_->navigation().PopScreen(kBackPressed);
  return true;
}

bool EditorInput::GoTo(const char* action) {
  if (!action || !*action) {
    return false;
  }
  if (!IsNamedAction(action) && !ActionIsMarketplaceLink(action)) {
    return false;
  }
  session_->navigation().GoToAction(action);
  return true;
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
