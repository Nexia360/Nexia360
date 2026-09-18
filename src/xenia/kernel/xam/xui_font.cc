/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "third_party/zlib-ng/zlib-ng.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

constexpr size_t kHeaderSize = 0x118;
constexpr float kSubSamples = 4.0f;

uint16_t Read16(const uint8_t* data) {
  return xe::load_and_swap<uint16_t>(data);
}
uint32_t Read32(const uint8_t* data) {
  return xe::load_and_swap<uint32_t>(data);
}
int16_t ReadSigned16(const uint8_t* data) {
  return static_cast<int16_t>(xe::load_and_swap<uint16_t>(data));
}

bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>* out,
             size_t expected) {
  out->assign(expected ? expected : size * 4, 0);
  zng_stream stream;
  std::memset(&stream, 0, sizeof(stream));
  if (zng_inflateInit(&stream) != Z_OK) {
    return false;
  }
  stream.next_in = const_cast<uint8_t*>(data);
  stream.avail_in = static_cast<uint32_t>(size);
  size_t produced = 0;
  int result = Z_OK;
  while (true) {
    stream.next_out = out->data() + produced;
    stream.avail_out = static_cast<uint32_t>(out->size() - produced);
    result = zng_inflate(&stream, Z_NO_FLUSH);
    produced = out->size() - stream.avail_out;
    if (result == Z_STREAM_END) {
      break;
    }
    if (result != Z_OK) {
      zng_inflateEnd(&stream);
      return false;
    }
    if (stream.avail_out == 0) {
      out->resize(out->size() * 2);
      continue;
    }
  }
  zng_inflateEnd(&stream);
  out->resize(produced);
  return true;
}

}  // namespace

bool Font::Load(const uint8_t* data, size_t size) {
  if (!data || size < kHeaderSize || std::memcmp(data, "xttf", 4) != 0) {
    return false;
  }
  image_.assign(data, data + size);
  const uint32_t compressed_directory = Read32(image_.data() + 0x10C);
  const uint32_t directory_size = Read32(image_.data() + 0x110);
  if (kHeaderSize + compressed_directory > image_.size()) {
    return false;
  }
  if (!Inflate(image_.data() + kHeaderSize, compressed_directory, &directory_,
               directory_size)) {
    XELOGW("xui font: directory inflate failed");
    return false;
  }
  if (!ParseDirectory() || !ParseMetrics() || !ParseCharacterMap()) {
    return false;
  }
  ParseName();
  return true;
}

// The sfnt name table, so an extracted font keeps the name the console gave
// it. Platform 3 strings are UTF-16BE; platform 1 is single byte.
void Font::ParseName() {
  size_t size = 0;
  const uint8_t* table = TableData("name", &size);
  if (!table || size < 6) {
    return;
  }
  const uint16_t count = Read16(table + 2);
  const uint16_t storage = Read16(table + 4);
  for (uint16_t i = 0; i < count; ++i) {
    const size_t at = 6 + size_t(i) * 12;
    if (at + 12 > size) {
      break;
    }
    const uint16_t platform = Read16(table + at);
    const uint16_t name_id = Read16(table + at + 6);
    const uint16_t length = Read16(table + at + 8);
    const uint16_t offset = Read16(table + at + 10);
    if (name_id != 4 && name_id != 6) {
      continue;
    }
    if (size_t(storage) + offset + length > size) {
      continue;
    }
    const uint8_t* value = table + storage + offset;
    std::string text;
    if (platform == 3) {
      for (uint16_t at_value = 0; at_value + 1 < length; at_value += 2) {
        const uint16_t code = Read16(value + at_value);
        if (code && code < 0x80) {
          text.push_back(static_cast<char>(code));
        }
      }
    } else {
      for (uint16_t at_value = 0; at_value < length; ++at_value) {
        if (value[at_value]) {
          text.push_back(static_cast<char>(value[at_value]));
        }
      }
    }
    if (text.empty()) {
      continue;
    }
    if (name_id == 6) {
      name_ = text;
      return;
    }
    if (name_.empty()) {
      name_ = text;
    }
  }
}

