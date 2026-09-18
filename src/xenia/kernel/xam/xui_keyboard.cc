/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_keyboard.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <string_view>
#include <utility>

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

constexpr const char* kSceneName = "KeyboardBase.xur";
constexpr const char* kPackageName = "vk";

constexpr float kPanelTop = 148.0f;

constexpr uint32_t kPanelFill = 0xFFF2F0EE;
constexpr uint32_t kKeyFill = 0xFFECECEC;
constexpr uint32_t kKeyEdge = 0xFFD2CCC6;
constexpr uint32_t kKeyFocus = 0xFF00B97C;
constexpr uint32_t kKeyText = 0xFF202020;
constexpr uint32_t kKeyFocusText = 0xFFFFFFFF;
constexpr uint32_t kGlyphTint = 0xFF544840;
constexpr uint32_t kEditText = 0xFF181818;
constexpr uint32_t kDescriptionText = 0xFFF0F0F0;
constexpr float kLegendHeight = 34.0f;
constexpr uint32_t kLegendBand = 0xE0201814;
constexpr uint32_t kLegendText = 0xFFF0F0F0;
constexpr uint32_t kLegendHint = 0xFFA0A0A0;
constexpr uint32_t kKeyActive = 0xFFCCE8D8;
constexpr uint32_t kGlyphUntinted = 0xFFFFFFFF;
constexpr uint32_t kEditFill = 0xFFF8F6F4;
constexpr uint32_t kEditEdge = 0xFFB8B0A8;

const char* const kLower[] = {
    "abcdefg123", "hijklmn456", "opqrstu789", "vwxyz.@_0-", ",;:'\"()!?/",
};
const char* const kCaps[] = {
    "ABCDEFG123", "HIJKLMN456", "OPQRSTU789", "VWXYZ.@_0-", ",;:'\"()!?/",
};
const char* const kSymbols[] = {
    "!@#$%^&*()",
    "-_=+[]{};:",
    "'\",./?\\|~`",
    "<>\xE2\x82\xAC\xC2\xA3\xC2\xA5\xC2\xA2\xC2\xA7\xC2\xB6\xC2\xB0\xC2\xB1",
    "\xC2\xA9\xC2\xAE\xE2\x84\xA2\xE2\x80\xA2\xE2\x80\xA6\xC2\xB5\xC2\xBC\xC2"
    "\xBD\xC2\xBE\xC2\xBF",
};
const char* const kAccents[] = {
    "\xC3\xA0\xC3\xA1\xC3\xA2\xC3\xA3\xC3\xA4\xC3\xA5\xC3\xA6\xC3\xA7\xC3\xA8"
    "\xC3\xA9",
    "\xC3\xAA\xC3\xAB\xC3\xAC\xC3\xAD\xC3\xAE\xC3\xAF\xC3\xB1\xC3\xB2\xC3\xB3"
    "\xC3\xB4",
    "\xC3\xB5\xC3\xB6\xC3\xB8\xC3\xB9\xC3\xBA\xC3\xBB\xC3\xBC\xC3\xBD\xC3\xBF"
    "\xC3\x9F",
    "\xC3\x80\xC3\x81\xC3\x82\xC3\x83\xC3\x84\xC3\x85\xC3\x86\xC3\x87\xC3\x88"
    "\xC3\x89",
    "\xC3\x8A\xC3\x8B\xC3\x8C\xC3\x8D\xC3\x8E\xC3\x8F\xC3\x91\xC3\x92\xC3\x93"
    "\xC3\x94",
};

struct NamedKey {
  const char* id;
  const char* caption;
  const char* glyph;
  bool glyph_above;
};

// The side columns are Left/Symbols/Caps and Right/Accents/Done, with
// Backspace and Space along the bottom - not a PC layout.
const NamedKey kNamedKeys[] = {
    {"Key.Left", "Left", "LB.png", true},
    {"Key.Prev", "Symbols", "LT.png", true},
    {"Key.Caps", "Caps", "Caps.png", true},
    {"Key.Right", "Right", "RB.png", true},
    {"Key.Next", "Accents", "RT.png", true},
    {"Key.OK", "Done", "Done.png", true},
    {"Key.BS", "Backspace", "btn_x.png", false},
    {"Key.Spc", "Space", "btn_y.png", false},
};

const NamedKey* FindNamedKey(const std::string& id) {
  for (const NamedKey& named : kNamedKeys) {
    if (id == named.id) {
      return &named;
    }
  }
  return nullptr;
}

