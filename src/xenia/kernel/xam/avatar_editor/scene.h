/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_EDITOR_SCENE_H_
#define XENIA_KERNEL_XAM_AVATAR_EDITOR_SCENE_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xam/avatar_editor/avatar_editor.h"

// The editor's scene classes. Seven levels of single inheritance in the
// original, flattened here into one object layout plus one ops table per class.
//
//   RefCounted          0x92008A80
//    SceneNode          0x92008AD0   a name and a child array
//     ScrollingList     0x92008B50   a window onto a virtual list of groups
//      ComponentList    0x920095B0   eleven groups visible, ready when the
//       ComponentListShell 0x92009628              collection has been built
//        CategoryGrid   0x920086E8   bound to one component category
//         CreatorGridScene 0x9200AB18
//
//   StaticScene         0x9200A2D8   derives from SceneNode, not from the list
//
// A group is eight items. Everything the list counts in groups - the visible
// window, the scroll target, the seeded position - is counted in eights.

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

// The original put a vtable pointer at +0x00. Guest memory here cannot hold
// host code, so the field holds the vtable's address as a plain class id and
// the ops table is selected from it.
constexpr uint32_t kClassRefCounted = 0x92008A80;
constexpr uint32_t kClassSceneNode = 0x92008AD0;
constexpr uint32_t kClassScrollingList = 0x92008B50;
constexpr uint32_t kClassComponentList = 0x920095B0;
constexpr uint32_t kClassComponentListShell = 0x92009628;
constexpr uint32_t kClassCategoryGrid = 0x920086E8;
constexpr uint32_t kClassCreatorGridScene = 0x9200AB18;
constexpr uint32_t kClassStaticScene = 0x9200A2D8;
constexpr uint32_t kClassCreatorGridTileFactory = 0x9200AAEC;

// The widget sub_920E1DD0 leaves on a menu button. Every menu screen's buttons
// are one of its subclasses; the differences between them are the side effects
// their activation runs before posting, not anything the screen reads.
constexpr uint32_t kClassMenuButton = 0x92009DF0;

// The object a grid hands back for one strip of eight. It is not part of the
// chain above: it holds the eight tiles and its group index, nothing else.
constexpr uint32_t kClassGroupView = 0x9200BA28;
constexpr uint32_t kClassEmptyCategoryView = 0x9200B418;

// The two background image widgets the navigation swaps between screens.
constexpr uint32_t kClassBackgroundImage = 0x9200C0A8;

// A scene whose constructor chain has not been followed far enough to say which
// vtable it ends up with.
constexpr uint32_t kClassUnidentified = 0;

// Strings.xus ordinal 0xC1 is the empty string, which is what a screen with no
// title of its own is handed.
constexpr uint32_t kEmptyStringOrdinal = 0xC1;

constexpr uint32_t kSceneNameChars = 64;
constexpr uint32_t kGridItemsPerGroup = 8;
constexpr int32_t kComponentListVisibleGroups = 11;

// app + 0x118, the body kind the collection was built for.
constexpr uint32_t kBuildBodyKindAddress = kApplicationAddress + 0x118;

#pragma pack(push, 1)

struct SceneObject {
  xe::be<uint32_t> class_id;
  xe::be<uint32_t> ref_count;
  xe::be<uint32_t> children;
  xe::be<int32_t> child_count;
  xe::be<uint32_t> latch_10;
  xe::be<uint16_t> name[kSceneNameChars];
  xe::be<int32_t> visible_groups;
  xe::be<int32_t> first_item;
  xe::be<int32_t> selected_group;
  xe::be<int32_t> category;
  xe::be<uint32_t> tile_factory_class_id;
};

// sub_920EA5B8's object. The first two words line up with SceneObject so a
// reference count reaches it, and the group index sits where SceneNode keeps
// its child array - which is why the list recognises a built group by asking
// the child for its group count.
struct GroupViewObject {
  xe::be<uint32_t> class_id;
  xe::be<uint32_t> ref_count;
  xe::be<int32_t> group;
  xe::be<uint32_t> tiles[kGridItemsPerGroup];
};

