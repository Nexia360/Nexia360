/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/messages_dialog.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <span>

#include "third_party/fmt/include/fmt/format.h"
// ClearActiveID lives in the internal header, not the public one.
#include "third_party/imgui/imgui_internal.h"
#include "xenia/app/emulator_window.h"
#include "xenia/apu/sdl/voice_chat.h"
#include "xenia/base/platform.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/friends_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/ui/imgui_guide_skin.h"
#include "xenia/ui/keyboard_ui.h"

namespace xe {
namespace app {

// Voice mail ceiling.
constexpr size_t kMaxRecordSeconds = 60;

// The voice mixer captures and plays at 16 kHz, but mail is STORED at 8 kHz -
// half the payload for speech that is going to be listened to once. The
// conversion happens at the edges: samples are halved as they are captured and
// doubled again on the way to the speaker.
constexpr uint32_t kCaptureRate = 16000;
constexpr uint32_t kStoreRate = 8000;

// A minute at 8 kHz is 480,000 samples - 960 KB, ~1.3 MB base64, inside what
// the hub accepts.
constexpr size_t kMaxRecordSamples = kMaxRecordSeconds * kStoreRate;

// ---- The console's Messages look -------------------------------------------
// Laid out after the console's own Messages screens: a dark frame with the
// title, your picture and the clock; Record / Messages (N New) tabs over a
// pale list whose focused row is the green bar; an open message is "Message
// Received" and its actions on the left, the sender's card (the orange
// gamertag bar, picture, Rep / Gamerscore / Zone) and the message on the
// slate right; A / B along the bottom.
namespace {

namespace skin = ui::guide_skin;

constexpr ImU32 kFrame = IM_COL32(16, 16, 18, 240);
constexpr ImU32 kFrameText = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kTabIdleTop = IM_COL32(124, 124, 144, 255);
constexpr ImU32 kTabIdleBottom = IM_COL32(64, 64, 84, 255);
constexpr ImU32 kTabIdleText = IM_COL32(176, 178, 188, 255);
constexpr ImU32 kTabTop = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kTabBottom = IM_COL32(218, 218, 220, 255);
constexpr ImU32 kGreenLeft = IM_COL32(126, 186, 0, 255);
constexpr ImU32 kGreenRight = IM_COL32(104, 160, 0, 255);
constexpr ImU32 kSlateTop = IM_COL32(140, 140, 156, 255);
constexpr ImU32 kSlateBottom = IM_COL32(112, 112, 130, 255);
constexpr ImU32 kOrangeTop = IM_COL32(234, 132, 34, 255);
constexpr ImU32 kOrangeBottom = IM_COL32(212, 104, 16, 255);
// The gamercard is white with dark text, as on the console.
constexpr ImU32 kBand = IM_COL32(232, 234, 236, 255);
constexpr ImU32 kBandLine = IM_COL32(190, 192, 198, 255);
constexpr ImU32 kBandLabel = IM_COL32(72, 74, 80, 255);
constexpr ImU32 kBandValue = IM_COL32(24, 26, 30, 255);
// "Message Text" and the message on the slate below the card.
constexpr ImU32 kCardLabel = IM_COL32(206, 206, 216, 255);
constexpr ImU32 kCardValue = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kStarOn = IM_COL32(128, 130, 136, 255);
constexpr ImU32 kStarOff = IM_COL32(128, 130, 136, 70);
constexpr ImU32 kGamerscore = IM_COL32(110, 180, 20, 255);
constexpr ImU32 kSubText = IM_COL32(72, 74, 80, 255);
constexpr ImU32 kBarFrame = IM_COL32(150, 150, 166, 255);
constexpr ImU32 kBarFill = IM_COL32(236, 236, 240, 255);

// A heavier weight without a bold font: the same text drawn twice, a pixel
// apart.
void BoldText(ImDrawList* dl, float size, ImVec2 pos, ImU32 col,
              const char* text, float wrap = 0.0f) {
  ImFont* font = ImGui::GetFont();
  dl->AddText(font, size, pos, col, text, nullptr, wrap);
  dl->AddText(font, size, ImVec2(pos.x + 0.8f, pos.y), col, text, nullptr,
              wrap);
}

ImVec2 TextSize(float size, const char* text, float wrap = 0.0f) {
  return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, wrap, text);
}

// "Shortened..." to fit width.
std::string Fit(float size, const std::string& text, float width) {
  if (TextSize(size, text.c_str()).x <= width) {
    return text;
  }
  std::string out = text;
  while (!out.empty() && TextSize(size, (out + "...").c_str()).x > width) {
    out.pop_back();
  }
  return out + "...";
}

void Gradient(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom) {
  dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
}

// The envelope: a filled card with the flap folded in.
void DrawEnvelope(ImDrawList* dl, ImVec2 pos, float h, ImU32 col, ImU32 flap) {
  const float w = h * 1.4f;
  dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col, 1.5f);
  dl->AddLine(ImVec2(pos.x + 1.0f, pos.y + 1.0f),
              ImVec2(pos.x + w * 0.5f, pos.y + h * 0.55f), flap, 1.6f);
  dl->AddLine(ImVec2(pos.x + w - 1.0f, pos.y + 1.0f),
              ImVec2(pos.x + w * 0.5f, pos.y + h * 0.55f), flap, 1.6f);
}

// The speaker: the box, its cone and two sound waves.
void DrawSpeaker(ImDrawList* dl, ImVec2 pos, float h, ImU32 col) {
  const float box = h * 0.32f;
  const float top = pos.y + h * 0.34f;
  dl->AddRectFilled(ImVec2(pos.x, top), ImVec2(pos.x + box, top + box), col);
  dl->AddQuadFilled(ImVec2(pos.x + box, top), ImVec2(pos.x + box * 2.2f, pos.y),
                    ImVec2(pos.x + box * 2.2f, pos.y + h),
                    ImVec2(pos.x + box, top + box), col);
  const ImVec2 centre(pos.x + box * 2.2f, pos.y + h * 0.5f);
  for (int i = 1; i <= 2; ++i) {
    dl->PathArcTo(centre, h * 0.22f * i + 1.0f, -0.9f, 0.9f, 8);
    dl->PathStroke(col, ImDrawFlags_None, 1.5f);
  }
}

// One Rep star, filled to `fill` (0..1) from the left.
void DrawStar(ImDrawList* dl, ImVec2 centre, float radius, float fill) {
  ImVec2 points[10];
  for (int i = 0; i < 10; ++i) {
    const float r = (i % 2) ? radius * 0.45f : radius;
    const float angle = -1.5707963f + i * 0.6283185f;
    points[i] =
        ImVec2(centre.x + std::cos(angle) * r, centre.y + std::sin(angle) * r);
  }
  dl->AddConcavePolyFilled(points, 10, kStarOff);
  if (fill > 0.0f) {
    dl->PushClipRect(
        ImVec2(centre.x - radius, centre.y - radius),
        ImVec2(centre.x - radius + radius * 2.0f * fill, centre.y + radius),
        true);
    dl->AddConcavePolyFilled(points, 10, kStarOn);
    dl->PopClipRect();
  }
}

void DrawLegendButton(ImDrawList* dl, ImVec2 pos, float size, ImU32 col,
                      const char* letter, const char* label) {
  const float r = size * 0.62f;
  const ImVec2 centre(pos.x + r, pos.y + size * 0.5f);
  dl->AddCircleFilled(centre, r, col, 20);
  dl->AddCircle(centre, r, IM_COL32(255, 255, 255, 90), 20, 1.0f);
  const ImVec2 letter_size = TextSize(size * 0.8f, letter);
  dl->AddText(
      ImGui::GetFont(), size * 0.8f,
      ImVec2(centre.x - letter_size.x * 0.5f, centre.y - letter_size.y * 0.5f),
      IM_COL32(40, 40, 40, 230), letter);
  BoldText(dl, size, ImVec2(pos.x + r * 2.0f + 8.0f, pos.y), kFrameText, label);
}

// The hub's "2026-11-04T01:06:12.000Z" in this machine's time, as the
// console's message view shows it: "11/04/2026" and "01:06 AM".
bool LocalTime(const std::string& iso, std::tm* out) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour,
                  &minute, &second) < 5) {
    return false;
  }
  std::tm utc = {};
  utc.tm_year = year - 1900;
  utc.tm_mon = month - 1;
  utc.tm_mday = day;
  utc.tm_hour = hour;
  utc.tm_min = minute;
  utc.tm_sec = second;
