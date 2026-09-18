/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_SESSION_H_
#define XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_SESSION_H_

#include <memory>

#include "xenia/kernel/xam/avatar_editor/avatar_editor.h"
#include "xenia/kernel/xam/avatar_editor/component_collection.h"
#include "xenia/kernel/xam/avatar_editor/editor_app.h"
#include "xenia/kernel/xam/avatar_editor/navigation.h"
#include "xenia/kernel/xam/avatar_editor/scene.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

// The editor's state lives where the original put it, so running it without the
// title loaded means committing that range ourselves first. It starts at the
// image base rather than at the first object because the translated code still
// reads string and table literals out of .rdata down at 0x92001CF8, 0x92006C18,
// 0x9200C068 and 0x92053098. The far end is the component collection, which
// runs 31 MB from 0x92331D3C, and the application's globals above it.
constexpr uint32_t kEditorMemoryBase = 0x92000000;
constexpr uint32_t kEditorMemorySize = 0x02620000;

// Scene objects are allocated between the end of the collection and those
// globals.
constexpr uint32_t kSceneHeapBase = 0x94400000;
constexpr uint32_t kSceneHeapSize = 0x00010000;

// The framework singleton the whole image reaches through *(0x92326DF4). The
// XEX builds it in its entry point (sub_920FEDB0 -> sub_920B67D8 ->
// sub_920B5C68), which this port never runs, so the session builds it instead.
constexpr uint32_t kFrameworkPointerAddress = 0x92326DF4;
constexpr uint32_t kFrameworkObjectAddress = 0x943F0000;
constexpr uint32_t kFrameworkObjectBytes = 0x200;
constexpr uint32_t kFrameworkClassId = 0x92005864;
constexpr uint32_t kFrameworkModuleHandle = 0x38;

// Owns one run of the translated editor: the guest memory it needs, the five
// sections, and the frame tick that drives them.
class EditorSession {
 public:
  static EditorSession* Open(Memory* memory, uint32_t user_index);
  static EditorSession* Current();
  static void Close();

  void Tick();

  EditorApp& app() { return app_; }
  Navigation& navigation() { return navigation_; }
  SceneTree& scenes() { return scenes_; }
  ComponentCollection& components() { return components_; }

  bool collection_is_ready() const;
  uint32_t current_command() const;
  int32_t navigation_depth() const;
  uint32_t registered_scene_count() const;

 private:
  EditorSession(Memory* memory, uint32_t user_index);

  void ConstructFramework();
  bool CommitEditorMemory();
  void ReleaseEditorMemory();

  Memory* memory_;
  uint32_t user_index_;
  bool memory_committed_ = false;
  Guest guest_;
  Navigation navigation_;
  SceneTree scenes_;
  ComponentCollection components_;
  std::unique_ptr<EditorSections> sections_;
  EditorApp app_;
};

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_EDITOR_EDITOR_SESSION_H_
