/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_INPUT_H_
#define XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_INPUT_H_

#include <stdint.h>

#include "xenia/kernel/xam/avatar_editor/editor_session.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

// What the current screen is showing, in the terms a drawer needs: which screen
// it is, how many items are on it, which one has the focus, and where the
// window onto them starts.
struct EditorFocus {
  uint32_t command = 0;
  uint32_t scene = 0;
  int32_t item_count = 0;
  int32_t focused_index = 0;
  int32_t scroll_offset = 0;
  int32_t columns = 1;
  bool is_grid = false;
};

// The editor's input layer: a UI raises an intent - move, activate, back, go
// somewhere - and this drives the translated navigation to match, leaving the
// new position in guest memory where the screen stack remembers it.
//
// The editor's own screens are the only thing it touches. It knows nothing
// about how a frame is drawn or where the intents came from.
class EditorInput {
 public:
  explicit EditorInput(EditorSession* session) : session_(session) {}

  // The input layer for the running editor, or null when none is open.
  static EditorInput* ForCurrentSession();

  // Moves the focus by one step on the current screen. A grid takes both axes
  // and never carries the focus across a row edge; a list takes the vertical
  // one. Answers whether the focus actually moved.
  bool MoveFocus(int32_t dx, int32_t dy);

  // Moves the focus straight to an index the caller has counted itself. A
  // screen the editor builds as a bare scene node carries no children, so it
  // reports no items and refuses every relative move - but whoever drew it
  // knows exactly how many buttons are on it.
  bool SetFocusIndex(int32_t index, int32_t item_count, int32_t columns = 1);

  // Tells the input layer how many items a screen really has. The scene
  // object the editor builds reports its own child count, which for these
  // screens is 1 - and Focus() clamps the remembered index to it, so the
  // focus snapped back to the first item on every read.
  void DeclareItemCount(int32_t item_count, int32_t columns);

  // The A press: activates the focused item on the current screen.
  bool Activate();

  // Opens a screen by the command its button carries. A menu button's press
  // is this: the screen's own tiles hold the command, and reaching them
  // through the scene's focused child depends on a focus the editor tracks
  // separately from the one the UI moved.
  bool OpenCommand(uint32_t command);

  // Puts the item at this index in the current category on the avatar. A grid
  // press dresses the avatar; it does not navigate anywhere.
  bool WearItemAt(int32_t index);

  // The B press: pops one screen. The root screen stays up and answers false.
  bool Back();

  // The action-string dispatcher: "creator", "closet", "award", "photo",
  // "marketplace", or a marketplace deeplink. An unknown action is refused
  // rather than dropping the user on an empty root screen.
  bool GoTo(const char* action);

  EditorFocus Focus() const;

  bool AtRootScreen() const;

 private:
  int32_t ItemCountOf(uint32_t scene) const;
  void RememberFocus(int32_t index, int32_t offset);
  static int32_t ScrollOffsetFor(int32_t index, int32_t columns,
                                 int32_t item_count, int32_t current_offset);

  EditorSession* session_;

  // The count and layout the screen declared, and the scene it declared them
  // for. Only trusted while that scene is still the current one.
  uint32_t declared_scene_ = 0;
  int32_t declared_count_ = 0;
  int32_t declared_columns_ = 1;
};

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_INPUT_H_
