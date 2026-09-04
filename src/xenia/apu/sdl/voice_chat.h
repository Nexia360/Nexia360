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

#include <atomic>
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

struct VoiceWasapiStream;

class VoiceChat {
 public:
  static VoiceChat& Get();

  void AddRef();
  void Release();
  void SetEnabled(bool enabled);

  // Runs the device without touching (or persisting) the user's voice chat
  // setting. For playing back a recorded message.
  void SetMessagePlayback(bool active);
  bool enabled();

  size_t ReadCapturePcm(int16_t* out, size_t max_samples);

  // Exact, gapless read of everything captured since `cursor`, which is
  // advanced by however much was taken. Returns 0 when nothing new has
  // arrived.
  //
  // ReadCapturePcm is paced by the clock and always returns a full page
  // whether or not there is new audio - right for live chat, wrong for
  // recording, where it yields overlapping pages and never reports "empty".
  // Start a recording with cursor = capture_position().
  size_t ReadCaptureSince(uint64_t& cursor, int16_t* out, size_t max_samples);

  // Total samples captured so far - the starting point for a recording.
  uint64_t capture_position();

  void PlayPcm(const int16_t* samples, size_t count);

  // Play a recording through to the end. It gets its own queue, mixed over
  // live chat on the way out: PlayPcm keeps only the most recent 100 ms - it
  // is a jitter buffer, where stale audio is worse than dropped audio - so a
  // message sharing that queue is cut to its last fraction of a second the
  // moment the guest plays a single voice packet.
  void PlayPcmMessage(const int16_t* samples, size_t count);

  // Drop anything still queued from PlayPcmMessage.
  void StopPcmMessage();

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

  void OnCapture(const int16_t* samples, size_t count);
  void FillPlayback(int16_t* out, size_t count);

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

  static void CaptureThunk(void* userdata, uint8_t* stream, int len);
  static void PlaybackThunk(void* userdata, uint8_t* stream, int len);

  std::mutex state_mutex_;
  int ref_count_ = 0;
  bool enabled_ = false;

  // Playing a voice message needs the device running without turning the
  // user's voice chat on behind their back - enabled_ is their setting and is
  // written to disk.
  bool message_playback_ = false;
  bool running_ = false;
  VoiceWasapiStream* wasapi_capture_ = nullptr;
  VoiceWasapiStream* wasapi_playback_ = nullptr;
  uint32_t capture_device_ = 0;
  uint32_t playback_device_ = 0;
  std::string mic_name_;     // "" = system default
  std::string output_name_;  // "" = system default
  int voice_volume_ = 100;

  // Read by the capture thread on every buffer. Atomic so that thread never
  // takes state_mutex_: Stop() joins it while holding that lock, and the two
  // together deadlock the process hard enough that the window stops pumping.
  std::atomic<int> mic_gain_{8};
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

  // Voice mail, kept apart from the chat jitter buffer above so the guest's
  // own voice traffic cannot truncate it. Mixed into the output in
  // FillPlayback. No priming: the whole recording is already here.
  std::deque<int16_t> message_pcm_;
};

}  // namespace sdl
}  // namespace apu
}  // namespace xe

#endif
