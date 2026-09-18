/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_avatar.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

#include "xenia/base/logging.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/avatar_editor/editor_input.h"
#include "xenia/kernel/xam/avatar_editor/editor_session.h"
#include "xenia/kernel/xam/avatar_editor/navigation.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

namespace avatar = xna::avatar;
namespace editor = avatar_editor;

constexpr const char* kPackageName = "avatareditor";
constexpr const char* kFrameScene = "EditorMain.xur";
// The closet grid: 8 square tiles, odd down the left column and even down the
// right, with the avatar standing between them. GridMain is the box-art
// layout the marketplace uses, not this.
constexpr const char* kGridScene = "Grid.xur";
constexpr const char* kBackgroundScene = "EditorBG.xur";
constexpr const char* kSkinScene = "EditorSkin.xur";
// Grid1x1ItemButton, the tile's own Visual, carries NO art - its images have
// no ImagePath and its preview/chosen are presenters the console fills with
// textures it renders itself. This one at least has the bg_box figure.
constexpr const char* kTileVisual = "GridOffscreenItemButton";

// EditorMain and GridMain are authored at this size, not the 852x480 the
// flash scenes use.
constexpr float kEditorWidth = 1280.0f;
constexpr float kEditorHeight = 720.0f;
constexpr float kGridWidth = 1016.0f;

constexpr float kLegendHeight = 42.0f;

constexpr uint32_t kLegendBand = 0xE0201814;
constexpr uint32_t kLegendText = 0xFFF0F0F0;
constexpr uint32_t kLegendHint = 0xFFA0A0A0;
constexpr uint32_t kPreviewTint = 0xFFFFFFFF;
constexpr uint32_t kCalloutFill = 0xD8201814;
// The menu's own text sits on the green backdrop, not on a band.
constexpr uint32_t kMenuHeading = 0xFF303028;
constexpr uint32_t kMenuCaption = 0xFFF8F8F8;
constexpr uint32_t kMenuCaptionBand = 0xC0201814;
constexpr uint32_t kMenuFocusRing = 0xFFFFFFFF;

// The editor's captions, by the ordinal the image's constructors carry.
constexpr const char* kStringTable = "Strings.xus";
constexpr uint32_t kStringTableHeaderBytes = 0x0C;

// The menu's tiles: four across the top row, the rest under them, with the
// focused one grown the way the console grows it.
constexpr size_t kMenuTopRow = 4;
constexpr float kMenuTileWidth = 132.0f;
constexpr float kMenuTileHeight = 214.0f;
constexpr float kMenuTileGap = 14.0f;
constexpr float kMenuFocusScale = 1.32f;

constexpr uint32_t kPreviewSize = 512;
constexpr uint32_t kIdleClip = 3;

// An item tile's own picture, rendered offscreen the way the console's
// GridOffscreenItemButton does.
constexpr uint32_t kThumbnailSize = 128;
// The square the picture is drawn in, as a fraction of the tile, so it sits
// inside the tile's border rather than over it.
constexpr float kThumbnailInset = 0.82f;
// How many tiles may be rendered in one frame. Turning a page asks for eight
// at once; they arrive over the next few frames instead of stalling one.
constexpr uint32_t kThumbnailsPerFrame = 2;
// Beyond this the oldest pictures are dropped and rendered again if the player
// comes back to them.
constexpr size_t kThumbnailLimit = 192;
// What the offscreen renderer leaves where nothing was drawn. Those pixels
// become transparent so the tile's own background shows through.
constexpr uint8_t kThumbnailClear[3] = {38, 41, 48};

// Whether an item's picture needs the head under it. A face texture has no
// model of its own and hair, hats and glasses read as nothing without one;
// everything else is a garment that shows its own shape.
bool ItemShowsHead(uint32_t slot) {
  switch (slot) {
    case avatar::kSlotHair:
    case avatar::kSlotHat:
    case avatar::kSlotGlasses:
    case avatar::kSlotEarrings:
    case avatar::kSlotEyes:
    case avatar::kSlotEyebrows:
    case avatar::kSlotMouth:
    case avatar::kSlotFacialHair:
    case avatar::kSlotFacePaint:
    case avatar::kSlotEyeShadow:
      return true;
    default:
      return false;
  }
}

// The grid reads its items out of guest memory and converts each name, so it
// takes a window onto a category rather than all of it.
constexpr uint32_t kMaxCategoryItems = 512;

void Walk(const Element& element,
          const std::function<void(const Element&)>& fn) {
  fn(element);
  for (const Element& child : element.children) {
    Walk(child, fn);
  }
}

const Element* FindIn(const Element& root, const std::string& id) {
  const Element* found = nullptr;
  Walk(root, [&](const Element& element) {
    if (!found && element.id == id) {
      found = &element;
    }
  });
  return found;
}

}  // namespace

const Element* EditorScreenBase::Find(const std::string& id) const {
  return FindIn(root_, id);
}

