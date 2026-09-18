/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_editor/scene.h"

#include <string.h>

#include "xenia/base/logging.h"
#include "xenia/kernel/xam/avatar_editor/asset_tile.h"
#include "xenia/kernel/xam/avatar_editor/component_collection.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

namespace {

// -- the editor's heap --------------------------------------------------------

#pragma pack(push, 1)
struct HeapHeader {
  xe::be<uint32_t> cursor;
  xe::be<uint32_t> first_free;
};
struct BlockHeader {
  xe::be<uint32_t> bytes;
  xe::be<uint32_t> next_free;
  uint8_t padding[8];
};
#pragma pack(pop)

static_assert(sizeof(BlockHeader) == 16, "a payload stays 16-byte aligned");

constexpr uint32_t kHeapPayloadBase = kObjectHeapBase + sizeof(HeapHeader);

// The closet keeps recently worn outfits, so one entry there is a real choice
// rather than the empty one.
constexpr int32_t kCategoryClosetRecents = 22;

constexpr uint32_t kEmptyCategoryViewBytes = 0x214;

// -- neighbours this section calls but does not own ---------------------------

// sub_920DA1D0, the localiser hanging off the graphics context. The key is an
// ordinal into the editor's Strings.xus, which lives in the asset package
// rather than in guest memory, so the screen that draws the editor resolves it
// in the viewer's language and the object keeps an empty name.
uint32_t SceneNameForKey(uint32_t name_key) {
  (void)name_key;
  return 0;
}

ComponentCollection Collection(const Guest& guest) {
  return ComponentCollection(guest);
}

// sub_92216938
bool ComponentCollectionIsReady(const Guest& guest) {
  return Collection(guest).IsReady();
}

// sub_92217138
int32_t ComponentCollectionCategorySize(const Guest& guest, int32_t category) {
  if (category < 0) {
    return 0;
  }
  return static_cast<int32_t>(
      Collection(guest).BucketEntryCount(static_cast<uint32_t>(category)));
}

// sub_922172C0
uint32_t ComponentCollectionRecord(const Guest& guest, int32_t category,
                                   int32_t index) {
  if (category < 0 || index < 0) {
    return 0;
  }
  ComponentCollection collection = Collection(guest);
  GuestAssetRecord* record = collection.BucketEntryAt(
      static_cast<uint32_t>(category), static_cast<uint32_t>(index));
  if (!record) {
    return 0;
  }
  return guest.memory()->HostToGuestVirtual(record);
}

// 0x9200AAEC slot 0 = sub_920BFD68, called as factory(descriptor, 1).
uint32_t MakeTileThroughFactory(const Guest& guest, uint32_t descriptor) {
  const ItemDescriptor* item = guest.At<ItemDescriptor>(descriptor);
  if (!item) {
    return 0;
  }
  // The factory is handed no command: a creator-grid tile wears its asset
  // where it stands rather than opening a screen, and the literal 1 the image
  // passes is the colour group, not a command id.
  return AssetTile::CreateForGridEntry(guest, item->record, 0, 1).address();
}

// sub_92105398
void PureCall(const char* slot) {
  XELOGE("avatar_editor: pure virtual scene slot {} called [sub_92105398]",
         slot);
}

// -- leaves shared across the vtables -----------------------------------------

// sub_920F5BD8
void DoNothing(SceneTree& tree, uint32_t scene) {
  (void)tree;
  (void)scene;
}

// sub_920F5BD8
void DoNothingWithGroup(SceneTree& tree, uint32_t scene, int32_t group) {
  (void)tree;
  (void)scene;
  (void)group;
}

// sub_920F5BD8
void DoNothingWithNameCopy(SceneTree& tree, uint32_t scene,
                           uint32_t destination, uint32_t capacity_chars) {
  (void)scene;
  if (destination && capacity_chars) {
    tree.guest().Store16(destination, 0);
  }
}

// sub_920F3CF0
int32_t AnswerZero(SceneTree& tree, uint32_t scene) {
  (void)tree;
  (void)scene;
  return 0;
}

// sub_920F3CF0
uint32_t AnswerZeroAtIndex(SceneTree& tree, uint32_t scene, int32_t index) {
  (void)tree;
  (void)scene;
  (void)index;
  return 0;
}

// sub_920F3CF0
bool AnswerNoMatch(SceneTree& tree, uint32_t scene) {
  (void)tree;
  (void)scene;
  return false;
}

// sub_920F3CF0
bool AnswerNotSelected(SceneTree& tree, uint32_t scene, int32_t flag) {
  (void)tree;
  (void)scene;
  (void)flag;
  return false;
}

// sub_920F3CF0
bool AnswerNoSeedPosition(SceneTree& tree, uint32_t scene, int32_t* group,
                          int32_t* item) {
  (void)tree;
  (void)scene;
  (void)group;
  (void)item;
  return false;
}

// sub_921A1FB0
int32_t AnswerOne(SceneTree& tree, uint32_t scene) {
  (void)tree;
  (void)scene;
  return 1;
}

// sub_921A1FB0
uint32_t AnswerOneAtIndex(SceneTree& tree, uint32_t scene, int32_t index) {
  (void)tree;
  (void)scene;
  (void)index;
  return 1;
}

void AbstractSlot10(SceneTree&, uint32_t) { PureCall("+0x10"); }
int32_t AbstractEnumerableCount(SceneTree&, uint32_t) {
  PureCall("+0x14");
  return 0;
}
uint32_t AbstractEnumerableAt(SceneTree&, uint32_t, int32_t) {
  PureCall("+0x18");
  return 0;
}
int32_t AbstractGroupCount(SceneTree&, uint32_t) {
  PureCall("+0x1C");
  return 0;
}
uint32_t AbstractGroupAt(SceneTree&, uint32_t, int32_t) {
  PureCall("+0x20");
  return 0;
}
int32_t AbstractMaterialisedCount(SceneTree&, uint32_t) {
  PureCall("+0x24");
  return 0;
}
uint32_t AbstractMaterialisedAt(SceneTree&, uint32_t, int32_t) {
  PureCall("+0x28");
  return 0;
}
int32_t AbstractAnswer40(SceneTree&, uint32_t) {
  PureCall("+0x40");
  return 0;
}
int32_t AbstractTakeLatch(SceneTree&, uint32_t) {
  PureCall("+0x44");
  return 0;
}
void AbstractCopyName(SceneTree&, uint32_t, uint32_t, uint32_t) {
  PureCall("+0x48");
}
bool AbstractIsSelected(SceneTree&, uint32_t, int32_t) {
  PureCall("+0x4C");
  return false;
}
int32_t AbstractItemCount(SceneTree&, uint32_t) {
  PureCall("+0x50");
  return 0;
}
int32_t AbstractGroupCountFromItems(SceneTree&, uint32_t) {
  PureCall("+0x54");
  return 0;
}
int32_t AbstractItemsInGroup(SceneTree&, uint32_t, int32_t) {
  PureCall("+0x58");
  return 0;
}
uint32_t AbstractAcquireItem(SceneTree&, uint32_t, int32_t) {
  PureCall("+0x5C");
  return 0;
}
uint32_t AbstractBuildGroup(SceneTree&, uint32_t, int32_t, int32_t, int32_t) {
  PureCall("+0x60");
  return 0;
}
void AbstractDescribeItem(SceneTree&, uint32_t, uint32_t, int32_t) {
  PureCall("+0x64");
}
uint32_t AbstractMakeGroupView(SceneTree&, uint32_t,
                               const std::vector<uint32_t>&, int32_t) {
  PureCall("+0x68");
  return 0;
}
uint32_t AbstractMakeTile(SceneTree&, uint32_t, uint32_t) {
  PureCall("+0x6C");
  return 0;
}
uint32_t AbstractBuildGroupView(SceneTree&, uint32_t, const uint32_t*, int32_t,
                                int32_t) {
  PureCall("+0x70");
  return 0;
}

// -- RefCounted, 0x92008A80 ---------------------------------------------------

// sub_920DB900
void DestroyRefCounted(SceneTree& tree, uint32_t scene, uint32_t deleting) {
  if (deleting & 1) {
    FreeGuestBytes(tree.guest(), scene);
  }
}

// -- SceneNode, 0x92008AD0 ----------------------------------------------------

// sub_920DB240
void RunSceneNodeDestructor(SceneTree& tree, uint32_t scene) {
  tree.ReleaseChildren(scene);
}

// sub_920DB3C0
void DestroySceneNode(SceneTree& tree, uint32_t scene, uint32_t deleting) {
  RunSceneNodeDestructor(tree, scene);
  if (deleting & 1) {
    FreeGuestBytes(tree.guest(), scene);
  }
}

// sub_922C3DC8
uint32_t ForwardEnumerableAtToSlot2C(SceneTree& tree, uint32_t scene,
                                     int32_t index) {
  const SceneOps* ops = SceneOpsForClass(tree.ClassIdOf(scene));
  return ops ? ops->answer_2C(tree, scene, index) : 0;
}

// sub_921A21A0
int32_t SceneNodeChildCount(SceneTree& tree, uint32_t scene) {
  return tree.Object(scene)->child_count;
}

// sub_920DAEB0
uint32_t SceneNodeChildAt(SceneTree& tree, uint32_t scene, int32_t index) {
  return tree.ChildAt(scene, index);
}

// sub_920DA600
int32_t ForwardMaterialisedCountToGroupCount(SceneTree& tree, uint32_t scene) {
  return tree.GroupCount(scene);
}

// sub_920DA610
uint32_t ForwardMaterialisedAtToGroupAt(SceneTree& tree, uint32_t scene,
                                        int32_t index) {
  return tree.GroupAt(scene, index);
}

// sub_920DAEE0
bool SceneNodeAnyChildMatches(SceneTree& tree, uint32_t scene) {
  const int32_t children = tree.MaterialisedCount(scene);
  for (int32_t child_index = 0; child_index < children; ++child_index) {
    const uint32_t child = tree.MaterialisedAt(scene, child_index);
    if (!child) {
      continue;
    }
    const int32_t items = tree.EnumerableCount(child);
    for (int32_t item = 0; item < items; ++item) {
      const uint32_t tile = tree.EnumerableAt(child, item);
      if (tile && (tree.TakeLatch(tile) & 0xFF) != 0) {
        return true;
      }
    }
  }
  return false;
}

// sub_920DAFC0
bool SceneNodeSeedPosition(SceneTree& tree, uint32_t scene, int32_t* group,
                           int32_t* item) {
  const int32_t groups = tree.GroupCount(scene);
  for (int32_t candidate_group = 0; candidate_group < groups;
       ++candidate_group) {
    const uint32_t row = tree.GroupAt(scene, candidate_group);
    if (!row) {
      continue;
    }
    const int32_t items = tree.EnumerableCount(row);
    for (int32_t candidate_item = 0; candidate_item < items; ++candidate_item) {
      const uint32_t tile = tree.EnumerableAt(row, candidate_item);
      if (tile && tree.IsSelected(tile, 0)) {
        *group = candidate_group;
        *item = candidate_item;
        return true;
      }
    }
  }
  return false;
}

// sub_920DAE50
int32_t SceneNodeTakeLatch(SceneTree& tree, uint32_t scene) {
  SceneObject* object = tree.Object(scene);
  const uint32_t taken = object->latch_10;
  object->latch_10 = 0;
  // The original folds `taken` through arithmetic that was never read out. Its
  // callers only test the low byte for non-zero, which the raw value answers.
  return static_cast<int32_t>(taken);
}

// sub_920DB0A8
void SceneNodeCopyName(SceneTree& tree, uint32_t scene, uint32_t destination,
                       uint32_t capacity_chars) {
  if (!destination || !capacity_chars) {
    return;
  }
  const SceneObject* object = tree.Object(scene);
  xe::be<uint16_t>* out = tree.guest().At<xe::be<uint16_t>>(destination);
  uint32_t copied = 0;
  while (copied + 1 < capacity_chars && copied < kSceneNameChars &&
         object->name[copied] != 0) {
    out[copied] = object->name[copied];
    ++copied;
  }
  out[copied] = 0;
}

// -- ScrollingList, 0x92008B50 ------------------------------------------------

// sub_920DB478
void RunScrollingListDestructor(SceneTree& tree, uint32_t scene) {
  RunSceneNodeDestructor(tree, scene);
}

// sub_920DB568
void DestroyScrollingList(SceneTree& tree, uint32_t scene, uint32_t deleting) {
  RunScrollingListDestructor(tree, scene);
  if (deleting & 1) {
    FreeGuestBytes(tree.guest(), scene);
  }
}

// sub_920DB488
void ScrollingListAttach(SceneTree& tree, uint32_t scene) {
  SceneObject* object = tree.Object(scene);
  object->first_item = 0;
  object->selected_group = 0;
}

// sub_920DB498
void ScrollingListDetach(SceneTree& tree, uint32_t scene) {
  tree.ReleaseChildren(scene);
}

// sub_920DB6B8
void ScrollingListScrollTo(SceneTree& tree, uint32_t scene, int32_t group) {
  SceneObject* object = tree.Object(scene);
  const int32_t groups = tree.GroupCountFromItems(scene);
  const int32_t window = object->visible_groups;

  int32_t first_group = 0;
  int32_t last_group = groups;
  if (groups > window) {
    first_group = group - window / 2;
    if (first_group > groups) {
      first_group = groups;
    }
    if (first_group < 0) {
      first_group = 0;
    }
    last_group = first_group + window;
    if (last_group > groups) {
      last_group = groups;
    }
    if (last_group < 0) {
      last_group = 0;
    }
    if (groups - first_group < window) {
      first_group = groups - window;
      last_group = groups;
    }
  }

  int32_t flat_base = 0;
  for (int32_t skipped = 0; skipped < first_group; ++skipped) {
    flat_base += tree.ItemsInGroup(scene, skipped);
  }

  std::vector<uint32_t> visible;
  int32_t selected_first_item = -1;
  for (int32_t wanted = first_group; wanted < last_group; ++wanted) {
    const int32_t items = tree.ItemsInGroup(scene, wanted);

    uint32_t view = 0;
    const int32_t built = object->child_count;
    for (int32_t i = 0; i < built; ++i) {
      const uint32_t child = tree.ChildAt(scene, i);
      // On a group view this slot answers with the group's own index, not with
      // a count; that is how an already-built group is recognised.
      if (child && tree.GroupCount(child) == wanted) {
        tree.AddRef(child);
        view = child;
        break;
      }
    }
    if (!view) {
      view = tree.BuildGroup(scene, wanted, flat_base, items);
    }
    if (view) {
      visible.push_back(view);
    }

    if (wanted == group) {
      selected_first_item = flat_base;
    }
    flat_base += items;
  }

  tree.ReleaseChildren(scene);
  tree.AdoptChildren(scene, visible);

  object->selected_group = group;
  object->first_item = groups ? selected_first_item : 0;
}

// sub_920DB4D0
int32_t ScrollingListGroupCount(SceneTree& tree, uint32_t scene) {
  return tree.GroupCountFromItems(scene);
}

// sub_920DB4E0
uint32_t ScrollingListGroupAt(SceneTree& tree, uint32_t scene, int32_t group) {
  const uint32_t first_child = tree.ChildAt(scene, 0);
  if (!first_child) {
    return 0;
  }
  const int32_t window_base = tree.GroupCount(first_child);
  return tree.ChildAt(scene, group - window_base);
}

// sub_920DB558
int32_t ScrollingListMaterialisedCount(SceneTree& tree, uint32_t scene) {
  return SceneNodeChildCount(tree, scene);
}

// sub_920DB560
uint32_t ScrollingListMaterialisedAt(SceneTree& tree, uint32_t scene,
                                     int32_t index) {
  return tree.ChildAt(scene, index);
}

// sub_920DB5C0
bool ScrollingListSeedPosition(SceneTree& tree, uint32_t scene, int32_t* group,
                               int32_t* item) {
  if (SceneNodeSeedPosition(tree, scene, group, item)) {
    return true;
  }

  const int32_t groups = tree.GroupCountFromItems(scene);
  int32_t flat_base = 0;
  for (int32_t candidate_group = 0; candidate_group < groups;
       ++candidate_group) {
    const int32_t items = tree.ItemsInGroup(scene, candidate_group);
    for (int32_t candidate_item = 0; candidate_item < items; ++candidate_item) {
      const uint32_t tile = tree.AcquireItem(scene, flat_base + candidate_item);
      if (!tile) {
        continue;
      }
      const bool selected = tree.IsSelected(tile, 0);
      // Released before the answer is used: the release is what can free it.
      tree.Release(tile);
      if (selected) {
        *group = candidate_group;
        *item = candidate_item;
        return true;
      }
    }
    flat_base += items;
  }
  return false;
}

// sub_920DDCF0
uint32_t ScrollingListAcquireItem(SceneTree& tree, uint32_t scene,
                                  int32_t flat_index) {
  const uint32_t descriptor =
      AllocateGuestBytes(tree.guest(), sizeof(ItemDescriptor));
  if (!descriptor) {
    return 0;
  }
  tree.DescribeItem(scene, descriptor, flat_index);
  const uint32_t tile = tree.MakeTile(scene, descriptor);
  FreeGuestBytes(tree.guest(), descriptor);
  return tile;
}

// -- ComponentList, 0x920095B0 ------------------------------------------------

// sub_920DDCE0
void RunComponentListDestructor(SceneTree& tree, uint32_t scene) {
  RunScrollingListDestructor(tree, scene);
}

// sub_920DDDD8
void DestroyComponentList(SceneTree& tree, uint32_t scene, uint32_t deleting) {
  RunComponentListDestructor(tree, scene);
  if (deleting & 1) {
    FreeGuestBytes(tree.guest(), scene);
  }
}

// sub_920D9060
int32_t ComponentListEnumerableCount(SceneTree& tree, uint32_t scene) {
  (void)scene;
  return ComponentCollectionIsReady(tree.guest()) ? 1 : 0;
}

// sub_920DDE30
uint32_t ComponentListBuildGroup(SceneTree& tree, uint32_t scene, int32_t group,
                                 int32_t flat_base, int32_t count) {
  std::vector<uint32_t> descriptors;
  descriptors.reserve(count > 0 ? static_cast<size_t>(count) : 0);
  for (int32_t i = 0; i < count; ++i) {
    const uint32_t descriptor =
        AllocateGuestBytes(tree.guest(), sizeof(ItemDescriptor));
    if (!descriptor) {
      break;
    }
    tree.DescribeItem(scene, descriptor, flat_base + i);
    descriptors.push_back(descriptor);
  }
  const uint32_t view = tree.MakeGroupView(scene, descriptors, group);
  for (uint32_t descriptor : descriptors) {
    FreeGuestBytes(tree.guest(), descriptor);
  }
  return view;
}

// -- ComponentListShell, 0x92009628 -------------------------------------------

// sub_920DDF88
void ComponentListShellAttach(SceneTree& tree, uint32_t scene) {
  ScrollingListAttach(tree, scene);
}

// sub_920DDF90
void ComponentListShellDetach(SceneTree& tree, uint32_t scene) {
  tree.ReleaseChildren(scene);
  // The second pass is in the image. It finds the array already null and
  // returns, so the effect is one release.
  ScrollingListDetach(tree, scene);
}

// -- CategoryGrid, 0x920086E8 -------------------------------------------------

// sub_920E6570
void DestroyCategoryGrid(SceneTree& tree, uint32_t scene, uint32_t deleting) {
  RunComponentListDestructor(tree, scene);
  if (deleting & 1) {
    FreeGuestBytes(tree.guest(), scene);
  }
}

// sub_920D9390
int32_t CategoryGridCategory(SceneTree& tree, uint32_t scene) {
  return tree.Object(scene)->category;
}

// sub_920D9540
int32_t CategoryGridItemCount(SceneTree& tree, uint32_t scene) {
  return ComponentCollectionCategorySize(tree.guest(),
                                         tree.Object(scene)->category);
}

// sub_920D8970
int32_t CategoryGridGroupCountFromItems(SceneTree& tree, uint32_t scene) {
  const int32_t items = tree.ItemCount(scene);
  return (items + static_cast<int32_t>(kGridItemsPerGroup) - 1) /
         static_cast<int32_t>(kGridItemsPerGroup);
}

// sub_920E3BA8
int32_t CategoryGridItemsInGroup(SceneTree& tree, uint32_t scene,
                                 int32_t group) {
  const int32_t items = tree.ItemCount(scene);
  const int32_t full_groups = items / static_cast<int32_t>(kGridItemsPerGroup);
  if (group < full_groups) {
    return static_cast<int32_t>(kGridItemsPerGroup);
  }
  return items - full_groups * static_cast<int32_t>(kGridItemsPerGroup);
}

// sub_920E6200
void CategoryGridDescribeItem(SceneTree& tree, uint32_t scene,
                              uint32_t descriptor, int32_t flat_index) {
  ItemDescriptor* out = tree.guest().At<ItemDescriptor>(descriptor);
  if (!out) {
    return;
  }
  const int32_t category = tree.Object(scene)->category;
  out->record = ComponentCollectionRecord(tree.guest(), category, flat_index);
  out->reserved_0C = 0;
  out->category = category;
  out->body_kind = tree.guest().Load32(kBuildBodyKindAddress);
}

// sub_920D9398
uint32_t CategoryGridMakeGroupView(SceneTree& tree, uint32_t scene,
                                   const std::vector<uint32_t>& descriptors,
                                   int32_t group) {
  const int32_t room = tree.ItemsInGroup(scene, group);

  uint32_t taken[kGridItemsPerGroup] = {};
  int32_t count = 0;
  while (count < static_cast<int32_t>(kGridItemsPerGroup) && count < room &&
         count < static_cast<int32_t>(descriptors.size())) {
    taken[count] = descriptors[static_cast<size_t>(count)];
    ++count;
  }

  return tree.BuildGroupView(scene, taken, count, group);
}

// -- CreatorGridScene, 0x9200AB18 ---------------------------------------------

// sub_920EF398
uint32_t CreatorGridMakeTile(SceneTree& tree, uint32_t scene,
                             uint32_t descriptor) {
  (void)scene;
  return MakeTileThroughFactory(tree.guest(), descriptor);
}

// Every asset id is sixteen bytes and the collection keeps the "no choice"
// id at 0x92017EF8.
bool RecordIsTheEmptyChoice(const Guest& guest, uint32_t record) {
  const GuestAssetRecord* asset = guest.At<GuestAssetRecord>(record);
  if (!asset) {
    return false;
  }
  return Collection(guest).AssetIdIsEmpty(asset->asset_id);
}

// sub_920E65C0, its first half
bool GroupOffersNothingButTheEmptyChoice(SceneTree& tree, uint32_t scene,
                                         const uint32_t* descriptors,
                                         int32_t count, int32_t group) {
  if (group != 0) {
    return false;
  }
  const int32_t items = tree.ItemCount(scene);
  if (items <= 0) {
    return true;
  }
  if (items != 1 || tree.Object(scene)->category == kCategoryClosetRecents) {
    return false;
  }
  if (count < 1 || !descriptors[0]) {
    return false;
  }
  const ItemDescriptor* first = tree.guest().At<ItemDescriptor>(descriptors[0]);
  return first && RecordIsTheEmptyChoice(tree.guest(), first->record);
}

// sub_920E9268, the view a category with nothing to offer gets instead
uint32_t BuildEmptyCategoryView(SceneTree& tree, uint32_t scene) {
  const uint32_t view =
      AllocateGuestBytes(tree.guest(), kEmptyCategoryViewBytes);
  if (!view) {
    return 0;
  }
  memset(tree.guest().At<uint8_t>(view), 0, kEmptyCategoryViewBytes);
  SceneObject* object = tree.Object(view);
  object->class_id = kClassEmptyCategoryView;
  object->ref_count = 1;
  object->category = tree.Category(scene);
  return view;
}

// sub_920E65C0
uint32_t CreatorGridBuildGroupView(SceneTree& tree, uint32_t scene,
                                   const uint32_t* descriptors, int32_t count,
                                   int32_t group) {
  uint32_t tiles[kGridItemsPerGroup] = {};
  for (int32_t i = 0; i < count && i < static_cast<int32_t>(kGridItemsPerGroup);
       ++i) {
    if (descriptors[i]) {
      tiles[i] = tree.MakeTile(scene, descriptors[i]);
    }
  }

  uint32_t view = 0;
  if (GroupOffersNothingButTheEmptyChoice(tree, scene, descriptors, count,
                                          group)) {
    view = BuildEmptyCategoryView(tree, scene);
  } else {
    view = AllocateGuestBytes(tree.guest(), sizeof(GroupViewObject));
    if (view) {
      GroupViewObject* object = tree.guest().At<GroupViewObject>(view);
      memset(object, 0, sizeof(*object));
      object->class_id = kClassGroupView;
      object->ref_count = 1;
      object->group = group;
      for (uint32_t i = 0; i < kGridItemsPerGroup; ++i) {
        object->tiles[i] = tiles[i];
        if (tiles[i]) {
          tree.AddRef(tiles[i]);
        }
      }
    }
  }

  // The tiles were made with a reference apiece; the view took its own, so
  // this drops the builder's whether or not a view was built.
  for (uint32_t i = 0; i < kGridItemsPerGroup; ++i) {
    if (tiles[i]) {
      tree.Release(tiles[i]);
    }
  }
  return view;
}

// -- GroupView, 0x9200BA28 ----------------------------------------------------

// sub_920EA630
void RunGroupViewDestructor(SceneTree& tree, uint32_t scene) {
  GroupViewObject* object = tree.guest().At<GroupViewObject>(scene);
  if (!object) {
    return;
  }
  for (uint32_t i = 0; i < kGridItemsPerGroup; ++i) {
    const uint32_t tile = object->tiles[i];
    if (tile) {
      object->tiles[i] = 0;
      tree.Release(tile);
    }
  }
}

// sub_920EA688
void DestroyGroupView(SceneTree& tree, uint32_t scene, uint32_t deleting) {
  RunGroupViewDestructor(tree, scene);
  if (deleting & 1) {
    FreeGuestBytes(tree.guest(), scene);
  }
}

// sub_92258AC8
int32_t GroupViewTileCount(SceneTree& tree, uint32_t scene) {
  (void)tree;
  (void)scene;
  return static_cast<int32_t>(kGridItemsPerGroup);
}

// sub_920E7DC0
uint32_t GroupViewTileAt(SceneTree& tree, uint32_t scene, int32_t index) {
  if (index < 0 || index >= static_cast<int32_t>(kGridItemsPerGroup)) {
    return 0;
  }
  const GroupViewObject* object = tree.guest().At<GroupViewObject>(scene);
  return object ? static_cast<uint32_t>(object->tiles[index]) : 0;
}

// sub_920DA5F8
int32_t GroupViewGroupIndex(SceneTree& tree, uint32_t scene) {
  const GroupViewObject* object = tree.guest().At<GroupViewObject>(scene);
  return object ? static_cast<int32_t>(object->group) : 0;
}

// -- AssetTile, 0x920097F0 and its subclasses ---------------------------------

// sub_920DC358
int32_t TileTakeLatch(SceneTree& tree, uint32_t scene) {
  const AssetTileFields* tile = tree.guest().At<AssetTileFields>(scene);
  return tile ? tile->new_badge : 0;
}

// sub_920DEFF0 with flag 0, whose comparison is against the live manifest
bool TileIsSelected(SceneTree& tree, uint32_t scene, int32_t flag) {
  const AssetTileFields* tile = tree.guest().At<AssetTileFields>(scene);
  if (!tile || flag) {
    return false;
  }

  const AvatarComponentInfo& info = tile->component_info;
  const uint8_t* manifest = tree.guest().At<uint8_t>(kLiveManifestAddress);
  if (!manifest) {
    return false;
  }

  const bool tile_is_the_empty_choice =
      Collection(tree.guest()).AssetIdIsEmpty(info.asset_id);

  const uint8_t* components = manifest + kManifestComponentsOffset;
  for (uint32_t i = 0; i < kManifestComponentCount; ++i) {
    const auto* entry = reinterpret_cast<const AvatarComponentInfo*>(
        components + i * kManifestEntryBytes);
    if (static_cast<uint16_t>(entry->type) !=
        static_cast<uint16_t>(info.type)) {
      continue;
    }
    return tile_is_the_empty_choice
               ? false
               : memcmp(entry, &info, sizeof(AvatarComponentInfo)) == 0;
  }

  // Nothing of this type is worn, so the tile that offers nothing is the one
  // showing the tick.
  return tile_is_the_empty_choice;
}

// -- StaticScene, 0x9200A2D8 --------------------------------------------------

// sub_920D98F8
void DestroyStaticScene(SceneTree& tree, uint32_t scene, uint32_t deleting) {
  RunSceneNodeDestructor(tree, scene);
  if (deleting & 1) {
    FreeGuestBytes(tree.guest(), scene);
  }
}

// -- the ops tables, derived exactly as the vtables are -----------------------

constexpr SceneOps MakeRefCountedOps() {
  SceneOps ops = {};
  ops.class_id = kClassRefCounted;
  ops.class_name = "RefCounted";
  ops.destroy = DestroyRefCounted;
  ops.attach = DoNothing;
  ops.detach = DoNothing;
  ops.scroll_to = DoNothingWithGroup;
  ops.slot_10 = AbstractSlot10;
  ops.enumerable_count = AbstractEnumerableCount;
  ops.enumerable_at = AbstractEnumerableAt;
  ops.group_count = AbstractGroupCount;
  ops.group_at = AbstractGroupAt;
  ops.materialised_count = AbstractMaterialisedCount;
  ops.materialised_at = AbstractMaterialisedAt;
  ops.answer_2C = AnswerZeroAtIndex;
  ops.any_child_matches = AnswerNoMatch;
  ops.answer_34 = AnswerZero;
  ops.category = AnswerZero;
  ops.seed_position = AnswerNoSeedPosition;
  ops.answer_40 = AbstractAnswer40;
  ops.take_latch = AbstractTakeLatch;
  ops.copy_name = AbstractCopyName;
  ops.is_selected = AbstractIsSelected;
  ops.item_count = AbstractItemCount;
  ops.group_count_from_items = AbstractGroupCountFromItems;
  ops.items_in_group = AbstractItemsInGroup;
  ops.acquire_item = AbstractAcquireItem;
  ops.build_group = AbstractBuildGroup;
  ops.describe_item = AbstractDescribeItem;
  ops.make_group_view = AbstractMakeGroupView;
  ops.make_tile = AbstractMakeTile;
  ops.build_group_view = AbstractBuildGroupView;
  return ops;
}

constexpr SceneOps MakeSceneNodeOps() {
  SceneOps ops = MakeRefCountedOps();
  ops.class_id = kClassSceneNode;
  ops.class_name = "SceneNode";
  ops.destroy = DestroySceneNode;
  ops.slot_10 = DoNothing;
  ops.enumerable_count = AnswerOne;
  ops.enumerable_at = ForwardEnumerableAtToSlot2C;
  ops.group_count = SceneNodeChildCount;
  ops.group_at = SceneNodeChildAt;
  ops.materialised_count = ForwardMaterialisedCountToGroupCount;
  ops.materialised_at = ForwardMaterialisedAtToGroupAt;
  ops.any_child_matches = SceneNodeAnyChildMatches;
  ops.seed_position = SceneNodeSeedPosition;
  ops.answer_40 = AnswerZero;
  ops.take_latch = SceneNodeTakeLatch;
  ops.copy_name = SceneNodeCopyName;
  ops.is_selected = AnswerNotSelected;
  return ops;
}

constexpr SceneOps MakeScrollingListOps() {
  SceneOps ops = MakeSceneNodeOps();
  ops.class_id = kClassScrollingList;
  ops.class_name = "ScrollingList";
  ops.destroy = DestroyScrollingList;
  ops.attach = ScrollingListAttach;
  ops.detach = ScrollingListDetach;
  ops.scroll_to = ScrollingListScrollTo;
  ops.group_count = ScrollingListGroupCount;
  ops.group_at = ScrollingListGroupAt;
  ops.materialised_count = ScrollingListMaterialisedCount;
  ops.materialised_at = ScrollingListMaterialisedAt;
  ops.seed_position = ScrollingListSeedPosition;
  ops.acquire_item = ScrollingListAcquireItem;
  return ops;
}

constexpr SceneOps MakeComponentListOps() {
  SceneOps ops = MakeScrollingListOps();
  ops.class_id = kClassComponentList;
  ops.class_name = "ComponentList";
  ops.destroy = DestroyComponentList;
  ops.enumerable_count = ComponentListEnumerableCount;
  ops.build_group = ComponentListBuildGroup;
  return ops;
}

constexpr SceneOps MakeComponentListShellOps() {
  SceneOps ops = MakeComponentListOps();
  ops.class_id = kClassComponentListShell;
  ops.class_name = "ComponentListShell";
  ops.attach = ComponentListShellAttach;
  ops.detach = ComponentListShellDetach;
  ops.answer_2C = AnswerOneAtIndex;
  return ops;
}

constexpr SceneOps MakeCategoryGridOps() {
  SceneOps ops = MakeComponentListShellOps();
  ops.class_id = kClassCategoryGrid;
  ops.class_name = "CategoryGrid";
  ops.destroy = DestroyCategoryGrid;
  ops.any_child_matches = AnswerNoMatch;
  ops.answer_34 = AnswerOne;
  ops.category = CategoryGridCategory;
  ops.item_count = CategoryGridItemCount;
  ops.group_count_from_items = CategoryGridGroupCountFromItems;
  ops.items_in_group = CategoryGridItemsInGroup;
  ops.describe_item = CategoryGridDescribeItem;
  ops.make_group_view = CategoryGridMakeGroupView;
  return ops;
}

constexpr SceneOps MakeCreatorGridSceneOps() {
  SceneOps ops = MakeCategoryGridOps();
  ops.class_id = kClassCreatorGridScene;
  ops.class_name = "CreatorGridScene";
  ops.make_tile = CreatorGridMakeTile;
  ops.build_group_view = CreatorGridBuildGroupView;
  return ops;
}

constexpr SceneOps MakeGroupViewOps() {
  SceneOps ops = MakeRefCountedOps();
  ops.class_id = kClassGroupView;
  ops.class_name = "GroupView";
  ops.destroy = DestroyGroupView;
  ops.slot_10 = DoNothing;
  ops.enumerable_count = GroupViewTileCount;
  ops.enumerable_at = GroupViewTileAt;
  ops.group_count = GroupViewGroupIndex;
  ops.materialised_count = AnswerZero;
  ops.materialised_at = AnswerZeroAtIndex;
  ops.answer_40 = AnswerZero;
  ops.take_latch = AnswerZero;
  ops.copy_name = DoNothingWithNameCopy;
  ops.is_selected = AnswerNotSelected;
  return ops;
}

constexpr SceneOps MakeEmptyCategoryViewOps() {
  SceneOps ops = MakeGroupViewOps();
  ops.class_id = kClassEmptyCategoryView;
  ops.class_name = "EmptyCategoryView";
  ops.destroy = DestroyRefCounted;
  ops.enumerable_count = AnswerZero;
  ops.enumerable_at = AnswerZeroAtIndex;
  ops.group_count = AnswerZero;
  ops.category = CategoryGridCategory;
  return ops;
}

constexpr SceneOps MakeBackgroundImageOps() {
  SceneOps ops = MakeRefCountedOps();
  ops.class_id = kClassBackgroundImage;
  ops.class_name = "BackgroundImage";
  ops.destroy = DestroyRefCounted;
  ops.slot_10 = DoNothing;
  ops.enumerable_count = AnswerZero;
  ops.enumerable_at = AnswerZeroAtIndex;
  ops.group_count = AnswerZero;
  ops.group_at = AnswerZeroAtIndex;
  ops.materialised_count = AnswerZero;
  ops.materialised_at = AnswerZeroAtIndex;
  ops.answer_40 = AnswerZero;
  ops.take_latch = AnswerZero;
  ops.copy_name = DoNothingWithNameCopy;
  ops.is_selected = AnswerNotSelected;
  return ops;
}

// The tiles a grid builds are widgets, not scenes, but the scene walks reach
// them through two of these slots and nothing else.
constexpr SceneOps MakeAssetTileOps() {
  SceneOps ops = MakeRefCountedOps();
  ops.class_id = kAssetTileVtable;
  ops.class_name = "AssetTile";
  ops.destroy = DestroyRefCounted;
  ops.slot_10 = DoNothing;
  ops.enumerable_count = AnswerZero;
  ops.enumerable_at = AnswerZeroAtIndex;
  ops.group_count = AnswerZero;
  ops.group_at = AnswerZeroAtIndex;
  ops.materialised_count = AnswerZero;
  ops.materialised_at = AnswerZeroAtIndex;
  ops.answer_40 = AnswerZero;
  ops.take_latch = TileTakeLatch;
  ops.copy_name = DoNothingWithNameCopy;
  ops.is_selected = TileIsSelected;
  return ops;
}

constexpr SceneOps MakeStaticSceneOps() {
  SceneOps ops = MakeSceneNodeOps();
  ops.class_id = kClassStaticScene;
  ops.class_name = "StaticScene";
  ops.destroy = DestroyStaticScene;
  // A menu screen's items ARE its buttons - it has no list behind it - so it
  // answers with its children rather than reporting a pure-virtual call once
  // a frame for something it is not missing.
  ops.item_count = SceneNodeChildCount;
  return ops;
}

constexpr SceneOps kRefCountedOps = MakeRefCountedOps();
constexpr SceneOps kSceneNodeOps = MakeSceneNodeOps();
constexpr SceneOps kScrollingListOps = MakeScrollingListOps();
constexpr SceneOps kComponentListOps = MakeComponentListOps();
constexpr SceneOps kComponentListShellOps = MakeComponentListShellOps();
constexpr SceneOps kCategoryGridOps = MakeCategoryGridOps();
constexpr SceneOps kCreatorGridSceneOps = MakeCreatorGridSceneOps();
constexpr SceneOps kStaticSceneOps = MakeStaticSceneOps();
constexpr SceneOps kGroupViewOps = MakeGroupViewOps();
constexpr SceneOps kEmptyCategoryViewOps = MakeEmptyCategoryViewOps();
constexpr SceneOps kAssetTileOps = MakeAssetTileOps();
constexpr SceneOps kBackgroundImageOps = MakeBackgroundImageOps();

}  // namespace