// The 16 bytes a grid fills in to say which asset a tile should show.
struct ItemDescriptor {
  xe::be<int32_t> category;
  xe::be<uint32_t> body_kind;
  xe::be<uint32_t> record;
  xe::be<uint32_t> reserved_0C;
};

#pragma pack(pop)

static_assert(offsetof(SceneObject, class_id) == 0x00, "");
static_assert(offsetof(SceneObject, ref_count) == 0x04, "");
static_assert(offsetof(SceneObject, children) == 0x08, "");
static_assert(offsetof(SceneObject, child_count) == 0x0C, "");
static_assert(offsetof(SceneObject, latch_10) == 0x10, "");
static_assert(offsetof(SceneObject, name) == 0x14, "");
static_assert(offsetof(SceneObject, visible_groups) == 0x94, "");
static_assert(offsetof(SceneObject, first_item) == 0x98, "");
static_assert(offsetof(SceneObject, selected_group) == 0x9C, "");
static_assert(offsetof(SceneObject, category) == 0xA0, "");
static_assert(offsetof(SceneObject, tile_factory_class_id) == 0xA4, "");
static_assert(sizeof(SceneObject) == 0xA8, "");

static_assert(offsetof(GroupViewObject, group) == 0x08, "");
static_assert(offsetof(GroupViewObject, tiles) == 0x0C, "");
static_assert(sizeof(GroupViewObject) == 0x2C, "");

static_assert(offsetof(ItemDescriptor, category) == 0x00, "");
static_assert(offsetof(ItemDescriptor, body_kind) == 0x04, "");
static_assert(offsetof(ItemDescriptor, record) == 0x08, "");
static_assert(offsetof(ItemDescriptor, reserved_0C) == 0x0C, "");
static_assert(sizeof(ItemDescriptor) == 16, "");

class SceneTree;

// One entry per vtable slot, in slot order. Slots whose meaning was never
// established keep their offset as their name.
struct SceneOps {
  uint32_t class_id;
  const char* class_name;

  void (*destroy)(SceneTree&, uint32_t scene, uint32_t deleting);  // +0x00
  void (*attach)(SceneTree&, uint32_t scene);                      // +0x04
  void (*detach)(SceneTree&, uint32_t scene);                      // +0x08
  void (*scroll_to)(SceneTree&, uint32_t scene, int32_t group);    // +0x0C
  void (*slot_10)(SceneTree&, uint32_t scene);                     // +0x10
  int32_t (*enumerable_count)(SceneTree&, uint32_t scene);         // +0x14
  uint32_t (*enumerable_at)(SceneTree&, uint32_t scene,
                            int32_t index);                         // +0x18
  int32_t (*group_count)(SceneTree&, uint32_t scene);               // +0x1C
  uint32_t (*group_at)(SceneTree&, uint32_t scene, int32_t group);  // +0x20
  int32_t (*materialised_count)(SceneTree&, uint32_t scene);        // +0x24
  uint32_t (*materialised_at)(SceneTree&, uint32_t scene,
                              int32_t index);                        // +0x28
  uint32_t (*answer_2C)(SceneTree&, uint32_t scene, int32_t index);  // +0x2C
  bool (*any_child_matches)(SceneTree&, uint32_t scene);             // +0x30
  int32_t (*answer_34)(SceneTree&, uint32_t scene);                  // +0x34
  int32_t (*category)(SceneTree&, uint32_t scene);                   // +0x38
  bool (*seed_position)(SceneTree&, uint32_t scene, int32_t* group,
                        int32_t* item);               // +0x3C
  int32_t (*answer_40)(SceneTree&, uint32_t scene);   // +0x40
  int32_t (*take_latch)(SceneTree&, uint32_t scene);  // +0x44
  void (*copy_name)(SceneTree&, uint32_t scene, uint32_t destination,
                    uint32_t capacity_chars);                     // +0x48
  bool (*is_selected)(SceneTree&, uint32_t scene, int32_t flag);  // +0x4C

