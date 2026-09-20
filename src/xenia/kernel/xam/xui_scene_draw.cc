/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_scene_draw.h"

#include <algorithm>
#include <utility>

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

// A skin visual can name another, so recursion needs a stop.
constexpr int kMaxVisualDepth = 6;

uint32_t BrushColor(const Node& node, const std::string_view brush,
                    const std::string_view field, uint32_t fallback) {
  const Value* value = Property(node, brush);
  if (!value || !value->object) {
    return fallback;
  }
  const std::string text = StringProperty(*value->object, field);
  return text.empty() ? fallback : ParseColor(text, fallback);
}

bool HasBrush(const Node& node, const std::string_view brush) {
  const Value* value = Property(node, brush);
  return value && value->object;
}

// Opacity multiplies into the alpha the art already carries rather than
// replacing it, so a half-faded control keeps its own translucency.
uint32_t Fade(uint32_t color, float opacity) {
  if (opacity >= 1.0f) {
    return color;
  }
  const float alpha =
      static_cast<float>((color >> 24) & 0xFF) * std::max(opacity, 0.0f);
  const auto scaled =
      static_cast<uint32_t>(std::clamp(static_cast<int>(alpha + 0.5f), 0, 255));
  return (color & 0x00FFFFFFu) | (scaled << 24);
}

}  // namespace

const AnimatedProperties* SceneDrawer::AnimationOf(const Element& element,
                                                   bool is_owner) const {
  const TimelinePlayer* player = timeline_;
  if (timeline_source_) {
    if (const TimelinePlayer* named = timeline_source_(element)) {
      player = named;
    }
  }
  if (!player) {
    return nullptr;
  }
  // A timeline names each element it drives by id. The untargeted one belongs
  // to the object the timeline is attached to and to nothing else - handing
  // it to every element as well applied one root opacity, or one Show, to the
  // whole subtree and made it vanish.
  if (!element.id.empty()) {
    if (const AnimatedProperties* found = player->Properties(element.id)) {
      return found;
    }
  }
  return is_owner ? player->Properties("") : nullptr;
}

bool SceneDrawer::Animate(const Element& element, Rect* rect, float* opacity,
                          bool is_owner) const {
  const AnimatedProperties* animation = AnimationOf(element, is_owner);
  if (!animation) {
    return true;
  }
  if (animation->has_show && !animation->show) {
    return false;
  }
  if (animation->has_position) {
    rect->x += animation->position.value[0];
    rect->y += animation->position.value[1];
  }
  if (animation->has_width) {
    rect->width = animation->width;
  }
  if (animation->has_height) {
    rect->height = animation->height;
  }
  if (animation->has_scale) {
    // Scale is applied from the element's pivot, which is its top-left corner
    // unless the scene says otherwise. Growing everything from the centre
    // makes focused controls drift off their own anchors.
    const float pivot_x = rect->x + element.pivot_x;
    const float pivot_y = rect->y + element.pivot_y;
    rect->width *= animation->scale.value[0];
    rect->height *= animation->scale.value[1];
    rect->x = pivot_x - element.pivot_x * animation->scale.value[0];
    rect->y = pivot_y - element.pivot_y * animation->scale.value[1];
  }
  if (animation->has_opacity) {
    *opacity *= animation->opacity;
  }
  return true;
}

const PackageEntry* SceneDrawer::FindArt(const std::string& name) const {
  if (name.empty()) {
    return nullptr;
  }
  if (package_) {
    if (const PackageEntry* entry = package_->Find(name)) {
      return entry;
    }
  }
  for (Package* package : fallbacks_) {
    if (const PackageEntry* entry = package->Find(name)) {
      return entry;
    }
  }
  return nullptr;
}

void SceneDrawer::Draw(const Element& element) {
  // The scene's root is the element a scene timeline is attached to.
  DrawOne(element, element.rect, 0, true);
}

void SceneDrawer::DrawNamedVisual(const std::string& name, const Rect& rect) {
  if (!skin_) {
    return;
  }
  if (const Node* visual = skin_->Find(name)) {
    DrawVisual(*visual, rect, 1);
  }
}

