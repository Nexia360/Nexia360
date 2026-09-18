/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_draw.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "third_party/stb/stb_image.h"
#include "xenia/base/memory.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

constexpr int kCurveSteps = 12;

float ReadFloat(const uint8_t* data) {
  const uint32_t bits = xe::load_and_swap<uint32_t>(data);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

PathPoint Cubic(const PathPoint& a, const PathPoint& b, const PathPoint& c,
                const PathPoint& d, float t) {
  const float u = 1.0f - t;
  PathPoint point;
  point.x = u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x +
            t * t * t * d.x;
  point.y = u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y +
            t * t * t * d.y;
  return point;
}

uint32_t Blend(uint32_t first, uint32_t second, float amount) {
  const float inverse = 1.0f - amount;
  uint32_t result = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const float a = static_cast<float>((first >> shift) & 0xFF);
    const float b = static_cast<float>((second >> shift) & 0xFF);
    const uint32_t value =
        static_cast<uint32_t>(a * inverse + b * amount) & 0xFF;
    result |= value << shift;
  }
  return result;
}

}  // namespace

bool ParseFigurePaths(const uint8_t* data, size_t size,
                      std::map<uint32_t, FigurePath>* out_paths) {
  out_paths->clear();
  size_t at = 0;
  while (at + 16 <= size) {
    const uint32_t record = xe::load_and_swap<uint32_t>(data + at);
    const size_t body = at + 4;
    if (record < 12 || body + record > size) {
      return false;
    }
    FigurePath path;
    path.width = ReadFloat(data + body);
    path.height = ReadFloat(data + body + 4);
    const uint32_t count = xe::load_and_swap<uint32_t>(data + body + 8);
    if (record != 12 + count * 24) {
      return false;
    }
    std::vector<PathPoint> nodes(size_t(count) * 3);
    for (uint32_t i = 0; i < count * 3; ++i) {
      nodes[i].x = ReadFloat(data + body + 12 + size_t(i) * 8);
      nodes[i].y = ReadFloat(data + body + 12 + size_t(i) * 8 + 4);
    }
    for (uint32_t i = 0; i < count; ++i) {
      const PathPoint& anchor = nodes[size_t(i) * 3];
      const PathPoint& out = nodes[size_t(i) * 3 + 1];
      const PathPoint& in = nodes[size_t(i) * 3 + 2];
      const PathPoint& next = nodes[(size_t((i + 1) % count)) * 3];
      if (path.points.empty()) {
        path.points.push_back(anchor);
      }
      for (int step = 1; step <= kCurveSteps; ++step) {
        path.points.push_back(
            Cubic(anchor, out, in, next, float(step) / kCurveSteps));
      }
    }
    out_paths->emplace(static_cast<uint32_t>(at), std::move(path));
    at = body + record;
  }
  return true;
}

std::u32string ToU32(const std::string_view text) {
  std::u32string out;
  out.reserve(text.size());
  size_t at = 0;
  while (at < text.size()) {
    const uint8_t lead = static_cast<uint8_t>(text[at]);
    char32_t code = lead;
    size_t extra = 0;
    if (lead >= 0xF0) {
      code = lead & 0x07;
      extra = 3;
    } else if (lead >= 0xE0) {
      code = lead & 0x0F;
      extra = 2;
    } else if (lead >= 0xC0) {
      code = lead & 0x1F;
      extra = 1;
    }
    ++at;
    for (size_t i = 0; i < extra && at < text.size(); ++i, ++at) {
      code = (code << 6) | (static_cast<uint8_t>(text[at]) & 0x3F);
    }
    out.push_back(code);
  }
  return out;
}

uint32_t ParseColor(const std::string_view text, uint32_t fallback) {
  if (text.size() != 9 || text[0] != '#') {
    return fallback;
  }
  uint32_t raw = 0;
  for (size_t i = 1; i < text.size(); ++i) {
    const char value = text[i];
    uint32_t digit;
    if (value >= '0' && value <= '9') {
      digit = uint32_t(value - '0');
    } else if (value >= 'a' && value <= 'f') {
      digit = uint32_t(value - 'a' + 10);
    } else if (value >= 'A' && value <= 'F') {
      digit = uint32_t(value - 'A' + 10);
    } else {
      return fallback;
    }
    raw = (raw << 4) | digit;
  }
  return (raw & 0xFF00FF00u) | ((raw & 0x00FF0000u) >> 16) |
         ((raw & 0x000000FFu) << 16);
}

