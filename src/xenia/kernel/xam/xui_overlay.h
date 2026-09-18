/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_OVERLAY_H_
#define XENIA_KERNEL_XAM_XUI_OVERLAY_H_

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "xenia/kernel/xam/xui_draw.h"
#include "xenia/kernel/xam/xui_font.h"
#include "xenia/kernel/xam/xui_lua.h"
#include "xenia/kernel/xam/xui_runtime.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/ui_drawer.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

// A screen drawn by the overlay. Subclasses own their own behaviour; the
// scene supplies geometry and art.
class Screen {
 public:
  virtual ~Screen() = default;

  // The screen asks the store for whatever package and art it needs.
  virtual bool Prepare(AssetStore& assets) = 0;
  virtual void Draw(Draw& draw, const Element& root) = 0;
  virtual bool closed() const { return closed_; }

  const Element& root() const { return root_; }
  Element& mutable_root() { return root_; }

  // The space this screen's rects are in. The flash scenes are authored at
  // 852x480, but the avatar editor's are 1280x720, so it is per screen.
  float design_width() const { return design_width_; }
  float design_height() const { return design_height_; }

 protected:
  Element root_;
  float design_width_ = kDesignWidth;
  float design_height_ = kDesignHeight;
  bool closed_ = false;
};

// Composites XUI screens over the guest frame while the guide is up.
class Overlay : public xe::ui::UIDrawer {
 public:
  Overlay(xe::ui::Presenter* presenter,
          xe::ui::ImmediateDrawer* immediate_drawer);
  ~Overlay();

  bool Open(const std::filesystem::path& asset_directory);
  bool ready() const { return assets_.loaded(); }

  // The Lua the dashboard's scenes are written in. One per overlay, because
  // the class library it builds is shared by every scene drawn through it.
  LuaHost* lua() { return lua_.get(); }
  AssetStore& assets() { return assets_; }

  // Screens are NOT owned: the host that pushed one keeps it alive and reads
  // its state, which a self-erasing overlay would leave dangling.
  bool Push(Screen* screen);
  void Remove(Screen* screen);
  void PopAll();
  bool has_screens() const { return !screens_.empty(); }

  void Draw(xe::ui::UIDrawContext& ui_draw_context) override;

 private:
  static bool GuideIsOpen();

  xe::ui::Presenter* presenter_ = nullptr;
  xe::ui::ImmediateDrawer* immediate_drawer_ = nullptr;
  AssetStore assets_;
  // Qualified: Overlay::Draw, the member function, otherwise shadows the type.
  std::unique_ptr<xui::Draw> draw_;
  std::unique_ptr<LuaHost> lua_;
  std::vector<Screen*> screens_;
  uint64_t clock_ = 0;
  bool attached_ = false;
};

// One overlay for the emulator's display window, shared by every screen, so
// the asset store and its textures are loaded once. Returns false when the
// dashboard assets are not installed, which is how callers know to keep their
// ImGui fallback.
bool OpenSharedOverlay(xe::ui::Presenter* presenter,
                       xe::ui::ImmediateDrawer* immediate_drawer,
                       const std::filesystem::path& asset_directory);
void CloseSharedOverlay();
Overlay* SharedOverlay();

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