void SceneDrawer::DrawOne(const Element& element, const Rect& rect, int depth,
                          bool is_owner) {
  if (!element.visible || !element.source) {
    return;
  }
  Rect placed = rect;
  float opacity = element.opacity;
  if (!Animate(element, &placed, &opacity, is_owner)) {
    return;
  }
  if (painter_ && painter_(element, placed)) {
    return;
  }

  const AnimatedProperties* animation = AnimationOf(element, is_owner);

  // A turned element turns its whole subtree with it, so the transform stays
  // on until its children have been drawn.
  const float degrees =
      element.rotation +
      (animation && animation->has_rotation ? animation->rotation : 0.0f);
  const bool turning = degrees != 0.0f;
  const bool was_rotated = draw_->rotated();
  if (turning) {
    draw_->SetRotation(degrees, placed.x + element.pivot_x,
                       placed.y + element.pivot_y);
  }

  const Node& node = *element.source;
  if (element.class_name == "XuiFigure") {
    DrawFigure(node, placed, false, animation, opacity);
  } else if (element.class_name == "XuiImage") {
    DrawImage(node, placed, opacity);
  } else if (element.class_name == "XuiLabel" ||
             element.class_name == "XuiText" ||
             element.class_name == "XuiButton") {
    DrawText(element, node, placed, opacity);
  }

  // The control's own look lives in the skin, keyed by its Visual property.
  if (element.visual && depth < kMaxVisualDepth) {
    DrawVisual(*element.visual, placed, depth + 1);
  }

  // Children were laid out against the element's authored position, so a
  // timeline that moved it has to carry them along.
  const float dx = placed.x - rect.x;
  const float dy = placed.y - rect.y;
  for (const Element& child : element.children) {
    Rect child_rect = child.rect;
    child_rect.x += dx;
    child_rect.y += dy;
    DrawOne(child, child_rect, depth);
  }

  if (turning && !was_rotated) {
    draw_->ClearRotation();
  }
}

void SceneDrawer::DrawVisual(const Node& visual, const Rect& rect, int depth) {
  if (depth >= kMaxVisualDepth) {
    return;
  }
  // The visual is authored in its own space; place it over the control.
  const Layout layout(skin_);
  Element instance = layout.Build(visual, rect.x, rect.y, 1.0f);
  // A visual is often authored as a bare container with no size of its own,
  // and its art is what has the extent. Falling back to a scale of one there
  // drew every such control at its authored size no matter how big the
  // control was.
  float width = instance.rect.width;
  float height = instance.rect.height;
  if (width <= 0.0f || height <= 0.0f) {
    float right = instance.rect.x;
    float bottom = instance.rect.y;
    std::function<void(const Element&)> extend = [&](const Element& element) {
      if (element.visible) {
        right = std::max(right, element.rect.x + element.rect.width);
        bottom = std::max(bottom, element.rect.y + element.rect.height);
      }
      for (const Element& child : element.children) {
        extend(child);
      }
    };
    extend(instance);
    if (width <= 0.0f) {
      width = right - instance.rect.x;
    }
    if (height <= 0.0f) {
      height = bottom - instance.rect.y;
    }
  }
  const float scale_x = width > 0.0f ? rect.width / width : 1.0f;
  const float scale_y = height > 0.0f ? rect.height / height : 1.0f;

  std::function<void(const Element&)> visit = [&](const Element& element) {
    if (!element.visible || !element.source) {
      return;
    }
    Rect placed;
    placed.x = rect.x + (element.rect.x - instance.rect.x) * scale_x;
    placed.y = rect.y + (element.rect.y - instance.rect.y) * scale_y;
    placed.width = element.rect.width * scale_x;
    placed.height = element.rect.height * scale_y;
    // The visual's own root is what its timeline is attached to; its children
    // are only driven when the timeline names them.
    const bool is_owner = &element == &instance;
    float opacity = element.opacity;
    if (!Animate(element, &placed, &opacity, is_owner)) {
      return;
    }
    if (painter_ && painter_(element, placed)) {
      return;
    }
    const AnimatedProperties* animation = AnimationOf(element, is_owner);
    const Node& node = *element.source;
    if (element.class_name == "XuiFigure") {
      DrawFigure(node, placed, true, animation, opacity);
    } else if (element.class_name == "XuiImage") {
      DrawImage(node, placed, opacity);
    }
    for (const Element& child : element.children) {
      visit(child);
    }
  };
  visit(instance);
}

