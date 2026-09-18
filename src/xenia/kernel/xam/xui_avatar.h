/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_AVATAR_H_
#define XENIA_KERNEL_XAM_XUI_AVATAR_H_

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "xenia/kernel/xam/avatar_editor/editor_render.h"
#include "xenia/kernel/xam/xui_overlay.h"
#include "xenia/kernel/xam/xui_scene_draw.h"
#include "xenia/kernel/xna/xna_avatar_format.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

// The console's Avatar Editor closet, drawn from the editor's own scenes:
// EditorBG the backdrop, EditorMain the frame and the XuiPerspectiveScene the
// avatar stands in, Grid the eight item slots either side of it.
//
// This class owns the assets, the tile geometry and the whole frame. What is
// actually on the tiles comes from a subclass, once per frame, as EditorFrame.
class EditorScreenBase : public Screen {
 public:
  static constexpr size_t kSlotLimit = 8;
  // A tile that stands for no catalogue item - the "None" choice, or an asset
  // the catalogue does not carry.
  static constexpr uint32_t kNoEntry = ~0u;

  explicit EditorScreenBase(xna::avatar::Catalog* catalog)
      : catalog_(catalog) {}

  bool Prepare(AssetStore& assets) override;
  void Draw(class Draw& draw, const Element& root) final;

  // What a host raises when a button is pressed. A subclass answers each one
  // against whatever holds its state.
  virtual void MoveFocus(int dx, int dy) = 0;
  virtual void Activate() = 0;
  virtual void Back() = 0;
  virtual void ChangePage(int delta) = 0;
  virtual void ChangeCategory(int delta) = 0;
  void Rotate(float degrees);
  void Commit();
  void Cancel();

  const xna::avatar::Description& description() const { return description_; }
  bool committed() const { return committed_; }
  bool cancelled() const { return cancelled_; }

 protected:
  // One box-art button on a menu screen.
  struct MenuTile {
    std::string caption;
    // The package image, "<widget>_selected.png".
    std::string art;
  };

  // What the frame shows this frame. A menu screen fills menu_tiles and the
  // heading; a category grid fills the eight closet slots instead.
  struct EditorFrame {
    std::string title;
    // Over a menu the console puts the player, not the screen.
    std::string heading;
    std::string status;
    std::string background;
    bool is_menu = false;
    bool offers_back = false;
    size_t focus_slot = 0;
    size_t page_index = 0;
    size_t page_count = 1;
    std::vector<MenuTile> menu_tiles;
    bool tile_used[kSlotLimit] = {};
    bool tile_worn[kSlotLimit] = {};
    std::string tile_name[kSlotLimit];
    // The catalogue entry each tile pictures.
    std::array<uint32_t, kSlotLimit> tile_entry;
    EditorFrame() { tile_entry.fill(kNoEntry); }
  };

  // Fills frame_ from wherever the subclass keeps its state.
  virtual void UpdateFrame() = 0;

  // Which tile lies that way from this one, or -1 when the edge of the grid is
  // that way. Geometry, not index arithmetic: the closet's two columns straddle
  // the avatar, so which tile is "right of" another is a matter of where it
  // sits.
  int SlotInDirection(size_t from, int dx, int dy) const;

  size_t slot_count() const { return slots_.size(); }
  void SetPreviewDescription(const xna::avatar::Description& description);

  // The catalogue entry an editor asset id names, or kNoEntry.
  uint32_t CatalogEntryForAsset(const uint8_t* asset_id);

  // The editor's own Strings.xus, which is what every caption in the image is
  // an ordinal into. Ordinals the table does not reach answer empty, which is
  // also what the editor's own empty string is.
  std::string EditorText(uint32_t ordinal) const;

  // Whichever profile the editor is editing for.
  std::string Gamertag() const;

  xna::avatar::Catalog* catalog_ = nullptr;
  xna::avatar::Description description_;
  EditorFrame frame_;

 private:
  void CollectSlots();
  void RebuildPreview();
  void LoadStrings();
  const Element* Find(const std::string& id) const;

  void DrawBackdrop(class Draw& draw);
  void DrawAvatar(class Draw& draw, const Rect& band, bool rebuilt);
  void DrawMenu(class Draw& draw, bool rebuilt);
  void DrawLegend(class Draw& draw);
  // Draws a package image and answers the rectangle it landed in. By default
  // it fits inside the rect; `fill_width` spans the rect's width instead and
  // takes whatever height the image's own shape asks for, anchored to the top.
  Rect DrawArt(class Draw& draw, const std::string& name, const Rect& rect,
               bool desaturate = false, bool fill_width = false);
  // One menu tile, whole: its panel, its picture and its caption.
  void DrawMenuTile(class Draw& draw, size_t index, const Rect& tile,
                    bool focused);
  // Where a menu tile sits. The focused one is bigger, about its own centre.
  Rect MenuTileRect(size_t index, const Rect& stage, float body) const;

