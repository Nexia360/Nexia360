/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_DRAW_H_
#define XENIA_KERNEL_XAM_XUI_DRAW_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xam/xui_font.h"
#include "xenia/kernel/xam/xui_runtime.h"
#include "xenia/ui/immediate_drawer.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

struct PathPoint {
  float x = 0.0f;
  float y = 0.0f;
};

// One CUST record: a closed path of cubic segments, authored in its own space.
struct FigurePath {
  float width = 0.0f;
  float height = 0.0f;
  std::vector<PathPoint> points;
};

bool ParseFigurePaths(const uint8_t* data, size_t size,
                      std::map<uint32_t, FigurePath>* out_paths);

enum class TextAlign {
  kLeft,
  kCenter,
  kRight,
};

class Draw {
 public:
  Draw(xe::ui::ImmediateDrawer* drawer, Font* font)
      : drawer_(drawer), font_(font) {}

  void SetScale(float scale) { scale_ = scale; }

  // Turns everything drawn from here on about a point in design space. Text,
  // art and vector fills all reach the drawer through the same quad, so one
  // transform covers the lot.
  void SetRotation(float degrees, float pivot_x, float pivot_y);
  void ClearRotation() { rotated_ = false; }
  bool rotated() const { return rotated_; }

  void FillRect(const Rect& rect, uint32_t color);
  void StrokeRect(const Rect& rect, uint32_t color);
  void FillPath(const FigurePath& path, const Rect& rect, uint32_t color,
                uint32_t gradient_color, float gradient_rotation);
  void DrawText(const std::u32string& text, float x, float y, float pixels,
                uint32_t color);
  void DrawTextIn(const std::u32string& text, const Rect& rect, float pixels,
                  uint32_t color, TextAlign align);
  void DrawImage(xe::ui::ImmediateTexture* texture, const Rect& rect,
                 uint32_t color);

  xe::ui::ImmediateTexture* GlyphTexture(uint32_t glyph, float pixels);
  // Decodes a package image (they are all plain PNG) and caches it by key.
  // `desaturate` drains the colour out of it on the way in - the console
  // greys everything the player is not looking at - and is cached separately
  // from the coloured one.
  xe::ui::ImmediateTexture* ImageTexture(const std::string& key,
                                         const uint8_t* data, size_t size,
                                         int* out_width, int* out_height,
                                         bool desaturate = false);
  // Already-decoded RGBA, for anything that renders its own pixels (the
  // avatar preview). `replace` re-uploads over the cached entry.
  xe::ui::ImmediateTexture* RawTexture(const std::string& key,
                                       const uint8_t* rgba, int width,
                                       int height, bool replace);
  float MeasureText(const std::u32string& text, float pixels);

 private:
  void Quad(float x, float y, float width, float height, uint32_t color,
            xe::ui::ImmediateTexture* texture);
  // Applies the current rotation to a point already in device space.
  void Place(float* x, float* y) const;
  xe::ui::ImmediateTexture* WhiteTexture();

  xe::ui::ImmediateDrawer* drawer_ = nullptr;
  Font* font_ = nullptr;
  float scale_ = 1.0f;
  bool rotated_ = false;
  float rotation_sin_ = 0.0f;
  float rotation_cos_ = 1.0f;
  float rotation_pivot_x_ = 0.0f;
  float rotation_pivot_y_ = 0.0f;
  struct Image {
    std::unique_ptr<xe::ui::ImmediateTexture> texture;
    int width = 0;
    int height = 0;
  };

  std::unique_ptr<xe::ui::ImmediateTexture> white_;
  std::map<uint64_t, std::unique_ptr<xe::ui::ImmediateTexture>> glyph_textures_;
  std::map<std::string, Image> images_;
};

std::u32string ToU32(const std::string_view text);

// Scene colours are "#AARRGGBB" strings; a vertex wants 0xAABBGGRR.
uint32_t ParseColor(const std::string_view text, uint32_t fallback);

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