#if XE_PLATFORM_WIN32
  const std::time_t t = _mkgmtime(&utc);
  return localtime_s(out, &t) == 0;
#else
  const std::time_t t = timegm(&utc);
  return localtime_r(&t, out) != nullptr;
#endif
}

std::string FormatDate(const std::string& iso) {
  std::tm tm = {};
  if (!LocalTime(iso, &tm)) {
    return iso.substr(0, std::min<size_t>(10, iso.size()));
  }
  return fmt::format("{:02}/{:02}/{:04}", tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_year + 1900);
}

std::string FormatClock(const std::tm& tm, bool with_half) {
  int hour = tm.tm_hour % 12;
  if (hour == 0) {
    hour = 12;
  }
  if (!with_half) {
    return fmt::format("{:02}:{:02}", hour, tm.tm_min);
  }
  return fmt::format("{:02}:{:02} {}", hour, tm.tm_min,
                     tm.tm_hour >= 12 ? "PM" : "AM");
}

std::string FormatTime(const std::string& iso) {
  std::tm tm = {};
  if (!LocalTime(iso, &tm)) {
    return "";
  }
  return FormatClock(tm, true);
}

std::string FormatDuration(uint32_t ms) {
  const uint32_t seconds = std::max<uint32_t>(1, (ms + 500) / 1000);
  return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

const char* kActions[] = {"Reply", "Voice Reply", "Mark Unread", "Delete"};
constexpr int kActionCount = 4;

// The focus manager's name for this dialog. The on-screen keyboard registers
// as its child (empty parent), so while it is up this dialog gets no input.
constexpr char kFocusName[] = "MessagesDialog";

}  // namespace

MessagesDialog::MessagesDialog(ui::ImGuiDrawer* imgui_drawer,
                               EmulatorWindow* emulator_window, Mode mode)
    : ui::ImGuiDialog(imgui_drawer),
      emulator_window_(emulator_window),
      mode_(mode) {
  compose_voice_ = mode_ == Mode::kVoice;
  Reload();

  const uint64_t xuid = LocalXuid();
  if (xuid) {
    auto* live = emulator_window_->emulator()->GetXboxLiveAPI();
    own_pic_query_ = std::async(std::launch::async, [live, xuid]() {
      return live->GetUserGamerpicTile(xuid, false);
    });
  }
}

MessagesDialog::~MessagesDialog() {
  // However it closed - its own B, or the menu item used again - the focus
  // it took goes with it.
  imgui_drawer()->GetFocusManager()->UIDropFocus(kFocusName);

  // Recording holds the capture device open - never leave it running because
  // the dialog went away.
  if (recording_) {
    recording_ = false;
  }

  if (voice_referenced_) {
    // A message still playing when the dialog goes away should stop with it.
    apu::sdl::VoiceChat::Get().StopPcmMessage();

    // Order matters: drop the temporary enable first, so the Release below is
    // what stops the device, once.
    apu::sdl::VoiceChat::Get().SetMessagePlayback(false);
    apu::sdl::VoiceChat::Get().Release();
    voice_referenced_ = false;
  }

  // An on-screen keyboard can still be up, holding a callback that writes into
  // our buffer. This is how it learns not to.
  *alive_ = false;

  // Last: whoever opened us is still holding a pointer to this object, and it
  // is about to be invalid.
  if (closed_callback_) {
    closed_callback_();
  }
}

uint64_t MessagesDialog::LocalXuid() const {
  auto* kernel_state = emulator_window_->emulator()->kernel_state();

  if (!kernel_state || !kernel_state->xam_state()) {
    return 0;
  }

  const auto profile = kernel_state->xam_state()->GetUserProfile(uint32_t(0));

  return profile ? profile->GetOnlineXUID() : 0;
}

void MessagesDialog::Reload() {
  const uint64_t xuid = LocalXuid();

  if (!xuid) {
    status_ = "Not signed in.";
    return;
  }

  loading_ = true;

  // Text and voice in one list, as the console's Messages shows them; the
  // icon before each says which it is.
  messages_query_ =
      emulator_window_->emulator()->GetXboxLiveAPI()->GetMessagesAsync(xuid,
                                                                       "");
}

void MessagesDialog::StartRecipientQuery() {
  const uint64_t xuid = LocalXuid();

  if (!xuid) {
    return;
  }

  recipients_loading_ = true;

  recipients_query_ =
      emulator_window_->emulator()->GetXboxLiveAPI()->GetRecentPlayersAsync(
          xuid);
}

void MessagesDialog::StartRecording() {
  if (recording_) {
    return;
  }

  if (!voice_referenced_) {
    apu::sdl::VoiceChat::Get().AddRef();
    voice_referenced_ = true;
  }

  apu::sdl::VoiceChat::Get().SetMessagePlayback(true);

  recorded_pcm_.clear();

  // Start from where the capture stream is NOW, so a recording never picks up
  // whatever was already sitting in the ring.
  capture_cursor_ = apu::sdl::VoiceChat::Get().capture_position();
  have_pending_sample_ = false;

  recording_ = true;
  status_ = "Recording...";
}

void MessagesDialog::StopRecording() {
  if (!recording_) {
    return;
  }

  recording_ = false;

  const size_t samples = recorded_pcm_.size();

  status_ = samples ? fmt::format("Recorded {:.1f}s.",
                                  double(samples) / double(kStoreRate))
                    : "Nothing was recorded - check the microphone.";
}