// sub_920FC228
uint32_t AllocateGuestBytes(const Guest& guest, uint32_t bytes) {
  if (!bytes) {
    return 0;
  }
  const uint32_t rounded = (bytes + 15) & ~15u;
  HeapHeader* heap = guest.At<HeapHeader>(kObjectHeapBase);
  if (!heap) {
    return 0;
  }
  if (!heap->cursor) {
    heap->cursor = kHeapPayloadBase;
  }

  uint32_t previous = 0;
  for (uint32_t block = heap->first_free; block;) {
    BlockHeader* header = guest.At<BlockHeader>(block);
    const uint32_t next = header->next_free;
    if (header->bytes >= rounded) {
      if (previous) {
        guest.At<BlockHeader>(previous)->next_free = next;
      } else {
        heap->first_free = next;
      }
      header->next_free = 0;
      return block + sizeof(BlockHeader);
    }
    previous = block;
    block = next;
  }

  const uint32_t block = (uint32_t(heap->cursor) + 15) & ~15u;
  const uint32_t end = block + sizeof(BlockHeader) + rounded;
  if (end > kObjectHeapBase + kObjectHeapSize) {
    XELOGE("avatar_editor: the editor heap is full, {} bytes refused", bytes);
    return 0;
  }
  heap->cursor = end;

  BlockHeader* header = guest.At<BlockHeader>(block);
  memset(header, 0, sizeof(*header));
  header->bytes = rounded;
  return block + sizeof(BlockHeader);
}