bool EditorScreenBase::Prepare(AssetStore& assets) {
  design_width_ = kEditorWidth;
  design_height_ = kEditorHeight;

  package_ = assets.GetPackage(kPackageName);
  if (!package_) {
    XELOGW("xui avatar: no {} package installed", kPackageName);
    return false;
  }
  shared_ = assets.GetPackage("sharedres");
  common_ = assets.GetPackage("avatarcommon");
  controlpack_ = assets.GetPackage("avatarcontrolpack");
  LoadStrings();

  // EditorSkin is where the controls keep their appearance - the item tile
  // visual GriBoxArtHeightButton, the button backgrounds, and so on. The
  // flash skin has none of them.
  if (const PackageEntry* entry = package_->Find(kSkinScene)) {
    skin_data_.assign(entry->data, entry->data + entry->size);
    if (skin_.Load(skin_data_.data(), skin_data_.size())) {
      ParseFigurePaths(nullptr, 0, &skin_figures_);
      if (const Scene::Section* custom = skin_.scene().FindSection("CUST")) {
        ParseFigurePaths(custom->data, custom->size, &skin_figures_);
      }
    }
  }
  const Skin* skin = skin_.loaded() ? &skin_ : assets.skin();

  const Layout layout(skin);

  // The closet backdrop - mirror, sheen, border, the gradient figures.
  if (Scene* background = package_->Open(kBackgroundScene)) {
    if (const Scene::Section* custom = background->FindSection("CUST")) {
      ParseFigurePaths(custom->data, custom->size, &background_figures_);
    }
    background_ = layout.Build(background->root(), 0.0f, 0.0f, 1.0f);
  }

  Scene* frame = package_->Open(kFrameScene);
  if (!frame) {
    XELOGW("xui avatar: {} is not in the package", kFrameScene);
    return false;
  }
  if (const Scene::Section* custom = frame->FindSection("CUST")) {
    ParseFigurePaths(custom->data, custom->size, &figures_);
  }

  root_ = layout.Build(frame->root(), 0.0f, 0.0f, 1.0f);
  stage_ = Find("scnMain");

  Scene* grid = package_->Open(kGridScene);
  if (!grid) {
    XELOGW("xui avatar: {} is not in the package", kGridScene);
    return false;
  }
  if (const Scene::Section* custom = grid->FindSection("CUST")) {
    ParseFigurePaths(custom->data, custom->size, &grid_figures_);
  }
  // The grid is a 1016-wide scene centred in the 1280-wide frame; its two
  // columns sit either side of the avatar.
  grid_ = layout.Build(grid->root(), (kEditorWidth - kGridWidth) * 0.5f, 0.0f,
                       1.0f);
  category_label_ = FindIn(grid_, "Category name");

  CollectSlots();
  if (slots_.empty()) {
    XELOGW("xui avatar: {} held no item slots", kGridScene);
    return false;
  }

  // Every tile shares one authored template, so they share its states and
  // differ only in where their playheads are. They go through the same host
  // the dashboard's scenes use, so a tile gains and loses focus by exactly
  // the same path a dashboard control does.
  skin_scene_ = &skin->scene();
  tile_visual_ = skin->Find(kTileVisual);
  Overlay* overlay = SharedOverlay();
  lua_ = overlay ? overlay->lua() : nullptr;
  if (lua_ && tile_visual_) {
    lua_->SetSkin(skin);
    // The tree is the grid's, so its own scene is what its pools come from.
    // The tile playheads are attached separately because their states live in
    // the skin, whose pools are its own.
    lua_->SetSceneRoot(grid, &grid_, kGridScene);
    for (Element* slot : slots_) {
      if (TimelinePlayer* player =
              lua_->AttachPlayer(slot, skin_scene_, tile_visual_)) {
        player->Play(kStateNormal);
      }
    }
  }
  // The first button press arrives before the first frame is drawn, and the
  // focus it moves is the one in frame_.
  UpdateFrame();
  return true;
}

// A kind-2 .xus: a header, then one NUL-terminated UTF-8 string per ordinal.
void EditorScreenBase::LoadStrings() {
  strings_.clear();
  const PackageEntry* entry = package_ ? package_->Find(kStringTable) : nullptr;
  if (!entry || entry->size <= kStringTableHeaderBytes) {
    XELOGW("xui avatar: {} is not in the package", kStringTable);
    return;
  }
  const char* at =
      reinterpret_cast<const char*>(entry->data) + kStringTableHeaderBytes;
  const char* end = reinterpret_cast<const char*>(entry->data) + entry->size;
  while (at < end) {
    const char* stop = at;
    while (stop < end && *stop) {
      ++stop;
    }
    strings_.emplace_back(at, size_t(stop - at));
    at = stop + 1;
  }
}

std::string EditorScreenBase::EditorText(uint32_t ordinal) const {
  if (ordinal >= strings_.size()) {
    return std::string();
  }
  std::string text = strings_[ordinal];
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  return text;
}

std::string EditorScreenBase::Gamertag() const {
  auto* state = kernel_state() ? kernel_state()->xam_state() : nullptr;
  if (!state) {
    return std::string();
  }
  for (uint32_t user = 0; user < XUserMaxUserCount; ++user) {
    if (!state->IsUserSignedIn(user)) {
      continue;
    }
    if (UserProfile* profile = state->GetUserProfile(user)) {
      return profile->name();
    }
  }
  return std::string();
}

void EditorScreenBase::CollectSlots() {
  slots_.clear();
  // The grid is eight fixed ItemButton slots, not a list: fill and page.
  for (int i = 1; i <= int(kSlotLimit); ++i) {
    const std::string id = "ItemButton" + std::to_string(i);
    Element* found = nullptr;
    Walk(grid_, [&](const Element& element) {
      // The scene wraps each button in a scene of the same id; take the
      // XuiButton, which is the one with the real rect.
      if (element.id == id && element.class_name == "XuiButton") {
        found = const_cast<Element*>(&element);
      }
    });
    if (found) {
      slots_.push_back(found);
    }
  }
}

int EditorScreenBase::SlotInDirection(size_t from, int dx, int dy) const {
  if (from >= slots_.size()) {
    return -1;
  }
  const Rect& start = slots_[from]->rect;
  const float from_x = start.x + start.width * 0.5f;
  const float from_y = start.y + start.height * 0.5f;

  int best = -1;
  float best_score = 0.0f;
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (i == from || !frame_.tile_used[i]) {
      continue;
    }
    const Rect& to = slots_[i]->rect;
    const float to_x = to.x + to.width * 0.5f;
    const float to_y = to.y + to.height * 0.5f;
    const float along = (to_x - from_x) * dx + (to_y - from_y) * dy;
    if (along <= 0.5f) {
      continue;
    }
    const float across =
        std::abs((to_x - from_x) * dy) + std::abs((to_y - from_y) * dx);
    const float score = along + across * 3.0f;
    if (best < 0 || score < best_score) {
      best = int(i);
      best_score = score;
    }
  }
  return best;
}

void EditorScreenBase::SetPreviewDescription(
    const avatar::Description& description) {
  // The tile pictures are rendered in the avatar's own body and colours, so
  // they only go stale when one of those changes - not when an item is worn.
  if (description.body != description_.body ||
      description.colors != description_.colors) {
    thumbnails_.clear();
  }
  description_ = description;
  preview_dirty_ = true;
}