void MessagesDialog::PumpRecording() {
  if (!recording_) {
    return;
  }

  // Everything captured since the last frame, exactly once. ReadCaptureSince
  // returns 0 when there is nothing new, which is what ends this loop -
  // ReadCapturePcm never does, because it is paced by the clock and always
  // hands back a full page whether or not any audio arrived.
  int16_t buffer[4096];

  while (true) {
    const size_t read = apu::sdl::VoiceChat::Get().ReadCaptureSince(
        capture_cursor_, buffer, sizeof(buffer) / sizeof(buffer[0]));

    if (!read) {
      break;
    }

    // 16 kHz in, 8 kHz stored. Averaging each pair rather than dropping every
    // other sample: a plain decimation would fold everything above 4 kHz back
    // into the band as aliasing, and speech has plenty up there.
    size_t i = 0;

    // A drain that ended mid-pair left one sample behind; it pairs with the
    // first sample here so the two streams stay in step.
    if (have_pending_sample_) {
      const int32_t averaged = (static_cast<int32_t>(pending_sample_) +
                                static_cast<int32_t>(buffer[0])) /
                               2;

      recorded_pcm_.push_back(static_cast<int16_t>(averaged));

      have_pending_sample_ = false;
      i = 1;
    }

    for (; i + 1 < read; i += 2) {
      const int32_t averaged = (static_cast<int32_t>(buffer[i]) +
                                static_cast<int32_t>(buffer[i + 1])) /
                               2;

      recorded_pcm_.push_back(static_cast<int16_t>(averaged));
    }

    if (i < read) {
      pending_sample_ = buffer[i];
      have_pending_sample_ = true;
    }

    if (recorded_pcm_.size() >= kMaxRecordSamples) {
      recorded_pcm_.resize(kMaxRecordSamples);
      StopRecording();
      status_ = fmt::format("Reached the {} second limit.", kMaxRecordSeconds);
      return;
    }
  }
}

// 8 kHz back up to what the mixer plays. Each stored sample is held for two
// output samples - crude, but this is speech being played once, and anything
// better would need a filter the playback path does not have.
static std::vector<int16_t> UpsampleToCaptureRate(
    const std::vector<int16_t>& stored) {
  std::vector<int16_t> out;
  out.reserve(stored.size() * 2);

  for (const int16_t sample : stored) {
    out.push_back(sample);
    out.push_back(sample);
  }

  return out;
}

void MessagesDialog::PlayMessage(const kernel::HubMessage& message) {
  const uint64_t xuid = LocalXuid();

  if (!xuid) {
    return;
  }

  std::vector<int16_t> pcm;
  uint32_t sample_rate = 0;

  if (!emulator_window_->emulator()->GetXboxLiveAPI()->GetMessageAudio(
          xuid, message.id, pcm, sample_rate)) {
    status_ = "Could not fetch that message.";
    return;
  }

  if (!voice_referenced_) {
    apu::sdl::VoiceChat::Get().AddRef();
    voice_referenced_ = true;
  }

  apu::sdl::VoiceChat::Get().SetMessagePlayback(true);

  // Older messages, or anything already at the mixer's rate, are played as
  // they are; the stored 8 kHz form is stretched back up first.
  const double seconds =
      double(pcm.size()) / double(sample_rate ? sample_rate : kStoreRate);

  if (!sample_rate || sample_rate == kCaptureRate) {
    apu::sdl::VoiceChat::Get().PlayPcmMessage(pcm.data(), pcm.size());
  } else {
    const std::vector<int16_t> playable = UpsampleToCaptureRate(pcm);
    apu::sdl::VoiceChat::Get().PlayPcmMessage(playable.data(), playable.size());
  }

  // The message view's progress bar follows this.
  play_started_ = std::chrono::steady_clock::now();
  play_ms_ = static_cast<uint32_t>(seconds * 1000.0);
  status_.clear();
}

void MessagesDialog::SendComposed() {
  const uint64_t xuid = LocalXuid();

  if (!xuid || !compose_to_xuid_) {
    status_ = "Pick who it goes to first.";
    return;
  }

  const uint64_t target = compose_to_xuid_;

  auto* live = emulator_window_->emulator()->GetXboxLiveAPI();

  kernel::SendMessageOutcome outcome = kernel::SendMessageOutcome::kInvalid;

  if (!compose_voice_) {
    const std::string text(compose_text_.data());

    if (text.empty()) {
      status_ = "Nothing to send.";
      return;
    }

    outcome = live->SendTextMessage(xuid, target, text);
  } else {
    if (recorded_pcm_.empty()) {
      status_ = "Record something first.";
      return;
    }

    outcome = live->SendVoiceMessage(xuid, target, recorded_pcm_, kStoreRate);
  }

  switch (outcome) {
    case kernel::SendMessageOutcome::kSent:
      status_ = "Sent.";
      compose_text_.fill('\0');
      recorded_pcm_.clear();
      BackToList();
      break;
    case kernel::SendMessageOutcome::kDenied:
      // The hub's rule, reported as the hub's rule: friends and people from a
      // recent session only.
      status_ =
          "They only accept messages from friends or players they have "
          "recently played with.";
      break;
    default:
      status_ = "That could not be sent.";
      break;
  }
}

int MessagesDialog::UnreadCount() const {
  return static_cast<int>(
      std::count_if(messages_.cbegin(), messages_.cend(),
                    [](const kernel::HubMessage& m) { return !m.read; }));
}

void MessagesDialog::OpenMessage(int index) {
  if (index < 0 || index >= static_cast<int>(messages_.size())) {
    return;
  }
  open_ = index;
  selected_ = index;
  action_ = 0;
  player_focus_ = -1;
  play_ms_ = 0;
  status_.clear();
  view_ = View::kMessage;

  auto& message = messages_[index];

  // Opening it is what makes it read, here and on the hub. Displaying a
  // message changes nothing else.
  const uint64_t xuid = LocalXuid();
  auto* live = emulator_window_->emulator()->GetXboxLiveAPI();
  if (!message.read && xuid) {
    message.read = true;
    live->MarkMessageRead(xuid, message.id);
    live->RefreshMessageCounts();
  }

  if (card_xuid_ != message.from_xuid) {
    card_xuid_ = message.from_xuid;
    card_.reset();
    card_pic_.reset();
    card_query_ = live->GetPlayerCardAsync(message.from_xuid);
  }
}

void MessagesDialog::BackToList() {
  if (play_ms_) {
    apu::sdl::VoiceChat::Get().StopPcmMessage();
    play_ms_ = 0;
  }
  if (recording_) {
    StopRecording();
  }
  view_ = View::kList;
  scroll_to_selected_ = true;
  open_ = -1;
}

void MessagesDialog::LeaveCompose() {
  if (recording_) {
    StopRecording();
  }
  status_.clear();
  if (compose_return_ == View::kMessage && open_ >= 0) {
    view_ = View::kMessage;
    action_ = 0;
    player_focus_ = -1;
  } else {
    BackToList();
  }
}

