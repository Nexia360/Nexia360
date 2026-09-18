/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_FONT_H_
#define XENIA_KERNEL_XAM_XUI_FONT_H_

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

struct GlyphMetrics {
  int16_t advance = 0;
  int16_t bearing = 0;
  int16_t min_x = 0;
  int16_t min_y = 0;
  int16_t max_x = 0;
  int16_t max_y = 0;
};

struct GlyphBitmap {
  int32_t width = 0;
  int32_t height = 0;
  int32_t left = 0;
  int32_t top = 0;
  float advance = 0.0f;
  std::vector<uint8_t> coverage;
};

class Font {
 public:
  bool Load(const uint8_t* data, size_t size);

  uint16_t units_per_em() const { return units_per_em_; }
  int16_t ascent() const { return ascent_; }
  int16_t descent() const { return descent_; }
  int16_t line_gap() const { return line_gap_; }
  uint32_t glyph_count() const { return glyph_count_; }
  const std::string& name() const { return name_; }
  float line_height(float pixels) const {
    return (ascent_ - descent_ + line_gap_) * pixels / units_per_em_;
  }

  uint32_t GlyphIndex(char32_t character) const;
  bool Metrics(uint32_t glyph, GlyphMetrics* out_metrics);
  const GlyphBitmap* Raster(uint32_t glyph, float pixels);
  float Measure(const std::u32string& text, float pixels);

 private:
  struct Contour {
    std::vector<std::pair<float, float>> points;
  };

  const uint8_t* TableData(const std::string_view tag, size_t* out_size) const;
  bool ParseDirectory();
  bool ParseMetrics();
  bool ParseCharacterMap();
  void ParseName();
  const std::vector<uint8_t>* Page(uint32_t index);
  bool GlyphData(uint32_t glyph, const uint8_t** out_data, size_t* out_size);
  bool Outline(uint32_t glyph, std::vector<Contour>* out_contours,
               GlyphMetrics* out_metrics, int depth);

  std::string name_;
  std::vector<uint8_t> image_;
  std::vector<uint8_t> directory_;
  std::map<std::string, std::pair<uint32_t, uint32_t>> tables_;
  std::vector<uint32_t> locations_;
  std::map<uint32_t, std::vector<uint8_t>> pages_;
  std::map<char32_t, uint32_t> character_map_;
  std::map<uint64_t, GlyphBitmap> raster_cache_;
  const uint8_t* glyph_region_ = nullptr;
  size_t glyph_region_size_ = 0;
  uint32_t page_size_ = 0;
  uint32_t glyph_count_ = 0;
  uint16_t units_per_em_ = 2048;
  int16_t ascent_ = 0;
  int16_t descent_ = 0;
  int16_t line_gap_ = 0;
  uint16_t metric_count_ = 0;
};

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