// sub_920FC260
void FreeGuestBytes(const Guest& guest, uint32_t address) {
  if (address < kHeapPayloadBase ||
      address >= kObjectHeapBase + kObjectHeapSize) {
    return;
  }
  HeapHeader* heap = guest.At<HeapHeader>(kObjectHeapBase);
  const uint32_t block = address - sizeof(BlockHeader);
  BlockHeader* header = guest.At<BlockHeader>(block);
  header->next_free = heap->first_free;
  heap->first_free = block;
}

const SceneOps* SceneOpsForClass(uint32_t class_id) {
  switch (class_id) {
    case kClassRefCounted:
      return &kRefCountedOps;
    case kClassSceneNode:
      return &kSceneNodeOps;
    case kClassScrollingList:
      return &kScrollingListOps;
    case kClassComponentList:
      return &kComponentListOps;
    case kClassComponentListShell:
      return &kComponentListShellOps;
    case kClassCategoryGrid:
      return &kCategoryGridOps;
    case kClassCreatorGridScene:
      return &kCreatorGridSceneOps;
    case kClassStaticScene:
      return &kStaticSceneOps;
    case kClassGroupView:
      return &kGroupViewOps;
    case kClassEmptyCategoryView:
      return &kEmptyCategoryViewOps;
    case kClassBackgroundImage:
      return &kBackgroundImageOps;
    case kClassMenuButton:
    case kAssetTileVtable:
    case kColourButtonTileVtable:
    case kSwatchTileVtable:
    case kMultiColourTileVtable:
      return &kAssetTileOps;
    default:
      return nullptr;
  }
}