// Nothing else in the tree draws untextured - ImGui and microprofile always
// bind one - so the immediate drawers' solid-colour path has no other caller.
// A white texel keeps every quad on the path that is known to work.
xe::ui::ImmediateTexture* Draw::WhiteTexture() {
  if (!white_) {
    const uint8_t texel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    white_ = drawer_->CreateTexture(
        1, 1, xe::ui::ImmediateTextureFilter::kNearest, true, texel);
  }
  return white_.get();
}

void Draw::SetRotation(float degrees, float pivot_x, float pivot_y) {
  rotated_ = degrees != 0.0f;
  const float radians = degrees * 3.14159265f / 180.0f;
  rotation_sin_ = std::sin(radians);
  rotation_cos_ = std::cos(radians);
  rotation_pivot_x_ = pivot_x * scale_;
  rotation_pivot_y_ = pivot_y * scale_;
}

void Draw::Place(float* x, float* y) const {
  if (!rotated_) {
    return;
  }
  const float dx = *x - rotation_pivot_x_;
  const float dy = *y - rotation_pivot_y_;
  *x = rotation_pivot_x_ + dx * rotation_cos_ - dy * rotation_sin_;
  *y = rotation_pivot_y_ + dx * rotation_sin_ + dy * rotation_cos_;
}

void Draw::Quad(float x, float y, float width, float height, uint32_t color,
                xe::ui::ImmediateTexture* texture) {
  if (width <= 0.0f || height <= 0.0f) {
    return;
  }
  if (!texture) {
    texture = WhiteTexture();
  }
  ui::ImmediateVertex vertices[4] = {
      {x, y, 0.0f, 0.0f, color},
      {x + width, y, 1.0f, 0.0f, color},
      {x + width, y + height, 1.0f, 1.0f, color},
      {x, y + height, 0.0f, 1.0f, color},
  };
  for (ui::ImmediateVertex& vertex : vertices) {
    Place(&vertex.x, &vertex.y);
  }
  const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};

  ui::ImmediateDrawBatch batch;
  batch.vertices = vertices;
  batch.vertex_count = 4;
  batch.indices = indices;
  batch.index_count = 6;
  drawer_->BeginDrawBatch(batch);

  ui::ImmediateDraw draw;
  draw.primitive_type = ui::ImmediatePrimitiveType::kTriangles;
  draw.count = 6;
  draw.texture = texture;
  drawer_->Draw(draw);
  drawer_->EndDrawBatch();
}

void Draw::FillRect(const Rect& rect, uint32_t color) {
  Quad(rect.x * scale_, rect.y * scale_, rect.width * scale_,
       rect.height * scale_, color, nullptr);
}

void Draw::StrokeRect(const Rect& rect, uint32_t color) {
  const float thickness = 1.0f;
  Quad(rect.x * scale_, rect.y * scale_, rect.width * scale_, thickness, color,
       nullptr);
  Quad(rect.x * scale_, (rect.y + rect.height) * scale_ - thickness,
       rect.width * scale_, thickness, color, nullptr);
  Quad(rect.x * scale_, rect.y * scale_, thickness, rect.height * scale_, color,
       nullptr);
  Quad((rect.x + rect.width) * scale_ - thickness, rect.y * scale_, thickness,
       rect.height * scale_, color, nullptr);
}