bool Font::ParseDirectory() {
  if (directory_.size() < 12) {
    return false;
  }
  const uint16_t count = Read16(directory_.data() + 4);
  if (directory_.size() < 12 + size_t(count) * 16) {
    return false;
  }
  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* entry = directory_.data() + 12 + size_t(i) * 16;
    std::string tag(reinterpret_cast<const char*>(entry), 4);
    tables_[tag] = {Read32(entry + 8), Read32(entry + 12)};
  }

  size_t xttf_size = 0;
  if (const uint8_t* meta = TableData("xttf", &xttf_size)) {
    if (xttf_size >= 8) {
      page_size_ = Read32(meta + 4);
    }
  }
  if (!page_size_) {
    page_size_ = 0x4000;
  }

  const auto glyph_table = tables_.find("xglf");
  if (glyph_table == tables_.end()) {
    return false;
  }
  if (glyph_table->second.first >= image_.size()) {
    return false;
  }
  glyph_region_ = image_.data() + glyph_table->second.first;
  glyph_region_size_ = std::min<size_t>(
      glyph_table->second.second, image_.size() - glyph_table->second.first);

  size_t location_size = 0;
  const uint8_t* locations = TableData("xloc", &location_size);
  if (!locations || location_size < 8) {
    return false;
  }
  const size_t entries = location_size / 4;
  locations_.resize(entries);
  for (size_t i = 0; i < entries; ++i) {
    locations_[i] = Read32(locations + i * 4);
  }
  glyph_count_ = static_cast<uint32_t>(entries - 1);
  return true;
}

const uint8_t* Font::TableData(const std::string_view tag,
                               size_t* out_size) const {
  const auto entry = tables_.find(std::string(tag));
  if (entry == tables_.end()) {
    return nullptr;
  }
  const uint32_t offset = entry->second.first;
  const uint32_t length = entry->second.second;
  if (offset + length > directory_.size()) {
    return nullptr;
  }
  *out_size = length;
  return directory_.data() + offset;
}

bool Font::ParseMetrics() {
  size_t head_size = 0;
  if (const uint8_t* head = TableData("head", &head_size)) {
    if (head_size >= 54) {
      units_per_em_ = Read16(head + 18);
    }
  }
  if (!units_per_em_) {
    units_per_em_ = 2048;
  }
  size_t hhea_size = 0;
  if (const uint8_t* hhea = TableData("hhea", &hhea_size)) {
    if (hhea_size >= 36) {
      ascent_ = ReadSigned16(hhea + 4);
      descent_ = ReadSigned16(hhea + 6);
      line_gap_ = ReadSigned16(hhea + 8);
      metric_count_ = Read16(hhea + 34);
    }
  }
  return true;
}

bool Font::ParseCharacterMap() {
  size_t cmap_size = 0;
  const uint8_t* cmap = TableData("cmap", &cmap_size);
  if (!cmap || cmap_size < 4) {
    return true;
  }
  const uint16_t tables = Read16(cmap + 2);
  uint32_t selected = 0;
  for (uint16_t i = 0; i < tables && 4 + size_t(i) * 8 + 8 <= cmap_size; ++i) {
    const uint8_t* record = cmap + 4 + size_t(i) * 8;
    const uint16_t platform = Read16(record);
    const uint16_t encoding = Read16(record + 2);
    const uint32_t offset = Read32(record + 4);
    if (platform == 3 && (encoding == 1 || encoding == 10)) {
      selected = offset;
      if (encoding == 10) {
        break;
      }
    }
  }
  if (!selected || selected + 4 > cmap_size) {
    return true;
  }
  const uint8_t* table = cmap + selected;
  const uint16_t format = Read16(table);
  if (format == 4) {
    const uint16_t segment_bytes = Read16(table + 6);
    const uint16_t segments = segment_bytes / 2;
    const uint8_t* ends = table + 14;
    const uint8_t* starts = ends + segment_bytes + 2;
    const uint8_t* deltas = starts + segment_bytes;
    const uint8_t* ranges = deltas + segment_bytes;
    for (uint16_t i = 0; i < segments; ++i) {
      const uint16_t end = Read16(ends + i * 2);
      const uint16_t start = Read16(starts + i * 2);
      const int16_t delta = ReadSigned16(deltas + i * 2);
      const uint16_t range = Read16(ranges + i * 2);
      if (start > end) {
        continue;
      }
      for (uint32_t code = start; code <= end && code != 0xFFFF; ++code) {
        uint32_t glyph = 0;
        if (range == 0) {
          glyph = (code + delta) & 0xFFFF;
        } else {
          const uint8_t* slot = ranges + i * 2 + range + (code - start) * 2;
          if (slot + 2 > cmap + cmap_size) {
            continue;
          }
          glyph = Read16(slot);
          if (glyph) {
            glyph = (glyph + delta) & 0xFFFF;
          }
        }
        if (glyph) {
          character_map_[code] = glyph;
        }
      }
    }
  } else if (format == 12) {
    const uint32_t groups = Read32(table + 12);
    for (uint32_t i = 0; i < groups; ++i) {
      const uint8_t* group = table + 16 + size_t(i) * 12;
      if (group + 12 > cmap + cmap_size) {
        break;
      }
      const uint32_t start = Read32(group);
      const uint32_t end = Read32(group + 4);
      const uint32_t glyph = Read32(group + 8);
      for (uint32_t code = start; code <= end && code - start < 0x10000;
           ++code) {
        character_map_[code] = glyph + (code - start);
      }
    }
  }
  return true;
}

