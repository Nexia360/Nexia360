/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_editor/editor_render.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "xenia/base/string.h"
#include "xenia/kernel/xam/avatar_editor/asset_tile.h"
#include "xenia/kernel/xam/avatar_editor/component_collection.h"
#include "xenia/kernel/xam/avatar_editor/editor_asset_record.h"
#include "xenia/kernel/xam/avatar_editor/editor_session.h"
#include "xenia/kernel/xam/avatar_editor/navigation.h"
#include "xenia/kernel/xam/avatar_editor/scene.h"
#include "xenia/kernel/xna/xna_avatar_format.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

namespace {

std::string WideFieldToUtf8(const xe::be<uint16_t>* text, uint32_t capacity) {
  if (!text) {
    return std::string();
  }
  std::u16string wide;
  for (uint32_t i = 0; i < capacity && text[i] != 0; ++i) {
    wide.push_back(char16_t(uint16_t(text[i])));
  }
  return xe::to_utf8(wide);
}

std::string ScreenNameAt(const Navigation& navigation, int32_t level) {
  const NavigationScreen& screen = navigation.state()->screens[level];
  const char* stored = screen.name;
  std::string name;
  for (uint32_t i = 0; i < kScreenNameBytes && stored[i]; ++i) {
    name.push_back(stored[i]);
  }
  return name;
}

// A menu's buttons are its scene's children, in the order its constructor laid
// them out. What each one shows is in the menu table; the object in guest
// memory holds what the press posts, and that is what is read back here so a
// button the construction dropped is not drawn.
void ReadMenuButtons(EditorSession& session, uint32_t item_limit,
                     EditorScreenView* view) {
  const MenuScreen* menu = MenuScreenForCommand(view->command);
  const uint32_t scene = session.navigation().CurrentScene();
  if (!menu || !scene) {
    return;
  }
  SceneTree& scenes = session.scenes();
  const SceneObject* object = scenes.Object(scene);
  if (!object) {
    return;
  }
  view->is_menu = true;
  const int32_t count =
      std::min<int32_t>(object->child_count, int32_t(menu->entry_count));
  view->item_total = count > 0 ? uint32_t(count) : 0;
  const uint32_t shown =
      item_limit ? std::min(view->item_total, item_limit) : view->item_total;

  const uint32_t body_type =
      session.components().body_kind() != 0 ? kBodyTypeSecond : kBodyTypeFirst;
  for (uint32_t index = 0; index < shown; ++index) {
    const uint32_t child = scenes.ChildAt(scene, int32_t(index));
    if (!child) {
      break;
    }
    const MenuEntry& entry = menu->entries[index];
    EditorScreenItem item;
    item.caption_string = entry.caption_string;
    item.art = entry.alternate_element && entry.alternate_body_type == body_type
                   ? entry.alternate_element
                   : entry.element;
    item.activate_command = scenes.guest().Load32(
        child + offsetof(AssetTileFields, activate_command));
    view->items.push_back(std::move(item));
  }
}

}  // namespace

const char* EditorCategoryName(int32_t category) {
  if (category < 0) {
    return nullptr;
  }
  const uint32_t type_mask =
      ComponentCollection::CategoryToTypeMask(uint32_t(category));
  const int32_t slot = xna::avatar::PrimarySlot(type_mask);
  if (slot < 0) {
    return nullptr;
  }
  const char* name = xna::avatar::SlotName(uint32_t(slot));
  return name && *name ? name : nullptr;
}

EditorScreenView ReadCurrentEditorScreen(uint32_t item_limit) {
  EditorScreenView view;
  EditorSession* session = EditorSession::Current();
  if (!session) {
    return view;
  }

  view.running = true;
  view.collection_ready = session->collection_is_ready();

  Navigation& navigation = session->navigation();
  const NavigationState* state = navigation.state();
  view.depth = state->depth;
  view.command = navigation.CurrentCommand();
  view.is_overlay = navigation.OverlayCommand() != 0;
  view.focused_item = navigation.RestoreIndex();
  view.scroll_offset = navigation.RestoreOffset();

  view.title_string = SceneTitleStringForCommand(view.command);
  if (!view.is_overlay && view.depth >= 0 &&
      view.depth < int32_t(kScreenStackLimit)) {
    view.screen_name = ScreenNameAt(navigation, view.depth);
    view.background_image = WideFieldToUtf8(
        state->screens[view.depth].background_image, kBackgroundImageChars);
    for (int32_t level = 0; level <= view.depth; ++level) {
      view.breadcrumb.push_back(ScreenNameAt(navigation, level));
    }
  }

  const int32_t category = SceneCategoryForCommand(view.command);
  if (category == kNoConstant) {
    ReadMenuButtons(*session, item_limit, &view);
    return view;
  }
  view.is_grid = true;
  view.category = category;
  view.category_type_mask =
      ComponentCollection::CategoryToTypeMask(uint32_t(category));
  if (!view.collection_ready) {
    return view;
  }

  std::lock_guard<std::recursive_mutex> lock(CollectionLock());
  ComponentCollection& components = session->components();
  view.item_total = components.BucketEntryCount(uint32_t(category));
  const uint32_t shown =
      item_limit ? std::min(view.item_total, item_limit) : view.item_total;
  view.items.reserve(shown);
  for (uint32_t index = 0; index < shown; ++index) {
    const EditorAssetRecord* record =
        components.BucketEntryAt(uint32_t(category), index);
    if (!record) {
      break;
    }
    EditorScreenItem item;
    item.name =
        WideFieldToUtf8(record->display_name, kEditorAssetRecordNameLength);
    item.component_type = record->type_mask;
    std::memcpy(item.asset_id, record->asset_id, sizeof(item.asset_id));
    item.colour_channel_count = EditorColourChannelCount(record);
    item.colour_group_count = EditorColourGroupCount(record);
    if (const xe::be<uint32_t>* group = EditorColourGroupAt(record, 0)) {
      for (uint32_t channel = 0; channel < kEditorColoursPerGroup; ++channel) {
        item.colours[channel] = group[channel];
      }
    }
    item.shows_new_badge = EditorRecordShowsNewBadge(record);
    item.purchased = EditorRecordIsPurchased(record);
    item.is_worn =
        ManifestWearsAsset(components.guest(), kLiveManifestAddress,
                           record->asset_id, uint16_t(record->type_mask));
    view.items.push_back(std::move(item));
  }
  return view;
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