void MessagesDialog::OpenKeyboard() {
  if (keyboard_open_) {
    return;
  }
  keyboard_open_ = true;

  // Editing belongs to the keyboard now, not to the field behind it.
  ImGui::ClearActiveID();

  auto* keyboard = ui::KeyboardDialog::ShowKeyboard(
      imgui_drawer(), "Message", std::string(compose_text_.data()),
      // Empty parent: it registers as a child of whatever is focused, which is
      // this dialog, so it takes the pad (and B) while it is up.
      ui::KeyboardDialog::InputType::kText, nullptr, "", "OnScreenKeyboard");

  // The dialog can be closed while the keyboard is still up, so the callback
  // checks that there is still something to write into.
  std::shared_ptr<bool> alive = alive_;

  keyboard->set_close_callback([this, alive, keyboard]() {
    if (!*alive) {
      return;
    }

    // Cancel leaves whatever was already typed alone.
    if (!keyboard->was_cancelled()) {
      const std::string& result = keyboard->result_text();

      size_t i = 0;

      for (; i < kMaxTextChars && i < result.size(); ++i) {
        compose_text_[i] = result[i];
      }

      compose_text_[i] = '\0';
    }

    keyboard_open_ = false;
  });
}

void MessagesDialog::HandlePad() {
  const ui::UIInput& in = input_;
  activate_ = in.Activated();

  if (view_ == View::kList) {
    const int count = static_cast<int>(messages_.size());
    if (count && in.NavDown() && selected_ < count - 1) {
      ++selected_;
      scroll_to_selected_ = true;
    }
    if (count && in.NavUp() && selected_ > 0) {
      --selected_;
      scroll_to_selected_ = true;
    }
    if (activate_ && selected_ >= 0 && selected_ < count) {
      activate_ = false;
      OpenMessage(selected_);
      return;
    }
    // LB / RB move between the tabs, as the guide's blades.
    if (in.lb_pressed || in.rb_pressed || in.y_released) {
      StartCompose(mode_ == Mode::kVoice, 0, "");
    }
    return;
  }

  if (view_ == View::kMessage) {
    const bool voice = open_ >= 0 &&
                       open_ < static_cast<int>(messages_.size()) &&
                       messages_[open_].voice;
    if (player_focus_ < 0) {
      if (in.NavDown() && action_ < kActionCount - 1) {
        ++action_;
      }
      if (in.NavUp() && action_ > 0) {
        --action_;
      }
      // Right goes over to the voice message's player, onto Play.
      if (in.NavRight() && voice) {
        player_focus_ = 1;
      }
    } else {
      if (in.NavLeft()) {
        --player_focus_;  // Left of Stop is the actions again (-1).
      }
      if (in.NavRight() && player_focus_ < 2) {
        ++player_focus_;
      }
    }
    return;
  }

  // Compose: Up / Down pick who it goes to, A types (or starts and stops the
  // recording), Y sends, X plays the recording back.
  const int count = static_cast<int>(recipients_.size());
  if (count && (in.NavDown() || in.NavUp())) {
    int at = compose_target_;
    if (at < 0) {
      at = 0;
    } else if (in.NavDown() && at < count - 1) {
      ++at;
    } else if (in.NavUp() && at > 0) {
      --at;
    }
    compose_target_ = at;
    compose_to_xuid_ = recipients_[at].xuid;
    compose_to_name_ = recipients_[at].gamertag;
  }
  if (activate_) {
    activate_ = false;
    if (!compose_voice_) {
      OpenKeyboard();
    } else if (recording_) {
      StopRecording();
    } else {
      StartRecording();
    }
  }
  if (in.x_released && compose_voice_ && !recording_ &&
      !recorded_pcm_.empty()) {
    const std::vector<int16_t> playable = UpsampleToCaptureRate(recorded_pcm_);
    apu::sdl::VoiceChat::Get().PlayPcmMessage(playable.data(), playable.size());
  }
  if (in.y_released) {
    SendComposed();
  }
}

void MessagesDialog::StartCompose(bool voice, uint64_t to_xuid,
                                  const std::string& to_name) {
  compose_return_ = view_;
  compose_voice_ = voice;
  compose_to_xuid_ = to_xuid;
  compose_to_name_ = to_name;
  compose_target_ = -1;
  status_.clear();
  view_ = View::kCompose;

  if (recipients_.empty() && !recipients_loading_) {
    StartRecipientQuery();
  }
}

void MessagesDialog::PollQueries() {
  // Collect the async results before drawing anything that reads them.
  if (loading_ && messages_query_.valid() &&
      messages_query_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    messages_ = messages_query_.get();
    loading_ = false;
    if (selected_ >= static_cast<int>(messages_.size())) {
      selected_ = static_cast<int>(messages_.size()) - 1;
    }
    // The newest message is selected from the start, as on the console.
    if (selected_ < 0 && !messages_.empty()) {
      selected_ = 0;
    }
    scroll_to_selected_ = true;
  }

  if (card_query_.valid() && card_query_.wait_for(std::chrono::seconds(0)) ==
                                 std::future_status::ready) {
    card_ = card_query_.get();
    if (card_ && !card_->gamerpic.empty()) {
      card_pic_ = imgui_drawer()->LoadImGuiIcon(
          std::span<const uint8_t>(card_->gamerpic));
    }
  }

  if (own_pic_query_.valid() && own_pic_query_.wait_for(std::chrono::seconds(
                                    0)) == std::future_status::ready) {
    const std::vector<uint8_t> pic = own_pic_query_.get();
    if (!pic.empty()) {
      own_pic_ = imgui_drawer()->LoadImGuiIcon(std::span<const uint8_t>(pic));
    }
  }

  if (recipients_loading_ && recipients_query_.valid() &&
      recipients_query_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    recipients_ = recipients_query_.get();
    recipients_loading_ = false;

    // Friends may not have shared a session recently, so they are added on
    // top of the recent list rather than being assumed to be in it.
    auto* kernel_state = emulator_window_->emulator()->kernel_state();

    if (kernel_state && kernel_state->xam_state()) {
      const auto profile =
          kernel_state->xam_state()->GetUserProfile(uint32_t(0));
      const auto friends =
          profile ? kernel_state->xam_state()->friends_manager()->GetFriends(
                        profile->xuid())
                  : std::nullopt;

      if (friends.has_value()) {
        for (const auto& entry : friends.value()) {
          const uint64_t friend_xuid = entry.xuid;

          const bool known =
              std::any_of(recipients_.cbegin(), recipients_.cend(),
                          [friend_xuid](const kernel::HubPlayer& player) {
                            return player.xuid == friend_xuid;
                          });

          if (known) {
            continue;
          }

          kernel::HubPlayer player = {};
          player.xuid = friend_xuid;
          player.gamertag = std::string(
              entry.Gamertag, strnlen(entry.Gamertag, sizeof(entry.Gamertag)));

          recipients_.push_back(player);
        }
      }
    }
  }
}