uint32_t SceneTree::ClassIdOf(uint32_t scene) const {
  const SceneObject* object = Object(scene);
  return object ? static_cast<uint32_t>(object->class_id) : 0;
}

const SceneOps* SceneTree::OpsFor(uint32_t scene, const char* slot) const {
  const uint32_t class_id = ClassIdOf(scene);
  const SceneOps* ops = SceneOpsForClass(class_id);
  if (!ops) {
    XELOGE("avatar_editor: object {:08X} of class {:08X} has no ops for {}",
           scene, class_id, slot);
  }
  return ops;
}

// sub_920A9410
void SceneTree::AddRef(uint32_t scene) {
  SceneObject* object = Object(scene);
  if (object) {
    object->ref_count = object->ref_count + 1;
  }
}

// sub_920A9430
void SceneTree::Release(uint32_t scene) {
  SceneObject* object = Object(scene);
  if (!object) {
    return;
  }
  const uint32_t remaining = object->ref_count - 1;
  object->ref_count = remaining;
  if (!remaining) {
    Destroy(scene, 1);
  }
}

// sub_920DAE38
void SceneTree::ConstructRefCounted(uint32_t scene) {
  SceneObject* object = Object(scene);
  object->ref_count = 1;
  object->class_id = kClassRefCounted;
}

