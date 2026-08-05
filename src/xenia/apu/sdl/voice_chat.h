/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SDL_VOICE_CHAT_H_
#define XENIA_APU_SDL_VOICE_CHAT_H_

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace xe {
namespace apu {
namespace sdl {

class VoiceChat {
 public:
  static VoiceChat& Get();

  void AddRef();
  void Release();
  void SetEnabled(bool enabled);
  bool enabled();

  size_t ReadCapturePcm(int16_t* out, size_t max_samples);

  void PlayPcm(const int16_t* samples, size_t count);

  static std::vector<std::string> EnumerateMics();
  static std::vector<std::string> EnumerateOutputs();
  void SetMic(const std::string& name);
  void SetOutput(const std::string& name);
  std::string mic_name();
  std::string output_name();

  void SetVoiceVolume(int volume);
  int voice_volume();

  void SetMicGain(int gain);
  int mic_gain();

  void SetSettingsPath(const std::filesystem::path& path);

 private:
  VoiceChat() = default;
  ~VoiceChat();

  bool Start();
  void Stop();
  void UpdateRunningLocked();
  void OpenCaptureLocked();
  void OpenPlaybackLocked();
  void SaveSettingsLocked();
  void LoadSettingsLocked();

  void OnCapture(const int16_t* samples, size_t count);
  void FillPlayback(int16_t* out, size_t count);
  static void CaptureThunk(void* userdata, uint8_t* stream, int len);
  static void PlaybackThunk(void* userdata, uint8_t* stream, int len);

  std::mutex state_mutex_;
  int ref_count_ = 0;
  bool enabled_ = false;
  bool running_ = false;
  uint32_t capture_device_ = 0;
  uint32_t playback_device_ = 0;
  std::string mic_name_;     // "" = system default
  std::string output_name_;  // "" = system default
  int voice_volume_ = 100;
  int mic_gain_ = 8;
  float agc_env_ = 0.0f;

  std::filesystem::path settings_path_;  // "" = persistence off

  std::mutex queue_mutex_;
  static constexpr size_t kCaptureFrame = 320;
  static constexpr size_t kCaptureRing = 1600;
  int16_t capture_ring_[kCaptureRing] = {};
  uint64_t capture_total_ = 0;
  uint64_t capture_read_ = 0;
  std::chrono::steady_clock::time_point last_request_{};
  std::deque<int16_t> playback_pcm_;
  bool playback_primed_ = false;
};

}  // namespace sdl
}  // namespace apu
}  // namespace xe

#endif