void MessagesDialog::DrawHeader(const ImVec2& origin, float width) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float font = ImGui::GetFontSize();
  const char* title = view_ == View::kMessage ? "Message" : "Messages";
  BoldText(dl, font * 1.9f, ImVec2(origin.x + 24.0f, origin.y + 16.0f),
           kFrameText, title);

  // Your picture, centred, as the console's frame shows it.
  const float pic = 52.0f;
  const ImVec2 pic_min(origin.x + (width - pic) * 0.5f, origin.y + 8.0f);
  const ImVec2 pic_max(pic_min.x + pic, pic_min.y + pic);
  if (own_pic_) {
    dl->AddImage(reinterpret_cast<ImTextureID>(own_pic_.get()), pic_min,
                 pic_max);
  } else {
    dl->AddRectFilled(pic_min, pic_max, IM_COL32(60, 60, 66, 255));
  }
  dl->AddRect(pic_min, pic_max, IM_COL32(0, 0, 0, 255), 0.0f, 0, 2.0f);

  // The clock.
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
#if XE_PLATFORM_WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  const std::string clock = FormatClock(local, false);
  const ImVec2 clock_size = TextSize(font * 1.3f, clock.c_str());
  BoldText(dl, font * 1.3f,
           ImVec2(origin.x + width - 24.0f - clock_size.x, origin.y + 26.0f),
           kFrameText, clock.c_str());
}

void MessagesDialog::DrawTabs(const ImVec2& origin, float width, float height) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float font = ImGui::GetFontSize() * 1.15f;
  const float half = width * 0.5f;
  const bool composing = view_ == View::kCompose;

  // Left: Record (or Create New for text) - the tab that writes one.
  const ImVec2 left_min = origin;
  const ImVec2 left_max(origin.x + half, origin.y + height);
  Gradient(dl, left_min, left_max, composing ? kTabTop : kTabIdleTop,
           composing ? kTabBottom : kTabIdleBottom);
  const char* left = mode_ == Mode::kVoice ? "Record" : "Create New";
  const ImVec2 left_size = TextSize(font, left);
  dl->AddText(ImGui::GetFont(), font,
              ImVec2(left_min.x + (half - left_size.x) * 0.5f,
                     left_min.y + (height - left_size.y) * 0.5f),
              composing ? skin::kText : kTabIdleText, left);

  ImGui::SetCursorScreenPos(left_min);
  if (ImGui::InvisibleButton("##tab_create", ImVec2(half, height)) &&
      !composing) {
    StartCompose(mode_ == Mode::kVoice, 0, "");
  }

  // Right: Messages (N New).
  const ImVec2 right_min(origin.x + half, origin.y);
  const ImVec2 right_max(origin.x + width, origin.y + height);
  Gradient(dl, right_min, right_max, composing ? kTabIdleTop : kTabTop,
           composing ? kTabIdleBottom : kTabBottom);
  const std::string right = fmt::format("Messages ({} New)", UnreadCount());
  const ImVec2 right_size = TextSize(font, right.c_str());
  dl->AddText(ImGui::GetFont(), font,
              ImVec2(right_min.x + (half - right_size.x) * 0.5f,
                     right_min.y + (height - right_size.y) * 0.5f),
              composing ? kTabIdleText : skin::kText, right.c_str());

  ImGui::SetCursorScreenPos(right_min);
  if (ImGui::InvisibleButton("##tab_messages", ImVec2(half, height)) &&
      composing) {
    BackToList();
  }
}

void MessagesDialog::DrawLegend(const ImVec2& origin, float width,
                                float height) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float size = ImGui::GetFontSize() * 1.25f;
  const float y = origin.y + (height - size) * 0.5f;

  // What each button does in this view, left to right, as the console lists
  // them along the bottom.
  struct Entry {
    ImU32 color;
    const char* letter;
    const char* label;
  };
  std::vector<Entry> entries;
  if (view_ == View::kCompose) {
    entries.push_back({skin::kButtonA, "A",
                       !compose_voice_ ? "Type"
                       : recording_    ? "Stop"
                                       : "Record"});
    entries.push_back({skin::kButtonB, "B", "Back"});
    if (compose_voice_ && !recorded_pcm_.empty() && !recording_) {
      entries.push_back({skin::kButtonX, "X", "Play"});
    }
    entries.push_back({skin::kButtonY, "Y", "Send"});
  } else {
    entries.push_back({skin::kButtonA, "A", "Select"});
    entries.push_back({skin::kButtonB, "B", "Back"});
    if (view_ == View::kList) {
      entries.push_back({skin::kButtonY, "Y",
                         mode_ == Mode::kVoice ? "Record" : "Create New"});
    }
  }
  float x = origin.x + 20.0f;
  for (const Entry& entry : entries) {
    DrawLegendButton(dl, ImVec2(x, y), size, entry.color, entry.letter,
                     entry.label);
    x += size * 2.0f + 18.0f + TextSize(size, entry.label).x + 24.0f;
  }

  // What just happened ("Sent.", a failure), at the right of the legend.
  if (!status_.empty() && view_ != View::kCompose) {
    const float font = ImGui::GetFontSize();
    const std::string shown = Fit(font, status_, width * 0.6f);
    const float w = TextSize(font, shown.c_str()).x;
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(), font,
        ImVec2(origin.x + width - 20.0f - w, origin.y + (height - font) * 0.5f),
        kFrameText, shown.c_str());
  }
}

