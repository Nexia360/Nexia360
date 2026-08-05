/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sdl/voice_chat.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

#include "xenia/base/logging.h"
#include "xenia/helper/sdl/sdl_helper.h"

namespace xe {
namespace apu {
namespace sdl {

constexpr int kSampleRate = 16000;
constexpr int kSdlBufferSamples = 512;
constexpr size_t kMaxRingSamples = kSampleRate;

static bool EnsureAudioInit() {
  if (!xe::helper::sdl::SDLHelper::Prepare()) {
    return false;
  }
  if (SDL_WasInit(SDL_INIT_AUDIO)) {
    return true;
  }
  return SDL_InitSubSystem(SDL_INIT_AUDIO) >= 0;
}

static std::vector<std::string> Enumerate(int iscapture) {
  std::vector<std::string> names;
  if (!EnsureAudioInit()) {
    return names;
  }
  int count = SDL_GetNumAudioDevices(iscapture);
  for (int i = 0; i < count; ++i) {
    const char* name = SDL_GetAudioDeviceName(i, iscapture);
    if (name) {
      names.push_back(name);
    }
  }
  return names;
}

VoiceChat& VoiceChat::Get() {
  static VoiceChat instance;
  return instance;
}

VoiceChat::~VoiceChat() { Stop(); }

std::vector<std::string> VoiceChat::EnumerateMics() { return Enumerate(1); }
std::vector<std::string> VoiceChat::EnumerateOutputs() { return Enumerate(0); }

void VoiceChat::SetMic(const std::string& name) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (mic_name_ == name) {
    return;
  }
  mic_name_ = name;
  if (running_) {
    OpenCaptureLocked();
  }
  SaveSettingsLocked();
}

void VoiceChat::SetOutput(const std::string& name) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (output_name_ == name) {
    return;
  }
  output_name_ = name;
  if (running_) {
    OpenPlaybackLocked();
  }
  SaveSettingsLocked();
}

void VoiceChat::SetVoiceVolume(int volume) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  volume = std::clamp(volume, 0, 100);
  if (voice_volume_ == volume) {
    return;
  }
  voice_volume_ = volume;
  SaveSettingsLocked();
}

int VoiceChat::voice_volume() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return voice_volume_;
}

void VoiceChat::SetMicGain(int gain) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  gain = std::clamp(gain, 1, 32);
  if (mic_gain_ == gain) {
    return;
  }
  mic_gain_ = gain;
  SaveSettingsLocked();
}

int VoiceChat::mic_gain() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return mic_gain_;
}

void VoiceChat::SetSettingsPath(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  settings_path_ = path;
  LoadSettingsLocked();
  UpdateRunningLocked();
}

void VoiceChat::SaveSettingsLocked() {
  if (settings_path_.empty()) {
    return;
  }
  std::ofstream f(settings_path_, std::ios::binary | std::ios::trunc);
  if (!f) {
    return;
  }
  f << "enabled=" << (enabled_ ? 1 : 0) << "\n";
  f << "mic=" << mic_name_ << "\n";
  f << "output=" << output_name_ << "\n";
  f << "volume=" << voice_volume_ << "\n";
  f << "mic_gain=" << mic_gain_ << "\n";
}

void VoiceChat::LoadSettingsLocked() {
  if (settings_path_.empty()) {
    return;
  }
  std::ifstream f(settings_path_, std::ios::binary);
  if (!f) {
    return;
  }
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    if (key == "enabled") {
      enabled_ = (val == "1");
    } else if (key == "mic") {
      mic_name_ = val;
    } else if (key == "output") {
      output_name_ = val;
    } else if (key == "volume") {
      try {
        voice_volume_ = std::clamp(std::stoi(val), 0, 100);
      } catch (...) {
      }
    } else if (key == "mic_gain") {
      try {
        mic_gain_ = std::clamp(std::stoi(val), 1, 32);
      } catch (...) {
      }
    }
  }
}

std::string VoiceChat::mic_name() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return mic_name_;
}

std::string VoiceChat::output_name() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return output_name_;
}

void VoiceChat::AddRef() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  ++ref_count_;
  UpdateRunningLocked();
}

void VoiceChat::Release() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (ref_count_ > 0) {
    --ref_count_;
  }
  UpdateRunningLocked();
}

void VoiceChat::SetEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  enabled_ = enabled;
  UpdateRunningLocked();
  SaveSettingsLocked();
}

bool VoiceChat::enabled() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return enabled_;
}

void VoiceChat::UpdateRunningLocked() {
  const bool want = enabled_ && ref_count_ > 0;
  if (want && !running_) {
    Start();
  } else if (!want && running_) {
    Stop();
  }
}

void VoiceChat::OpenCaptureLocked() {
  if (capture_device_) {
    SDL_CloseAudioDevice(capture_device_);
    capture_device_ = 0;
  }
  SDL_AudioSpec want = {};
  want.freq = kSampleRate;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = kSdlBufferSamples;
  want.userdata = this;
  want.callback = &VoiceChat::CaptureThunk;
  SDL_AudioSpec have = {};
  const char* dev = mic_name_.empty() ? nullptr : mic_name_.c_str();
  capture_device_ = SDL_OpenAudioDevice(dev, 1, &want, &have, 0);
  if (!capture_device_) {
    XELOGW("VoiceChat: open mic failed (no mic?): {}", SDL_GetError());
    return;
  }
  SDL_PauseAudioDevice(capture_device_, 0);
}