// "Key.<row>_<column>"
bool ParseCell(const std::string& id, int* out_row, int* out_column) {
  if (id.compare(0, 4, "Key.") != 0) {
    return false;
  }
  size_t at = 4;
  int row = 0;
  if (at >= id.size() || !std::isdigit(static_cast<unsigned char>(id[at]))) {
    return false;
  }
  while (at < id.size() && std::isdigit(static_cast<unsigned char>(id[at]))) {
    row = row * 10 + (id[at++] - '0');
  }
  if (at >= id.size() || id[at] != '_') {
    return false;
  }
  ++at;
  if (at >= id.size() || !std::isdigit(static_cast<unsigned char>(id[at]))) {
    return false;
  }
  int column = 0;
  while (at < id.size() && std::isdigit(static_cast<unsigned char>(id[at]))) {
    column = column * 10 + (id[at++] - '0');
  }
  if (at != id.size()) {
    return false;
  }
  *out_row = row;
  *out_column = column;
  return true;
}

void Walk(const Element& element,
          const std::function<void(const Element&)>& fn) {
  fn(element);
  for (const Element& child : element.children) {
    Walk(child, fn);
  }
}

const char* const* PageTable(KeyboardScreen::Page page) {
  switch (page) {
    case KeyboardScreen::Page::kCaps:
      return kCaps;
    case KeyboardScreen::Page::kSymbols:
      return kSymbols;
    case KeyboardScreen::Page::kAccents:
      return kAccents;
    default:
      return kLower;
  }
}

}  // namespace

KeyboardScreen::KeyboardScreen(const std::string& title,
                               const std::string& description,
                               const std::string& initial_text)
    : title_(title),
      description_(description),
      text_(initial_text),
      cursor_(initial_text.size()) {}

void KeyboardScreen::MoveCursor(int delta) {
  while (delta < 0 && cursor_ > 0) {
    --cursor_;
    while (cursor_ > 0 &&
           (static_cast<uint8_t>(text_[cursor_]) & 0xC0) == 0x80) {
      --cursor_;
    }
    ++delta;
  }
  while (delta > 0 && cursor_ < text_.size()) {
    ++cursor_;
    while (cursor_ < text_.size() &&
           (static_cast<uint8_t>(text_[cursor_]) & 0xC0) == 0x80) {
      ++cursor_;
    }
    --delta;
  }
}

bool KeyboardScreen::Prepare(AssetStore& assets) {
  package_ = assets.GetPackage(kPackageName);
  if (!package_) {
    XELOGW("xui keyboard: no {} package installed", kPackageName);
    return false;
  }
  Scene* scene = package_->Open(kSceneName);
  if (!scene) {
    XELOGW("xui keyboard: {} is not in the package", kSceneName);
    return false;
  }
  if (const Scene::Section* custom = scene->FindSection("CUST")) {
    ParseFigurePaths(custom->data, custom->size, &figures_);
  }

  shared_ = assets.GetPackage("sharedres");

  // The console composites this scene above a legend bar at the bottom of the
  // screen, so the scene gets the frame less that band.
  const Layout layout(assets.skin());
  root_ = layout.BuildFitted(scene->root(), kDesignWidth,
                             kDesignHeight - kLegendHeight);

  CollectKeys();
  if (keys_.empty()) {
    XELOGW("xui keyboard: the scene held no keys");
    return false;
  }
  ApplyCaptions();

  edit_ = layout.FindById(root_, "FormattedText");
  if (!edit_) {
    edit_ = layout.FindById(root_, "XuiEdit1");
  }
  description_element_ = layout.FindById(root_, "XuiText1");
  return true;
}