void EditorScreenBase::Rotate(float degrees) {
  // The preview takes its facing in radians; adding degrees straight into it
  // turned the avatar about fifty-seven times too fast.
  yaw_ += degrees * 3.14159265f / 180.0f;
  preview_dirty_ = true;
}

void EditorScreenBase::Commit() {
  committed_ = true;
  closed_ = true;
}

void EditorScreenBase::Cancel() {
  cancelled_ = true;
  closed_ = true;
}

void EditorScreenBase::RebuildPreview() {
  preview_dirty_ = false;
  preview_.clear();
  preview_width_ = 0;
  preview_height_ = 0;
  if (!catalog_) {
    return;
  }
  const avatar::Scene scene = avatar::BuildScene(*catalog_, description_);
  avatar::Matrix local[avatar::kMaxJoints];
  avatar::BindPose(avatar::MainSkeleton(), local);
  // Clip 3 is the standing idle; the bind pose is a T-pose and the console
  // never shows it.
  if (auto clip = catalog_->LoadClip(kIdleClip)) {
    avatar::SamplePose(*clip, avatar::MainSkeleton(), 0.0f, local);
  }
  avatar::RenderPreview(scene, local, nullptr, avatar::Expression(),
                        kPreviewSize, kPreviewSize, yaw_, &preview_);
  if (preview_.size() >= size_t(kPreviewSize) * kPreviewSize * 4) {
    preview_width_ = kPreviewSize;
    preview_height_ = kPreviewSize;
  }
}

Rect EditorScreenBase::DrawArt(class Draw& draw, const std::string& name,
                               const Rect& rect, bool desaturate,
                               bool fill_width) {
  const PackageEntry* art = package_ ? package_->Find(name) : nullptr;
  if (!art) {
    return Rect();
  }
  int width = 0;
  int height = 0;
  xe::ui::ImmediateTexture* texture = draw.ImageTexture(
      name, art->data, art->size, &width, &height, desaturate);
  if (!texture || width <= 0 || height <= 0) {
    return Rect();
  }
  // A tile's picture spans the tile and takes whatever height its own shape
  // asks for; fitting it inside the whole tile instead left the panel showing
  // underneath it as a grey band.
  const float scale = fill_width ? rect.width / float(width)
                                 : std::min(rect.width / float(width),
                                            rect.height / float(height));
  Rect fit;
  fit.width = float(width) * scale;
  fit.height = float(height) * scale;
  fit.x = rect.x + (rect.width - fit.width) * 0.5f;
  fit.y = fill_width ? rect.y : rect.y + (rect.height - fit.height) * 0.5f;
  draw.DrawImage(texture, fit, kPreviewTint);
  return fit;
}

// The editor names its assets by the catalogue's own 16-byte ids, so the
// catalogue answers which entry a tile stands for. Built once, on the first
// tile that asks.
uint32_t EditorScreenBase::CatalogEntryForAsset(const uint8_t* asset_id) {
  if (!catalog_ || !asset_id) {
    return kNoEntry;
  }
  if (!catalog_assets_built_) {
    catalog_assets_built_ = true;
    // Keyed by the id the editor was actually handed, not the pack's own
    // bytes: two thirds of the entries carry none, and those that do share
    // them between variant rows, so a map on those collapsed 877 entries onto
    // one key and answered for the wrong one.
    for (const avatar::Entry& entry : catalog_->entries()) {
      catalog_assets_.emplace(avatar::ManifestAssetId(entry), entry.index);
    }
  }
  std::array<uint8_t, 16> key;
  std::memcpy(key.data(), asset_id, key.size());
  const auto found = catalog_assets_.find(key);
  if (found != catalog_assets_.end()) {
    return found->second;
  }
  // MEASUREMENT: an id the catalogue does not carry means the editor and the
  // catalogue are not speaking about the same assets. Said once per distinct
  // id, not once a frame. Delete once it has answered.
  if (unmatched_assets_.insert(key).second) {
    std::string hex;
    for (uint8_t byte : key) {
      hex += fmt::format("{:02X}", byte);
    }
    XELOGW("avatar editor: asset {} is in no catalogue entry ({} known)", hex,
           catalog_assets_.size());
  }
  return kNoEntry;
}