void Draw::FillPath(const FigurePath& path, const Rect& rect, uint32_t color,
                    uint32_t gradient_color, float gradient_rotation) {
  if (path.points.size() < 3) {
    return;
  }
  const float scale_x =
      path.width > 0.0f && rect.width > 0.0f ? rect.width / path.width : 1.0f;
  const float scale_y = path.height > 0.0f && rect.height > 0.0f
                            ? rect.height / path.height
                            : 1.0f;
  const bool gradient = gradient_color != color;
  // A rotation of zero sweeps the fill straight down, and the angle turns
  // clockwise from there. Measuring it from the x axis instead lays every
  // sheen on its side.
  const float radians = gradient_rotation * 3.14159265f / 180.0f;
  const float axis_x = std::sin(radians);
  const float axis_y = std::cos(radians);

  std::vector<ui::ImmediateVertex> vertices;
  std::vector<uint16_t> indices;
  vertices.reserve(path.points.size());

  // The ramp has to span the shape along the axis it is actually pointing,
  // so the extent is measured rather than assumed to be the width or the
  // height. Taking the absolute projection instead mirrored every gradient
  // about the shape's origin and turned a single sweep into two.
  float lowest = 0.0f;
  float highest = 0.0f;
  if (gradient) {
    bool first = true;
    for (const PathPoint& point : path.points) {
      const float projection = point.x * axis_x + point.y * axis_y;
      if (first || projection < lowest) {
        lowest = projection;
      }
      if (first || projection > highest) {
        highest = projection;
      }
      first = false;
    }
  }
  const float span = highest - lowest;

  auto shade = [&](float local_x, float local_y) {
    if (!gradient || span <= 0.0f) {
      return color;
    }
    const float amount = std::min(
        1.0f,
        std::max(0.0f, (local_x * axis_x + local_y * axis_y - lowest) / span));
    return Blend(color, gradient_color, amount);
  };

  for (const PathPoint& point : path.points) {
    ui::ImmediateVertex vertex;
    vertex.x = (rect.x + point.x * scale_x) * scale_;
    vertex.y = (rect.y + point.y * scale_y) * scale_;
    Place(&vertex.x, &vertex.y);
    vertex.u = 0.0f;
    vertex.v = 0.0f;
    vertex.color = shade(point.x, point.y);
    vertices.push_back(vertex);
  }
  for (size_t i = 1; i + 1 < vertices.size(); ++i) {
    indices.push_back(0);
    indices.push_back(static_cast<uint16_t>(i));
    indices.push_back(static_cast<uint16_t>(i + 1));
  }
  if (indices.empty()) {
    return;
  }

  ui::ImmediateDrawBatch batch;
  batch.vertices = vertices.data();
  batch.vertex_count = static_cast<int>(vertices.size());
  batch.indices = indices.data();
  batch.index_count = static_cast<int>(indices.size());
  drawer_->BeginDrawBatch(batch);

  ui::ImmediateDraw draw;
  draw.primitive_type = ui::ImmediatePrimitiveType::kTriangles;
  draw.count = static_cast<int>(indices.size());
  draw.texture = WhiteTexture();
  drawer_->Draw(draw);
  drawer_->EndDrawBatch();
}

xe::ui::ImmediateTexture* Draw::GlyphTexture(uint32_t glyph, float pixels) {
  const uint64_t key =
      (uint64_t(glyph) << 20) | uint32_t(std::lround(pixels * 4.0f));
  const auto cached = glyph_textures_.find(key);
  if (cached != glyph_textures_.end()) {
    return cached->second.get();
  }
  const GlyphBitmap* bitmap = font_->Raster(glyph, pixels);
  if (!bitmap || !bitmap->width || !bitmap->height) {
    return nullptr;
  }
  std::vector<uint8_t> rgba(size_t(bitmap->width) * bitmap->height * 4, 0xFF);
  for (size_t i = 0; i < bitmap->coverage.size(); ++i) {
    rgba[i * 4 + 3] = bitmap->coverage[i];
  }
  auto texture = drawer_->CreateTexture(bitmap->width, bitmap->height,
                                        xe::ui::ImmediateTextureFilter::kLinear,
                                        false, rgba.data());
  auto result = glyph_textures_.emplace(key, std::move(texture));
  return result.first->second.get();
}

