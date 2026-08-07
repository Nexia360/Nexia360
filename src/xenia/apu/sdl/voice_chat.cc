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
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif
#include "xenia/helper/sdl/sdl_helper.h"

#if XE_PLATFORM_WIN32
#include <audioclient.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#endif

namespace xe {
namespace apu {
namespace sdl {

constexpr int kSampleRate = 16000;
constexpr int kSdlBufferSamples = 512;
constexpr size_t kMaxRingSamples = kSampleRate;

#if !XE_PLATFORM_WIN32
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
#endif

#if XE_PLATFORM_WIN32

struct VoiceWasapiStream {
  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  IAudioRenderClient* render = nullptr;
  HANDLE event = nullptr;
  std::thread thread;
  std::atomic<bool> run{false};
  VoiceChat* owner = nullptr;
  UINT32 buffer_frames = 0;
  UINT32 dev_rate = 48000;
  UINT32 dev_channels = 2;
  bool dev_float = true;
  UINT32 dev_bits = 32;
  std::vector<int16_t> src;
  double pos = 0.0;
  std::vector<int16_t> mono;
  double cpos = 0.0;
};

static bool WasapiComInit() {
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

static IMMDeviceEnumerator* WasapiEnumerator() {
  IMMDeviceEnumerator* e = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&e)))) {
    return nullptr;
  }
  return e;
}

static std::string WasapiFriendlyName(IMMDevice* device) {
  IPropertyStore* props = nullptr;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props) {
    return std::string();
  }
  PROPVARIANT v;
  PropVariantInit(&v);
  std::string out;
  if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) &&
      v.vt == VT_LPWSTR && v.pwszVal) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr, 0,
                                      nullptr, nullptr);
    if (n > 1) {
      out.resize(static_cast<size_t>(n) - 1);
      WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, &out[0], n, nullptr,
                          nullptr);
    }
  }
  PropVariantClear(&v);
  props->Release();
  return out;
}

static std::vector<std::string> WasapiEnumerate(bool capture) {
  std::vector<std::string> names;
  if (!WasapiComInit()) {
    return names;
  }
  IMMDeviceEnumerator* e = WasapiEnumerator();
  if (!e) {
    return names;
  }
  IMMDeviceCollection* col = nullptr;
  if (SUCCEEDED(e->EnumAudioEndpoints(capture ? eCapture : eRender,
                                      DEVICE_STATE_ACTIVE, &col)) &&
      col) {
    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
      IMMDevice* dev = nullptr;
      if (SUCCEEDED(col->Item(i, &dev)) && dev) {
        std::string n = WasapiFriendlyName(dev);
        if (!n.empty()) {
          names.push_back(n);
        }
        dev->Release();
      }
    }
    col->Release();
  }
  e->Release();
  return names;
}

static IMMDevice* WasapiFindDevice(IMMDeviceEnumerator* e, bool capture,
                                   const std::string& name) {
  const EDataFlow flow = capture ? eCapture : eRender;
  if (!name.empty()) {
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) &&
        col) {
      UINT count = 0;
      col->GetCount(&count);
      for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(col->Item(i, &dev)) && dev) {
          if (WasapiFriendlyName(dev) == name) {
            col->Release();
            return dev;
          }
          dev->Release();
        }
      }
      col->Release();
    }
    XELOGE("VoiceChat: {} '{}' not present, using system default",
           capture ? "mic" : "output", name);
    for (const auto& n : WasapiEnumerate(capture)) {
      XELOGE("VoiceChat: available {}: '{}'", capture ? "mic" : "output", n);
    }
  }
  IMMDevice* dev = nullptr;
  if (FAILED(e->GetDefaultAudioEndpoint(flow, eCommunications, &dev))) {
    if (FAILED(e->GetDefaultAudioEndpoint(flow, eConsole, &dev))) {
      return nullptr;
    }
  }
  return dev;
}

static void WasapiDescribeFormat(VoiceWasapiStream* s, const WAVEFORMATEX* f) {
  s->dev_rate = f->nSamplesPerSec;
  s->dev_channels = f->nChannels;
  s->dev_bits = f->wBitsPerSample;
  s->dev_float = false;
  if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    s->dev_float = true;
  } else if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
    s->dev_float =
        IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
  }
}