// The item alone, wearing nothing else and standing in nothing else, so the
// renderer's own framing fills the picture with it. A face texture has no
// model, and hair, hats and glasses want something to sit on, so those come
// with the head.
void EditorScreenBase::RenderThumbnail(uint32_t entry_index,
                                       ItemThumbnail* out) {
  const avatar::Entry* entry = catalog_ ? catalog_->Find(entry_index) : nullptr;
  if (!entry) {
    return;
  }
  // A face texture - eyes, eyebrows, a mouth - covers no worn slot, so
  // PrimarySlot answers -1 for it. It is not unshowable: it goes on the head,
  // as a component, which is where the manifest keeps it too.
  const int32_t slot = avatar::PrimarySlot(entry->kind);
  // A chin, a nose or a pair of ears is a blend shape, not a component and not
  // a slot - it reshapes the head it is shown on.
  const int32_t shape = avatar::BlendShapeSlot(entry->kind);

  avatar::Description worn;
  worn.body = description_.body;
  worn.height = description_.height;
  worn.weight = description_.weight;
  worn.height_bits = description_.height_bits;
  worn.weight_bits = description_.weight_bits;
  // The skin, hair and iris colours are the editor's, so a head-only picture
  // is the player's head rather than a grey one.
  worn.colors = description_.colors;
  worn.blend_shapes = description_.blend_shapes;
  if (slot >= 0) {
    avatar::PlaceItem(*catalog_, &worn, uint32_t(slot), uint16_t(entry_index));
  } else if (shape >= 0) {
    worn.blend_shapes[uint32_t(shape)] = avatar::ManifestAssetId(*entry);
  } else {
    std::array<uint8_t, 32> component = {};
    const std::array<uint8_t, 16> asset_id = avatar::ManifestAssetId(*entry);
    std::memcpy(component.data(), asset_id.data(), asset_id.size());
    // The type is the component's own sixteen-bit field, big-endian like the
    // rest of the manifest.
    component[16] = uint8_t(entry->kind >> 8);
    component[17] = uint8_t(entry->kind);
    worn.components.push_back(component);
  }

  avatar::Scene scene = avatar::BuildScene(*catalog_, worn);
  // A face texture has nothing of its own to look at, so it is always shown
  // on the head.
  const bool with_head = slot < 0 || ItemShowsHead(uint32_t(slot));
  std::vector<avatar::Part> shown;
  for (avatar::Part& part : scene.parts) {
    const bool is_item = part.entry == entry_index;
    const bool is_head = part.kind == avatar::kKindHead;
    if (is_item || (with_head && is_head)) {
      shown.push_back(std::move(part));
    }
  }
  scene.parts = std::move(shown);
  if (scene.parts.empty()) {
    return;
  }

  avatar::Matrix local[avatar::kMaxJoints];
  avatar::BindPose(avatar::MainSkeleton(), local);
  if (auto clip = catalog_->LoadClip(kIdleClip)) {
    avatar::SamplePose(*clip, avatar::MainSkeleton(), 0.0f, local);
  }
  avatar::RenderPreview(scene, local, nullptr, avatar::Expression(),
                        kThumbnailSize, kThumbnailSize, 0.0f, &out->rgba);
  const size_t pixels = size_t(kThumbnailSize) * kThumbnailSize;
  if (out->rgba.size() < pixels * 4) {
    out->rgba.clear();
    return;
  }
  for (size_t pixel = 0; pixel < pixels; ++pixel) {
    uint8_t* at = out->rgba.data() + pixel * 4;
    if (at[0] == kThumbnailClear[0] && at[1] == kThumbnailClear[1] &&
        at[2] == kThumbnailClear[2]) {
      at[3] = 0;
      continue;
    }
    out->blank = false;
  }
  if (out->blank) {
    out->rgba.clear();
  }
}

void EditorScreenBase::DrawTileThumbnail(class Draw& draw, uint32_t entry_index,
                                         const Rect& tile, uint32_t* budget) {
  if (entry_index == kNoEntry || !catalog_) {
    return;
  }
  auto found = thumbnails_.find(entry_index);
  if (found == thumbnails_.end()) {
    if (!*budget) {
      return;
    }
    --*budget;
    if (thumbnails_.size() >= kThumbnailLimit) {
      auto oldest = thumbnails_.begin();
      for (auto at = thumbnails_.begin(); at != thumbnails_.end(); ++at) {
        if (at->second.last_drawn < oldest->second.last_drawn) {
          oldest = at;
        }
      }
      thumbnails_.erase(oldest);
    }
    ItemThumbnail made;
    RenderThumbnail(entry_index, &made);
    // A tripwire, now that the pictures come out: an item that renders to
    // nothing is a catalogue entry the editor should not have been offered.
    if (made.blank || made.rgba.empty()) {
      const avatar::Entry* entry = catalog_->Find(entry_index);
      XELOGW("avatar editor: item {} kind {:08X} slot {} rendered blank",
             entry_index, entry ? entry->kind : 0,
             entry ? avatar::PrimarySlot(entry->kind) : -1);
    }
    found = thumbnails_.emplace(entry_index, std::move(made)).first;
  }
  ItemThumbnail& picture = found->second;
  picture.last_drawn = ++thumbnail_clock_;
  if (picture.blank || picture.rgba.empty()) {
    return;
  }
  xe::ui::ImmediateTexture* texture = draw.RawTexture(
      "avatar_item_" + std::to_string(entry_index), picture.rgba.data(),
      int(kThumbnailSize), int(kThumbnailSize), !picture.uploaded);
  if (!texture) {
    return;
  }
  picture.uploaded = true;
  const float size = std::min(tile.width, tile.height) * kThumbnailInset;
  Rect fit;
  fit.width = size;
  fit.height = size;
  fit.x = tile.x + (tile.width - size) * 0.5f;
  fit.y = tile.y + (tile.height - size) * 0.5f;
  draw.DrawImage(texture, fit, kPreviewTint);
}

// The picture the navigation put behind this screen. It is authored at the
// frame's own size, so it covers rather than fits.
void EditorScreenBase::DrawBackdrop(class Draw& draw) {
  const PackageEntry* art = package_ && !frame_.background.empty()
                                ? package_->Find(frame_.background)
                                : nullptr;
  if (!art) {
    return;
  }
  int width = 0;
  int height = 0;
  xe::ui::ImmediateTexture* texture = draw.ImageTexture(
      frame_.background, art->data, art->size, &width, &height);
  if (!texture) {
    return;
  }
  Rect full;
  full.x = 0.0f;
  full.y = 0.0f;
  full.width = kEditorWidth;
  full.height = kEditorHeight;
  draw.DrawImage(texture, full, kPreviewTint);
}

void EditorScreenBase::DrawAvatar(class Draw& draw, const Rect& band,
                                  bool rebuilt) {
  if (!preview_width_ || !preview_height_) {
    return;
  }
  xe::ui::ImmediateTexture* texture =
      draw.RawTexture("avatar_preview", preview_.data(), int(preview_width_),
                      int(preview_height_), rebuilt);
  if (!texture) {
    return;
  }
  const float size = std::min(band.width, band.height);
  Rect fit;
  fit.width = size;
  fit.height = size;
  fit.x = band.x + (band.width - size) * 0.5f;
  fit.y = band.y + (band.height - size) * 0.5f;
  draw.DrawImage(texture, fit, kPreviewTint);
}