void MessagesDialog::DrawList(const ImVec2& origin, const ImVec2& size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Gradient(dl, origin, ImVec2(origin.x + size.x, origin.y + size.y),
           skin::kPanelTop, skin::kPanelBottom);

  const float font = ImGui::GetFontSize() * 1.15f;

  if (messages_.empty()) {
    const char* text = loading_ ? "Loading..." : "No messages.";
    dl->AddText(ImGui::GetFont(), font,
                ImVec2(origin.x + 30.0f, origin.y + 20.0f), skin::kText, text);
    return;
  }

  ImGui::SetCursorScreenPos(origin);
  // No ImGui navigation: the pad comes through the focus manager (HandlePad).
  // With ImGui's own, the scrolling child took focus as ONE target - Down only
  // scrolled it, and no row could be selected.
  ImGui::BeginChild("##message_rows", size, ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
  ImDrawList* rows = ImGui::GetWindowDrawList();

  const float width = ImGui::GetContentRegionAvail().x;
  const float row_height = font + 34.0f;
  // The preview column on the focused row, which shows the whole message.
  const float column = width * 0.42f;
  const float icon_h = font * 0.75f;

  for (int i = 0; i < static_cast<int>(messages_.size()); ++i) {
    const auto& message = messages_[i];
    const bool focused = selected_ == i;
    const std::string sender =
        message.from_name.empty() ? "Unknown" : message.from_name;
    const std::string preview =
        message.voice ? "Sent you a voice message" : message.text;

    float height = row_height;
    const float wrap = width - column - 30.0f;
    if (focused && !message.voice) {
      height =
          std::max(height, TextSize(font, preview.c_str(), wrap).y + 28.0f);
    }

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    // The pad moved the selection here: bring the row into view.
    if (scroll_to_selected_ && focused) {
      ImGui::SetScrollHereY(0.3f);
    }
    // The mouse: a click opens the row, and the selection follows the pointer
    // only while it moves, so a resting pointer never fights the d-pad (as the
    // guide does it).
    if (ImGui::InvisibleButton(fmt::format("##row{}", i).c_str(),
                               ImVec2(width, height))) {
      selected_ = i;
      OpenMessage(i);
    }
    if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                                   ImGui::GetIO().MouseDelta.y != 0.0f)) {
      selected_ = i;
    }

    const ImVec2 max(pos.x + width, pos.y + height);
    if (focused) {
      rows->AddRectFilledMultiColor(pos, max, kGreenLeft, kGreenRight,
                                    kGreenRight, kGreenLeft);
    }
    rows->AddLine(ImVec2(pos.x, max.y - 1.0f), ImVec2(max.x, max.y - 1.0f),
                  skin::kRowLine);

    const ImU32 text_col = focused ? skin::kSelectedText : skin::kText;
    const ImU32 flap = focused ? kGreenRight : skin::kPanelBottom;

    // The sender, then the message: the envelope (or the speaker and the
    // envelope, for voice) right before it.
    const float sender_y = pos.y + (row_height - font) * 0.5f;
    BoldText(rows, font, ImVec2(pos.x + 30.0f, sender_y), text_col,
             Fit(font, sender, column - 60.0f).c_str());

    float text_x;
    std::string shown;
    if (focused && !message.voice) {
      text_x = pos.x + column;
      rows->AddText(ImGui::GetFont(), font, ImVec2(text_x, pos.y + 14.0f),
                    text_col, preview.c_str(), nullptr, wrap);
    } else {
      shown = Fit(font, preview, width - column - 30.0f);
      const float text_w = TextSize(font, shown.c_str()).x;
      text_x = pos.x + width - 30.0f - text_w;
      rows->AddText(ImGui::GetFont(), font, ImVec2(text_x, sender_y), text_col,
                    shown.c_str());
    }
    const float icon_y = (focused && !message.voice)
                             ? pos.y + 14.0f + (font - icon_h) * 0.5f
                             : sender_y + (font - icon_h) * 0.5f;
    float icon_x = text_x - icon_h * 1.4f - 10.0f;
    DrawEnvelope(rows, ImVec2(icon_x, icon_y), icon_h, text_col, flap);
    if (message.voice) {
      icon_x -= icon_h * 1.3f + 10.0f;
      DrawSpeaker(rows, ImVec2(icon_x, icon_y), icon_h, text_col);
    }
  }
  scroll_to_selected_ = false;

  ImGui::EndChild();
}

void MessagesDialog::DrawCard(const ImVec2& origin, const ImVec2& size,
                              const kernel::HubMessage& message) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float font = ImGui::GetFontSize() * 1.1f;
  const float pad = 20.0f;

  Gradient(dl, origin, ImVec2(origin.x + size.x, origin.y + size.y), kSlateTop,
           kSlateBottom);

  // The orange gamertag bar.
  const float bar_h = font + 22.0f;
  Gradient(dl, origin, ImVec2(origin.x + size.x, origin.y + bar_h), kOrangeTop,
           kOrangeBottom);
  BoldText(dl, font * 1.1f,
           ImVec2(origin.x + pad, origin.y + (bar_h - font * 1.1f) * 0.5f),
           kCardValue,
           (message.from_name.empty() ? "Unknown" : message.from_name).c_str());

  // The band: picture, then Rep / Gamerscore / Zone.
  const float band_y = origin.y + bar_h;
  const float pic = font * 3.6f;
  const float band_h = pic + 30.0f;
  dl->AddRectFilled(ImVec2(origin.x, band_y),
                    ImVec2(origin.x + size.x, band_y + band_h), kBand);
  dl->AddLine(ImVec2(origin.x, band_y + band_h),
              ImVec2(origin.x + size.x, band_y + band_h), kBandLine);

  const ImVec2 pic_min(origin.x + pad, band_y + 15.0f);
  const ImVec2 pic_max(pic_min.x + pic, pic_min.y + pic);
  if (card_pic_) {
    dl->AddImage(reinterpret_cast<ImTextureID>(card_pic_.get()), pic_min,
                 pic_max);
  } else {
    dl->AddRectFilled(pic_min, pic_max, IM_COL32(96, 96, 110, 255));
  }
  dl->AddRect(pic_min, pic_max, IM_COL32(60, 60, 70, 255));

  const float label_x = pic_max.x + 24.0f;
  const float right = origin.x + size.x - pad;
  const float line = pic / 3.0f;
  const float row0 = pic_min.y + (line - font) * 0.5f;
  const char* labels[] = {"Rep", "Gamerscore", "Zone"};
  for (int i = 0; i < 3; ++i) {
    dl->AddText(ImGui::GetFont(), font, ImVec2(label_x, row0 + line * i),
                kBandLabel, labels[i]);
  }

  if (card_) {
    // Rep: five stars, filled in quarters.
    const float star = font * 0.42f;
    for (int i = 0; i < 5; ++i) {
      const float fill = std::clamp(card_->rep - float(i), 0.0f, 1.0f);
      DrawStar(dl,
               ImVec2(right - star - (4 - i) * (star * 2.0f + 3.0f),
                      row0 + font * 0.5f),
               star, fill);
    }

    // Gamerscore, with the G.
    const float g = font * 0.5f;
    const ImVec2 g_centre(right - g, row0 + line + font * 0.5f);
    dl->AddCircleFilled(g_centre, g, kGamerscore, 16);
    const ImVec2 g_size = TextSize(font * 0.75f, "G");
    BoldText(dl, font * 0.75f,
             ImVec2(g_centre.x - g_size.x * 0.5f, g_centre.y - g_size.y * 0.5f),
             kCardValue, "G");
    const std::string score = std::to_string(card_->gamerscore);
    const float score_w = TextSize(font, score.c_str()).x;
    BoldText(dl, font, ImVec2(right - g * 2.0f - 6.0f - score_w, row0 + line),
             kBandValue, score.c_str());

    const std::string zone = card_->zone.empty() ? "-" : card_->zone;
    const float zone_w = TextSize(font, zone.c_str()).x;
    BoldText(dl, font, ImVec2(right - zone_w, row0 + line * 2.0f), kBandValue,
             zone.c_str());
  }

  // What it is, then what it says (or the player, for voice).
  float y = band_y + band_h + 22.0f;
  BoldText(dl, font * 1.05f, ImVec2(origin.x + pad, y), kCardLabel,
           message.voice ? "Voice Message" : "Message Text");
  y += font * 1.05f + 18.0f;

  if (!message.voice) {
    dl->AddText(ImGui::GetFont(), font, ImVec2(origin.x + pad, y), kCardLabel,
                message.text.c_str(), nullptr, size.x - pad * 2.0f);
    return;
  }

  // The progress bar, then Stop / Play / Done.
  const float bar_w = size.x - pad * 2.0f;
  const float bar_height = font + 10.0f;
  const ImVec2 bar_min(origin.x + pad, y);
  const ImVec2 bar_max(bar_min.x + bar_w, bar_min.y + bar_height);
  dl->AddRectFilled(bar_min, bar_max, IM_COL32(126, 126, 144, 255));
  dl->AddRect(bar_min, bar_max, kBarFrame);
  if (play_ms_) {
    const float elapsed =
        float(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - play_started_)
                  .count());
    const float done = std::clamp(elapsed / float(play_ms_), 0.0f, 1.0f);
    dl->AddRectFilled(
        ImVec2(bar_min.x + 2.0f, bar_min.y + 2.0f),
        ImVec2(bar_min.x + 2.0f + (bar_w - 4.0f) * done, bar_max.y - 2.0f),
        kBarFill);
    if (done >= 1.0f) {
      play_ms_ = 0;
    }
  }

  y = bar_max.y + 10.0f;
  const float button_h = font + 26.0f;
  const float square_w = button_h * 1.2f;
  const ImVec2 stop_min(origin.x + pad, y);
  const ImVec2 play_min(stop_min.x + square_w, y);
  const ImVec2 done_min(play_min.x + square_w, y);
  const ImVec2 done_max(origin.x + size.x - pad, y + button_h);

  struct Button {
    const char* id;
    ImVec2 min;
    ImVec2 max;
  };
  const Button buttons[] = {
      {"##stop", stop_min, ImVec2(stop_min.x + square_w, y + button_h)},
      {"##play", play_min, ImVec2(play_min.x + square_w, y + button_h)},
      {"##done", done_min, done_max},
  };
  for (int i = 0; i < 3; ++i) {
    const Button& b = buttons[i];
    ImGui::SetCursorScreenPos(b.min);
    bool pressed = ImGui::InvisibleButton(
        b.id, ImVec2(b.max.x - b.min.x, b.max.y - b.min.y));
    if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                                   ImGui::GetIO().MouseDelta.y != 0.0f)) {
      player_focus_ = i;
    }
    const bool focus = player_focus_ == i;
    // A on the button the pad is on.
    if (focus && activate_) {
      activate_ = false;
      pressed = true;
    }
    if (focus) {
      dl->AddRectFilledMultiColor(b.min, b.max, kGreenLeft, kGreenRight,
                                  kGreenRight, kGreenLeft);
    } else {
      Gradient(dl, b.min, b.max, skin::kPanelTop, skin::kPanelBottom);
    }
    dl->AddRect(b.min, b.max, kBarFrame);
    const ImU32 glyph = focus ? skin::kSelectedText : IM_COL32(80, 80, 86, 255);
    const ImVec2 c((b.min.x + b.max.x) * 0.5f, (b.min.y + b.max.y) * 0.5f);
    const float s = button_h * 0.22f;
    if (i == 0) {
      dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s),
                        glyph);
    } else if (i == 1) {
      dl->AddTriangleFilled(ImVec2(c.x - s, c.y - s * 1.2f),
                            ImVec2(c.x - s, c.y + s * 1.2f),
                            ImVec2(c.x + s * 1.2f, c.y), glyph);
    } else {
      const ImVec2 done_size = TextSize(font, "Done");
      dl->AddText(ImGui::GetFont(), font,
                  ImVec2(c.x - done_size.x * 0.5f, c.y - done_size.y * 0.5f),
                  glyph, "Done");
    }
    if (pressed) {
      if (i == 0) {
        apu::sdl::VoiceChat::Get().StopPcmMessage();
        play_ms_ = 0;
      } else if (i == 1) {
        PlayMessage(message);
      } else {
        BackToList();
      }
    }
  }
}