// sub_920DB0E0
void SceneTree::ConstructSceneNode(uint32_t scene, uint32_t name_key) {
  ConstructRefCounted(scene);
  SceneObject* object = Object(scene);
  object->children = 0;
  object->child_count = 0;
  object->latch_10 = 0;
  SetName(scene, name_key);
  object->class_id = kClassSceneNode;
}

// sub_920DB420
void SceneTree::ConstructScrollingList(uint32_t scene, uint32_t name_key,
                                       int32_t visible_groups) {
  ConstructSceneNode(scene, name_key);
  SceneObject* object = Object(scene);
  object->visible_groups = visible_groups;
  object->first_item = 0;
  object->selected_group = 0;
  object->class_id = kClassScrollingList;
}

// sub_920DDCA0
void SceneTree::ConstructComponentList(uint32_t scene, uint32_t name_key) {
  ConstructScrollingList(scene, name_key, kComponentListVisibleGroups);
  Object(scene)->class_id = kClassComponentList;
}

// sub_920DDF48
void SceneTree::ConstructComponentListShell(uint32_t scene, uint32_t name_key) {
  ConstructComponentList(scene, name_key);
  Object(scene)->class_id = kClassComponentListShell;
}

// sub_920D9340
void SceneTree::ConstructCategoryGrid(uint32_t scene, uint32_t name_key,
                                      int32_t category) {
  ConstructComponentListShell(scene, name_key);
  SceneObject* object = Object(scene);
  object->category = category;
  object->class_id = kClassCategoryGrid;
}