// A menu screen: the player over the top, the avatar standing on the left, and
// the screen's buttons as box art in two rows on the right.
void EditorScreenBase::DrawMenu(class Draw& draw, bool rebuilt) {
  const float body = kEditorHeight - kLegendHeight;

  if (!frame_.heading.empty()) {
    Rect title;
    title.x = 0.0f;
    title.width = kEditorWidth;
    title.y = 26.0f;
    title.height = 40.0f;
    draw.DrawTextIn(ToU32(frame_.heading), title, 32.0f, kMenuHeading,
                    TextAlign::kCenter);
  }

  Rect stage;
  stage.x = 30.0f;
  stage.y = 80.0f;
  stage.width = kEditorWidth * 0.36f;
  stage.height = body - 100.0f;
  DrawAvatar(draw, stage, rebuilt);

  const size_t count = frame_.menu_tiles.size();
  if (!count) {
    return;
  }

  // The focused tile is bigger than its neighbours and overlaps them, so it
  // goes down last and covers them rather than being cut into by whichever
  // tile happened to come after it.
  for (size_t i = 0; i < count; ++i) {
    if (i != frame_.focus_slot) {
      DrawMenuTile(draw, i, MenuTileRect(i, stage, body), false);
    }
  }
  if (frame_.focus_slot < count) {
    DrawMenuTile(draw, frame_.focus_slot,
                 MenuTileRect(frame_.focus_slot, stage, body), true);
  }
}

Rect EditorScreenBase::MenuTileRect(size_t index, const Rect& stage,
                                    float body) const {
  const size_t count = frame_.menu_tiles.size();
  const size_t top = std::min(count, kMenuTopRow);
  const size_t bottom = count - top;
  const float rows_height =
      bottom ? kMenuTileHeight * 2.0f + kMenuTileGap : kMenuTileHeight;
  const float rows_top = (body - rows_height) * 0.5f + 20.0f;
  const float area_left = stage.x + stage.width + 20.0f;
  const float area_width = kEditorWidth - 40.0f - area_left;

  const bool second = index >= top;
  const size_t column = second ? index - top : index;
  const size_t columns = second ? bottom : top;
  const float row_width =
      float(columns) * kMenuTileWidth + float(columns - 1) * kMenuTileGap;
  const float row_left = area_left + (area_width - row_width) * 0.5f;
  const float row_top =
      rows_top + (second ? kMenuTileHeight + kMenuTileGap : 0.0f);

  Rect tile;
  tile.x = row_left + float(column) * (kMenuTileWidth + kMenuTileGap);
  tile.y = row_top;
  tile.width = kMenuTileWidth;
  tile.height = kMenuTileHeight;
  if (index == frame_.focus_slot) {
    // The focused tile grows about its own centre, so the row it is in does
    // not shift under it.
    const float grow_x = kMenuTileWidth * (kMenuFocusScale - 1.0f) * 0.5f;
    const float grow_y = kMenuTileHeight * (kMenuFocusScale - 1.0f) * 0.5f;
    tile.x -= grow_x;
    tile.y -= grow_y;
    tile.width *= kMenuFocusScale;
    tile.height *= kMenuFocusScale;
  }
  return tile;
}

void EditorScreenBase::DrawMenuTile(class Draw& draw, size_t index,
                                    const Rect& tile, bool focused) {
  const MenuTile& entry = frame_.menu_tiles[index];
  // Everything the player is not looking at loses its colour. The picture
  // spans the tile and the caption sits directly under it, so no panel shows
  // through between them.
  const Rect picture =
      DrawArt(draw, entry.art + "_selected.png", tile, !focused, true);
  const float pixels = focused ? 16.0f : 13.0f;
  Rect caption;
  caption.x = tile.x;
  caption.width = tile.width;
  caption.height = pixels + 10.0f;
  caption.y = picture.height > 0.0f ? picture.y + picture.height
                                    : tile.y + tile.height - caption.height;
  if (focused) {
    Rect ring = tile;
    if (picture.height > 0.0f) {
      ring.y = picture.y;
      ring.height = picture.height + caption.height;
    }
    draw.StrokeRect(ring, kMenuFocusRing);
  }

  if (entry.caption.empty()) {
    return;
  }
  draw.FillRect(caption, kMenuCaptionBand);
  draw.DrawTextIn(ToU32(entry.caption), caption, pixels, kMenuCaption,
                  TextAlign::kCenter);
}