static void WasapiWriteFrame(VoiceWasapiStream* s, BYTE* dst, int16_t v) {
  if (s->dev_float) {
    float f = static_cast<float>(v) / 32768.0f;
    auto* o = reinterpret_cast<float*>(dst);
    for (UINT32 c = 0; c < s->dev_channels; ++c) {
      o[c] = f;
    }
  } else if (s->dev_bits == 16) {
    auto* o = reinterpret_cast<int16_t*>(dst);
    for (UINT32 c = 0; c < s->dev_channels; ++c) {
      o[c] = v;
    }
  } else if (s->dev_bits == 32) {
    auto* o = reinterpret_cast<int32_t*>(dst);
    for (UINT32 c = 0; c < s->dev_channels; ++c) {
      o[c] = static_cast<int32_t>(v) << 16;
    }
  }
}

static int16_t WasapiReadFrame(VoiceWasapiStream* s, const BYTE* src) {
  double acc = 0.0;
  if (s->dev_float) {
    const auto* i = reinterpret_cast<const float*>(src);
    for (UINT32 c = 0; c < s->dev_channels; ++c) {
      acc += i[c];
    }
    acc = acc * 32768.0 / s->dev_channels;
  } else if (s->dev_bits == 16) {
    const auto* i = reinterpret_cast<const int16_t*>(src);
    for (UINT32 c = 0; c < s->dev_channels; ++c) {
      acc += i[c];
    }
    acc /= s->dev_channels;
  } else if (s->dev_bits == 32) {
    const auto* i = reinterpret_cast<const int32_t*>(src);
    for (UINT32 c = 0; c < s->dev_channels; ++c) {
      acc += static_cast<double>(i[c]) / 65536.0;
    }
    acc /= s->dev_channels;
  }
  if (acc > 32767.0) {
    acc = 32767.0;
  } else if (acc < -32768.0) {
    acc = -32768.0;
  }
  return static_cast<int16_t>(acc);
}

static void WasapiCaptureThread(VoiceWasapiStream* s) {
  WasapiComInit();
  const double step = static_cast<double>(s->dev_rate) / kSampleRate;
  std::vector<int16_t> out;
  while (s->run.load(std::memory_order_relaxed)) {
    if (WaitForSingleObject(s->event, 200) != WAIT_OBJECT_0) {
      continue;
    }
    UINT32 packet = 0;
    while (SUCCEEDED(s->capture->GetNextPacketSize(&packet)) && packet) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(
              s->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
        break;
      }
      if (frames) {
        const size_t base = s->mono.size();
        s->mono.resize(base + frames);
        const UINT32 stride = s->dev_channels * (s->dev_bits / 8);
        for (UINT32 i = 0; i < frames; ++i) {
          s->mono[base + i] = (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                                  ? 0
                                  : WasapiReadFrame(s, data + i * stride);
        }
      }
      s->capture->ReleaseBuffer(frames);
      packet = 0;
    }
    out.clear();
    while (s->cpos + 1.0 < static_cast<double>(s->mono.size())) {
      const size_t idx = static_cast<size_t>(s->cpos);
      const double f = s->cpos - static_cast<double>(idx);
      const double a = s->mono[idx];
      const double b = s->mono[idx + 1];
      out.push_back(static_cast<int16_t>(a + (b - a) * f));
      s->cpos += step;
    }
    if (!out.empty()) {
      s->owner->OnCapture(out.data(), out.size());
    }
    const size_t consumed = static_cast<size_t>(s->cpos);
    if (consumed) {
      s->mono.erase(s->mono.begin(), s->mono.begin() + consumed);
      s->cpos -= static_cast<double>(consumed);
    }
    if (s->mono.size() > static_cast<size_t>(s->dev_rate)) {
      s->mono.clear();
      s->cpos = 0.0;
    }
  }
  CoUninitialize();
}

