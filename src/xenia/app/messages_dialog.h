/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_MESSAGES_DIALOG_H_
#define XENIA_APP_MESSAGES_DIALOG_H_

#include <array>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "xenia/kernel/XLiveAPI.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace app {

class EmulatorWindow;

// The Social inbox. One dialog, two modes - text and voice - because they are
// the same list with a different payload, and the Social menu opens whichever
// the user picked.
//
// Everything is one popup with internal modes rather than nested popups: ImGui
// allows a single popup per level, and opening a second corrupts the stack
// (and with it the on-screen keyboard).
class MessagesDialog final : public ui::ImGuiDialog {
 public:
  enum class Mode {
    kText,
    kVoice,
  };

  MessagesDialog(ui::ImGuiDrawer* imgui_drawer, EmulatorWindow* emulator_window,
                 Mode mode);
  ~MessagesDialog() override;

  // Raised from the destructor. The dialog deletes itself the frame after it
  // closes - by its own Close button or by the menu item being used again -
  // so this is the one place that knows it is really gone.
  void set_closed_callback(std::function<void()> callback) {
    closed_callback_ = std::move(callback);
  }

  // Whoever opened this dialog asks it to close; the drawer does the deleting
  // on the next frame. Protected on the base, which only suits dialogs that
  // close themselves.
  using ui::ImGuiDialog::Close;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // What the body of the popup is showing right now.
  enum class View {
    kList,
    kCompose,
  };

  void Reload();
  void DrawList();
  void DrawCompose();

  // Everyone this profile is allowed to write to: friends, plus anyone from a
  // session in the last 48 hours. The hub enforces the same rule on send, so
  // this list is a convenience, not the gate.
  void StartRecipientQuery();

  void PumpRecording();
  void StartRecording();
  void StopRecording();
  void PlayMessage(const kernel::HubMessage& message);
  void SendComposed();

  uint64_t LocalXuid() const;

  EmulatorWindow* emulator_window_;
  Mode mode_;
  View view_ = View::kList;
  bool opened_ = false;

  std::future<std::vector<kernel::HubMessage>> messages_query_;
  std::vector<kernel::HubMessage> messages_;
  bool loading_ = false;

  // Index into messages_, or -1 for none.
  int selected_ = -1;

  std::future<std::vector<kernel::HubPlayer>> recipients_query_;
  std::vector<kernel::HubPlayer> recipients_;
  bool recipients_loading_ = false;
  int compose_target_ = -1;

  // 250 characters plus the terminator. The buffer size is what actually caps
  // typing - ImGui will not write past it - and the hub refuses anything
  // longer, so the two agree.
  static constexpr size_t kMaxTextChars = 250;

  std::array<char, kMaxTextChars + 1> compose_text_ = {};
  std::string status_;

  // The on-screen keyboard, opened explicitly: the drawer's automatic one
  // deliberately never opens inside a modal, and this dialog is one.
  bool keyboard_open_ = false;

  // Outlives us if the dialog is closed while the keyboard is still up, so the
  // keyboard's close callback can tell whether there is still a buffer to
  // write into.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  std::function<void()> closed_callback_;

  // Voice capture. Samples accumulate here while recording and are sent as-is:
  // signed 16-bit mono at the mixer's rate, no codec on either end.
  bool recording_ = false;
  std::vector<int16_t> recorded_pcm_;

  // Where in the mixer's capture stream this recording has reached.
  uint64_t capture_cursor_ = 0;

  // Left over when a drain ends on an odd sample: paired with the first sample
  // of the next drain so the 16 kHz -> 8 kHz averaging never loses alignment.
  bool have_pending_sample_ = false;
  int16_t pending_sample_ = 0;
  bool voice_referenced_ = false;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_MESSAGES_DIALOG_H_