void KeyboardScreen::CollectKeys() {
  keys_.clear();
  grid_.clear();
  functions_.clear();

  // The scene carries several key sets stacked on each other - the Latin one,
  // the Japanese one, and the modal variants. Only KeysGroup's own children
  // are the Latin keyboard; walking the whole tree interleaves them.
  const Element* group = nullptr;
  Walk(root_, [&](const Element& element) {
    if (!group && element.id == "KeysGroup") {
      group = &element;
    }
  });
  if (!group) {
    group = &root_;
  }

  std::vector<const Element*> found;
  std::vector<Rect> cells;
  // An allowlist, not a denylist: KeysGroup also holds the Japanese-only
  // entries (JComma, JPeriod, the clause keys) and a hidden spare.
  for (const Element& element : group->children) {
    int row = 0;
    int column = 0;
    if (!element.visible ||
        (!ParseCell(element.id, &row, &column) && !FindNamedKey(element.id))) {
      continue;
    }
    bool duplicate = false;
    for (const Rect& taken : cells) {
      if (std::abs(taken.x - element.rect.x) < 2.0f &&
          std::abs(taken.y - element.rect.y) < 2.0f) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    cells.push_back(element.rect);
    found.push_back(&element);
  }

  std::vector<const Element*> characters;
  for (const Element* element : found) {
    int row = 0;
    int column = 0;
    if (ParseCell(element->id, &row, &column)) {
      characters.push_back(element);
      continue;
    }
    if (const NamedKey* named = FindNamedKey(element->id)) {
      Key key;
      key.element = element;
      key.id = element->id;
      key.glyph = named->glyph;
      key.caption = ToU32(named->caption);
      functions_.push_back(keys_.size());
      keys_.push_back(std::move(key));
    }
  }

  std::sort(characters.begin(), characters.end(),
            [](const Element* a, const Element* b) {
              if (std::abs(a->rect.y - b->rect.y) >= 6.0f) {
                return a->rect.y < b->rect.y;
              }
              return a->rect.x < b->rect.x;
            });

  for (const Element* element : characters) {
    if (grid_.empty() || std::abs(keys_[grid_.back().front()].element->rect.y -
                                  element->rect.y) >= 6.0f) {
      grid_.emplace_back();
    }
    Key key;
    key.element = element;
    key.id = element->id;
    key.character = true;
    key.row = static_cast<int>(grid_.size()) - 1;
    key.column = static_cast<int>(grid_.back().size());
    grid_.back().push_back(keys_.size());
    keys_.push_back(std::move(key));
  }

  if (!grid_.empty() && !grid_.front().empty()) {
    focus_ = grid_.front().front();
  }
}

void KeyboardScreen::ApplyCaptions() {
  const char* const* table = PageTable(page_);
  for (std::vector<size_t>& row : grid_) {
    for (size_t index : row) {
      Key& key = keys_[index];
      const std::u32string characters =
          key.row < 5 ? ToU32(table[key.row]) : std::u32string();
      key.caption = size_t(key.column) < characters.size()
                        ? std::u32string(1, characters[key.column])
                        : std::u32string();
    }
  }
}

void KeyboardScreen::SetPage(Page page) {
  page_ = page;
  ApplyCaptions();
}

const KeyboardScreen::Key* KeyboardScreen::Focused() const {
  return focus_ < keys_.size() ? &keys_[focus_] : nullptr;
}

void KeyboardScreen::MoveFocus(int dx, int dy) {
  const Key* current = Focused();
  if (!current) {
    return;
  }
  // Focus follows geometry, so the function keys around the grid are reachable
  // without hard-coding their neighbours.
  const Rect& from = current->element->rect;
  const float from_x = from.x + from.width * 0.5f;
  const float from_y = from.y + from.height * 0.5f;

  size_t best = focus_;
  float best_score = 0.0f;
  for (size_t i = 0; i < keys_.size(); ++i) {
    if (i == focus_) {
      continue;
    }
    const Rect& to = keys_[i].element->rect;
    const float to_x = to.x + to.width * 0.5f;
    const float to_y = to.y + to.height * 0.5f;
    const float along = (to_x - from_x) * dx + (to_y - from_y) * dy;
    if (along <= 0.5f) {
      continue;
    }
    const float across =
        std::abs((to_x - from_x) * dy) + std::abs((to_y - from_y) * dx);
    const float score = along + across * 3.0f;
    if (best == focus_ || score < best_score) {
      best = i;
      best_score = score;
    }
  }
  focus_ = best;
}

void KeyboardScreen::FocusKey(const std::string& id) {
  for (size_t i = 0; i < keys_.size(); ++i) {
    if (keys_[i].id == id) {
      focus_ = i;
      return;
    }
  }
}

void KeyboardScreen::Activate() {
  const Key* key = Focused();
  if (!key) {
    return;
  }
  if (key->character) {
    if (!key->caption.empty()) {
      Insert(key->caption[0]);
    }
    return;
  }
  if (key->id == "Key.Spc") {
    Insert(U' ');
  } else if (key->id == "Key.BS") {
    Backspace();
  } else if (key->id == "Key.OK") {
    Commit();
  } else if (key->id == "Key.Caps") {
    SetPage(page_ == Page::kCaps ? Page::kLower : Page::kCaps);
  } else if (key->id == "Key.Prev") {
    SetPage(page_ == Page::kSymbols ? Page::kLower : Page::kSymbols);
  } else if (key->id == "Key.Next") {
    SetPage(page_ == Page::kAccents ? Page::kLower : Page::kAccents);
  } else if (key->id == "Key.Left") {
    MoveCursor(-1);
  } else if (key->id == "Key.Right") {
    MoveCursor(1);
  }
}

void KeyboardScreen::Insert(char32_t character) {
  // Back to UTF-8; the accent and symbol pages are the only source of the
  // multi-byte ones.
  std::string encoded;
  if (character < 0x80) {
    encoded.push_back(static_cast<char>(character));
  } else if (character < 0x800) {
    encoded.push_back(static_cast<char>(0xC0 | (character >> 6)));
    encoded.push_back(static_cast<char>(0x80 | (character & 0x3F)));
  } else {
    encoded.push_back(static_cast<char>(0xE0 | (character >> 12)));
    encoded.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
    encoded.push_back(static_cast<char>(0x80 | (character & 0x3F)));
  }
  cursor_ = std::min(cursor_, text_.size());
  text_.insert(cursor_, encoded);
  cursor_ += encoded.size();
}

void KeyboardScreen::Backspace() {
  cursor_ = std::min(cursor_, text_.size());
  const size_t end = cursor_;
  MoveCursor(-1);
  if (cursor_ != end) {
    text_.erase(cursor_, end - cursor_);
  }
}

void KeyboardScreen::Commit() {
  done_ = true;
  closed_ = true;
}

void KeyboardScreen::Cancel() {
  cancelled_ = true;
  closed_ = true;
}

void KeyboardScreen::Draw(class Draw& draw, const Element& root) {
  const float zoom = root.rect.height > 0.0f ? root.rect.height / 480.0f : 1.0f;

  Rect panel;
  panel.x = root.rect.x;
  panel.y = root.rect.y + kPanelTop * zoom;
  panel.width = root.rect.width;
  panel.height = root.rect.height - kPanelTop * zoom;
  draw.FillRect(panel, kPanelFill);

  Walk(root, [&](const Element& element) {
    if (!element.visible || !element.source ||
        element.class_name != "XuiFigure" ||
        element.id.compare(0, 4, "Key.") == 0) {
      return;
    }
    const Value* points = Property(*element.source, "Points");
    if (!points) {
      return;
    }
    const auto figure = figures_.find(points->index);
    if (figure == figures_.end()) {
      return;
    }
    uint32_t fill = 0xFF808080;
    uint32_t gradient = fill;
    float rotation = 0.0f;
    const Value* brush = Property(*element.source, "Fill");
    if (brush && brush->object) {
      fill = ParseColor(StringProperty(*brush->object, "FillColor"), fill);
      gradient =
          ParseColor(StringProperty(*brush->object, "GradientColor"), fill);
      rotation = FloatProperty(*brush->object, "GradientRotation", 0.0f);
    }
    draw.FillPath(figure->second, element.rect, fill, gradient, rotation);
  });

  for (size_t i = 0; i < keys_.size(); ++i) {
    const Key& key = keys_[i];
    const Rect& rect = key.element->rect;
    const bool focused = i == focus_;
    // The key that selected the current page stays lit, so which page is up
    // is visible without pressing anything.
    const bool active = (key.id == "Key.Prev" && page_ == Page::kSymbols) ||
                        (key.id == "Key.Next" && page_ == Page::kAccents) ||
                        (key.id == "Key.Caps" && page_ == Page::kCaps);
    draw.FillRect(rect, focused ? kKeyFocus : (active ? kKeyActive : kKeyFill));
    if (!focused) {
      draw.StrokeRect(rect, kKeyEdge);
    }
    const uint32_t text_color = focused ? kKeyFocusText : kKeyText;
    const float size = (key.caption.size() <= 1 ? 15.0f : 11.0f) * zoom;

    if (key.glyph.empty()) {
      draw.DrawTextIn(key.caption, rect, size, text_color, TextAlign::kCenter);
      continue;
    }

    const PackageEntry* art = package_->Find(key.glyph);
    int width = 0;
    int height = 0;
    ui::ImmediateTexture* texture =
        art ? draw.ImageTexture(key.glyph, art->data, art->size, &width,
                                &height)
            : nullptr;
    if (!texture) {
      draw.DrawTextIn(key.caption, rect, size, text_color, TextAlign::kCenter);
      continue;
    }

    const float glyph_width = width * zoom;
    const float glyph_height = height * zoom;
    const NamedKey* named = FindNamedKey(key.id);
    const bool above = named && named->glyph_above;

    Rect glyph_rect;
    Rect caption_rect = rect;
    if (above) {
      glyph_rect.x = rect.x + (rect.width - glyph_width) * 0.5f;
      glyph_rect.y = rect.y + 4.0f * zoom;
      caption_rect.y = glyph_rect.y + glyph_height;
      caption_rect.height = rect.height - (caption_rect.y - rect.y);
    } else {
      glyph_rect.x = rect.x + 8.0f * zoom;
      glyph_rect.y = rect.y + (rect.height - glyph_height) * 0.5f;
      caption_rect.x = glyph_rect.x + glyph_width + 4.0f * zoom;
      caption_rect.width = rect.width - (caption_rect.x - rect.x);
    }
    glyph_rect.width = glyph_width;
    glyph_rect.height = glyph_height;
    // Only the silhouettes ship near-white and take the control's tint. The
    // face buttons (btn_x, btn_y) are already the real blue and yellow, so
    // tinting them turns them navy and olive.
    const uint32_t glyph_color =
        above ? (focused ? kKeyFocusText : kGlyphTint) : kGlyphUntinted;
    draw.DrawImage(texture, glyph_rect, glyph_color);
    draw.DrawTextIn(key.caption, caption_rect, size, text_color,
                    TextAlign::kCenter);
  }

  if (edit_) {
    // XuiEdit draws itself from the skin, which we do not render yet, so the
    // field is filled here: a light box inside the darker header panel, as
    // the console shows it.
    draw.FillRect(edit_->rect, kEditFill);
    draw.StrokeRect(edit_->rect, kEditEdge);
    Rect field = edit_->rect;
    field.x += 6.0f * zoom;
    field.width -= 12.0f * zoom;
    const float pixels = 18.0f * zoom;
    draw.DrawTextIn(ToU32(text_), field, pixels, kEditText, TextAlign::kLeft);

    Rect caret;
    caret.x =
        field.x + draw.MeasureText(ToU32(std::string_view(text_).substr(
                                       0, std::min(cursor_, text_.size()))),
                                   pixels);
    caret.width = std::max(1.0f, zoom);
    caret.height = pixels;
    caret.y = field.y + (field.height - caret.height) * 0.5f;
    draw.FillRect(caret, kEditText);
  }
  if (description_element_ && !description_.empty()) {
    Rect field = description_element_->rect;
    field.x += 6.0f * zoom;
    draw.DrawTextIn(ToU32(description_), field, 16.0f * zoom, kDescriptionText,
                    TextAlign::kLeft);
  }

  DrawLegend(draw, root);
}

// The bar the console shows under the keyboard: the real A and B button art
// with Select and Back. The page hints ride along on the right, because
// Symbols and Accents are otherwise invisible affordances.
void KeyboardScreen::DrawLegend(class Draw& draw, const Element& root) {
  Rect band;
  band.x = 0.0f;
  band.width = kDesignWidth;
  band.y = root.rect.y + root.rect.height;
  band.height = kDesignHeight - band.y;
  if (band.height <= 0.0f) {
    return;
  }
  draw.FillRect(band, kLegendBand);

  const float size = band.height * 0.62f;
  float pen = root.rect.x;

  auto entry = [&](const char* image, const char* letter, const char* caption) {
    const PackageEntry* art = shared_ ? shared_->Find(image) : nullptr;
    int width = 0;
    int height = 0;
    xe::ui::ImmediateTexture* texture =
        art ? draw.ImageTexture(image, art->data, art->size, &width, &height)
            : nullptr;
    if (texture && height > 0) {
      Rect icon;
      icon.height = size;
      icon.width = size * float(width) / float(height);
      icon.x = pen;
      icon.y = band.y + (band.height - icon.height) * 0.5f;
      draw.DrawImage(texture, icon, kGlyphUntinted);
      // The art is a bare coloured circle; the console draws the letter over
      // it.
      draw.DrawTextIn(ToU32(letter), icon, size * 0.68f, kLegendText,
                      TextAlign::kCenter);
      pen += icon.width + 5.0f;
    }
    const std::u32string text = ToU32(caption);
    Rect label = band;
    label.x = pen;
    label.width = draw.MeasureText(text, size * 0.72f);
    draw.DrawTextIn(text, label, size * 0.72f, kLegendText, TextAlign::kLeft);
    pen += label.width + 22.0f;
  };

  entry("A-Button.png", "A", "Select");
  entry("B-Button.png", "B", "Back");

  Rect hints = band;
  hints.width = root.rect.x + root.rect.width - 4.0f;
  draw.DrawTextIn(
      ToU32("LB/RB Cursor    LT Symbols    RT Accents    L3 Caps    "
            "X Backspace    Y Space    Start Done"),
      hints, size * 0.62f, kLegendHint, TextAlign::kRight);
}

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