  int32_t (*item_count)(SceneTree&, uint32_t scene);              // +0x50
  int32_t (*group_count_from_items)(SceneTree&, uint32_t scene);  // +0x54
  int32_t (*items_in_group)(SceneTree&, uint32_t scene,
                            int32_t group);  // +0x58
  uint32_t (*acquire_item)(SceneTree&, uint32_t scene,
                           int32_t flat_index);  // +0x5C
  uint32_t (*build_group)(SceneTree&, uint32_t scene, int32_t group,
                          int32_t flat_base, int32_t count);  // +0x60
  void (*describe_item)(SceneTree&, uint32_t scene, uint32_t descriptor,
                        int32_t flat_index);  // +0x64
  uint32_t (*make_group_view)(SceneTree&, uint32_t scene,
                              const std::vector<uint32_t>& descriptors,
                              int32_t group);  // +0x68
  uint32_t (*make_tile)(SceneTree&, uint32_t scene,
                        uint32_t descriptor);  // +0x6C
  uint32_t (*build_group_view)(SceneTree&, uint32_t scene,
                               const uint32_t* descriptors, int32_t count,
                               int32_t group);  // +0x70
};

const SceneOps* SceneOpsForClass(uint32_t class_id);

// sub_920FC228 / sub_920FC260, the editor's operator new and operator delete.
// The original's heap is a real allocator inside the image; this one is a
// first-fit free list over a range of the same guest memory, with its whole
// state in guest memory so that opening a session zeroes it.
constexpr uint32_t kObjectHeapBase = 0x94410000;
constexpr uint32_t kObjectHeapSize = 0x00100000;

uint32_t AllocateGuestBytes(const Guest& guest, uint32_t bytes);
void FreeGuestBytes(const Guest& guest, uint32_t address);

// Every scene lives in guest memory at the address the registry allocated for
// it; this class is the code that reads and writes those objects.
class SceneTree {
 public:
  explicit SceneTree(Memory* memory) : guest_(memory) {}

  const Guest& guest() const { return guest_; }
  SceneObject* Object(uint32_t scene) const {
    return guest_.At<SceneObject>(scene);
  }
  uint32_t ClassIdOf(uint32_t scene) const;

  void AddRef(uint32_t scene);
  void Release(uint32_t scene);

  void ConstructRefCounted(uint32_t scene);
  void ConstructSceneNode(uint32_t scene, uint32_t name_key);
  void ConstructScrollingList(uint32_t scene, uint32_t name_key,
                              int32_t visible_groups);
  void ConstructComponentList(uint32_t scene, uint32_t name_key);
  void ConstructComponentListShell(uint32_t scene, uint32_t name_key);
  void ConstructCategoryGrid(uint32_t scene, uint32_t name_key,
                             int32_t category);
  void ConstructCreatorGridScene(uint32_t scene, uint32_t name_key,
                                 int32_t category);
  void ConstructStaticScene(uint32_t scene, uint32_t name_key);
  void ConstructStaticScene(uint32_t scene, uint32_t name_key,
                            uint32_t command);