// sub_920E64B8, sub_920E6510
void SceneTree::ConstructCreatorGridScene(uint32_t scene, uint32_t name_key,
                                          int32_t category) {
  ConstructCategoryGrid(scene, name_key, category);
  SceneObject* object = Object(scene);
  object->tile_factory_class_id = kClassCreatorGridTileFactory;
  object->class_id = kClassCreatorGridScene;
}

// the 0x9200A2D8 constructors, e.g. sub_920D9230
//
// Each of them builds one child - sub_920E38A0's sub_920EA1A8, sub_920DA0E0's
// sub_920E8D98, and so on - and that child is the menu: its slots hold the
// buttons. Nothing reads the container itself, so the buttons are adopted
// straight onto the screen, where the navigation already looks for them.
void SceneTree::ConstructStaticScene(uint32_t scene, uint32_t name_key) {
  ConstructStaticScene(scene, name_key, 0);
}

void SceneTree::ConstructStaticScene(uint32_t scene, uint32_t name_key,
                                     uint32_t command) {
  ConstructSceneNode(scene, name_key);
  Object(scene)->class_id = kClassStaticScene;

  const MenuScreen* menu = command ? MenuScreenForCommand(command)
                                   : MenuScreenForTitleString(name_key);
  if (!menu) {
    return;
  }
  const uint32_t body_type = ComponentCollection(guest_).body_kind() != 0
                                 ? kBodyTypeSecond
                                 : kBodyTypeFirst;

  std::vector<uint32_t> buttons;
  buttons.reserve(menu->entry_count);
  for (uint32_t i = 0; i < menu->entry_count; ++i) {
    const MenuEntry& entry = menu->entries[i];
    const uint32_t button = AllocateGuestBytes(guest_, kAssetTileBytes);
    if (!button) {
      break;
    }
    memset(guest_.At<uint8_t>(button), 0, kAssetTileBytes);
    SceneObject* header = Object(button);
    header->class_id = kClassMenuButton;
    header->ref_count = 1;

    AssetTileFields* fields = guest_.At<AssetTileFields>(button);
    const bool takes_alternate =
        entry.alternate_element && entry.alternate_body_type == body_type;
    fields->activate_command =
        takes_alternate ? entry.alternate_command : entry.command;
    // A menu button wears no component, and a zero type would match the
    // manifest's unused slots when the press commits.
    fields->component_info.type = 0xFFFF;
    buttons.push_back(button);
  }
  AdoptChildren(scene, buttons);
}