void MessagesDialog::DrawMessage(const ImVec2& origin, const ImVec2& size) {
  if (open_ < 0 || open_ >= static_cast<int>(messages_.size())) {
    BackToList();
    return;
  }
  const kernel::HubMessage message = messages_[open_];

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float font = ImGui::GetFontSize() * 1.15f;
  const float half = size.x * 0.5f;

  // Left: Message Received, when, then what can be done with it.
  Gradient(dl, origin, ImVec2(origin.x + half, origin.y + size.y),
           skin::kPanelTop, skin::kPanelBottom);
  float y = origin.y + 24.0f;
  BoldText(dl, font * 1.05f, ImVec2(origin.x + 30.0f, y), skin::kText,
           "Message Received");
  y += font * 1.05f + 12.0f;
  std::vector<std::string> lines = {FormatDate(message.created_at),
                                    FormatTime(message.created_at)};
  if (message.voice) {
    lines.push_back("Duration: " + FormatDuration(message.duration_ms));
  }
  for (const auto& line : lines) {
    dl->AddText(ImGui::GetFont(), font, ImVec2(origin.x + 30.0f, y), kSubText,
                line.c_str());
    y += font + 10.0f;
  }
  y += 12.0f;
  dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + half, y), skin::kRowLine);

  const float action_h = font + 34.0f;
  for (int i = 0; i < kActionCount; ++i) {
    const ImVec2 min(origin.x, y);
    const ImVec2 max(origin.x + half, y + action_h);
    ImGui::SetCursorScreenPos(min);
    bool pressed = ImGui::InvisibleButton(fmt::format("##action{}", i).c_str(),
                                          ImVec2(half, action_h));
    if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                                   ImGui::GetIO().MouseDelta.y != 0.0f)) {
      action_ = i;
      player_focus_ = -1;
    }
    // The pad is on the actions (not the player) and on this one.
    const bool focused = player_focus_ < 0 && action_ == i;
    if (focused && activate_) {
      activate_ = false;
      pressed = true;
    }
    if (focused) {
      dl->AddRectFilledMultiColor(min, max, kGreenLeft, kGreenRight,
                                  kGreenRight, kGreenLeft);
    }
    dl->AddLine(ImVec2(min.x, max.y - 1.0f), ImVec2(max.x, max.y - 1.0f),
                skin::kRowLine);
    BoldText(dl, font, ImVec2(min.x + 30.0f, min.y + (action_h - font) * 0.5f),
             focused ? skin::kSelectedText : skin::kText, kActions[i]);
    y = max.y;

    if (!pressed) {
      continue;
    }
    const uint64_t xuid = LocalXuid();
    auto* live = emulator_window_->emulator()->GetXboxLiveAPI();
    if (i == 0 || i == 1) {
      StartCompose(i == 1, message.from_xuid, message.from_name);
    } else if (i == 2) {
      if (xuid) {
        live->MarkMessageUnread(xuid, message.id);
        live->RefreshMessageCounts();
      }
      messages_[open_].read = false;
      BackToList();
    } else {
      if (xuid) {
        live->DeleteMessage(xuid, message.id);
        live->RefreshMessageCounts();
      }
      messages_.erase(messages_.begin() + open_);
      selected_ = std::min(selected_, static_cast<int>(messages_.size()) - 1);
      BackToList();
    }
    return;
  }

  // Right: the sender's card and the message.
  DrawCard(ImVec2(origin.x + half, origin.y), ImVec2(size.x - half, size.y),
           message);
}