uint32_t Font::GlyphIndex(char32_t character) const {
  const auto entry = character_map_.find(character);
  return entry == character_map_.end() ? 0 : entry->second;
}

const std::vector<uint8_t>* Font::Page(uint32_t index) {
  const auto cached = pages_.find(index);
  if (cached != pages_.end()) {
    return &cached->second;
  }
  const size_t offset = size_t(index) * 4096;
  if (offset >= glyph_region_size_) {
    return nullptr;
  }
  const size_t available = glyph_region_size_ - offset;
  std::vector<uint8_t> inflated;
  if (!Inflate(glyph_region_ + offset, std::min<size_t>(available, 4096),
               &inflated, page_size_)) {
    return nullptr;
  }
  auto result = pages_.emplace(index, std::move(inflated));
  return &result.first->second;
}

bool Font::GlyphData(uint32_t glyph, const uint8_t** out_data,
                     size_t* out_size) {
  if (glyph + 1 >= locations_.size()) {
    return false;
  }
  const uint32_t start = locations_[glyph];
  const uint32_t end = locations_[glyph + 1];
  const uint32_t page = start >> 16;
  const uint32_t offset = start & 0xFFFF;
  const uint32_t next_page = end >> 16;
  const uint32_t next_offset = end & 0xFFFF;
  if (page != next_page) {
    const std::vector<uint8_t>* data = Page(page);
    if (!data || offset >= data->size()) {
      return false;
    }
    *out_data = data->data() + offset;
    *out_size = data->size() - offset;
    return true;
  }
  if (next_offset <= offset) {
    return false;
  }
  const std::vector<uint8_t>* data = Page(page);
  if (!data || next_offset > data->size()) {
    return false;
  }
  *out_data = data->data() + offset;
  *out_size = next_offset - offset;
  return true;
}

bool Font::Metrics(uint32_t glyph, GlyphMetrics* out_metrics) {
  size_t hmtx_size = 0;
  const uint8_t* hmtx = TableData("hmtx", &hmtx_size);
  if (hmtx && metric_count_) {
    const uint32_t index = std::min<uint32_t>(glyph, metric_count_ - 1);
    if (size_t(index) * 4 + 4 <= hmtx_size) {
      out_metrics->advance = static_cast<int16_t>(Read16(hmtx + index * 4));
      out_metrics->bearing = ReadSigned16(hmtx + index * 4 + 2);
    }
  }
  const uint8_t* data = nullptr;
  size_t size = 0;
  if (GlyphData(glyph, &data, &size) && size >= 10) {
    out_metrics->min_x = ReadSigned16(data + 2);
    out_metrics->min_y = ReadSigned16(data + 4);
    out_metrics->max_x = ReadSigned16(data + 6);
    out_metrics->max_y = ReadSigned16(data + 8);
  }
  return true;
}

