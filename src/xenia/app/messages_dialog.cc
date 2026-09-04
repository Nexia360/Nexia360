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
#include <chrono>
#include <cstring>

#include "third_party/fmt/include/fmt/format.h"
// ClearActiveID lives in the internal header, not the public one.
#include "third_party/imgui/imgui_internal.h"
#include "xenia/app/emulator_window.h"
#include "xenia/apu/sdl/voice_chat.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/friends_manager.h"
#include "xenia/kernel/xam/xam_state.h"
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

MessagesDialog::MessagesDialog(ui::ImGuiDrawer* imgui_drawer,
                               EmulatorWindow* emulator_window, Mode mode)
    : ui::ImGuiDialog(imgui_drawer),
      emulator_window_(emulator_window),
      mode_(mode) {
  Reload();
}

MessagesDialog::~MessagesDialog() {
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

  selected_ = -1;
  loading_ = true;

  messages_query_ = emulator_window_->emulator()->GetXboxLiveAPI()->
      GetMessagesAsync(xuid, mode_ == Mode::kText ? "text" : "voice");
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

  status_ = fmt::format("Playing {:.1f}s from {}.", seconds,
                        message.from_name);
}

void MessagesDialog::SendComposed() {
  const uint64_t xuid = LocalXuid();

  if (!xuid || compose_target_ < 0 ||
      compose_target_ >= static_cast<int>(recipients_.size())) {
    status_ = "Pick who it goes to first.";
    return;
  }

  const uint64_t target = recipients_[compose_target_].xuid;

  auto* live = emulator_window_->emulator()->GetXboxLiveAPI();

  kernel::SendMessageOutcome outcome = kernel::SendMessageOutcome::kInvalid;

  if (mode_ == Mode::kText) {
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
      view_ = View::kList;
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

void MessagesDialog::DrawList() {
  if (messages_.empty()) {
    ImGui::TextUnformatted(loading_ ? "Loading..." : "No messages.");
  } else {
    ImGui::BeginChild("##messages", ImVec2(560.0f, 220.0f), true);

    for (int i = 0; i < static_cast<int>(messages_.size()); ++i) {
      const auto& message = messages_[i];

      // Unread is what the badge counts, so it is what the row leads with.
      const std::string label = fmt::format(
          "{}{} - {}##msg{}", message.read ? "" : "* ",
          message.from_name.empty() ? "Unknown" : message.from_name,
          message.created_at.substr(0, std::min<size_t>(
                                           16, message.created_at.size())),
          i);

      if (ImGui::Selectable(label.c_str(), selected_ == i)) {
        selected_ = i;

        // Opening it is what makes it read, here and on the hub.
        if (!messages_[i].read) {
          messages_[i].read = true;

          const uint64_t xuid = LocalXuid();

          if (xuid) {
            auto* live = emulator_window_->emulator()->GetXboxLiveAPI();
            live->MarkMessageRead(xuid, messages_[i].id);
            live->RefreshMessageCounts();
          }
        }
      }
    }

    ImGui::EndChild();
  }

  if (selected_ >= 0 && selected_ < static_cast<int>(messages_.size())) {
    const auto& message = messages_[selected_];

    ImGui::Separator();

    if (mode_ == Mode::kText) {
      ImGui::TextWrapped("%s", message.text.c_str());
    } else {
      ImGui::Text("Voice message, %.1f seconds",
                  double(message.duration_ms) / 1000.0);

      if (ImGui::Button("Play")) {
        PlayMessage(message);
      }

      ImGui::SameLine();
    }

    if (ImGui::Button("Delete")) {
      const uint64_t xuid = LocalXuid();

      if (xuid) {
        auto* live = emulator_window_->emulator()->GetXboxLiveAPI();
        live->DeleteMessage(xuid, message.id);
        live->RefreshMessageCounts();
      }

      Reload();
    }
  }

  ImGui::Separator();

  if (ImGui::Button(mode_ == Mode::kText ? "New Message" : "New Voice Message")) {
    view_ = View::kCompose;
    status_.clear();

    if (recipients_.empty() && !recipients_loading_) {
      StartRecipientQuery();
    }
  }

  ImGui::SameLine();

  if (ImGui::Button("Refresh")) {
    Reload();
  }
}

void MessagesDialog::DrawCompose() {
  if (recipients_loading_) {
    ImGui::TextUnformatted("Loading players...");
  } else if (recipients_.empty()) {
    ImGui::TextUnformatted(
        "Nobody to write to yet - add a friend, or play a session with");
    ImGui::TextUnformatted("someone and they will appear here.");
  } else {
    ImGui::TextUnformatted("To:");
    ImGui::BeginChild("##recipients", ImVec2(560.0f, 140.0f), true);

    for (int i = 0; i < static_cast<int>(recipients_.size()); ++i) {
      const std::string label =
          fmt::format("{}##to{}",
                      recipients_[i].gamertag.empty() ? "Unknown"
                                                      : recipients_[i].gamertag,
                      i);

      if (ImGui::Selectable(label.c_str(), compose_target_ == i)) {
        compose_target_ = i;
      }
    }

    ImGui::EndChild();
  }

  ImGui::Separator();

  if (mode_ == Mode::kText) {
    ImGui::InputTextMultiline("##body", compose_text_.data(),
                              compose_text_.size(), ImVec2(560.0f, 90.0f));

    // Activated by a click or by A on the pad. The drawer's automatic
    // on-screen keyboard refuses to open inside a popup - it is a popup
    // itself, and opening one at the same level would tear this dialog down
    // and take the half-typed message with it - so it is opened here and the
    // result written straight back into our own buffer.
    if (ImGui::IsItemActivated() && !keyboard_open_) {
      keyboard_open_ = true;

      // Editing belongs to the keyboard now, not to the field behind it.
      ImGui::ClearActiveID();

      auto* keyboard = ui::KeyboardDialog::ShowKeyboard(
          imgui_drawer(), "Message", std::string(compose_text_.data()),
          // Empty parent: it registers as a child of whatever is focused,
          // which is this dialog, so B goes to the keyboard rather than
          // closing the compose view behind it.
          ui::KeyboardDialog::InputType::kText, nullptr, "",
          "OnScreenKeyboard");

      // The dialog can be closed while the keyboard is still up, so the
      // callback checks that there is still something to write into.
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
    if (recording_) {
      StopRecording();
    }

    view_ = View::kList;
    status_.clear();
  }
}

void MessagesDialog::OnDraw(ImGuiIO& io) {
  const char* title = mode_ == Mode::kText ? "Text Messages" : "Voice Messages";

  if (!opened_) {
    ImGui::OpenPopup(title);
    opened_ = true;
  }

  // Collect the async results before drawing anything that reads them.
  if (loading_ && messages_query_.valid() &&
      messages_query_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    messages_ = messages_query_.get();
    loading_ = false;
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
      const uint64_t xuid = LocalXuid();
      const auto friends =
          kernel_state->xam_state()->friends_manager()->GetFriends(xuid);

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
          player.gamertag = std::string(entry.Gamertag);

          recipients_.push_back(player);
        }
      }
    }
  }

  PumpRecording();

  bool open = true;

  if (ImGui::BeginPopupModal(title, &open,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (view_ == View::kList) {
      DrawList();
    } else {
      DrawCompose();
    }

    if (!status_.empty()) {
      ImGui::Separator();
      ImGui::TextWrapped("%s", status_.c_str());
    }

    ImGui::Separator();

    if (ImGui::Button("Close")) {
      ImGui::CloseCurrentPopup();
      open = false;
    }

    ImGui::EndPopup();
  }

  if (!open) {
    Close();
  }
}

}  // namespace app
}  // namespace xe