static void WasapiRenderThread(VoiceWasapiStream* s) {
  WasapiComInit();
  const double step = static_cast<double>(kSampleRate) / s->dev_rate;
  while (s->run.load(std::memory_order_relaxed)) {
    if (WaitForSingleObject(s->event, 200) != WAIT_OBJECT_0) {
      continue;
    }
    UINT32 padding = 0;
    if (FAILED(s->client->GetCurrentPadding(&padding))) {
      continue;
    }
    const UINT32 frames =
        s->buffer_frames > padding ? (s->buffer_frames - padding) : 0;
    if (!frames) {
      continue;
    }
    const size_t need =
        static_cast<size_t>(s->pos + frames * step) + 2;
    if (s->src.size() < need) {
      const size_t base = s->src.size();
      s->src.resize(need);
      s->owner->FillPlayback(s->src.data() + base, need - base);
    }
    BYTE* data = nullptr;
    if (FAILED(s->render->GetBuffer(frames, &data))) {
      continue;
    }
    const UINT32 stride = s->dev_channels * (s->dev_bits / 8);
    for (UINT32 i = 0; i < frames; ++i) {
      const size_t idx = static_cast<size_t>(s->pos);
      const double f = s->pos - static_cast<double>(idx);
      const double a = idx < s->src.size() ? s->src[idx] : 0;
      const double b = (idx + 1) < s->src.size() ? s->src[idx + 1] : a;
      WasapiWriteFrame(s, data + i * stride,
                       static_cast<int16_t>(a + (b - a) * f));
      s->pos += step;
    }
    s->render->ReleaseBuffer(frames, 0);
    const size_t consumed = static_cast<size_t>(s->pos);
    if (consumed && consumed <= s->src.size()) {
      s->src.erase(s->src.begin(), s->src.begin() + consumed);
      s->pos -= static_cast<double>(consumed);
    }
  }
  CoUninitialize();
}

static void WasapiClose(VoiceWasapiStream** slot) {
  VoiceWasapiStream* s = *slot;
  if (!s) {
    return;
  }
  s->run.store(false);
  if (s->event) {
    SetEvent(s->event);
  }
  if (s->thread.joinable()) {
    s->thread.join();
  }
  if (s->client) {
    s->client->Stop();
  }
  if (s->capture) {
    s->capture->Release();
  }
  if (s->render) {
    s->render->Release();
  }
  if (s->client) {
    s->client->Release();
  }
  if (s->event) {
    CloseHandle(s->event);
  }
  delete s;
  *slot = nullptr;
}

static bool WasapiOpen(VoiceWasapiStream** slot, bool capture,
                       const std::string& name, VoiceChat* owner) {
  WasapiClose(slot);
  if (!WasapiComInit()) {
    XELOGE("VoiceChat: COM init failed");
    return false;
  }
  IMMDeviceEnumerator* e = WasapiEnumerator();
  if (!e) {
    XELOGE("VoiceChat: no device enumerator");
    return false;
  }
  IMMDevice* device = WasapiFindDevice(e, capture, name);
  e->Release();
  if (!device) {
    XELOGE("VoiceChat: no {} endpoint", capture ? "mic" : "output");
    return false;
  }

  auto* s = new VoiceWasapiStream();
  s->owner = owner;

  HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&s->client));
  device->Release();
  if (FAILED(hr) || !s->client) {
    XELOGE("VoiceChat: {} activate failed: {:08X}", capture ? "mic" : "output",
           static_cast<uint32_t>(hr));
    delete s;
    return false;
  }

  WAVEFORMATEX* mix = nullptr;
  hr = s->client->GetMixFormat(&mix);
  if (FAILED(hr) || !mix) {
    XELOGE("VoiceChat: {} GetMixFormat failed: {:08X}",
           capture ? "mic" : "output", static_cast<uint32_t>(hr));
    s->client->Release();
    delete s;
    return false;
  }
  WasapiDescribeFormat(s, mix);

  const REFERENCE_TIME duration = 300000;
  hr = s->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, 0,
                             mix, nullptr);
  CoTaskMemFree(mix);
  if (FAILED(hr)) {
    XELOGE("VoiceChat: {} initialize failed: {:08X}",
           capture ? "mic" : "output", static_cast<uint32_t>(hr));
    s->client->Release();
    delete s;
    return false;
  }

  XELOGD("VoiceChat: {} endpoint {} Hz {} ch {} bit {}",
         capture ? "mic" : "output", s->dev_rate, s->dev_channels, s->dev_bits,
         s->dev_float ? "float" : "int");

  s->client->GetBufferSize(&s->buffer_frames);
  s->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!s->event || FAILED(s->client->SetEventHandle(s->event))) {
    XELOGE("VoiceChat: {} event setup failed", capture ? "mic" : "output");
    if (s->event) {
      CloseHandle(s->event);
    }
    s->client->Release();
    delete s;
    return false;
  }

  if (capture) {
    hr = s->client->GetService(__uuidof(IAudioCaptureClient),
                               reinterpret_cast<void**>(&s->capture));
  } else {
    hr = s->client->GetService(__uuidof(IAudioRenderClient),
                               reinterpret_cast<void**>(&s->render));
  }
  if (FAILED(hr)) {
    XELOGE("VoiceChat: {} GetService failed: {:08X}",
           capture ? "mic" : "output", static_cast<uint32_t>(hr));
    CloseHandle(s->event);
    s->client->Release();
    delete s;
    return false;
  }

  if (FAILED(s->client->Start())) {
    XELOGE("VoiceChat: {} start failed", capture ? "mic" : "output");
    WasapiClose(&s);
    return false;
  }

  s->run.store(true);
  s->thread = std::thread(capture ? WasapiCaptureThread : WasapiRenderThread, s);
  *slot = s;
  return true;
}
#endif