void SceneDrawer::DrawFigure(const Node& node, const Rect& rect, bool from_skin,
                             const AnimatedProperties* animation,
                             float opacity) {
  const Value* points = Property(node, "Points");
  const auto* table = from_skin ? skin_figures_ : figures_;
  if (!points || !table) {
    return;
  }
  const auto figure = table->find(points->index);
  if (figure == table->end()) {
    return;
  }
  // A timeline drives a brush colour through the brush's own property, so an
  // animated colour replaces the authored one.
  const auto animated = [&](const char* name, uint32_t authored) -> uint32_t {
    if (!animation) {
      return authored;
    }
    const auto found = animation->colors.find(name);
    return found == animation->colors.end() ? authored : found->second;
  };
  if (HasBrush(node, "Fill")) {
    const uint32_t fill = animated(
        "FillColor", BrushColor(node, "Fill", "FillColor", 0xFF808080));
    const uint32_t gradient = animated(
        "GradientColor", BrushColor(node, "Fill", "GradientColor", fill));
    float rotation = 0.0f;
    if (const Value* brush = Property(node, "Fill")) {
      if (brush->object) {
        rotation = FloatProperty(*brush->object, "GradientRotation", 0.0f);
      }
    }
    draw_->FillPath(figure->second, rect, Fade(fill, opacity),
                    Fade(gradient, opacity), rotation);
  }
  if (HasBrush(node, "Stroke")) {
    const uint32_t stroke = animated(
        "StrokeColor", BrushColor(node, "Stroke", "StrokeColor", 0x00000000));
    if (stroke & 0xFF000000u) {
      draw_->StrokeRect(rect, Fade(stroke, opacity));
    }
  }
}

void SceneDrawer::DrawImage(const Node& node, const Rect& rect, float opacity) {
  const std::string path = StringProperty(node, "ImagePath");
  const PackageEntry* art = FindArt(path);
  const uint8_t* data = art ? art->data : nullptr;
  size_t size = art ? art->size : 0;
  // Only once every package has declined it: a title icon is a PNG the host
  // holds, not an entry in any XZP.
  std::span<const uint8_t> external;
  if (!data && art_source_) {
    external = art_source_(path);
    data = external.data();
    size = external.size();
  }
  if (!data || !size) {
    return;
  }
  xe::ui::ImmediateTexture* texture =
      draw_->ImageTexture(path, data, size, nullptr, nullptr);
  if (texture) {
    draw_->DrawImage(texture, rect, Fade(0xFFFFFFFF, opacity));
  }
}

void SceneDrawer::DrawText(const Element& element, const Node& node,
                           const Rect& rect, float opacity) {
  std::string text;
  if (!text_source_ || !text_source_(element, &text)) {
    text = StringProperty(node, "Text");
  }
  if (text.empty()) {
    return;
  }
  const std::u32string wide = ToU32(text);
  float pixels = std::max(12.0f, std::min(rect.height * 0.7f, 28.0f));
  // Item names run long; shrink to the control rather than spilling over the
  // neighbouring ones.
  const float room = std::max(rect.width - 8.0f, 1.0f);
  for (int i = 0; i < 6 && draw_->MeasureText(wide, pixels) > room; ++i) {
    pixels *= 0.85f;
  }
  draw_->DrawTextIn(wide, rect, pixels, Fade(0xFFF0F0F0, opacity),
                    TextAlign::kCenter);
}

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
