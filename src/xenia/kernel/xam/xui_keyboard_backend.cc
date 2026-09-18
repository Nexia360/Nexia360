/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_keyboard_backend.h"

#include <memory>
#include <string>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/xam/xui_keyboard.h"
#include "xenia/kernel/xam/xui_overlay.h"
#include "xenia/ui/keyboard_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

class Backend : public xe::ui::KeyboardBackend {
 public:
  bool Open(const std::string& title, const std::string& description,
            const std::string& initial_text) override {
    Overlay* overlay = SharedOverlay();
    if (!overlay || !overlay->ready()) {
      return false;
    }
    screen_ =
        std::make_unique<KeyboardScreen>(title, description, initial_text);
    if (!overlay->Push(screen_.get())) {
      screen_.reset();
      return false;
    }
    return true;
  }

  void Close() override {
    if (screen_) {
      if (Overlay* overlay = SharedOverlay()) {
        overlay->Remove(screen_.get());
      }
      screen_.reset();
    }
  }

  void Perform(Action action) override {
    if (!screen_) {
      return;
    }
    switch (action) {
      case Action::kLeft:
        screen_->MoveFocus(-1, 0);
        break;
      case Action::kRight:
        screen_->MoveFocus(1, 0);
        break;
      case Action::kCursorLeft:
        screen_->MoveCursor(-1);
        break;
      case Action::kCursorRight:
        screen_->MoveCursor(1);
        break;
      case Action::kUp:
        screen_->MoveFocus(0, -1);
        break;
      case Action::kDown:
        screen_->MoveFocus(0, 1);
        break;
      case Action::kActivate:
        screen_->Activate();
        break;
      case Action::kBackspace:
        screen_->Backspace();
        break;
      case Action::kSpace:
        screen_->Insert(U' ');
        break;
      case Action::kCaps:
        screen_->SetPage(screen_->page() == KeyboardScreen::Page::kCaps
                             ? KeyboardScreen::Page::kLower
                             : KeyboardScreen::Page::kCaps);
        break;
      case Action::kSymbols:
        screen_->SetPage(screen_->page() == KeyboardScreen::Page::kSymbols
                             ? KeyboardScreen::Page::kLower
                             : KeyboardScreen::Page::kSymbols);
        break;
      case Action::kAccents:
        screen_->SetPage(screen_->page() == KeyboardScreen::Page::kAccents
                             ? KeyboardScreen::Page::kLower
                             : KeyboardScreen::Page::kAccents);
        break;
      case Action::kDone:
        screen_->Commit();
        break;
      case Action::kCancel:
        screen_->Cancel();
        break;
    }
  }

  void Append(const std::string& utf8) override {
    if (!screen_) {
      return;
    }
    for (char32_t character : ToU32(utf8)) {
      screen_->Insert(character);
    }
    // Typing on a real keyboard means the grid is not what the user is
    // driving, so park focus on Done - Enter then commits rather than
    // activating whichever key happened to hold focus.
    screen_->FocusKey("Key.OK");
  }

  std::string text() const override {
    return screen_ ? screen_->text() : std::string();
  }

  bool finished(bool* out_cancelled) const override {
    if (!screen_ || !screen_->closed()) {
      return false;
    }
    *out_cancelled = screen_->cancelled();
    return true;
  }

 private:
  std::unique_ptr<KeyboardScreen> screen_;
};

std::unique_ptr<Backend> backend;

}  // namespace

bool InstallKeyboardBackend(xe::ui::Presenter* presenter,
                            xe::ui::ImmediateDrawer* immediate_drawer,
                            const std::filesystem::path& asset_directory) {
  UninstallKeyboardBackend();
  if (!OpenSharedOverlay(presenter, immediate_drawer, asset_directory)) {
    XELOGI("xui keyboard: no dashboard assets at {}, keeping the ImGui one",
           xe::path_to_utf8(asset_directory));
    return false;
  }
  backend = std::make_unique<Backend>();
  xe::ui::SetKeyboardBackend(backend.get());
  XELOGI("xui keyboard: presenting the console's keyboard from {}",
         xe::path_to_utf8(asset_directory));
  return true;
}

void UninstallKeyboardBackend() {
  xe::ui::SetKeyboardBackend(nullptr);
  backend.reset();
  CloseSharedOverlay();
}

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