// sub_921059A0, as sub_920DB0E0 calls it
void SceneTree::SetName(uint32_t scene, uint32_t name_key) {
  SceneObject* object = Object(scene);
  const uint32_t source = SceneNameForKey(name_key);
  const xe::be<uint16_t>* text =
      source ? guest_.At<xe::be<uint16_t>>(source) : nullptr;
  uint32_t copied = 0;
  while (text && copied + 1 < kSceneNameChars && text[copied] != 0) {
    object->name[copied] = text[copied];
    ++copied;
  }
  object->name[copied] = 0;
}

void SceneTree::Destroy(uint32_t scene, uint32_t deleting) {
  const SceneOps* ops = OpsFor(scene, "destroy");
  if (ops) {
    ops->destroy(*this, scene, deleting);
  }
}

void SceneTree::Attach(uint32_t scene) {
  const SceneOps* ops = OpsFor(scene, "attach");
  if (ops) {
    ops->attach(*this, scene);
  }
}

void SceneTree::Detach(uint32_t scene) {
  const SceneOps* ops = OpsFor(scene, "detach");
  if (ops) {
    ops->detach(*this, scene);
  }
}

void SceneTree::ScrollTo(uint32_t scene, int32_t group) {
  const SceneOps* ops = OpsFor(scene, "scroll_to");
  if (ops) {
    ops->scroll_to(*this, scene, group);
  }
}

