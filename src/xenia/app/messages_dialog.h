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
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xenia/kernel/XLiveAPI.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/ui_focus_manager.h"

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
  // What the body of the popup is showing right now: the console's Messages
  // list, one message opened (Message Received on the left, the sender's card
  // and the message on the right), or writing / recording one.
  enum class View {
    kList,
    kMessage,
    kCompose,
  };

  void Reload();
  void DrawHeader(const ImVec2& origin, float width);
  void DrawTabs(const ImVec2& origin, float width, float height);
  void DrawLegend(const ImVec2& origin, float width, float height);
  void DrawList(const ImVec2& origin, const ImVec2& size);
  void DrawMessage(const ImVec2& origin, const ImVec2& size);
  void DrawCard(const ImVec2& origin, const ImVec2& size,
                const kernel::HubMessage& message);
  void DrawCompose();

  void OpenMessage(int index);
  void BackToList();
  void LeaveCompose();
  void OpenKeyboard();
  // The pad, as the guide takes it: through the focus manager, one view at a
  // time. ImGui's own navigation is off for this dialog.
  void HandlePad();
  void StartCompose(bool voice, uint64_t to_xuid, const std::string& to_name);
  void PollQueries();
  int UnreadCount() const;

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
  // The message opened (View::kMessage), and the action focused in it.
  int open_ = -1;
  int action_ = 0;
  // Scroll the list to the selected row on the next frame.
  bool scroll_to_selected_ = false;
  // In an open voice message: the player button the pad is on (0 Stop,
  // 1 Play, 2 Done), or -1 while it is on the actions.
  int player_focus_ = -1;
  // This frame's pad input, and A pressed on what the pad is on.
  ui::UIInput input_ = {};
  bool activate_ = false;
  // Where B from the compose view goes back to.
  View compose_return_ = View::kList;

  // The opened message's sender card and picture.
  std::future<std::optional<kernel::HubPlayerCard>> card_query_;
  std::optional<kernel::HubPlayerCard> card_;
  uint64_t card_xuid_ = 0;
  std::unique_ptr<ui::ImmediateTexture> card_pic_;

  // The signed-in profile's own picture, in the header as on the console.
  std::future<std::vector<uint8_t>> own_pic_query_;
  std::unique_ptr<ui::ImmediateTexture> own_pic_;

  // The voice message playing in the message view, for its progress bar.
  std::chrono::steady_clock::time_point play_started_;
  uint32_t play_ms_ = 0;

  std::future<std::vector<kernel::HubPlayer>> recipients_query_;
  std::vector<kernel::HubPlayer> recipients_;
  bool recipients_loading_ = false;
  int compose_target_ = -1;
  // Who the message being written goes to, and whether it is a voice one -
  // set by picking a recipient, or by Reply / Voice Reply on an open message.
  uint64_t compose_to_xuid_ = 0;
  std::string compose_to_name_;
  bool compose_voice_ = false;

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