VoiceChat& VoiceChat::Get() {
  static VoiceChat instance;
  return instance;
}

VoiceChat::~VoiceChat() { Stop(); }

std::vector<std::string> VoiceChat::EnumerateMics() {
#if XE_PLATFORM_WIN32
  return WasapiEnumerate(true);
#else
  return Enumerate(1);
#endif
}
std::vector<std::string> VoiceChat::EnumerateOutputs() {
#if XE_PLATFORM_WIN32
  return WasapiEnumerate(false);
#else
  return Enumerate(0);
#endif
}

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
#if XE_PLATFORM_WIN32
  WasapiOpen(&wasapi_capture_, true, mic_name_, this);
  return;
#else
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
  if (!capture_device_ && dev) {
    XELOGE("VoiceChat: mic '{}' open failed ({}), using system default",
           mic_name_, SDL_GetError());
    for (const auto& n : Enumerate(1)) {
      XELOGE("VoiceChat: available mic: '{}'", n);
    }
    capture_device_ = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
  }
  if (!capture_device_) {
    XELOGE("VoiceChat: open mic failed: {}", SDL_GetError());
    return;
  }
  SDL_PauseAudioDevice(capture_device_, 0);
#endif
}
void VoiceChat::OpenPlaybackLocked() {
#if XE_PLATFORM_WIN32
  WasapiOpen(&wasapi_playback_, false, output_name_, this);
  return;
#else
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
  if (!playback_device_ && dev) {
    XELOGE("VoiceChat: output '{}' open failed ({}), using system default",
           output_name_, SDL_GetError());
    for (const auto& n : Enumerate(0)) {
      XELOGE("VoiceChat: available output: '{}'", n);
    }
    playback_device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  }
  if (!playback_device_) {
    XELOGE("VoiceChat: open output failed: {}", SDL_GetError());
    return;
  }
  SDL_PauseAudioDevice(playback_device_, 0);
#endif
}
bool VoiceChat::Start() {
  if (running_) {
    return true;
  }
#if !XE_PLATFORM_WIN32
  if (!EnsureAudioInit()) {
    XELOGE("VoiceChat: audio init failed");
    return false;
  }
#endif
  OpenPlaybackLocked();
  OpenCaptureLocked();
  running_ = true;
#if XE_PLATFORM_WIN32
  XELOGD("VoiceChat: started (mic={}, output={})", wasapi_capture_ != nullptr,
         wasapi_playback_ != nullptr);
#else
  XELOGD("VoiceChat: started (mic={}, output={})", capture_device_ != 0,
         playback_device_ != 0);
#endif
  return true;
}

void VoiceChat::Stop() {
  if (!running_) {
    return;
  }
#if XE_PLATFORM_WIN32
  WasapiClose(&wasapi_capture_);
  WasapiClose(&wasapi_playback_);
#else
  if (capture_device_) {
    SDL_CloseAudioDevice(capture_device_);
    capture_device_ = 0;
  }
  if (playback_device_) {
    SDL_CloseAudioDevice(playback_device_);
    playback_device_ = 0;
  }
#endif
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