void EditorScreenBase::Draw(class Draw& draw, const Element& root) {
  UpdateFrame();

  const Skin* skin = skin_.loaded() ? &skin_ : nullptr;

  const bool rebuilt = preview_dirty_;
  if (preview_dirty_) {
    RebuildPreview();
  }

  DrawBackdrop(draw);

  if (frame_.is_menu) {
    DrawMenu(draw, rebuilt);
    DrawLegend(draw);
    return;
  }

  // The avatar is not part of any scene - it is a XuiPerspectiveScene the
  // console fills with the 3D model, so the host paints that one element.
  // scnMain spans the whole frame; the avatar actually stands in the gap
  // between the two tile columns.
  float stage_left = 0.0f;
  float stage_right = kEditorWidth;
  for (const Element* slot : slots_) {
    const Rect& rect = slot->rect;
    if (rect.x < kEditorWidth * 0.5f) {
      stage_left = std::max(stage_left, rect.x + rect.width);
    } else {
      stage_right = std::min(stage_right, rect.x);
    }
  }
  auto paint_stage = [&](const Element& element, const Rect&) {
    if (&element != stage_) {
      return false;
    }
    if (!preview_width_ || !preview_height_) {
      return true;
    }
    xe::ui::ImmediateTexture* texture =
        draw.RawTexture("avatar_preview", preview_.data(), int(preview_width_),
                        int(preview_height_), rebuilt);
    if (!texture) {
      return true;
    }
    const float band_width = std::max(stage_right - stage_left, 1.0f);
    const float band_height = kEditorHeight - kLegendHeight;
    const float size = std::min(band_width, band_height) * 0.98f;
    Rect fit;
    fit.width = size;
    fit.height = size;
    fit.x = stage_left + (band_width - size) * 0.5f;
    fit.y = (band_height - size) * 0.5f;
    draw.DrawImage(texture, fit, kPreviewTint);
    return true;
  };

  SceneDrawer background(&draw, package_, skin);
  background.AddFallbackPackage(common_);
  background.AddFallbackPackage(controlpack_);
  background.AddFallbackPackage(shared_);
  background.SetFigures(&background_figures_);
  background.SetSkinFigures(&skin_figures_);
  if (background_.source) {
    background.Draw(background_);
  }

  SceneDrawer frame(&draw, package_, skin);
  frame.AddFallbackPackage(common_);
  frame.AddFallbackPackage(controlpack_);
  frame.AddFallbackPackage(shared_);
  frame.SetFigures(&figures_);
  frame.SetSkinFigures(&skin_figures_);
  frame.SetPainter(paint_stage);
  frame.Draw(root);

  SceneDrawer grid(&draw, package_, skin);
  grid.AddFallbackPackage(common_);
  grid.AddFallbackPackage(controlpack_);
  grid.AddFallbackPackage(shared_);
  grid.SetFigures(&grid_figures_);
  grid.SetSkinFigures(&skin_figures_);
  // An empty tile draws nothing. Which of the two backgrounds shows is the
  // tile's Focus and Normal spans talking, so it is only decided here when
  // the skin gave this tile no states to play.
  grid.SetPainter([&](const Element& element, const Rect& rect) {
    for (size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i] != &element) {
        continue;
      }
      if (!frame_.tile_used[i]) {
        return true;
      }
      if (lua_ && lua_->PlayerFor(slots_[i])) {
        return false;
      }
      if (i != frame_.focus_slot && element.id == "button bg selected") {
        return true;
      }
      if (i == frame_.focus_slot && element.id == "button bg unselected") {
        return true;
      }
      return false;
    }
    return false;
  });
  // The console names only the FOCUSED item, in a callout above its tile -
  // it does not label every tile.
  grid.SetTextSource([&](const Element& element, std::string* out) {
    if (&element == category_label_) {
      *out = frame_.title;
      return true;
    }
    return false;
  });
  // Each tile's own Visual (GridTopLevel) is empty - the console composes the
  // tile at runtime from this offscreen template - so draw that template at
  // the tile's rect, hiding whichever background state does not apply.
  // Which tile has focus is the editor's to say - it comes from the running
  // title, not from the scene tree - but what focus looks like is the host's,
  // and the playheads are advanced with everything else the overlay runs.
  if (lua_) {
    Element* wanted =
        frame_.focus_slot < slots_.size() && frame_.tile_used[frame_.focus_slot]
            ? slots_[frame_.focus_slot]
            : nullptr;
    lua_->ForceFocus(wanted);
  }

  uint32_t thumbnail_budget = kThumbnailsPerFrame;
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (!frame_.tile_used[i]) {
      continue;
    }
    const bool focused = i == frame_.focus_slot;
    SceneDrawer tile(&draw, package_, skin);
    tile.AddFallbackPackage(common_);
    tile.AddFallbackPackage(controlpack_);
    tile.AddFallbackPackage(shared_);
    tile.SetSkinFigures(&skin_figures_);
    TimelinePlayer* player = lua_ ? lua_->PlayerFor(slots_[i]) : nullptr;
    tile.SetTimeline(player);
    if (!player) {
      tile.SetPainter([&](const Element& element, const Rect&) {
        if (element.id == "button bg selected") {
          return !focused;
        }
        if (element.id == "button bg unselected") {
          return focused;
        }
        return false;
      });
    }
    tile.DrawNamedVisual(kTileVisual, slots_[i]->rect);
    // The template's own preview presenter carries no art: the picture of the
    // item goes on top of it.
    DrawTileThumbnail(draw, frame_.tile_entry[i], slots_[i]->rect,
                      &thumbnail_budget);
  }

  grid.Draw(grid_);

  if (frame_.focus_slot < slots_.size() &&
      frame_.tile_used[frame_.focus_slot]) {
    std::string caption = frame_.tile_name[frame_.focus_slot];
    if (caption.empty()) {
      caption = "(unnamed)";
    }
    if (frame_.tile_worn[frame_.focus_slot]) {
      caption += " - worn";
    }
    const Rect& tile = slots_[frame_.focus_slot]->rect;
    const std::u32string text = ToU32(caption);
    const float pixels = 16.0f;
    Rect callout;
    callout.width = draw.MeasureText(text, pixels) + 16.0f;
    callout.height = pixels + 8.0f;
    callout.x = tile.x + (tile.width - callout.width) * 0.5f;
    callout.y = tile.y - callout.height - 2.0f;
    draw.FillRect(callout, kCalloutFill);
    draw.DrawTextIn(text, callout, pixels, kLegendText, TextAlign::kCenter);
  }

  // Page N of M, the way the console shows it under the columns, or whatever
  // the screen has to say when there is nothing to page through.
  Rect label;
  label.x = stage_left;
  label.width = std::max(stage_right - stage_left, 1.0f);
  label.height = 24.0f;
  label.y = kEditorHeight - kLegendHeight - label.height - 10.0f;
  if (!frame_.status.empty()) {
    draw.DrawTextIn(ToU32(frame_.status), label, 15.0f, kLegendHint,
                    TextAlign::kCenter);
  } else if (frame_.page_count > 1) {
    draw.DrawTextIn(ToU32("Page " + std::to_string(frame_.page_index + 1) +
                          " of " + std::to_string(frame_.page_count)),
                    label, 15.0f, kLegendHint, TextAlign::kCenter);
  }

  DrawLegend(draw);
}