void VoiceChat::OpenPlaybackLocked() {
  if (playback_device_) {
    SDL_CloseAudioDevice(playback_device_);
    playback_device_ = 0;
  }
  SDL_AudioSpec want = {};
  want.freq = kSampleRate;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = kSdlBufferSamples;
  want.userdata = this;
  want.callback = &VoiceChat::PlaybackThunk;
  SDL_AudioSpec have = {};
  const char* dev = output_name_.empty() ? nullptr : output_name_.c_str();
  playback_device_ = SDL_OpenAudioDevice(dev, 0, &want, &have, 0);
  if (!playback_device_) {
    XELOGE("VoiceChat: open output failed: {}", SDL_GetError());
    return;
  }
  SDL_PauseAudioDevice(playback_device_, 0);
}

bool VoiceChat::Start() {
  if (running_) {
    return true;
  }
  if (!EnsureAudioInit()) {
    XELOGE("VoiceChat: audio init failed");
    return false;
  }
  OpenPlaybackLocked();
  OpenCaptureLocked();
  running_ = true;
  XELOGD("VoiceChat: started (mic={}, output={})", capture_device_ != 0,
         playback_device_ != 0);
  return true;
}

void VoiceChat::Stop() {
  if (!running_) {
    return;
  }
  if (capture_device_) {
    SDL_CloseAudioDevice(capture_device_);
    capture_device_ = 0;
  }
  if (playback_device_) {
    SDL_CloseAudioDevice(playback_device_);
    playback_device_ = 0;
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    capture_total_ = 0;
    capture_read_ = 0;
    last_request_ = {};
    std::fill(std::begin(capture_ring_), std::end(capture_ring_),
              static_cast<int16_t>(0));
    playback_pcm_.clear();
  }
  running_ = false;
  XELOGD("VoiceChat: stopped");
}

void VoiceChat::OnCapture(const int16_t* samples, size_t count) {
  int max_gain;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    max_gain = mic_gain_;
  }
  int block_peak = 0;
  for (size_t i = 0; i < count; ++i) {
    int a = samples[i] < 0 ? -samples[i] : samples[i];
    if (a > block_peak) {
      block_peak = a;
    }
  }
  if (block_peak > agc_env_) {
    agc_env_ = static_cast<float>(block_peak);
  } else {
    agc_env_ = agc_env_ * 0.98f + block_peak * 0.02f;
  }
  constexpr float kTarget = 0.25f * 32767.0f;
  constexpr float kNoiseFloor = 250.0f;
  float gain = 1.0f;
  if (agc_env_ > kNoiseFloor) {
    gain = std::clamp(kTarget / agc_env_, 1.0f, static_cast<float>(max_gain));
  }
  std::lock_guard<std::mutex> lock(queue_mutex_);
  for (size_t i = 0; i < count; ++i) {
    int v = static_cast<int>(samples[i] * gain);
    capture_ring_[capture_total_ % kCaptureRing] =
        static_cast<int16_t>(std::clamp(v, -32768, 32767));
    ++capture_total_;
  }
}

size_t VoiceChat::ReadCapturePcm(int16_t* out, size_t max_samples) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  constexpr size_t kPage = kCaptureFrame;
  const auto now = std::chrono::steady_clock::now();
  const bool first = (last_request_ == std::chrono::steady_clock::time_point{});
  if (first) {
    capture_read_ = capture_total_;
  } else {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        now - last_request_)
                        .count();
    capture_read_ += static_cast<uint64_t>(us) * kSampleRate / 1000000;
  }
  last_request_ = now;
  const uint64_t lo =
      capture_total_ > kCaptureRing ? capture_total_ - kCaptureRing : 0;
  const uint64_t hi = capture_total_ > kPage ? capture_total_ - kPage : 0;
  if (capture_read_ < lo) {
    capture_read_ = lo;
  }
  if (capture_read_ > hi) {
    capture_read_ = hi;
  }
  const size_t n = std::min(max_samples, kPage);
  for (size_t i = 0; i < n; ++i) {
    out[i] = capture_ring_[(capture_read_ + i) % kCaptureRing];
  }
  return n;
}

void VoiceChat::PlayPcm(const int16_t* samples, size_t count) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  playback_pcm_.insert(playback_pcm_.end(), samples, samples + count);
  constexpr size_t kMaxPlaybackSamples = kSampleRate / 10;
  if (playback_pcm_.size() > kMaxPlaybackSamples) {
    playback_pcm_.erase(
        playback_pcm_.begin(),
        playback_pcm_.begin() + (playback_pcm_.size() - kMaxPlaybackSamples));
  }
}

void VoiceChat::FillPlayback(int16_t* out, size_t count) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  constexpr size_t kPrimeSamples = kSampleRate / 20;
  if (!playback_primed_) {
    if (playback_pcm_.size() < kPrimeSamples) {
      std::memset(out, 0, count * sizeof(int16_t));
      return;
    }
    playback_primed_ = true;
  }
  size_t avail = std::min(count, playback_pcm_.size());
  for (size_t i = 0; i < avail; ++i) {
    out[i] = playback_pcm_[i];
  }
  playback_pcm_.erase(playback_pcm_.begin(), playback_pcm_.begin() + avail);
  for (size_t i = avail; i < count; ++i) {
    out[i] = 0;
  }
  if (playback_pcm_.empty()) {
    playback_primed_ = false;
  }
}

void VoiceChat::CaptureThunk(void* userdata, uint8_t* stream, int len) {
  auto* self = static_cast<VoiceChat*>(userdata);
  self->OnCapture(reinterpret_cast<const int16_t*>(stream),
                  static_cast<size_t>(len) / sizeof(int16_t));
}

void VoiceChat::PlaybackThunk(void* userdata, uint8_t* stream, int len) {
  auto* self = static_cast<VoiceChat*>(userdata);
  self->FillPlayback(reinterpret_cast<int16_t*>(stream),
                     static_cast<size_t>(len) / sizeof(int16_t));
}

}  // namespace sdl
}  // namespace apu
}  // namespace xe