xe::ui::ImmediateTexture* Draw::ImageTexture(const std::string& key,
                                             const uint8_t* data, size_t size,
                                             int* out_width, int* out_height,
                                             bool desaturate) {
  // The grey copy is a different texture, so it needs its own cache entry.
  const std::string cache_key = desaturate ? key + "\x01grey" : key;
  const auto cached = images_.find(cache_key);
  if (cached == images_.end()) {
    Image image;
    int width = 0;
    int height = 0;
    int channels = 0;
    uint8_t* pixels = stbi_load_from_memory(data, static_cast<int>(size),
                                            &width, &height, &channels, 4);
    if (pixels) {
      if (desaturate) {
        // Rec. 601 luma, which is what the console's own grey art matches.
        // Alpha is left alone so the art keeps its shape.
        const size_t count = size_t(width) * size_t(height);
        for (size_t i = 0; i < count; ++i) {
          uint8_t* texel = pixels + i * 4;
          const uint32_t luma =
              (texel[0] * 77u + texel[1] * 150u + texel[2] * 29u) >> 8;
          texel[0] = texel[1] = texel[2] = static_cast<uint8_t>(luma);
        }
      }
      image.width = width;
      image.height = height;
      image.texture = drawer_->CreateTexture(
          width, height, xe::ui::ImmediateTextureFilter::kLinear, false,
          pixels);
      stbi_image_free(pixels);
    }
    images_.emplace(cache_key, std::move(image));
  }
  const Image& image = images_[cache_key];
  if (out_width) {
    *out_width = image.width;
  }
  if (out_height) {
    *out_height = image.height;
  }
  return image.texture.get();
}

xe::ui::ImmediateTexture* Draw::RawTexture(const std::string& key,
                                           const uint8_t* rgba, int width,
                                           int height, bool replace) {
  if (width <= 0 || height <= 0 || !rgba) {
    return nullptr;
  }
  const auto cached = images_.find(key);
  if (cached != images_.end() && !replace) {
    return cached->second.texture.get();
  }
  Image image;
  image.width = width;
  image.height = height;
  image.texture = drawer_->CreateTexture(
      width, height, xe::ui::ImmediateTextureFilter::kLinear, false, rgba);
  if (cached != images_.end()) {
    cached->second = std::move(image);
    return cached->second.texture.get();
  }
  auto result = images_.emplace(key, std::move(image));
  return result.first->second.texture.get();
}

float Draw::MeasureText(const std::u32string& text, float pixels) {
  return font_ ? font_->Measure(text, pixels) : 0.0f;
}

void Draw::DrawText(const std::u32string& text, float x, float y, float pixels,
                    uint32_t color) {
  if (!font_) {
    return;
  }
  const float baseline =
      y + font_->ascent() * pixels / float(font_->units_per_em());
  float pen = x;
  for (char32_t character : text) {
    const uint32_t glyph = font_->GlyphIndex(character);
    const GlyphBitmap* bitmap = font_->Raster(glyph, pixels);
    if (!bitmap) {
      continue;
    }
    if (bitmap->width && bitmap->height) {
      if (xe::ui::ImmediateTexture* texture = GlyphTexture(glyph, pixels)) {
        Quad((pen + bitmap->left) * scale_, (baseline + bitmap->top) * scale_,
             bitmap->width * scale_, bitmap->height * scale_, color, texture);
      }
    }
    pen += bitmap->advance;
  }
}

void Draw::DrawTextIn(const std::u32string& text, const Rect& rect,
                      float pixels, uint32_t color, TextAlign align) {
  const float width = MeasureText(text, pixels);
  float x = rect.x;
  if (align == TextAlign::kCenter) {
    x = rect.x + (rect.width - width) * 0.5f;
  } else if (align == TextAlign::kRight) {
    x = rect.x + rect.width - width;
  }
  const float line = font_ ? font_->line_height(pixels) : pixels;
  const float y = rect.y + (rect.height - line) * 0.5f;
  DrawText(text, x, y, pixels, color);
}

void Draw::DrawImage(xe::ui::ImmediateTexture* texture, const Rect& rect,
                     uint32_t color) {
  Quad(rect.x * scale_, rect.y * scale_, rect.width * scale_,
       rect.height * scale_, color, texture);
}

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