int32_t SceneTree::EnumerableCount(uint32_t scene) {
  const SceneOps* ops = OpsFor(scene, "enumerable_count");
  return ops ? ops->enumerable_count(*this, scene) : 0;
}

uint32_t SceneTree::EnumerableAt(uint32_t scene, int32_t index) {
  const SceneOps* ops = OpsFor(scene, "enumerable_at");
  return ops ? ops->enumerable_at(*this, scene, index) : 0;
}

int32_t SceneTree::GroupCount(uint32_t scene) {
  const SceneOps* ops = OpsFor(scene, "group_count");
  return ops ? ops->group_count(*this, scene) : 0;
}

uint32_t SceneTree::GroupAt(uint32_t scene, int32_t group) {
  const SceneOps* ops = OpsFor(scene, "group_at");
  return ops ? ops->group_at(*this, scene, group) : 0;
}

int32_t SceneTree::MaterialisedCount(uint32_t scene) {
  const SceneOps* ops = OpsFor(scene, "materialised_count");
  return ops ? ops->materialised_count(*this, scene) : 0;
}

uint32_t SceneTree::MaterialisedAt(uint32_t scene, int32_t index) {
  const SceneOps* ops = OpsFor(scene, "materialised_at");
  return ops ? ops->materialised_at(*this, scene, index) : 0;
}

bool SceneTree::AnyChildMatches(uint32_t scene) {
  const SceneOps* ops = OpsFor(scene, "any_child_matches");
  return ops ? ops->any_child_matches(*this, scene) : false;
}

int32_t SceneTree::Category(uint32_t scene) {
  const SceneOps* ops = OpsFor(scene, "category");
  return ops ? ops->category(*this, scene) : 0;
}

bool SceneTree::SeedPosition(uint32_t scene, int32_t* group, int32_t* item) {
  const SceneOps* ops = OpsFor(scene, "seed_position");
  return ops ? ops->seed_position(*this, scene, group, item) : false;
}

int32_t SceneTree::TakeLatch(uint32_t scene) {
  const SceneOps* ops = OpsFor(scene, "take_latch");
  return ops ? ops->take_latch(*this, scene) : 0;
}

void SceneTree::CopyName(uint32_t scene, uint32_t destination,
                         uint32_t capacity_chars) {
  const SceneOps* ops = OpsFor(scene, "copy_name");
  if (ops) {
    ops->copy_name(*this, scene, destination, capacity_chars);
  }
}

bool SceneTree::IsSelected(uint32_t scene, int32_t flag) {
  const SceneOps* ops = OpsFor(scene, "is_selected");
  return ops ? ops->is_selected(*this, scene, flag) : false;
}

int32_t SceneTree::ItemCount(uint32_t scene) {
  const SceneOps* ops = OpsFor(scene, "item_count");
  return ops ? ops->item_count(*this, scene) : 0;
}

int32_t SceneTree::GroupCountFromItems(uint32_t scene) {
  const SceneOps* ops = OpsFor(scene, "group_count_from_items");
  return ops ? ops->group_count_from_items(*this, scene) : 0;
}

int32_t SceneTree::ItemsInGroup(uint32_t scene, int32_t group) {
  const SceneOps* ops = OpsFor(scene, "items_in_group");
  return ops ? ops->items_in_group(*this, scene, group) : 0;
}

uint32_t SceneTree::AcquireItem(uint32_t scene, int32_t flat_index) {
  const SceneOps* ops = OpsFor(scene, "acquire_item");
  return ops ? ops->acquire_item(*this, scene, flat_index) : 0;
}

uint32_t SceneTree::BuildGroup(uint32_t scene, int32_t group, int32_t flat_base,
                               int32_t count) {
  const SceneOps* ops = OpsFor(scene, "build_group");
  return ops ? ops->build_group(*this, scene, group, flat_base, count) : 0;
}

void SceneTree::DescribeItem(uint32_t scene, uint32_t descriptor,
                             int32_t flat_index) {
  const SceneOps* ops = OpsFor(scene, "describe_item");
  if (ops) {
    ops->describe_item(*this, scene, descriptor, flat_index);
  }
}

uint32_t SceneTree::MakeGroupView(uint32_t scene,
                                  const std::vector<uint32_t>& descriptors,
                                  int32_t group) {
  const SceneOps* ops = OpsFor(scene, "make_group_view");
  return ops ? ops->make_group_view(*this, scene, descriptors, group) : 0;
}

uint32_t SceneTree::MakeTile(uint32_t scene, uint32_t descriptor) {
  const SceneOps* ops = OpsFor(scene, "make_tile");
  return ops ? ops->make_tile(*this, scene, descriptor) : 0;
}

uint32_t SceneTree::BuildGroupView(uint32_t scene, const uint32_t* descriptors,
                                   int32_t count, int32_t group) {
  const SceneOps* ops = OpsFor(scene, "build_group_view");
  return ops ? ops->build_group_view(*this, scene, descriptors, count, group)
             : 0;
}

// sub_920DAEB0
uint32_t SceneTree::ChildAt(uint32_t scene, int32_t index) const {
  const SceneObject* object = Object(scene);
  if (!object || index < 0 || index >= object->child_count) {
    return 0;
  }
  const uint32_t children = object->children;
  return children ? guest_.Load32(children + index * 4) : 0;
}

// sub_920DB1C0
void SceneTree::ReleaseChildren(uint32_t scene) {
  SceneObject* object = Object(scene);
  if (!object || !object->children) {
    return;
  }
  const int32_t count = object->child_count;
  for (int32_t i = 0; i < count; ++i) {
    const uint32_t slot = object->children + i * 4;
    const uint32_t child = guest_.Load32(slot);
    if (child) {
      Release(child);
      guest_.Store32(slot, 0);
    }
  }
  FreeGuestBytes(guest_, object->children);
  object->children = 0;
  object->child_count = 0;
}

// sub_920DB280
void SceneTree::AdoptChildren(uint32_t scene,
                              const std::vector<uint32_t>& children) {
  SceneObject* object = Object(scene);
  if (!object) {
    return;
  }
  if (children.empty()) {
    object->children = 0;
    object->child_count = 0;
    return;
  }
  const uint32_t array = AllocateGuestBytes(
      guest_, static_cast<uint32_t>(children.size()) * sizeof(uint32_t));
  if (!array) {
    object->children = 0;
    object->child_count = 0;
    return;
  }
  for (size_t i = 0; i < children.size(); ++i) {
    guest_.Store32(array + static_cast<uint32_t>(i) * 4, children[i]);
  }
  object->children = array;
  object->child_count = static_cast<int32_t>(children.size());
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