void MessagesDialog::DrawCompose() {
  if (compose_to_xuid_) {
    ImGui::Text("To: %s", compose_to_name_.empty() ? "Unknown"
                                                   : compose_to_name_.c_str());
  } else if (recipients_loading_) {
    ImGui::TextUnformatted("Loading players...");
  } else if (recipients_.empty()) {
    ImGui::TextUnformatted(
        "Nobody to write to yet - add a friend, or play a session with");
    ImGui::TextUnformatted("someone and they will appear here.");
  }

  if (!recipients_loading_ && !recipients_.empty()) {
    if (!compose_to_xuid_) {
      ImGui::TextUnformatted("To:");
    }
    ImGui::BeginChild("##recipients", ImVec2(-1.0f, 140.0f),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoNav);

    for (int i = 0; i < static_cast<int>(recipients_.size()); ++i) {
      const std::string label = fmt::format(
          "{}##to{}",
          recipients_[i].gamertag.empty() ? "Unknown" : recipients_[i].gamertag,
          i);

      if (ImGui::Selectable(label.c_str(),
                            compose_to_xuid_ == recipients_[i].xuid)) {
        compose_target_ = i;
        compose_to_xuid_ = recipients_[i].xuid;
        compose_to_name_ = recipients_[i].gamertag;
      }
    }

    ImGui::EndChild();
  }

  ImGui::Separator();

  if (!compose_voice_) {
    ImGui::InputTextMultiline("##body", compose_text_.data(),
                              compose_text_.size(), ImVec2(-1.0f, 90.0f));

    // Activated by a click (A on the pad goes through HandlePad). The
    // drawer's automatic on-screen keyboard refuses to open inside a popup -
    // it is a popup itself, and opening one at the same level would tear this
    // dialog down and take the half-typed message with it - so it is opened
    // here and the result written straight back into our own buffer.
    if (ImGui::IsItemActivated()) {
      OpenKeyboard();
    }

    const size_t used = std::strlen(compose_text_.data());

    // Turns red as it fills so the limit is visible before it bites.
    if (used >= kMaxTextChars) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%zu / %zu", used,
                         kMaxTextChars);
    } else {
      ImGui::Text("%zu / %zu", used, kMaxTextChars);
    }
  } else {
    if (recording_) {
      ImGui::Text("Recording %.1fs - %zu seconds maximum",
                  double(recorded_pcm_.size()) / double(kStoreRate),
                  kMaxRecordSeconds);

      if (ImGui::Button("Stop")) {
        StopRecording();
      }
    } else {
      ImGui::Text("%.1f seconds recorded",
                  double(recorded_pcm_.size()) / double(kStoreRate));

      if (ImGui::Button("Record")) {
        StartRecording();
      }

      if (!recorded_pcm_.empty()) {
        ImGui::SameLine();

        if (ImGui::Button("Play Back")) {
          const std::vector<int16_t> playable =
              UpsampleToCaptureRate(recorded_pcm_);

          apu::sdl::VoiceChat::Get().PlayPcmMessage(playable.data(),
                                                    playable.size());
        }
      }
    }
  }

  ImGui::Separator();

  if (ImGui::Button("Send")) {
    SendComposed();
  }

  ImGui::SameLine();

  if (ImGui::Button("Cancel")) {
    LeaveCompose();
  }

  if (!status_.empty()) {
    ImGui::Separator();
    ImGui::TextWrapped("%s", status_.c_str());
  }
}

void MessagesDialog::OnDraw(ImGuiIO& io) {
  const char* title = mode_ == Mode::kText ? "Text Messages" : "Voice Messages";

  // The pad comes through the focus manager, as the guide takes it: the
  // dialog takes focus when it opens, and reads its input by name.
  auto* focus_manager = imgui_drawer()->GetFocusManager();
  if (!opened_) {
    ImGui::OpenPopup(title);
    focus_manager->UISetFocus(kFocusName);
    opened_ = true;
  }
  input_ = focus_manager->XamInputFocus(kFocusName);
  activate_ = false;

  PollQueries();
  PumpRecording();

  // The console's frame: wide, centred, no title bar of its own. Wide enough
  // for the drawer's larger (title) font, which the whole dialog uses.
  const float width = std::clamp(io.DisplaySize.x * 0.9f, 640.0f, 1400.0f);
  const float height =
      std::min(std::max(width * 0.56f, 420.0f), io.DisplaySize.y - 40.0f);
  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg,
                        ImGui::ColorConvertU32ToFloat4(kFrame));

  bool open = true;
  const bool visible = ImGui::BeginPopupModal(
      title, &open,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
          ImGuiWindowFlags_NoScrollWithMouse |
          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);

  if (visible) {
    // The drawer's title font (font_size + 6, the one the guide's rows use):
    // larger and still crisp, where scaling the small one would blur it.
    ImGui::PushFont(imgui_drawer()->GetTitleFont());
    // The selected row / action / button is drawn as the console's green bar;
    // ImGui's own nav frame around a widget would be a second highlight.
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float header_h = 70.0f;
    const float tabs_h = ImGui::GetFontSize() * 1.15f + 30.0f;
    const float legend_h = 52.0f;
    const float side = 0.0f;
    const ImVec2 body_min(origin.x + side, origin.y + header_h);
    const float body_w = width - side * 2.0f;

    DrawHeader(origin, width);

    // B: back out one step - the open message to the list, writing to where
    // it came from, the list out of the dialog. While the keyboard is up it
    // has the focus, so none of this input arrives here.
    const bool back =
        input_.ShouldClose() ||
        (!keyboard_open_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    if (!back) {
      HandlePad();
    }

    if (view_ == View::kMessage) {
      DrawMessage(body_min, ImVec2(body_w, height - header_h - legend_h));
      if (back && view_ == View::kMessage) {
        BackToList();
      }
    } else {
      DrawTabs(body_min, body_w, tabs_h);
      const ImVec2 list_min(body_min.x, body_min.y + tabs_h);
      const ImVec2 list_size(body_w, height - header_h - tabs_h - legend_h);
      if (view_ == View::kList) {
        DrawList(list_min, list_size);
        if (back) {
          open = false;
        }
      } else {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        Gradient(dl, list_min,
                 ImVec2(list_min.x + list_size.x, list_min.y + list_size.y),
                 skin::kPanelTop, skin::kPanelBottom);
        ImGui::SetCursorScreenPos(
            ImVec2(list_min.x + 24.0f, list_min.y + 16.0f));
        ImGui::BeginChild(
            "##compose", ImVec2(list_size.x - 48.0f, list_size.y - 24.0f),
            ImGuiChildFlags_None,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
        DrawCompose();
        ImGui::EndChild();
        if (back && view_ == View::kCompose) {
          LeaveCompose();
        }
      }
    }

    DrawLegend(ImVec2(origin.x, origin.y + height - legend_h), width, legend_h);

    ImGui::PopStyleColor();
    ImGui::PopFont();

    if (!open) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (!open) {
    Close();
  }
}

}  // namespace app
}  // namespace xe
