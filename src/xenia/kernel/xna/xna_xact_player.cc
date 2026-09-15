/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_xact_player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_system.h"
#include "xenia/apu/xma_context.h"
#include "xenia/apu/xma_decoder.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/base/xxhash.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/memory.h"

extern "C" {
#if XE_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "third_party/FFmpeg/libavcodec/avcodec.h"
#include "third_party/FFmpeg/libavformat/avformat.h"
#include "third_party/FFmpeg/libavutil/channel_layout.h"
#include "third_party/FFmpeg/libavutil/error.h"
#include "third_party/FFmpeg/libavutil/mem.h"
#if XE_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

// Defined in audio_system.cc and not declared in any header; the queue depth
// the guest is held to is the one a hosted title should respect too.
DECLARE_uint32(apu_max_queued_frames);

namespace xe {
namespace kernel {
namespace xna {

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 2;
// A wave bank block. The value the real bank uses, and what the extradata has
// to agree with for the decoder to find its frames.
constexpr uint32_t kBytesPerBlock = 2048;

struct Voice {
  uint64_t owner = 0;
  uint32_t category = UINT32_MAX;
  XnaSamples samples;
  std::deque<XnaSamples> queue;
  bool streaming = false;
  bool finished = false;
  bool paused = false;
  bool loop = false;
  uint32_t loop_begin = 0;
  uint32_t loop_length = 0;
  double cursor = 0.0;
  double base_step = 1.0;
  double seconds = 0.0;
  float volume = 1.0f;
  float pan = 0.0f;
  float pitch = 0.0f;
};

struct PendingStart {
  uint64_t generation = 0;
  bool detached = false;
  XnaVoiceParams params;
};

std::mutex player_mutex;
std::vector<Voice> voices;
std::unordered_map<uint64_t, PendingStart> pending_starts;
std::unordered_map<uint32_t, float> category_volumes;
std::unordered_set<uint32_t> paused_categories;
uint64_t next_generation = 1;
float master_gain = 1.0f;

constexpr size_t kDecodedCacheBytes = size_t(256) << 20;
std::unordered_map<uint64_t, XnaSamples> decoded_cache;
std::deque<uint64_t> decoded_order;
size_t decoded_cache_bytes = 0;

std::mutex driver_mutex;
std::unique_ptr<xe::threading::Semaphore> driver_semaphore;
std::unique_ptr<apu::AudioDriver> driver;
std::unique_ptr<xe::threading::Thread> mixer_thread;
bool mixer_running = false;

// Decoding through Nexia's OWN XMA decoder, not a codec of this file's
// choosing.
//
// The APU already owns this: XmaDecoder hands out a context, the context data
// lives in guest memory, and a register write starts it. Every implementation
// behind the xma_decoder cvar - new, old, master and fake - is constructed the
// same way and driven through that same structure, so routing through it works
// for all of them. Reaching into XmaContextNew directly would work for exactly
// one.
//
// The decoder writes INTERLEAVED BIG-ENDIAN int16 into the output buffer, which
// is what the guest expects - ConvertFrame byte-swaps each sample on its way
// out - so reading it back on the host means swapping it again.
bool DecodeWithXmaDecoder(const uint8_t* data, uint32_t size, bool is_stereo,
                          uint32_t sample_rate, std::vector<float>* out) {
  auto* state = kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  auto* audio = emulator ? emulator->audio_system() : nullptr;
  auto* xma = audio ? audio->xma_decoder() : nullptr;
  auto* memory = state ? state->memory() : nullptr;
  if (!xma || !memory) {
    XELOGE("[xna] XACT: the XMA decoder is not available");
    return false;
  }

  // Input is whole 2KB packets; a tail shorter than a packet is not one.
  const uint32_t packet_count = size / kBytesPerBlock;
  if (!packet_count) {
    return false;
  }
  const uint32_t input_bytes = packet_count * kBytesPerBlock;

  // The output buffer is a ring of 256-byte blocks and the count is a 5-bit
  // field, so 31 is the most it can describe.
  constexpr uint32_t kOutputBlocks = 31;
  constexpr uint32_t kBytesPerOutputBlock = 256;
  const uint32_t output_bytes = kOutputBlocks * kBytesPerOutputBlock;

  const uint32_t input_guest =
      memory->SystemHeapAlloc(input_bytes, kBytesPerBlock, kSystemHeapPhysical);
  const uint32_t output_guest = memory->SystemHeapAlloc(
      output_bytes, kBytesPerOutputBlock, kSystemHeapPhysical);
  if (!input_guest || !output_guest) {
    XELOGE("[xna] XACT: could not reserve guest memory for a decode");
    return false;
  }
  std::memcpy(memory->TranslateVirtual<uint8_t*>(input_guest), data,
              input_bytes);
  std::memset(memory->TranslateVirtual<uint8_t*>(output_guest), 0,
              output_bytes);

  const uint32_t context_ptr = xma->AllocateContext();
  if (!context_ptr) {
    memory->SystemHeapFree(input_guest);
    memory->SystemHeapFree(output_guest);
    XELOGE("[xna] XACT: no free XMA context");
    return false;
  }

  // Register indices, needed before the context is configured because the
  // CLEAR has to happen first.
  const uint32_t physical = memory->GetPhysicalAddress(context_ptr);
  const uint32_t index =
      (physical - xma->context_array_ptr()) / sizeof(apu::XMA_CONTEXT_DATA);
  const uint32_t register_offset = (index >> 5) * 4;
  const uint32_t register_bit = xe::byte_swap(uint32_t(1) << (index & 0x1F));

  // CLEAR BEFORE CONFIGURING, NOT AFTER.
  //
  // ClearLocked zeroes input_buffer_0_valid and output_buffer_valid and resets
  // the read offset to the packet header - so clearing after setting them up
  // threw both flags away, and Work() then returned at its first guard without
  // decoding or logging anything. The kernel survives this order only because
  // the guest calls XMASetInputBuffer0Valid and XMAEnableContext afterwards;
  // collapsing those steps loses the ordering.
  std::memset(memory->TranslateVirtual<uint8_t*>(context_ptr), 0,
              sizeof(apu::XMA_CONTEXT_DATA));
  xma->WriteRegister(0x1A80 + register_offset, register_bit);

  {
    // Built from the context's own memory rather than default-constructed:
    // XMA_CONTEXT_DATA declares an explicit constructor and so has no default
    // one. Reading it back also keeps what the clear just set - notably the
    // read offset, which starts at the packet header rather than at zero.
    apu::XMA_CONTEXT_DATA context(
        memory->TranslateVirtual<uint8_t*>(context_ptr));
    context.input_buffer_0_ptr = memory->GetPhysicalAddress(input_guest);
    context.input_buffer_0_packet_count = packet_count;
    context.input_buffer_0_valid = 1;
    context.output_buffer_ptr = memory->GetPhysicalAddress(output_guest);
    context.output_buffer_block_count = kOutputBlocks;
    context.sample_rate = sample_rate <= 24000   ? 0
                          : sample_rate <= 32000 ? 1
                          : sample_rate <= 44100 ? 2
                                                 : 3;
    context.is_stereo = is_stereo ? 1 : 0;
    context.subframe_decode_count = 4;
    context.stop_when_done = 1;
    // THE DECODER WILL NOT WRITE WITHOUT THIS. It is how the guest says the
    // output buffer has room: XmaContext bails out at the top of its work when
    // output_buffer_valid is clear, which is why an otherwise correct context
    // decoded nothing at all.
    context.output_buffer_valid = 1;
    context.Store(memory->TranslateVirtual<uint8_t*>(context_ptr));
  }

  // Driving it the way the kernel does: the CLEAR above reset the context,
  // and each KICK runs one pass. A kick is not a request - WriteRegister
  // blocks until the worker has finished and the context data is updated - so
  // the decode is a loop of kicks rather than one kick and a wait.

  XELOGI("[xna] XACT: decoding {} packet(s) on XMA context {} ({})",
         packet_count, index, is_stereo ? "stereo" : "mono");

  uint32_t read_block = 0;
  uint32_t passes_run = 0;
  const uint32_t max_passes = packet_count * 64 + 64;
  uint64_t last_state = UINT64_MAX;
  for (uint32_t pass = 0; pass < max_passes; ++pass) {
    passes_run = pass + 1;
    xma->WriteRegister(0x1940 + register_offset, register_bit);

    apu::XMA_CONTEXT_DATA context(
        memory->TranslateVirtual<uint8_t*>(context_ptr));

    const uint64_t state =
        (uint64_t(context.input_buffer_0_valid) << 0) |
        (uint64_t(context.input_buffer_1_valid) << 1) |
        (uint64_t(context.output_buffer_valid) << 2) |
        (uint64_t(context.current_buffer) << 3) |
        (uint64_t(context.error_set) << 4) |
        (uint64_t(context.error_status) << 5) |
        (uint64_t(context.parser_error_set) << 10) |
        (uint64_t(context.parser_error_status) << 11) |
        (uint64_t(context.output_buffer_write_offset) << 16) |
        (uint64_t(context.output_buffer_read_offset) << 21) |
        (uint64_t(context.input_buffer_read_offset) << 26);
    if (state != last_state) {
      last_state = state;
      XELOGI(
          "[xna] XMA pass {}: in0={} in1={} out={} buf={} err={}/{} "
          "parser={}/{} write={} read={} inoff={}",
          passes_run, uint32_t(context.input_buffer_0_valid),
          uint32_t(context.input_buffer_1_valid),
          uint32_t(context.output_buffer_valid),
          uint32_t(context.current_buffer), uint32_t(context.error_set),
          uint32_t(context.error_status), uint32_t(context.parser_error_set),
          uint32_t(context.parser_error_status),
          uint32_t(context.output_buffer_write_offset),
          uint32_t(context.output_buffer_read_offset),
          uint32_t(context.input_buffer_read_offset));
    }

    const uint32_t write_block = context.output_buffer_write_offset;
    if (write_block != read_block) {
      const auto* blocks =
          memory->TranslateVirtual<const uint8_t*>(output_guest);
      while (read_block != write_block) {
        const auto* samples = reinterpret_cast<const uint16_t*>(
            blocks + read_block * kBytesPerOutputBlock);
        for (uint32_t i = 0; i < kBytesPerOutputBlock / sizeof(uint16_t); ++i) {
          const int16_t sample =
              static_cast<int16_t>(xe::byte_swap(samples[i]));
          out->push_back(sample / 32768.0f);
        }
        read_block = (read_block + 1) % kOutputBlocks;
      }
    } else if (!context.input_buffer_0_valid) {
      // Nothing new came out and there is no input left to make more.
      break;
    }
    // Tell it how far behind we are and that there is room again.
    context.output_buffer_read_offset = read_block;
    context.output_buffer_valid = 1;
    context.Store(memory->TranslateVirtual<uint8_t*>(context_ptr));
  }

  xma->ReleaseContext(context_ptr);
  memory->SystemHeapFree(input_guest);
  memory->SystemHeapFree(output_guest);
  if (out->empty()) {
    XELOGW(
        "[xna] XACT: the XMA decoder produced nothing for {} packet(s) after "
        "{} pass(es)",
        packet_count, passes_run);
  }
  return !out->empty();
}

void ExpandMono(std::vector<float>* samples) {
  std::vector<float> stereo;
  stereo.reserve(samples->size() * 2);
  for (float sample : *samples) {
    stereo.push_back(sample);
    stereo.push_back(sample);
  }
  samples->swap(stereo);
}

bool DecodePcm(const XnaAudioFormat& format, const uint8_t* data, uint32_t size,
               std::vector<float>* out) {
  const uint32_t channels = std::max<uint32_t>(format.channels, 1);
  const uint32_t bytes = format.bits_per_sample == 8 ? 1 : 2;
  const uint32_t frame_bytes = bytes * channels;
  const uint32_t frames = size / frame_bytes;
  const auto read = [&](const uint8_t* at, uint32_t channel) -> float {
    const uint8_t* sample = at + channel * bytes;
    if (bytes == 1) {
      return (float(sample[0]) - 128.0f) / 128.0f;
    }
    const uint16_t raw = format.big_endian
                             ? uint16_t((sample[0] << 8) | sample[1])
                             : uint16_t(sample[0] | (sample[1] << 8));
    return float(int16_t(raw)) / 32768.0f;
  };
  out->reserve(size_t(frames) * kChannels);
  for (uint32_t i = 0; i < frames; ++i) {
    const uint8_t* at = data + size_t(i) * frame_bytes;
    const float left = read(at, 0);
    out->push_back(left);
    out->push_back(channels > 1 ? read(at, 1) : left);
  }
  return !out->empty();
}

void AppendFrame(const AVFrame* frame, std::vector<float>* out) {
  const int channels = frame->ch_layout.nb_channels;
  const int count = frame->nb_samples;
  if (channels <= 0 || count <= 0) {
    return;
  }
  const auto format = static_cast<AVSampleFormat>(frame->format);
  const auto sample = [&](int channel, int index) -> float {
    const int c = std::min(channel, channels - 1);
    switch (format) {
      case AV_SAMPLE_FMT_FLTP:
        return reinterpret_cast<const float*>(frame->extended_data[c])[index];
      case AV_SAMPLE_FMT_FLT:
        return reinterpret_cast<const float*>(
            frame->extended_data[0])[index * channels + c];
      case AV_SAMPLE_FMT_S16P:
        return reinterpret_cast<const int16_t*>(
                   frame->extended_data[c])[index] /
               32768.0f;
      case AV_SAMPLE_FMT_S16:
        return reinterpret_cast<const int16_t*>(
                   frame->extended_data[0])[index * channels + c] /
               32768.0f;
      case AV_SAMPLE_FMT_S32P:
        return reinterpret_cast<const int32_t*>(
                   frame->extended_data[c])[index] /
               2147483648.0f;
      case AV_SAMPLE_FMT_S32:
        return reinterpret_cast<const int32_t*>(
                   frame->extended_data[0])[index * channels + c] /
               2147483648.0f;
      default:
        return 0.0f;
    }
  };
  out->reserve(out->size() + size_t(count) * kChannels);
  for (int i = 0; i < count; ++i) {
    out->push_back(sample(0, i));
    out->push_back(sample(1, i));
  }
}

void DrainDecoder(AVCodecContext* context, AVFrame* frame,
                  std::vector<float>* out) {
  while (avcodec_receive_frame(context, frame) == 0) {
    AppendFrame(frame, out);
    av_frame_unref(frame);
  }
}

bool DecodeWma(const XnaAudioFormat& format, const uint8_t* data, uint32_t size,
               std::vector<float>* out) {
  if (!format.block_align || !format.channels || !format.sample_rate) {
    return false;
  }
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_WMAV2);
  if (!codec) {
    XELOGE("[xna] audio: FFmpeg has no WMA v2 decoder");
    return false;
  }
  AVCodecContext* context = avcodec_alloc_context3(codec);
  if (!context) {
    return false;
  }
  context->sample_rate = int(format.sample_rate);
  av_channel_layout_default(&context->ch_layout, int(format.channels));
  context->block_align = int(format.block_align);
  context->bit_rate = int64_t(format.avg_bytes_per_second) * 8;
  context->extradata =
      static_cast<uint8_t*>(av_mallocz(6 + AV_INPUT_BUFFER_PADDING_SIZE));
  if (context->extradata) {
    context->extradata_size = 6;
    context->extradata[4] = 31;
  }
  if (avcodec_open2(context, codec, nullptr) < 0) {
    XELOGE("[xna] audio: could not open the WMA v2 decoder");
    avcodec_free_context(&context);
    return false;
  }
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  std::vector<uint8_t> padded(format.block_align + AV_INPUT_BUFFER_PADDING_SIZE,
                              0);
  uint32_t rejected = 0;
  for (uint32_t at = 0; packet && frame && at + format.block_align <= size;
       at += format.block_align) {
    std::memcpy(padded.data(), data + at, format.block_align);
    packet->data = padded.data();
    packet->size = int(format.block_align);
    if (avcodec_send_packet(context, packet) < 0) {
      ++rejected;
      continue;
    }
    DrainDecoder(context, frame, out);
  }
  if (packet && frame) {
    avcodec_send_packet(context, nullptr);
    DrainDecoder(context, frame, out);
  }
  if (rejected) {
    XELOGW("[xna] audio: WMA decoder rejected {} of {} packet(s)", rejected,
           size / format.block_align);
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&context);
  return !out->empty();
}

struct MemoryInput {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t position = 0;
};

int ReadMemory(void* opaque, uint8_t* buffer, int size) {
  auto* input = static_cast<MemoryInput*>(opaque);
  const size_t left = input->size - input->position;
  const size_t count = std::min(left, size_t(std::max(size, 0)));
  if (!count) {
    return AVERROR_EOF;
  }
  std::memcpy(buffer, input->data + input->position, count);
  input->position += count;
  return int(count);
}

int64_t SeekMemory(void* opaque, int64_t offset, int whence) {
  auto* input = static_cast<MemoryInput*>(opaque);
  whence &= ~AVSEEK_FORCE;
  if (whence == AVSEEK_SIZE) {
    return int64_t(input->size);
  }
  int64_t target = -1;
  if (whence == SEEK_SET) {
    target = offset;
  } else if (whence == SEEK_CUR) {
    target = int64_t(input->position) + offset;
  } else if (whence == SEEK_END) {
    target = int64_t(input->size) + offset;
  }
  if (target < 0 || target > int64_t(input->size)) {
    return -1;
  }
  input->position = size_t(target);
  return target;
}

float CategoryGain(uint32_t category) {
  auto found = category_volumes.find(category);
  return found != category_volumes.end() ? found->second : 1.0f;
}

void MixVoice(Voice& voice, float* frame, size_t frames_per_submit,
              size_t driver_channels) {
  if (voice.paused || voice.finished ||
      paused_categories.count(voice.category)) {
    return;
  }
  const float gain = voice.volume * master_gain * CategoryGain(voice.category);
  const float left_gain = gain * (voice.pan > 0.0f ? 1.0f - voice.pan : 1.0f);
  const float right_gain = gain * (voice.pan < 0.0f ? 1.0f + voice.pan : 1.0f);
  const double step = voice.base_step * std::exp2(double(voice.pitch));
  size_t f = 0;
  while (f < frames_per_submit) {
    const size_t frames = voice.samples ? voice.samples->size() / kChannels : 0;
    if (!frames || (!voice.loop && size_t(voice.cursor) >= frames)) {
      if (voice.streaming) {
        if (frames) {
          voice.cursor = std::max(0.0, voice.cursor - double(frames));
        }
        voice.samples.reset();
        if (voice.queue.empty()) {
          return;
        }
        voice.samples = voice.queue.front();
        voice.queue.pop_front();
        continue;
      }
      voice.finished = true;
      return;
    }
    if (voice.loop) {
      const bool region =
          voice.loop_length &&
          size_t(voice.loop_begin) + voice.loop_length <= frames;
      const double begin = region ? double(voice.loop_begin) : 0.0;
      const double end = region ? double(voice.loop_begin) + voice.loop_length
                                : double(frames);
      if (voice.cursor >= end && end > begin) {
        voice.cursor = begin + std::fmod(voice.cursor - begin, end - begin);
      }
    }
    const size_t index = std::min(size_t(voice.cursor), frames - 1);
    const float t = static_cast<float>(voice.cursor - double(index));
    const float* a = voice.samples->data() + index * kChannels;
    const float* b = index + 1 < frames ? a + kChannels : a;
    frame[f * driver_channels + 0] += (a[0] + (b[0] - a[0]) * t) * left_gain;
    frame[f * driver_channels + 1] += (a[1] + (b[1] - a[1]) * t) * right_gain;
    voice.cursor += step;
    voice.seconds += 1.0 / double(kSampleRate);
    ++f;
  }
}

void MixerMain() {
  constexpr size_t kDriverChannels = apu::AudioDriver::kFrameChannelsDefault;
  constexpr size_t kFramesPerSubmit = apu::AudioDriver::kChannelSamplesDefault;
  std::vector<float> frame(apu::AudioDriver::kFrameSamplesMax, 0.0f);

  while (mixer_running) {
    bool idle = false;
    {
      std::lock_guard<std::mutex> lock(player_mutex);
      idle = voices.empty();
    }
    if (idle) {
      xe::threading::Sleep(std::chrono::milliseconds(5));
      continue;
    }

    xe::threading::Wait(driver_semaphore.get(), false);
    std::fill(frame.begin(), frame.end(), 0.0f);

    {
      std::lock_guard<std::mutex> lock(player_mutex);
      for (auto& voice : voices) {
        MixVoice(voice, frame.data(), kFramesPerSubmit, kDriverChannels);
      }
      voices.erase(std::remove_if(voices.begin(), voices.end(),
                                  [](const Voice& v) { return v.finished; }),
                   voices.end());
    }

    if (driver) {
      driver->SubmitFrame(frame.data());
    }
  }
}

bool EnsureDriver() {
  std::lock_guard<std::mutex> lock(driver_mutex);
  if (driver) {
    return true;
  }
  auto* state = kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  auto* audio = emulator ? emulator->audio_system() : nullptr;
  if (!audio) {
    return false;
  }
  // THE SEMAPHORE'S COUNT IS THE DRIVER'S QUEUE DEPTH, NOT ITS CEILING.
  //
  // Starting it at kMaximumQueuedFrames (64) let the mixer submit 64 buffers
  // before the driver had retired a single one. XAudio2's ring is exactly 64
  // frames deep, so the 65th submission overwrites a buffer that is still
  // queued for playback, and its own BuffersQueued assert is the boundary being
  // crossed. The audio system clamps the guest to apu_max_queued_frames for
  // this reason; the same clamp applies here.
  const uint32_t queued_frames =
      std::clamp(cvars::apu_max_queued_frames,
                 static_cast<uint32_t>(apu::AudioSystem::kMinimumQueuedFrames),
                 static_cast<uint32_t>(apu::AudioSystem::kMaximumQueuedFrames));
  driver_semaphore =
      xe::threading::Semaphore::Create(queued_frames, queued_frames);
  if (!driver_semaphore) {
    return false;
  }
  // The driver mixes to its own channel count; the frames handed to it are the
  // fixed size it asks for.
  driver.reset(audio->CreateDriver(driver_semaphore.get(), kSampleRate,
                                   apu::AudioDriver::kFrameChannelsDefault,
                                   false));
  if (!driver || !driver->Initialize()) {
    driver.reset();
    driver_semaphore.reset();
    return false;
  }
  mixer_running = true;
  mixer_thread = xe::threading::Thread::Create({}, []() { MixerMain(); });
  if (mixer_thread) {
    mixer_thread->set_name("XNA Audio Mixer");
  }
  XELOGI("[xna] audio is live at {} Hz", kSampleRate);
  return true;
}

Voice MakeVoice(uint64_t owner, XnaSamples samples, uint32_t sample_rate,
                const XnaVoiceParams& params) {
  Voice voice;
  voice.owner = owner;
  voice.category = params.category;
  voice.samples = std::move(samples);
  voice.base_step =
      double(sample_rate ? sample_rate : kSampleRate) / double(kSampleRate);
  voice.volume = params.volume;
  voice.pan = std::clamp(params.pan, -1.0f, 1.0f);
  voice.pitch = params.pitch;
  voice.loop = params.loop;
  voice.loop_begin = params.loop_begin;
  voice.loop_length = params.loop_length;
  voice.paused = params.paused;
  return voice;
}

template <typename Change>
void ForOwner(uint64_t owner, Change change) {
  std::lock_guard<std::mutex> lock(player_mutex);
  for (auto& voice : voices) {
    if (voice.owner == owner && !voice.finished) {
      change(voice);
    }
  }
}

template <typename Change>
void ForPending(uint64_t owner, Change change) {
  auto pending = pending_starts.find(owner);
  if (pending != pending_starts.end()) {
    change(pending->second.params);
  }
}

void RemoveOwnerLocked(uint64_t owner) {
  voices.erase(
      std::remove_if(voices.begin(), voices.end(),
                     [owner](const Voice& v) { return v.owner == owner; }),
      voices.end());
}

}  // namespace

XnaSamples XnaDecodeAudio(const XnaAudioFormat& format, const uint8_t* data,
                          uint32_t size, bool cache) {
  if (!data || !size) {
    return nullptr;
  }
  const uint64_t key = cache ? XXH3_64bits(data, size) ^
                                   (uint64_t(format.codec) << 56) ^
                                   (uint64_t(format.sample_rate) << 20) ^
                                   (uint64_t(format.channels) << 8) ^
                                   (uint64_t(format.bits_per_sample) << 1) ^
                                   uint64_t(format.big_endian ? 1 : 0)
                             : 0;
  if (cache) {
    std::lock_guard<std::mutex> lock(player_mutex);
    auto found = decoded_cache.find(key);
    if (found != decoded_cache.end()) {
      return found->second;
    }
  }
  auto decoded = std::make_shared<std::vector<float>>();
  bool ok = false;
  switch (format.codec) {
    case kXnaAudioPcm:
      ok = DecodePcm(format, data, size, decoded.get());
      break;
    case kXnaAudioXma:
      ok = DecodeWithXmaDecoder(data, size, format.channels >= 2,
                                format.sample_rate, decoded.get());
      if (ok && format.channels < 2) {
        ExpandMono(decoded.get());
      }
      break;
    case kXnaAudioWma:
      ok = DecodeWma(format, data, size, decoded.get());
      break;
    default:
      XELOGW("[xna] audio: codec {} is not decoded ({} channel(s), {} Hz)",
             format.codec, format.channels, format.sample_rate);
      break;
  }
  if (!ok || decoded->empty()) {
    return nullptr;
  }
  if (!cache) {
    return decoded;
  }
  std::lock_guard<std::mutex> lock(player_mutex);
  if (decoded_cache.emplace(key, decoded).second) {
    decoded_order.push_back(key);
    decoded_cache_bytes += decoded->size() * sizeof(float);
    while (decoded_cache_bytes > kDecodedCacheBytes &&
           decoded_order.size() > 1) {
      auto oldest = decoded_cache.find(decoded_order.front());
      if (oldest != decoded_cache.end()) {
        decoded_cache_bytes -= oldest->second->size() * sizeof(float);
        decoded_cache.erase(oldest);
      }
      decoded_order.pop_front();
    }
  }
  return decoded;
}

XnaSamples XnaDecodeMediaFile(const std::vector<uint8_t>& file,
                              uint32_t* sample_rate) {
  if (file.empty()) {
    return nullptr;
  }
  MemoryInput input;
  input.data = file.data();
  input.size = file.size();
  constexpr int kIoBufferSize = 32768;
  auto* io_buffer = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
  if (!io_buffer) {
    return nullptr;
  }
  AVIOContext* io = avio_alloc_context(io_buffer, kIoBufferSize, 0, &input,
                                       ReadMemory, nullptr, SeekMemory);
  if (!io) {
    av_free(io_buffer);
    return nullptr;
  }
  AVFormatContext* container = avformat_alloc_context();
  if (!container) {
    av_freep(&io->buffer);
    avio_context_free(&io);
    return nullptr;
  }
  container->pb = io;
  container->flags |= AVFMT_FLAG_CUSTOM_IO;
  auto decoded = std::make_shared<std::vector<float>>();
  AVCodecContext* context = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  if (avformat_open_input(&container, nullptr, nullptr, nullptr) < 0) {
    XELOGW("[xna] audio: FFmpeg could not open a {} byte media file",
           file.size());
    container = nullptr;
  } else if (avformat_find_stream_info(container, nullptr) >= 0) {
    const int stream =
        av_find_best_stream(container, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    const AVCodec* codec =
        stream >= 0 ? avcodec_find_decoder(
                          container->streams[stream]->codecpar->codec_id)
                    : nullptr;
    if (codec) {
      context = avcodec_alloc_context3(codec);
    }
    if (context &&
        avcodec_parameters_to_context(
            context, container->streams[stream]->codecpar) >= 0 &&
        avcodec_open2(context, codec, nullptr) >= 0) {
      packet = av_packet_alloc();
      frame = av_frame_alloc();
      while (packet && frame && av_read_frame(container, packet) >= 0) {
        if (packet->stream_index == stream &&
            avcodec_send_packet(context, packet) >= 0) {
          DrainDecoder(context, frame, decoded.get());
        }
        av_packet_unref(packet);
      }
      if (packet && frame) {
        avcodec_send_packet(context, nullptr);
        DrainDecoder(context, frame, decoded.get());
      }
      if (sample_rate) {
        *sample_rate = uint32_t(context->sample_rate);
      }
    } else {
      XELOGW("[xna] audio: no decoder for the media file's audio stream");
    }
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&context);
  if (container) {
    avformat_close_input(&container);
  }
  av_freep(&io->buffer);
  avio_context_free(&io);
  if (decoded->empty()) {
    return nullptr;
  }
  return decoded;
}

bool XnaVoiceStart(uint64_t owner, XnaSamples samples, uint32_t sample_rate,
                   const XnaVoiceParams& params) {
  if (!samples || samples->empty()) {
    return false;
  }
  if (!EnsureDriver()) {
    XELOGW("[xna] audio could not open an audio driver");
    return false;
  }
  std::lock_guard<std::mutex> lock(player_mutex);
  pending_starts.erase(owner);
  RemoveOwnerLocked(owner);
  voices.push_back(MakeVoice(owner, std::move(samples), sample_rate, params));
  return true;
}

void XnaVoiceStartAsync(uint64_t owner,
                        std::function<XnaSamples(uint32_t*)> decode,
                        const XnaVoiceParams& params) {
  if (!EnsureDriver()) {
    XELOGW("[xna] audio could not open an audio driver");
    return;
  }
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(player_mutex);
    RemoveOwnerLocked(owner);
    generation = next_generation++;
    PendingStart pending;
    pending.generation = generation;
    pending.params = params;
    pending_starts[owner] = pending;
  }
  std::thread([owner, generation, decode = std::move(decode)]() {
    uint32_t sample_rate = 0;
    XnaSamples samples = decode(&sample_rate);
    std::lock_guard<std::mutex> lock(player_mutex);
    auto pending = pending_starts.find(owner);
    if (pending == pending_starts.end() ||
        pending->second.generation != generation) {
      return;
    }
    const PendingStart start = pending->second;
    pending_starts.erase(pending);
    if (!samples || samples->empty()) {
      XELOGW("[xna] audio: voice {:016X} decoded to nothing", owner);
      return;
    }
    voices.push_back(MakeVoice(start.detached ? 0 : owner, std::move(samples),
                               sample_rate, start.params));
    XELOGI("[xna] audio: voice {:016X} started, {:.1f} s at {} Hz", owner,
           double(voices.back().samples->size() / kChannels) /
               double(sample_rate ? sample_rate : kSampleRate),
           sample_rate);
  }).detach();
}

bool XnaVoiceQueue(uint64_t owner, XnaSamples samples, uint32_t sample_rate,
                   const XnaVoiceParams& params) {
  if (!EnsureDriver()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(player_mutex);
  for (auto& voice : voices) {
    if (voice.owner == owner && voice.streaming && !voice.finished) {
      if (samples && !samples->empty()) {
        voice.queue.push_back(std::move(samples));
      }
      return true;
    }
  }
  Voice voice = MakeVoice(owner, nullptr, sample_rate, params);
  voice.streaming = true;
  if (samples && !samples->empty()) {
    voice.queue.push_back(std::move(samples));
  }
  voices.push_back(std::move(voice));
  return true;
}

uint32_t XnaVoicePending(uint64_t owner) {
  std::lock_guard<std::mutex> lock(player_mutex);
  for (const auto& voice : voices) {
    if (voice.owner == owner && !voice.finished) {
      return uint32_t(voice.queue.size()) + (voice.samples ? 1 : 0);
    }
  }
  return 0;
}

void XnaVoiceStop(uint64_t owner) {
  std::lock_guard<std::mutex> lock(player_mutex);
  pending_starts.erase(owner);
  RemoveOwnerLocked(owner);
}

void XnaVoiceDetach(uint64_t owner) {
  std::lock_guard<std::mutex> lock(player_mutex);
  auto pending = pending_starts.find(owner);
  if (pending != pending_starts.end()) {
    pending->second.detached = true;
  }
  for (auto& voice : voices) {
    if (voice.owner == owner) {
      voice.owner = 0;
    }
  }
}

void XnaVoiceSetPaused(uint64_t owner, bool paused) {
  ForOwner(owner, [paused](Voice& voice) { voice.paused = paused; });
  std::lock_guard<std::mutex> lock(player_mutex);
  ForPending(owner,
             [paused](XnaVoiceParams& params) { params.paused = paused; });
}

void XnaVoiceSetVolume(uint64_t owner, float volume) {
  ForOwner(owner, [volume](Voice& voice) { voice.volume = volume; });
  std::lock_guard<std::mutex> lock(player_mutex);
  ForPending(owner,
             [volume](XnaVoiceParams& params) { params.volume = volume; });
}

void XnaVoiceSetPan(uint64_t owner, float pan) {
  const float clamped = std::clamp(pan, -1.0f, 1.0f);
  ForOwner(owner, [clamped](Voice& voice) { voice.pan = clamped; });
  std::lock_guard<std::mutex> lock(player_mutex);
  ForPending(owner,
             [clamped](XnaVoiceParams& params) { params.pan = clamped; });
}

void XnaVoiceSetPitch(uint64_t owner, float pitch) {
  ForOwner(owner, [pitch](Voice& voice) { voice.pitch = pitch; });
  std::lock_guard<std::mutex> lock(player_mutex);
  ForPending(owner, [pitch](XnaVoiceParams& params) { params.pitch = pitch; });
}

void XnaVoiceSetLoop(uint64_t owner, bool loop) {
  ForOwner(owner, [loop](Voice& voice) { voice.loop = loop; });
  std::lock_guard<std::mutex> lock(player_mutex);
  ForPending(owner, [loop](XnaVoiceParams& params) { params.loop = loop; });
}

XnaVoiceState XnaVoiceQuery(uint64_t owner, double* seconds) {
  std::lock_guard<std::mutex> lock(player_mutex);
  for (const auto& voice : voices) {
    if (voice.owner == owner && !voice.finished) {
      if (seconds) {
        *seconds = voice.seconds;
      }
      return voice.paused ? XnaVoiceState::kPaused : XnaVoiceState::kPlaying;
    }
  }
  auto pending = pending_starts.find(owner);
  if (pending != pending_starts.end()) {
    if (seconds) {
      *seconds = 0.0;
    }
    return pending->second.params.paused ? XnaVoiceState::kPaused
                                         : XnaVoiceState::kPlaying;
  }
  return XnaVoiceState::kNone;
}

void XnaVoiceStopCategory(uint32_t category) {
  std::lock_guard<std::mutex> lock(player_mutex);
  const size_t before = voices.size();
  voices.erase(std::remove_if(voices.begin(), voices.end(),
                              [category](const Voice& v) {
                                return v.category == category;
                              }),
               voices.end());
  for (auto it = pending_starts.begin(); it != pending_starts.end();) {
    it = it->second.params.category == category ? pending_starts.erase(it)
                                                : std::next(it);
  }
  if (before != voices.size()) {
    XELOGI("[xna] audio: category {} stop silenced {} voice(s)", category,
           before - voices.size());
  }
}

void XnaVoicePauseCategory(uint32_t category, bool paused) {
  std::lock_guard<std::mutex> lock(player_mutex);
  if (paused) {
    paused_categories.insert(category);
  } else {
    paused_categories.erase(category);
  }
}

void XnaVoiceSetCategoryVolume(uint32_t category, float volume) {
  std::lock_guard<std::mutex> lock(player_mutex);
  category_volumes[category] = volume;
}

void XnaAudioSetMasterVolume(float volume) {
  std::lock_guard<std::mutex> lock(player_mutex);
  master_gain = volume;
}

bool XactPlayWave(uint32_t cue, uint32_t category, const uint8_t* data,
                  uint32_t size, const XnaAudioFormat& format, float volume) {
  if (!data || !size) {
    return false;
  }
  const uint64_t owner = XnaVoiceOwner(kXnaVoiceCue, cue);
  XnaVoiceParams params;
  params.category = category;
  params.volume = volume;
  if (format.codec == kXnaAudioWma) {
    auto bytes = std::make_shared<std::vector<uint8_t>>(data, data + size);
    XnaVoiceStartAsync(
        owner,
        [bytes, format](uint32_t* sample_rate) {
          *sample_rate = format.sample_rate;
          return XnaDecodeAudio(format, bytes->data(), uint32_t(bytes->size()));
        },
        params);
    XELOGI("[xna] XACT cue {} decoding {} byte(s) of WMA in the background",
           cue, size);
    return true;
  }
  XnaSamples samples = XnaDecodeAudio(format, data, size);
  if (!samples) {
    XELOGW("[xna] XACT cue {} produced no samples and will not play", cue);
    return false;
  }
  if (!XnaVoiceStart(owner, samples, format.sample_rate, params)) {
    return false;
  }
  XELOGI("[xna] XACT cue {} playing {} frame(s) at {} Hz", cue,
         samples->size() / kChannels, format.sample_rate);
  return true;
}

bool XactIsCuePlaying(uint32_t cue) {
  return XnaVoiceQuery(XnaVoiceOwner(kXnaVoiceCue, cue), nullptr) !=
         XnaVoiceState::kNone;
}

void XactStopCue(uint32_t cue) {
  XnaVoiceStop(XnaVoiceOwner(kXnaVoiceCue, cue));
}

void XactDetachCue(uint32_t cue) {
  XnaVoiceDetach(XnaVoiceOwner(kXnaVoiceCue, cue));
}

void XactStopCategory(uint32_t category) { XnaVoiceStopCategory(category); }

void XactStopAll() {
  std::lock_guard<std::mutex> lock(player_mutex);
  pending_starts.clear();
  voices.clear();
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