bool Font::Outline(uint32_t glyph, std::vector<Contour>* out_contours,
                   GlyphMetrics* out_metrics, int depth) {
  if (depth > 4) {
    return false;
  }
  const uint8_t* data = nullptr;
  size_t size = 0;
  if (!GlyphData(glyph, &data, &size) || size < 10) {
    return true;
  }
  const int16_t contour_count = ReadSigned16(data);
  out_metrics->min_x = ReadSigned16(data + 2);
  out_metrics->min_y = ReadSigned16(data + 4);
  out_metrics->max_x = ReadSigned16(data + 6);
  out_metrics->max_y = ReadSigned16(data + 8);

  if (contour_count < 0) {
    size_t at = 10;
    while (at + 4 <= size) {
      const uint16_t flags = Read16(data + at);
      const uint16_t component = Read16(data + at + 2);
      at += 4;
      int32_t dx = 0;
      int32_t dy = 0;
      if (flags & 0x0001) {
        if (at + 4 > size) {
          break;
        }
        dx = ReadSigned16(data + at);
        dy = ReadSigned16(data + at + 2);
        at += 4;
      } else {
        if (at + 2 > size) {
          break;
        }
        dx = static_cast<int8_t>(data[at]);
        dy = static_cast<int8_t>(data[at + 1]);
        at += 2;
      }
      if (flags & 0x0008) {
        at += 2;
      } else if (flags & 0x0040) {
        at += 4;
      } else if (flags & 0x0080) {
        at += 8;
      }
      std::vector<Contour> nested;
      GlyphMetrics ignored;
      if (Outline(component, &nested, &ignored, depth + 1)) {
        for (auto& contour : nested) {
          for (auto& point : contour.points) {
            point.first += dx;
            point.second += dy;
          }
          out_contours->push_back(std::move(contour));
        }
      }
      if (!(flags & 0x0020)) {
        break;
      }
    }
    return true;
  }

  const size_t ends_at = 10;
  if (ends_at + size_t(contour_count) * 2 + 2 > size) {
    return true;
  }
  std::vector<uint16_t> ends(contour_count);
  for (int16_t i = 0; i < contour_count; ++i) {
    ends[i] = Read16(data + ends_at + size_t(i) * 2);
  }
  const uint32_t point_count = contour_count ? ends.back() + 1u : 0u;
  size_t at = ends_at + size_t(contour_count) * 2;
  const uint16_t instruction_length = Read16(data + at);
  at += 2 + instruction_length;

  std::vector<uint8_t> flags;
  flags.reserve(point_count);
  while (flags.size() < point_count && at < size) {
    const uint8_t flag = data[at++];
    flags.push_back(flag);
    if (flag & 0x08) {
      if (at >= size) {
        break;
      }
      uint8_t repeat = data[at++];
      while (repeat-- && flags.size() < point_count) {
        flags.push_back(flag);
      }
    }
  }
  if (flags.size() < point_count) {
    return true;
  }

  std::vector<int32_t> xs(point_count);
  int32_t value = 0;
  for (uint32_t i = 0; i < point_count; ++i) {
    const uint8_t flag = flags[i];
    if (flag & 0x02) {
      if (at >= size) {
        break;
      }
      const uint8_t delta = data[at++];
      value += (flag & 0x10) ? delta : -int32_t(delta);
    } else if (!(flag & 0x10)) {
      if (at + 2 > size) {
        break;
      }
      value += ReadSigned16(data + at);
      at += 2;
    }
    xs[i] = value;
  }
  std::vector<int32_t> ys(point_count);
  value = 0;
  for (uint32_t i = 0; i < point_count; ++i) {
    const uint8_t flag = flags[i];
    if (flag & 0x04) {
      if (at >= size) {
        break;
      }
      const uint8_t delta = data[at++];
      value += (flag & 0x20) ? delta : -int32_t(delta);
    } else if (!(flag & 0x20)) {
      if (at + 2 > size) {
        break;
      }
      value += ReadSigned16(data + at);
      at += 2;
    }
    ys[i] = value;
  }

  uint32_t start = 0;
  for (int16_t c = 0; c < contour_count; ++c) {
    const uint32_t end = ends[c];
    if (end < start || end >= point_count) {
      break;
    }
    const uint32_t count = end - start + 1;
    Contour contour;
    auto on_curve = [&](uint32_t i) {
      return (flags[start + (i % count)] & 0x01) != 0;
    };
    auto point_at = [&](uint32_t i) {
      const uint32_t index = start + (i % count);
      return std::make_pair(float(xs[index]), float(ys[index]));
    };
    std::pair<float, float> origin;
    if (on_curve(0)) {
      origin = point_at(0);
    } else if (on_curve(count - 1)) {
      origin = point_at(count - 1);
    } else {
      const auto a = point_at(0);
      const auto b = point_at(count - 1);
      origin = {(a.first + b.first) * 0.5f, (a.second + b.second) * 0.5f};
    }
    contour.points.push_back(origin);
    auto current = origin;
    for (uint32_t i = 1; i <= count; ++i) {
      const auto point = point_at(i);
      if (on_curve(i)) {
        contour.points.push_back(point);
        current = point;
        continue;
      }
      auto next = point_at(i + 1);
      if (!on_curve(i + 1)) {
        next = {(point.first + next.first) * 0.5f,
                (point.second + next.second) * 0.5f};
      }
      constexpr int kSteps = 8;
      for (int step = 1; step <= kSteps; ++step) {
        const float t = float(step) / kSteps;
        const float u = 1.0f - t;
        contour.points.push_back(
            {u * u * current.first + 2 * u * t * point.first +
                 t * t * next.first,
             u * u * current.second + 2 * u * t * point.second +
                 t * t * next.second});
      }
      current = next;
    }
    out_contours->push_back(std::move(contour));
    start = end + 1;
  }
  return true;
}

