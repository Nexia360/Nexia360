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

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace xe {
namespace apu {
namespace sdl {

// Host audio I/O for chat voice (16 kHz mono PCM, no codec -- the guest's
// XamVoice/XHV2 does the Siren encode/decode and the network itself):
//   * capture: the selected mic -> ReadCapturePcm, fed into the title's mic
//     buffer so it encodes + transmits.
//   * playback: decoded remote voice the title hands back -> PlayPcm, out the
//     selected Voice Chat Output device.
// Refcounted by the title's XamVoice* calls plus the Sound-menu enable.
class VoiceChat {
 public:
  static VoiceChat& Get();

  void AddRef();
  void Release();
  void SetEnabled(bool enabled);
  bool enabled();

  // Mic in: raw captured host PCM (host-endian int16). Returns count written
  // (0 when not capturing / disabled).
  size_t ReadCapturePcm(int16_t* out, size_t max_samples);

  // Remote voice out: queue decoded PCM for the Voice Chat Output device.
  void PlayPcm(const int16_t* samples, size_t count);

  // Device lists/selection. Safe at any time. Empty name = system default.
  static std::vector<std::string> EnumerateMics();
  static std::vector<std::string> EnumerateOutputs();
  void SetMic(const std::string& name);
  void SetOutput(const std::string& name);
  std::string mic_name();
  std::string output_name();

  // Voice chat volume, 0..100 (what the guest's VOICE_VOLUME profile setting
  // reports to XHV). Set from the Sound menu, persisted with the rest.
  void SetVoiceVolume(int volume);
  int voice_volume();

  // Mic capture gain (integer multiplier) applied to host mic PCM before it's
  // handed to the title's encoder. Set from the Sound menu, persisted.
  void SetMicGain(int gain);
  int mic_gain();

  // Persist enabled/mic/output to a config file. SetSettingsPath loads the file
  // (applying it) and remembers where to write; subsequent changes auto-save.
  void SetSettingsPath(const std::filesystem::path& path);

 private:
  VoiceChat() = default;
  ~VoiceChat();

  bool Start();
  void Stop();
  void UpdateRunningLocked();
  void OpenCaptureLocked();
  void OpenPlaybackLocked();
  void SaveSettingsLocked();   // write settings_path_ from current state
  void LoadSettingsLocked();   // read settings_path_ into current state

  void OnCapture(const int16_t* samples, size_t count);
  void FillPlayback(int16_t* out, size_t count);
  static void CaptureThunk(void* userdata, uint8_t* stream, int len);
  static void PlaybackThunk(void* userdata, uint8_t* stream, int len);

  std::mutex state_mutex_;
  int ref_count_ = 0;
  bool enabled_ = false;
  bool running_ = false;
  uint32_t capture_device_ = 0;   // SDL_AudioDeviceID (mic)
  uint32_t playback_device_ = 0;  // SDL_AudioDeviceID (output)
  std::string mic_name_;          // "" = system default
  std::string output_name_;       // "" = system default
  int voice_volume_ = 100;        // 0..100, reported to guest as VOICE_VOLUME
  int mic_gain_ = 8;              // capture AGC max gain (ceiling)
  float agc_env_ = 0.0f;          // capture AGC running peak envelope

  std::filesystem::path settings_path_;  // "" = persistence off

  std::mutex queue_mutex_;
  std::deque<int16_t> capture_pcm_;   // raw mic ring
  std::deque<int16_t> playback_pcm_;  // decoded remote voice ring
  bool playback_primed_ = false;      // jitter-buffer prime/underrun state
};

}  // namespace sdl
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SDL_VOICE_CHAT_H_
