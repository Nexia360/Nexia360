/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_RENDER_H_
#define XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_RENDER_H_

#include <stdint.h>

#include <string>
#include <vector>

// What the running editor currently has on screen, read out of guest memory so
// the XUI screen that draws the editor's own scenes can fill its tiles from it.
// Reading only: nothing here ticks the editor, posts a command or allocates in
// the editor's heaps.

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

struct EditorScreenItem {
  std::string name;
  // A menu button names its caption as an ordinal into the editor's
  // Strings.xus and carries the stem of its box art; an asset tile has its own
  // name and no art.
  uint32_t caption_string = 0;
  std::string art;
  uint32_t component_type = 0;
  // The catalogue asset behind the tile, which is what its thumbnail is
  // rendered from.
  uint8_t asset_id[16] = {};
  uint32_t colour_channel_count = 0;
  int32_t colour_group_count = 0;
  // The first colour group, as 0xRRGGBB per channel, for a swatch beside the
  // name.
  uint32_t colours[3] = {0, 0, 0};
  bool shows_new_badge = false;
  bool purchased = false;
  // Whether the manifest the editor has been writing into already carries this
  // asset.
  bool is_worn = false;
  // What activating the item posts. A grid item takes the category's own
  // command; a menu item takes the screen it opens.
  uint32_t activate_command = 0;
};

struct EditorScreenView {
  bool running = false;
  bool collection_ready = false;

  uint32_t command = 0;
  int32_t depth = -1;
  bool is_overlay = false;
  // The title the screen's registration hands its constructor, as an ordinal
  // into the editor's Strings.xus.
  uint32_t title_string = 0;
  std::string screen_name;
  std::string background_image;
  // One name per stack level, root first, the current screen last.
  std::vector<std::string> breadcrumb;

  // A menu's items are its buttons, each with box art and a caption; a grid's
  // are the category's assets. A screen is one or the other.
  bool is_menu = false;
  bool is_grid = false;
  int32_t category = -1;
  uint32_t category_type_mask = 0;
  int32_t focused_item = -1;
  int32_t scroll_offset = -1;

  // The category holds item_total assets; items carries the ones read back. A
  // screen that is not a grid counts the children its scene has built instead.
  uint32_t item_total = 0;
  std::vector<EditorScreenItem> items;
};

// Names a component category after what its grid offers.
const char* EditorCategoryName(int32_t category);

// Reads the screen the editor is on. Answers a view with running = false when
// no editor is open.
EditorScreenView ReadCurrentEditorScreen(uint32_t item_limit);

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_RENDER_H_