// A menu offers the press and, below the root, the way back; a grid also pages
// and turns the avatar. The console shows nothing a screen cannot do.
void EditorScreenBase::DrawLegend(class Draw& draw) {
  Rect band;
  band.x = 0.0f;
  band.width = kEditorWidth;
  band.height = kLegendHeight;
  band.y = kEditorHeight - kLegendHeight;
  if (!frame_.is_menu) {
    draw.FillRect(band, kLegendBand);
  }

  float pen = 40.0f;
  auto entry = [&](const char* image, const char* letter, const char* caption) {
    const PackageEntry* art = shared_ ? shared_->Find(image) : nullptr;
    int width = 0;
    int height = 0;
    xe::ui::ImmediateTexture* texture =
        art ? draw.ImageTexture(image, art->data, art->size, &width, &height)
            : nullptr;
    const float size = band.height * 0.56f;
    if (texture && height > 0) {
      Rect icon;
      icon.height = size;
      icon.width = size * float(width) / float(height);
      icon.x = pen;
      icon.y = band.y + (band.height - icon.height) * 0.5f;
      draw.DrawImage(texture, icon, kPreviewTint);
      draw.DrawTextIn(ToU32(letter), icon, size * 0.68f, kLegendText,
                      TextAlign::kCenter);
      pen += icon.width + 6.0f;
    }
    const std::u32string text = ToU32(caption);
    Rect label_rect = band;
    label_rect.x = pen;
    label_rect.width = draw.MeasureText(text, size * 0.68f);
    draw.DrawTextIn(text, label_rect, size * 0.68f, kLegendText,
                    TextAlign::kLeft);
    pen += label_rect.width + 26.0f;
  };
  entry("A-Button.png", "A", "Select");
  if (frame_.offers_back) {
    entry("B-Button.png", "B", "Back");
  }
  if (frame_.is_menu) {
    return;
  }

  Rect hints = band;
  hints.width = kEditorWidth - 40.0f;
  draw.DrawTextIn(ToU32("LT/RT Page    Left stick Turn    Start Save"), hints,
                  band.height * 0.42f, kLegendHint, TextAlign::kRight);
}

// The translated editor driving those scenes.

void TranslatedEditorScreen::ReadManifest() {
  editor::EditorSession* session = editor::EditorSession::Current();
  if (!session || !catalog_) {
    return;
  }
  const uint8_t* live =
      session->navigation().guest().At<uint8_t>(editor::kLiveManifestAddress);
  if (!live) {
    return;
  }
  if (manifest_.size() == editor::kManifestBytes &&
      std::memcmp(manifest_.data(), live, editor::kManifestBytes) == 0) {
    return;
  }
  manifest_.assign(live, live + editor::kManifestBytes);
  // The tiles have been writing the worn items into this buffer, so it is what
  // the avatar on the stage should be wearing.
  avatar::Description described;
  if (avatar::ParseManifest(catalog_, manifest_.data(), manifest_.size(),
                            &described)) {
    SetPreviewDescription(described);
  }
  // The worn ticks are read off the same buffer, so the item list goes with it.
  read_command_ = ~0u;
}

void TranslatedEditorScreen::ReadScreen() {
  editor::EditorSession* session = editor::EditorSession::Current();
  if (!session) {
    return;
  }
  const uint32_t command = session->current_command();
  const int32_t depth = session->navigation_depth();
  const bool ready = session->collection_is_ready();
  if (command == read_command_ && depth == read_depth_ &&
      ready == read_collection_ready_) {
    return;
  }
  view_ = editor::ReadCurrentEditorScreen(kMaxCategoryItems);
  read_command_ = command;
  read_depth_ = depth;
  read_collection_ready_ = ready;
}

void TranslatedEditorScreen::UpdateFrame() {
  frame_ = EditorFrame();

  editor::EditorSession* session = editor::EditorSession::Current();
  if (!session) {
    frame_.title = "Avatar";
    frame_.status = "The editor is no longer running.";
    return;
  }
  ReadManifest();
  ReadScreen();

  frame_.title = EditorText(view_.title_string);
  if (frame_.title.empty() && !view_.screen_name.empty()) {
    frame_.title = view_.screen_name;
  }
  if (view_.is_grid) {
    if (const char* named = editor::EditorCategoryName(view_.category)) {
      frame_.title = named;
    }
  }
  frame_.background = view_.background_image.empty()
                          ? std::string(editor::kRootBackgroundImage)
                          : view_.background_image;
  frame_.offers_back = view_.depth > 0 || view_.is_overlay;

  const size_t count = view_.items.size();
  editor::EditorInput* input = editor::EditorInput::ForCurrentSession();
  const int32_t focused =
      input && count
          ? std::clamp(input->Focus().focused_index, 0, int32_t(count) - 1)
          : 0;

  if (view_.is_menu) {
    frame_.is_menu = true;
    // The console puts the player over its top-level menu; the screens under it
    // are titled after themselves.
    frame_.heading =
        view_.command == editor::kCommandRootScreen ? Gamertag() : frame_.title;
    frame_.focus_slot = size_t(focused);
    for (const editor::EditorScreenItem& item : view_.items) {
      MenuTile tile;
      tile.caption = EditorText(item.caption_string);
      tile.art = item.art;
      frame_.menu_tiles.push_back(std::move(tile));
    }
    return;
  }

  if (view_.is_grid && !view_.collection_ready) {
    frame_.status = "Building this category's item list...";
    return;
  }
  if (!count) {
    frame_.status = "This category holds nothing for this avatar.";
    return;
  }

  page_base_ = (size_t(focused) / kSlotLimit) * kSlotLimit;
  frame_.focus_slot = size_t(focused) - page_base_;
  frame_.page_index = page_base_ / kSlotLimit;
  frame_.page_count = (count + kSlotLimit - 1) / kSlotLimit;

  for (size_t i = 0; i < kSlotLimit && page_base_ + i < count; ++i) {
    const avatar_editor::EditorScreenItem& item = view_.items[page_base_ + i];
    frame_.tile_used[i] = true;
    frame_.tile_worn[i] = item.is_worn;
    frame_.tile_name[i] = item.name;
    frame_.tile_entry[i] = CatalogEntryForAsset(item.asset_id);
  }
  if (view_.item_total > count) {
    frame_.status = std::to_string(count) + " of " +
                    std::to_string(view_.item_total) + " items";
  }
}