  void Destroy(uint32_t scene, uint32_t deleting);
  void Attach(uint32_t scene);
  void Detach(uint32_t scene);
  void ScrollTo(uint32_t scene, int32_t group);
  int32_t EnumerableCount(uint32_t scene);
  uint32_t EnumerableAt(uint32_t scene, int32_t index);
  int32_t GroupCount(uint32_t scene);
  uint32_t GroupAt(uint32_t scene, int32_t group);
  int32_t MaterialisedCount(uint32_t scene);
  uint32_t MaterialisedAt(uint32_t scene, int32_t index);
  bool AnyChildMatches(uint32_t scene);
  int32_t Category(uint32_t scene);
  bool SeedPosition(uint32_t scene, int32_t* group, int32_t* item);
  int32_t TakeLatch(uint32_t scene);
  void CopyName(uint32_t scene, uint32_t destination, uint32_t capacity_chars);
  bool IsSelected(uint32_t scene, int32_t flag);
  int32_t ItemCount(uint32_t scene);
  int32_t GroupCountFromItems(uint32_t scene);
  int32_t ItemsInGroup(uint32_t scene, int32_t group);
  uint32_t AcquireItem(uint32_t scene, int32_t flat_index);
  uint32_t BuildGroup(uint32_t scene, int32_t group, int32_t flat_base,
                      int32_t count);
  void DescribeItem(uint32_t scene, uint32_t descriptor, int32_t flat_index);
  uint32_t MakeGroupView(uint32_t scene,
                         const std::vector<uint32_t>& descriptors,
                         int32_t group);
  uint32_t MakeTile(uint32_t scene, uint32_t descriptor);
  uint32_t BuildGroupView(uint32_t scene, const uint32_t* descriptors,
                          int32_t count, int32_t group);

  uint32_t ChildAt(uint32_t scene, int32_t index) const;
  void ReleaseChildren(uint32_t scene);
  void AdoptChildren(uint32_t scene, const std::vector<uint32_t>& children);
  void SetName(uint32_t scene, uint32_t name_key);

 private:
  const SceneOps* OpsFor(uint32_t scene, const char* slot) const;

  Guest guest_;
};

// One registration from sub_920EDEA8: the scene the navigation builds for a
// command, the class it ends up as, and the constants its constructor is
// handed. Generated by tools/registry.py in the port project.
struct SceneRegistration {
  uint32_t command;
  uint32_t construct;
  uint32_t class_id;
  uint32_t object_bytes;
  int32_t constants[3];
};

// Marks a constructor argument that is absent rather than zero; the two are
// different in the image.
constexpr int32_t kNoConstant = -1;

const SceneRegistration* SceneRegistrations(size_t* count);
const SceneRegistration* SceneRegistrationFor(uint32_t command);

// The category a grid command is bound to, or kNoConstant. It is the last
// constant the constructor is handed and lands at SceneObject::category.
int32_t SceneCategoryForCommand(uint32_t command);

bool CommandNeedsBuiltCollection(uint32_t command);

// The string the screen puts over itself, as an ordinal into the editor's
// Strings.xus. 0xC1 is the empty string, which several screens use on purpose.
uint32_t SceneTitleStringForCommand(uint32_t command);

// One button on a menu screen, from sub_920E1DD0 and the subclasses that wrap
// it. The console reaches the art through the widget's name, so the name is
// what a menu entry carries rather than a file.
struct MenuEntry {
  // The widget's name inside GridTopLevel. Its box art is the package image
  // "<element>_selected.png".
  const char* element;
  // The caption, as an ordinal into Strings.xus.
  uint16_t caption_string;
  // What pressing it posts.
  uint16_t command;
  // The screen also carries this button for the other body, with its own art
  // and sometimes its own command. Null when one button serves both.
  const char* alternate_element;
  uint16_t alternate_command;
  // kBodyTypeFirst or kBodyTypeSecond - which body the alternate is for.
  uint16_t alternate_body_type;
};

// A menu screen's buttons in the order its constructor lays them out, which is
// the order they appear: left to right, then the second row.
struct MenuScreen {
  uint32_t command;
  // The title the registration hands the constructor. No two static scenes
  // share one, so it names the screen where only the name_key is in hand.
  uint32_t title_string;
  const MenuEntry* entries;
  uint32_t entry_count;
};

const MenuScreen* MenuScreenForCommand(uint32_t command);
const MenuScreen* MenuScreenForTitleString(uint32_t title_string);

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_EDITOR_SCENE_H_
