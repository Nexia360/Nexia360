/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_SCENE_DRAW_H_
#define XENIA_KERNEL_XAM_XUI_SCENE_DRAW_H_

#include <functional>
#include <map>
#include <string>

#include "xenia/kernel/xam/xui_draw.h"
#include "xenia/kernel/xam/xui_runtime.h"
#include "xenia/kernel/xam/xui_timeline.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

// Draws a scene from its own contents - XuiFigure paths with their fills,
// XuiImage art from the package, label text - and expands each element's
// skin Visual in place, which is where controls like buttons keep their
// appearance. Without this a scene supplies only geometry.
class SceneDrawer {
 public:
  SceneDrawer(Draw* draw, Package* package, const Skin* skin)
      : draw_(draw), package_(package), skin_(skin) {}

  // Extra packages to search for art, in order, when the scene's own package
  // does not have it (the editor's common/controlpack, sharedres).
  void AddFallbackPackage(Package* package) {
    if (package) {
      fallbacks_.push_back(package);
    }
  }

  // CUST paths for the scene being drawn; the skin has its own.
  void SetFigures(const std::map<uint32_t, FigurePath>* figures) {
    figures_ = figures;
  }
  void SetSkinFigures(const std::map<uint32_t, FigurePath>* figures) {
    skin_figures_ = figures;
  }

  // Elements the host paints itself (the avatar stage, item previews).
  using Painter = std::function<bool(const Element&, const Rect&)>;
  void SetPainter(Painter painter) { painter_ = std::move(painter); }

  // Text the host supplies for an element, replacing the scene's own.
  using TextSource = std::function<bool(const Element&, std::string*)>;
  void SetTextSource(TextSource source) { text_source_ = std::move(source); }

  // The playing state of whatever is being drawn. A control's appearance is
  // not a separate set of art: the scene animates the control's own elements,
  // so the timeline has to be sampled while they are drawn.
  void SetTimeline(const TimelinePlayer* timeline) { timeline_ = timeline; }

  // Where several playheads are running at once - a scene whose controls each
  // hold their own state - the drawer asks which one governs an element
  // instead of being handed a single one.
  using TimelineSource = std::function<const TimelinePlayer*(const Element&)>;
  void SetTimelineSource(TimelineSource source) {
    timeline_source_ = std::move(source);
  }

  // Where a timeline has put an element, and how visible it is. Returns false
  // when the timeline has hidden it outright. `is_owner` marks the element
  // the timeline is attached to, which is the only one the timeline's
  // untargeted track applies to.
  bool Animate(const Element& element, Rect* rect, float* opacity,
               bool is_owner) const;

  // What a timeline is doing to one element right now, or null when nothing
  // is driving it.
  const AnimatedProperties* AnimationOf(const Element& element,
                                        bool is_owner) const;

  void Draw(const Element& element);
  // Draw a named skin visual at a rect. The console composes some controls
  // itself - an item tile's Visual is an empty GridTopLevel and the real
  // template is a separate offscreen one - so the host asks for it by name.
  void DrawNamedVisual(const std::string& name, const Rect& rect);

 private:
  void DrawOne(const Element& element, const Rect& rect, int depth,
               bool is_owner = false);
  void DrawVisual(const Node& visual, const Rect& rect, int depth);
  void DrawFigure(const Node& node, const Rect& rect, bool from_skin,
                  const AnimatedProperties* animation, float opacity);
  void DrawImage(const Node& node, const Rect& rect, float opacity);
  void DrawText(const Element& element, const Node& node, const Rect& rect,
                float opacity);
  const PackageEntry* FindArt(const std::string& name) const;

  class Draw* draw_ = nullptr;
  Package* package_ = nullptr;
  const Skin* skin_ = nullptr;
  std::vector<Package*> fallbacks_;
  const std::map<uint32_t, FigurePath>* figures_ = nullptr;
  const std::map<uint32_t, FigurePath>* skin_figures_ = nullptr;
  Painter painter_;
  TextSource text_source_;
  const TimelinePlayer* timeline_ = nullptr;
  TimelineSource timeline_source_;
};

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