// The tile the press lands on is a matter of where the tiles sit, but the move
// itself belongs to the editor: it is the editor's screen stack that remembers
// where the focus was.
void TranslatedEditorScreen::MoveFocus(int dx, int dy) {
  editor::EditorInput* input = editor::EditorInput::ForCurrentSession();
  if (!input) {
    return;
  }
  // Every move names the index it wants and the count the screen actually
  // drew. Asking for a relative step instead let the editor clamp it against
  // the scene object's own child count, which is 1 for these screens.
  if (frame_.is_menu) {
    // A menu's buttons are one run laid out in two rows, so down is a row's
    // width away and right is one along.
    const int step = dy ? int(kMenuTopRow) * dy : dx;
    const int at = int(frame_.focus_slot) + step;
    if (at < 0 || at >= int(frame_.menu_tiles.size())) {
      return;
    }
    input->SetFocusIndex(at, int32_t(frame_.menu_tiles.size()));
    return;
  }

  // A grid page is one row as wide as a group, and the item that page shows
  // is an offset into the whole category.
  const int target = SlotInDirection(frame_.focus_slot, dx, dy);
  if (target >= 0) {
    input->SetFocusIndex(int32_t(page_base_) + target,
                         int32_t(view_.items.size()), int32_t(kSlotLimit));
    return;
  }

  // Nothing that way on this page, so step to the next page's edge.
  const int delta = dy ? dy : dx;
  const int32_t count = int32_t(view_.items.size());
  const int32_t at =
      int32_t(page_base_ + frame_.focus_slot) + delta * int32_t(kSlotLimit);
  if (at >= 0 && at < count) {
    input->SetFocusIndex(at, count, int32_t(kSlotLimit));
  }
}

void TranslatedEditorScreen::Activate() {
  editor::EditorInput* input = editor::EditorInput::ForCurrentSession();
  if (!input) {
    return;
  }
  // A menu button opens the screen its own tile names. Going through the
  // scene's focused child instead asks the editor which tile it thinks is
  // selected, which is not the one the player moved to.
  if (frame_.is_menu) {
    if (frame_.focus_slot < view_.items.size() &&
        input->OpenCommand(view_.items[frame_.focus_slot].activate_command)) {
      return;
    }
    input->Activate();
    return;
  }

  // A grid press dresses the avatar and stays on the screen. Falling through
  // to the editor's own activate asked the scene for a focused child it does
  // not have, and that arm pops the screen instead.
  input->WearItemAt(int32_t(page_base_ + frame_.focus_slot));
}

void TranslatedEditorScreen::Back() {
  editor::EditorInput* input = editor::EditorInput::ForCurrentSession();
  if (!input || !input->Back()) {
    Cancel();
  }
}

void TranslatedEditorScreen::ChangePage(int delta) {
  if (editor::EditorInput* input = editor::EditorInput::ForCurrentSession()) {
    input->MoveFocus(0, delta);
  }
}

// The editor has no category to step through in place - a category is a screen,
// and reaching one means activating its tile on the screen above.
void TranslatedEditorScreen::ChangeCategory(int) {}

// The same closet driven by the host's catalog.

AvatarEditorScreen::AvatarEditorScreen(avatar::Catalog* catalog,
                                       const avatar::Description& description)
    : EditorScreenBase(catalog) {
  description_ = description;
}

bool AvatarEditorScreen::Prepare(AssetStore& assets) {
  RefreshItems();
  return EditorScreenBase::Prepare(assets);
}

void AvatarEditorScreen::RefreshItems() {
  items_.clear();
  if (catalog_) {
    items_ = avatar::ItemsForSlot(*catalog_, slot_, description_.body);
  }
  page_ = 0;
  focus_ = 0;
}

// Page 0 slot 0 is the "None" choice, so the list is one longer.
size_t AvatarEditorScreen::PageCount() const {
  return (items_.size() + kSlotLimit) / kSlotLimit;
}

void AvatarEditorScreen::UpdateFrame() {
  frame_ = EditorFrame();
  frame_.title = avatar::SlotName(slot_);
  frame_.background = editor::kRootBackgroundImage;
  frame_.offers_back = true;
  frame_.focus_slot = focus_;
  frame_.page_index = page_;
  frame_.page_count = std::max<size_t>(PageCount(), 1);

  const size_t first = page_ * kSlotLimit;
  for (size_t i = 0; i < kSlotLimit; ++i) {
    const size_t at = first + i;
    if (at == 0) {
      frame_.tile_used[i] = true;
      frame_.tile_name[i] = "None";
      frame_.tile_worn[i] = description_.items[slot_] == avatar::kNoItem;
      continue;
    }
    if (at - 1 >= items_.size()) {
      break;
    }
    const uint32_t item = items_[at - 1];
    const avatar::Entry* entry = catalog_ ? catalog_->Find(item) : nullptr;
    frame_.tile_used[i] = true;
    frame_.tile_name[i] =
        entry && !entry->name.empty() ? entry->name : std::string("(unnamed)");
    frame_.tile_worn[i] = description_.items[slot_] == uint16_t(item);
    frame_.tile_entry[i] = item;
  }
}

void AvatarEditorScreen::ChangeCategory(int delta) {
  const int count = int(avatar::kSlotCount);
  int next = int(slot_) + delta;
  while (next < 0) {
    next += count;
  }
  slot_ = uint32_t(next % count);
  RefreshItems();
  UpdateFrame();
}

void AvatarEditorScreen::ChangePage(int delta) {
  const size_t pages = PageCount();
  if (!pages) {
    return;
  }
  int next = int(page_) + delta;
  while (next < 0) {
    next += int(pages);
  }
  page_ = size_t(next) % pages;
  focus_ = 0;
  UpdateFrame();
}

void AvatarEditorScreen::MoveFocus(int dx, int dy) {
  const int neighbour = SlotInDirection(focus_, dx, dy);
  if (neighbour >= 0) {
    focus_ = size_t(neighbour);
    UpdateFrame();
    return;
  }
  // Nothing that way on this page: the columns page sideways.
  if (dx) {
    ChangePage(dx);
  }
}

void AvatarEditorScreen::Activate() {
  if (!catalog_ || focus_ >= slot_count() || !frame_.tile_used[focus_]) {
    return;
  }
  const size_t at = page_ * kSlotLimit + focus_;
  const uint16_t chosen = at == 0 ? avatar::kNoItem : uint16_t(items_[at - 1]);
  avatar::PlaceItem(*catalog_, &description_, slot_, chosen);
  SetPreviewDescription(description_);
  UpdateFrame();
}

void AvatarEditorScreen::Back() { Cancel(); }

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