  // The console composes an item tile from a picture it renders offscreen -
  // the item by itself, or the head wearing it when the item is a face
  // texture. One picture per catalogue entry, kept for as long as the closet
  // is open so paging back and forth costs nothing.
  struct ItemThumbnail {
    std::vector<uint8_t> rgba;
    // Nothing of the item reached the picture, so the tile stays bare rather
    // than rendering it again every frame.
    bool blank = true;
    bool uploaded = false;
    uint64_t last_drawn = 0;
  };
  void RenderThumbnail(uint32_t entry_index, ItemThumbnail* out);
  // Renders at most one picture, and only while budget allows; a page's worth
  // is spread over the frames that follow it.
  void DrawTileThumbnail(class Draw& draw, uint32_t entry_index,
                         const Rect& tile, uint32_t* budget);

  std::map<uint32_t, ItemThumbnail> thumbnails_;
  std::map<std::array<uint8_t, 16>, uint32_t> catalog_assets_;
  std::set<std::array<uint8_t, 16>> unmatched_assets_;
  bool catalog_assets_built_ = false;
  uint64_t thumbnail_clock_ = 0;

  std::vector<std::string> strings_;

  Package* package_ = nullptr;
  Package* shared_ = nullptr;
  Package* common_ = nullptr;
  Package* controlpack_ = nullptr;

  Element grid_;
  // The eight fixed ItemButton slots of Grid.xur, in scene order.
  std::vector<Element*> slots_;
  float yaw_ = 0.0f;

  // The editor carries its OWN skin (EditorSkin.xur) - the flash skin does
  // not know GridTopLevel or the item tile visuals.
  std::vector<uint8_t> skin_data_;
  Skin skin_;

  // The tile template's own state machine, one playhead per slot, held by the
  // same host that runs the dashboard's scenes so a tile gains and loses
  // focus exactly as any other control does. Each slot is a distinct element
  // sharing one authored visual, so the playheads are attached rather than
  // derived from the slots' own nodes.
  const Scene* skin_scene_ = nullptr;
  const Node* tile_visual_ = nullptr;
  LuaHost* lua_ = nullptr;

  Element background_;
  std::map<uint32_t, FigurePath> figures_;
  std::map<uint32_t, FigurePath> grid_figures_;
  std::map<uint32_t, FigurePath> background_figures_;
  std::map<uint32_t, FigurePath> skin_figures_;
  const Element* stage_ = nullptr;
  const Element* category_label_ = nullptr;

  std::vector<uint8_t> preview_;
  uint32_t preview_width_ = 0;
  uint32_t preview_height_ = 0;
  bool preview_dirty_ = true;

  bool committed_ = false;
  bool cancelled_ = false;
};

// The translated AvatarEditor.xex driving those scenes. Every piece of state
// here is read out of the running editor: the screen it has on its navigation
// stack, the category that screen's grid scene carries, the items the component
// collection put in that category's bucket, the focus the screen stack
// remembers, and the manifest the tiles have been writing into. Input is handed
// straight to the editor's own input layer.
class TranslatedEditorScreen : public EditorScreenBase {
 public:
  explicit TranslatedEditorScreen(xna::avatar::Catalog* catalog)
      : EditorScreenBase(catalog) {}

  void MoveFocus(int dx, int dy) override;
  void Activate() override;
  void Back() override;
  void ChangePage(int delta) override;
  void ChangeCategory(int delta) override;

 protected:
  void UpdateFrame() override;

 private:
  void ReadScreen();
  void ReadManifest();

  avatar_editor::EditorScreenView view_;
  uint32_t read_command_ = ~0u;
  int32_t read_depth_ = -2;
  bool read_collection_ready_ = false;
  std::vector<uint8_t> manifest_;
  // Where the eight visible tiles start in the category's bucket.
  size_t page_base_ = 0;
};

// The same closet driven by the host's own catalog, for when no translated
// session is running - a signed-in profile's avatar edited straight through the
// catalog.
class AvatarEditorScreen : public EditorScreenBase {
 public:
  AvatarEditorScreen(xna::avatar::Catalog* catalog,
                     const xna::avatar::Description& description);

  bool Prepare(AssetStore& assets) override;

  void MoveFocus(int dx, int dy) override;
  void Activate() override;
  void Back() override;
  void ChangePage(int delta) override;
  void ChangeCategory(int delta) override;

 protected:
  void UpdateFrame() override;

 private:
  void RefreshItems();
  size_t PageCount() const;

  std::vector<uint32_t> items_;
  uint32_t slot_ = 0;
  size_t page_ = 0;
  size_t focus_ = 0;
};

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
