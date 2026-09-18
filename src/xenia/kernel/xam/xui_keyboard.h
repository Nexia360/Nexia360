/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_KEYBOARD_H_
#define XENIA_KERNEL_XAM_XUI_KEYBOARD_H_

#include <map>
#include <string>
#include <vector>

#include "xenia/kernel/xam/xui_overlay.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

// The console's own KeyboardBase scene, driven by our host code: the module
// that normally supplies captions, picks the page, tracks focus and owns the
// text buffer is replaced by this class.
class KeyboardScreen : public Screen {
 public:
  enum class Page {
    kLower,
    kCaps,
    kSymbols,
    kAccents,
  };

  KeyboardScreen(const std::string& title, const std::string& description,
                 const std::string& initial_text);

  bool Prepare(AssetStore& assets) override;
  void Draw(class Draw& draw, const Element& root) override;

  void MoveFocus(int dx, int dy);
  void MoveCursor(int delta);
  void FocusKey(const std::string& id);
  void Activate();
  void Backspace();
  void Insert(char32_t character);
  void SetPage(Page page);
  void Commit();
  void Cancel();

  Page page() const { return page_; }
  const std::string& text() const { return text_; }
  bool cancelled() const { return cancelled_; }
  bool done() const { return done_; }

 private:
  struct Key {
    const Element* element = nullptr;
    std::string id;
    std::string glyph;
    std::u32string caption;
    int row = 0;
    int column = 0;
    bool character = false;
  };

  void CollectKeys();
  void ApplyCaptions();
  const Key* Focused() const;

  std::string title_;
  std::string description_;
  std::string text_;
  // Byte offset into text_; always on a UTF-8 character boundary.
  size_t cursor_ = 0;
  Page page_ = Page::kLower;
  bool cancelled_ = false;
  bool done_ = false;

  void DrawLegend(class Draw& draw, const Element& root);

  Package* package_ = nullptr;
  Package* shared_ = nullptr;
  std::map<uint32_t, FigurePath> figures_;
  std::vector<Key> keys_;
  std::vector<std::vector<size_t>> grid_;
  std::vector<size_t> functions_;
  size_t focus_ = 0;
  const Element* edit_ = nullptr;
  const Element* description_element_ = nullptr;
};

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