const GlyphBitmap* Font::Raster(uint32_t glyph, float pixels) {
  const uint64_t key = (uint64_t(glyph) << 16) | uint32_t(pixels * 4.0f);
  const auto cached = raster_cache_.find(key);
  if (cached != raster_cache_.end()) {
    return &cached->second;
  }

  std::vector<Contour> contours;
  GlyphMetrics metrics;
  Metrics(glyph, &metrics);
  Outline(glyph, &contours, &metrics, 0);

  const float scale = pixels / units_per_em_;
  GlyphBitmap bitmap;
  bitmap.advance = metrics.advance * scale;
  if (contours.empty()) {
    auto result = raster_cache_.emplace(key, std::move(bitmap));
    return &result.first->second;
  }

  float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
  for (const auto& contour : contours) {
    for (const auto& point : contour.points) {
      min_x = std::min(min_x, point.first * scale);
      max_x = std::max(max_x, point.first * scale);
      min_y = std::min(min_y, -point.second * scale);
      max_y = std::max(max_y, -point.second * scale);
    }
  }
  const int32_t left = int32_t(std::floor(min_x));
  const int32_t top = int32_t(std::floor(min_y));
  const int32_t width = int32_t(std::ceil(max_x)) - left + 1;
  const int32_t height = int32_t(std::ceil(max_y)) - top + 1;
  if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
    auto result = raster_cache_.emplace(key, std::move(bitmap));
    return &result.first->second;
  }

  bitmap.width = width;
  bitmap.height = height;
  bitmap.left = left;
  bitmap.top = top;
  bitmap.coverage.assign(size_t(width) * height, 0);

  struct Edge {
    float x0, y0, x1, y1;
  };
  std::vector<Edge> edges;
  for (const auto& contour : contours) {
    const size_t count = contour.points.size();
    for (size_t i = 0; i < count; ++i) {
      const auto& a = contour.points[i];
      const auto& b = contour.points[(i + 1) % count];
      edges.push_back({a.first * scale - left, -a.second * scale - top,
                       b.first * scale - left, -b.second * scale - top});
    }
  }

  std::vector<float> crossings;
  for (int32_t y = 0; y < height; ++y) {
    std::vector<float> row(width, 0.0f);
    for (int sub = 0; sub < int(kSubSamples); ++sub) {
      const float sample = y + (sub + 0.5f) / kSubSamples;
      crossings.clear();
      for (const auto& edge : edges) {
        const bool downward = edge.y0 <= sample && edge.y1 > sample;
        const bool upward = edge.y1 <= sample && edge.y0 > sample;
        if (!downward && !upward) {
          continue;
        }
        const float t = (sample - edge.y0) / (edge.y1 - edge.y0);
        crossings.push_back(edge.x0 + t * (edge.x1 - edge.x0));
      }
      std::sort(crossings.begin(), crossings.end());
      for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
        const float span_start = crossings[i];
        const float span_end = crossings[i + 1];
        const int32_t first =
            std::max<int32_t>(0, int32_t(std::floor(span_start)));
        const int32_t last =
            std::min<int32_t>(width - 1, int32_t(std::ceil(span_end)));
        for (int32_t x = first; x <= last; ++x) {
          const float covered = std::min<float>(x + 1.0f, span_end) -
                                std::max<float>(float(x), span_start);
          if (covered > 0.0f) {
            row[x] += covered / kSubSamples;
          }
        }
      }
    }
    for (int32_t x = 0; x < width; ++x) {
      const float amount = std::min(1.0f, row[x]);
      bitmap.coverage[size_t(y) * width + x] =
          static_cast<uint8_t>(amount * 255.0f + 0.5f);
    }
  }

  auto result = raster_cache_.emplace(key, std::move(bitmap));
  return &result.first->second;
}

float Font::Measure(const std::u32string& text, float pixels) {
  float advance = 0.0f;
  for (char32_t character : text) {
    const uint32_t glyph = GlyphIndex(character);
    GlyphMetrics metrics;
    Metrics(glyph, &metrics);
    advance += metrics.advance * pixels / units_per_em_;
  }
  return advance;
}

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
