/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_editor/editor_session.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/xam/avatar_editor/image_constants.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

namespace {

EditorSession* g_session = nullptr;

class WiredSections : public EditorSections {
 public:
  WiredSections(Navigation* navigation, SceneTree* scenes,
                ComponentCollection* components, uint32_t user_index)
      : navigation_(navigation),
        scenes_(scenes),
        components_(components),
        user_index_(user_index) {}

  uint32_t CurrentNavigationCommand() override {
    return navigation_->CurrentCommand();
  }

  void PostNavigationCommand(uint32_t command) override {
    navigation_->PushCommand(command, nullptr, nullptr, -1, -1);
  }

  void TickNavigation() override { navigation_->Tick(); }

  // The title runs this off its own press handler; here the app tick calls it
  // every frame. Activating unconditionally means a screen whose focused item
  // is not there yet - a grid still filling from the component collection -
  // gets popped on the very next tick, over and over. The press has its own
  // path through EditorInput::Activate, so nothing is lost by refusing to
  // activate on a frame where nobody pressed anything.
  void AdvanceNavigationAfterInput() override {}

  void DispatchNavigationActionName(const char* action_name) override {
    navigation_->GoToAction(action_name);
  }

  // sub_920EDEA8
  void PopulateSceneRegistry() override {
    size_t count = 0;
    const SceneRegistration* registrations = SceneRegistrations(&count);
    const Guest& guest = scenes_->guest();
    uint32_t next_object = kSceneHeapBase;
    uint32_t built = 0;

    for (size_t i = 0; i < count; ++i) {
      const SceneRegistration& registration = registrations[i];

      // Every class writes a name up to +0x94, so a registration whose own
      // allocation is smaller still needs that much room to be constructed
      // into.
      uint32_t object_bytes = registration.object_bytes;
      if (object_bytes < sizeof(SceneObject)) {
        object_bytes = sizeof(SceneObject);
      }
      if (next_object + object_bytes > kSceneHeapBase + kSceneHeapSize) {
        XELOGE("avatar_editor: scene heap exhausted at command {}",
               registration.command);
        break;
      }

      const uint32_t scene = next_object;
      next_object += (object_bytes + 15) & ~15u;
      std::memset(guest.At<uint8_t>(scene), 0, object_bytes);

      const uint32_t name_key =
          static_cast<uint32_t>(registration.constants[0]);
      switch (registration.class_id) {
        case kClassCreatorGridScene:
          scenes_->ConstructCreatorGridScene(scene, name_key,
                                             registration.constants[1]);
          break;
        case kClassStaticScene:
          scenes_->ConstructStaticScene(scene, name_key, registration.command);
          break;
        default:
          // The class behind these registrations has not been identified, but
          // every one of them derives from the scene node - which is what the
          // navigation needs to stop refusing them.
          scenes_->ConstructSceneNode(scene, name_key);
          break;
      }

      guest.Store32(kSceneRegistryAddress + registration.command * 4, scene);
      ++built;
    }

    XELOGI("avatar_editor: scene registry populated, {} of {} commands", built,
           count);
  }

  void StartComponentCollectionBuild() override {
    components_->Build(user_index_, 0);
  }

  void TickComponentCollection() override {}

  bool IsComponentCollectionReady() override { return components_->IsReady(); }

 private:
  Navigation* navigation_;
  SceneTree* scenes_;
  ComponentCollection* components_;
  uint32_t user_index_;
};

}  // namespace

EditorSession::EditorSession(Memory* memory, uint32_t user_index)
    : memory_(memory),
      user_index_(user_index),
      guest_(memory),
      navigation_(memory),
      scenes_(memory),
      components_(guest_),
      sections_(std::make_unique<WiredSections>(&navigation_, &scenes_,
                                                &components_, user_index)),
      app_(memory, sections_.get()) {}

bool EditorSession::CommitEditorMemory() {
  auto* heap = memory_->LookupHeap(kEditorMemoryBase);
  if (!heap) {
    XELOGE("avatar_editor: no heap covers {:08X}", kEditorMemoryBase);
    return false;
  }
  memory_committed_ =
      heap->AllocFixed(kEditorMemoryBase, kEditorMemorySize, 0,
                       kMemoryAllocationReserve | kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite);
  if (!memory_committed_) {
    // AvatarEditor.xex itself is loaded, so the range is already mapped with
    // the image's own contents. Run against those rather than failing, and
    // leave them alone on the way out.
    if (!memory_->TranslateVirtual(kEditorMemoryBase)) {
      XELOGE("avatar_editor: could not commit {:08X}..{:08X}",
             kEditorMemoryBase, kEditorMemoryBase + kEditorMemorySize);
      return false;
    }
    XELOGI("avatar_editor: running against memory that is already mapped");
    return true;
  }
  std::memset(memory_->TranslateVirtual(kEditorMemoryBase), 0,
              kEditorMemorySize);
  SeedImageConstants(guest_);
  return true;
}

void EditorSession::ReleaseEditorMemory() {
  if (!memory_committed_) {
    return;
  }
  auto* heap = memory_->LookupHeap(kEditorMemoryBase);
  if (heap) {
    heap->Release(kEditorMemoryBase, nullptr);
  }
  memory_committed_ = false;
}

EditorSession* EditorSession::Open(Memory* memory, uint32_t user_index) {
  Close();

  auto* session = new EditorSession(memory, user_index);
  if (!session->CommitEditorMemory()) {
    delete session;
    return nullptr;
  }

  session->ConstructFramework();
  session->navigation_.Reset();
  session->app_.Construct();
  session->sections_->PopulateSceneRegistry();
  session->app_.Start();

  g_session = session;
  XELOGI("avatar_editor: session open for user {}", user_index);
  return session;
}

EditorSession* EditorSession::Current() { return g_session; }

void EditorSession::Close() {
  if (!g_session) {
    return;
  }
  g_session->components_.RequestCancel();
  g_session->ReleaseEditorMemory();
  delete g_session;
  g_session = nullptr;
  XELOGI("avatar_editor: session closed");
}

// sub_920B5C68
void EditorSession::ConstructFramework() {
  std::memset(guest_.At<uint8_t>(kFrameworkObjectAddress), 0,
              kFrameworkObjectBytes);
  guest_.Store32(kFrameworkObjectAddress, kFrameworkClassId);
  // The original stores GetModuleHandleA(0) here; the image is not loaded, so
  // its own base is the closest true answer and nothing dereferences it.
  guest_.Store32(kFrameworkObjectAddress + kFrameworkModuleHandle,
                 kEditorMemoryBase);
  guest_.Store32(kFrameworkPointerAddress, kFrameworkObjectAddress);
}

void EditorSession::Tick() { app_.Tick(); }

bool EditorSession::collection_is_ready() const {
  return components_.IsReady();
}

uint32_t EditorSession::current_command() const {
  return navigation_.CurrentCommand();
}

int32_t EditorSession::navigation_depth() const {
  return navigation_.state()->depth;
}

uint32_t EditorSession::registered_scene_count() const {
  uint32_t filled = 0;
  for (uint32_t command = 0; command < kSceneRegistrySize; ++command) {
    if (guest_.Load32(kSceneRegistryAddress + command * 4)) {
      ++filled;
    }
  }
  return filled;
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
