/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The console entry points that are actually implemented, plus the bookkeeping
// for the ones that are not. Everything else is a generated stub - see
// xna_exports_generated.cc.

#include "xenia/kernel/xna/xna_avatar.h"
#include "xenia/kernel/xna/xna_exports.h"
#include "xenia/kernel/xna/xna_gpu.h"
#include "xenia/kernel/xna/xna_host.h"
#include "xenia/kernel/xna/xna_launcher.h"
#include "xenia/kernel/xna/xna_network_session.h"
#include "xenia/kernel/xna/xna_os.h"
#include "xenia/kernel/xna/xna_present.h"
#include "xenia/kernel/xna/xna_xact.h"
#include "xenia/kernel/xna/xna_xact_player.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xna {

namespace {

std::mutex usage_mutex;
std::map<std::string, uint64_t> usage_counts;

// A stub that keeps returning the same answer is a lie the caller may believe
// forever. GraphicsAdapter enumerates display modes with a loop that ends when
// this fails, so a stub returning success enumerated until memory ran out and
// looked exactly like a hang. Anything called this often is doing the same.
constexpr uint64_t kRunawayThreshold = 100000;

// The console's DISPLAY_MODE_INFO, declared for real rather than poked through
// a pointer: { int Width; int Height; SurfaceFormat Format; }.
struct DisplayModeInfo {
  int32_t width;
  int32_t height;
  int32_t format;  // SurfaceFormat::Color == 0
};

// The console's ADAPTER_DESC: four ints, all reported verbatim by
// GraphicsAdapter.
struct AdapterDesc {
  int32_t vendor_id;
  int32_t device_id;
  int32_t sub_system_id;
  int32_t revision;
};

// Anything but uint.MaxValue, which GraphicsAdapter treats as "no adapter",
// and the same for the device, which GraphicsDevice tests the same way.
constexpr uint32_t kAdapterHandle = 1;
constexpr uint32_t kDeviceHandle = 1;

std::atomic<uint64_t> present_count{0};
std::atomic<uint64_t> packet_count{0};
std::atomic<uint64_t> packet_bytes{0};
std::atomic<uint64_t> sprite_batch_count{0};
std::atomic<uint64_t> sprite_count{0};
std::atomic<uint64_t> user_draw_count{0};
std::atomic<uint64_t> user_primitive_count{0};

// One mode. The console offered a fixed list per video standard; Nexia
// presents a single buffer, so one mode is the honest answer.
constexpr int32_t kModeWidth = 1280;
constexpr int32_t kModeHeight = 720;

void WriteMode(DisplayModeInfo* mode) {
  if (!mode) {
    return;
  }
  mode->width = kModeWidth;
  mode->height = kModeHeight;
  mode->format = 0;
}

}  // namespace

void CountPresent() { present_count.fetch_add(1, std::memory_order_relaxed); }

void CountPackets(uint32_t size) {
  packet_count.fetch_add(1, std::memory_order_relaxed);
  packet_bytes.fetch_add(size, std::memory_order_relaxed);
}

void CountSprites(uint32_t count) {
  sprite_batch_count.fetch_add(1, std::memory_order_relaxed);
  sprite_count.fetch_add(count, std::memory_order_relaxed);
}

void CountUserPrimitives(uint32_t count) {
  user_draw_count.fetch_add(1, std::memory_order_relaxed);
  user_primitive_count.fetch_add(count, std::memory_order_relaxed);
}

void XnaExportUnimplemented(const char* entry) {
  if (!entry) {
    return;
  }
  bool first = false;
  bool runaway = false;
  {
    std::lock_guard<std::mutex> lock(usage_mutex);
    auto& count = usage_counts[entry];
    first = count == 0;
    ++count;
    runaway = count == kRunawayThreshold;
  }
  if (first) {
    XELOGW("[xna] {} is not implemented", entry);
  }
  if (runaway) {
    XELOGE(
        "[xna] {} called {} times and is still unimplemented - it probably "
        "ends a loop by failing, and any other answer keeps that loop running",
        entry, kRunawayThreshold);
  }
}

namespace {
std::mutex title_path_mutex;
std::string title_path;
}  // namespace

void SetXnaTitlePath(const std::string& path) {
  std::lock_guard<std::mutex> lock(title_path_mutex);
  title_path = path;
}

std::string XnaTitlePath() {
  std::lock_guard<std::mutex> lock(title_path_mutex);
  return title_path;
}

void XnaExportDefaulted(const char* entry) {
  if (!entry) {
    return;
  }
  bool first = false;
  {
    std::lock_guard<std::mutex> lock(usage_mutex);
    auto& count = usage_counts[entry];
    first = count == 0;
    ++count;
  }
  if (first) {
    XELOGW("[xna] {} answered with a default", entry);
  }
}

std::string DescribeXnaExportUsage() {
  std::vector<std::pair<std::string, uint64_t>> rows;
  {
    std::lock_guard<std::mutex> lock(usage_mutex);
    rows.assign(usage_counts.cbegin(), usage_counts.cend());
  }
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  std::string text =
      "presented " + std::to_string(present_count.load()) + " frame(s), " +
      std::to_string(packet_count.load()) + " command packet(s) totalling " +
      std::to_string(packet_bytes.load()) + " bytes\n" +
      std::to_string(rows.size()) + " console entry point(s) reached:\n";
  for (const auto& row : rows) {
    text += "  " + std::to_string(row.second) + "  " + row.first + "\n";
  }
  return text;
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe

// ---- D3D --------------------------------------------------------------------

extern "C" uint32_t xna_D3D_D3D_GetSupportedDisplayMode(uint32_t adapter,
                                                        uint32_t index,
                                                        void* out_mode) {
  // The caller is a bare loop that continues while this returns 0:
  //   for (uint i = 0; GetSupportedDisplayMode(h, i, out mode) == 0; i++)
  // so ending the enumeration means returning non-zero, and returning success
  // forever means enumerating forever.
  if (index > 0) {
    return 1;
  }
  xe::kernel::xna::WriteMode(
      reinterpret_cast<xe::kernel::xna::DisplayModeInfo*>(out_mode));
  return 0;
}

// The second out parameter is WIDESCREEN, not a refresh rate - GraphicsAdapter
// calls it `out var widescreen` and uses it for IsWideScreen. 1280x720 is 16:9.
extern "C" uint32_t xna_D3D_D3D_GetCurrentDisplayMode(
    uint32_t adapter, void* out_mode, uint32_t* out_widescreen) {
  xe::kernel::xna::WriteMode(
      reinterpret_cast<xe::kernel::xna::DisplayModeInfo*>(out_mode));
  if (out_widescreen) {
    *out_widescreen = 1;
  }
  return 0;
}

// GraphicsAdapter keeps this handle and only checks it against uint.MaxValue,
// so any other value serves as "the one adapter".
extern "C" uint32_t xna_D3D_D3D_CreateDirect3DHandle(void* out_desc) {
  if (out_desc) {
    auto* desc = reinterpret_cast<xe::kernel::xna::AdapterDesc*>(out_desc);
    // Reported straight through to the title as GraphicsAdapter.VendorId and
    // friends. Nothing depends on them being real hardware ids.
    desc->vendor_id = 0x1414;  // Microsoft
    desc->device_id = 0x5353;  // Xenos
    desc->sub_system_id = 0;
    desc->revision = 0;
  }
  return xe::kernel::xna::kAdapterHandle;
}

extern "C" uint32_t xna_D3D_D3D_ReleaseHandle(uint32_t handle) { return 0; }

// Zero means supported. Both profiles are: Reach is a subset of HiDef, and the
// emulator's GPU is well past either.
extern "C" uint32_t xna_D3D_D3D_IsProfileSupported(uint32_t adapter,
                                                   uint32_t profile) {
  return 0;
}

// Adjusts the requested format in place; leaving it untouched means "you may
// have exactly what you asked for", which is what the caller tests for.
// GraphicsAdapter has already clamped the format and depth format to ones the
// profile allows before calling, so agreeing is safe.
extern "C" uint32_t xna_D3D_D3D_QueryFormat(uint32_t adapter,
                                            bool is_back_buffer,
                                            uint32_t profile, void* info) {
  return 0;
}

// ---- device -----------------------------------------------------------------
//
// GraphicsDevice keeps the returned handle and only tests it against
// uint.MaxValue, so any other value is a working device. Present and Reset are
// passed through ThrowExceptionFromResult, so anything but 0 becomes an
// exception inside the title.

extern "C" uint32_t xna_D3D_D3D_CreateDeviceHandle(uint32_t adapter,
                                                   void* settings,
                                                   uint32_t profile) {
  return xe::kernel::xna::kDeviceHandle;
}

extern "C" void xna_D3D_D3D_Device_ReleaseHandle(uint32_t device) {}

extern "C" uint32_t xna_D3D_D3D_Device_Reset(uint32_t device, void* settings) {
  return 0;
}

extern "C" uint32_t xna_D3D_D3D_Device_Present(uint32_t device) {
  xe::kernel::xna::CountPresent();
  // Where the window finally gets an image. Until this existed, Present
  // counted calls and nothing else, which is why a title that decoded a
  // whole command stream still showed nothing.
  // Geometry went to EDRAM through the command processor and has to be
  // resolved before it can be shown; the sprite path draws straight to the
  // guest output and is what puts the HUD up when there was no geometry.
  // ONE OWNER OF THE SWAP, NOT ONE PER FRAME.
  //
  // Choosing per frame meant a frame with geometry swapped the resolved image
  // and a frame without swapped the sprite target, so the two alternated and
  // the screen flashed between them. Once geometry has been seen the resolve
  // owns the output; the sprite path keeps it until then.
  // Only when geometry actually went through the command processor - with it
  // off, this is the 2D path exactly as it was.
  if (xe::kernel::xna::XnaGpuHasEverDrawn()) {
    xe::kernel::xna::XnaGpuPresent(1280, 720);
    return 0;
  }
  xe::kernel::xna::XnaPresentFrame();
  return 0;
}

// ---- async dispatcher -------------------------------------------------------
//
// XNA runs a thread that waits on the console for asynchronous completions and
// callbacks - song changed, capture buffer ready, an async operation finished -
// and hands them to FrameworkDispatcher. SoundEffect's static constructor
// starts it, so a failure here takes the whole audio subsystem down with a
// TypeInitializationException before the game has drawn anything.

// The single return value AsyncDispatcherThreadFunction compares against
// before it would throw: `if (rc == 0x80040203) return;`.
static constexpr uint32_t kDispatcherShutdown = 0x80040203;

static std::mutex async_mutex;
static std::vector<uint32_t> finished_async_ops;
static std::map<uint32_t, std::string> async_container_names;
static std::map<uint32_t, std::string> open_containers;
static uint32_t next_async_op = 1;

static std::string ReadUtf16(const uint8_t* at, const uint8_t* end) {
  std::u16string wide;
  while (at + 2 <= end) {
    uint16_t unit = 0;
    std::memcpy(&unit, at, sizeof(unit));
    if (!unit) {
      break;
    }
    wide.push_back(static_cast<char16_t>(unit));
    at += 2;
  }
  return xe::to_utf8(wide);
}

static uint32_t BeginCompletedAsyncOperation() {
  std::lock_guard<std::mutex> lock(async_mutex);
  const uint32_t id = next_async_op++;
  finished_async_ops.push_back(id);
  return id;
}

namespace xe {
namespace kernel {
namespace xna {

uint32_t XnaReserveAsyncOperation() {
  std::lock_guard<std::mutex> lock(async_mutex);
  return next_async_op++;
}

void XnaCompleteAsyncOperation(uint32_t id) {
  std::lock_guard<std::mutex> lock(async_mutex);
  finished_async_ops.push_back(id);
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe

extern "C" uint32_t xna_Net_KernelAsyncDispatcher_Initialize_Entrypoint() {
  return 0;
}

extern "C" uint32_t
xna_Net_KernelAsyncDispatcher_WaitForAsyncOperationToFinish_Entrypoint(
    uint32_t* out_call_type, uint32_t* out_call_args) {
  // Confirmed against the caller's IL (tools/xna/dump-il.ps1):
  // AsyncDispatcherThreadFunction is `while (true) { rc = Wait(...); ... }`
  // with no delay of its own, so this has to BLOCK - returning immediately
  // would spin a core for the life of the title. Anything but 0 or the one
  // shutdown code below goes through ThrowExceptionFromResult, and a call type
  // of 1 is the one value HandleManagedCallback returns early on, so idling
  // and reporting "no call" is a normal wakeup.
  //
  // Returning kDispatcherShutdown ends that loop cleanly - the only way to
  // stop the thread without an exception - which is what a title exit will
  // want once there is one to hook.
  uint32_t finished = 0;
  {
    std::lock_guard<std::mutex> lock(async_mutex);
    if (!finished_async_ops.empty()) {
      finished = finished_async_ops.front();
      finished_async_ops.erase(finished_async_ops.begin());
    }
  }
  if (finished) {
    if (out_call_type) {
      *out_call_type = 3;
    }
    if (out_call_args) {
      *out_call_args = finished;
    }
    XELOGI("[xna] async operation {} completed", finished);
    return 0;
  }

  xe::threading::Sleep(std::chrono::milliseconds(50));
  if (out_call_type) {
    *out_call_type = 1;  // ManagedCallType::NoManagedCall
  }
  if (out_call_args) {
    *out_call_args = 0;
  }
  return 0;
}

// ---- system -----------------------------------------------------------------

// FrameworkDispatcher polls this every frame for system notifications -media
// state, storage device changes. None are raised yet.
extern "C" uint32_t xna_SYSTEM_System_DispatcherUpdate(
    uint32_t* out_notifications) {
  if (out_notifications) {
    *out_notifications = 0;
  }
  return 0;
}

// ---- Nexia's own
// -------------------------------------------------------------

// Not a console entry point: the managed host reads this when a title returns
// or dies, so the run ends with a list of what the title actually asked the
// console for. The counts live here now that every P/Invoke binds straight to
// this executable, so the managed side has nothing of its own left to report.
//
// The buffer belongs to this function and is valid until the next call. Only
// the title thread reads it, at the two points where the title is over.
extern "C" const char* Nexia_XnaExportUsage() {
  static std::string text;
  text = xe::kernel::xna::DescribeXnaExportUsage();
  return text.c_str();
}

// ---- audio ------------------------------------------------------------------
//
// The four global 3D-audio parameters. SoundEffect keeps its own copy of each
// and POPS the result of these calls, so they cannot fail - they exist so the
// mixer can be told. Nothing mixes yet, so the values are held here until there
// is something to hand them to; a stub would have been indistinguishable in
// behaviour but would have kept claiming, once per run, that a call the title
// makes at startup is missing.

namespace {
std::atomic<float> master_volume{1.0f};
std::atomic<float> distance_scale{1.0f};
std::atomic<float> doppler_scale{1.0f};
std::atomic<float> speed_of_sound{343.5f};
}  // namespace

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__SetMasterVolume(
    float value) {
  master_volume.store(value, std::memory_order_relaxed);
  xe::kernel::xna::XnaAudioSetMasterVolume(value);
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__SetDistanceScale(
    float value) {
  distance_scale.store(value, std::memory_order_relaxed);
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__SetDopplerScale(
    float value) {
  doppler_scale.store(value, std::memory_order_relaxed);
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__SetSpeedOfSound(
    float value) {
  speed_of_sound.store(value, std::memory_order_relaxed);
  return 0;
}

// ---- storage ----------------------------------------------------------------

// Where the title lives. XNA calls this once, lazily, from TitleLocation.Path,
// then takes Path.GetDirectoryName of the answer and prefixes every relative
// content path with it - so this must report the EXECUTABLE, not the folder it
// sits in, or every asset resolves one directory too high.
//
// The buffer is a StringBuilder, which this runtime marshals as UTF-16 even
// though the P/Invoke declares no CharSet - unspecified reads as "Ansi" in the
// metadata and that is not what actually happens. Writing bytes here hands the
// title a path made of its own characters re-read two at a time. The caller
// allocates 260 CHARACTERS, so the capacity is in characters too. The result
// goes through ThrowExceptionFromResult, so anything but 0 throws.
extern "C" uint32_t xna_STORAGE_STORAGE_GetStorageLocation(uint16_t* buffer,
                                                           int32_t capacity) {
  if (!buffer || capacity <= 0) {
    return 0x80070057;  // E_INVALIDARG
  }
  const std::string path = xe::kernel::xna::XnaTitlePath();
  if (path.empty()) {
    // Nothing is hosted, which should be impossible from inside a title.
    return 0x80004005;  // E_FAIL
  }
  const std::u16string wide = xe::to_utf16(path);
  if (wide.size() + 1 > static_cast<size_t>(capacity)) {
    return 0x8007007A;  // ERROR_INSUFFICIENT_BUFFER
  }
  std::memcpy(buffer, wide.c_str(), (wide.size() + 1) * sizeof(uint16_t));
  return 0;
}

// ---- gamer services ---------------------------------------------------------
//
// The entire GamerServices subsystem is two entry points. Everything the title
// can ask for - sign-in state, friends, presence, the guide, the keyboard,
// message boxes, the marketplace - is a command written into one buffer and
// handed to DispatchCommand, in the same spirit as the HLCB packet stream.
//
// The reply is written back over the same buffer, and GamerServicesDispatcher
// reads it as a list of notification records starting at offset 4:
//
//     [type] [payload...] [type] [payload...] ... [0] [flags]
//
// The loop is `while (type != 0)`, so A ZERO TYPE IS THE TERMINATOR. Zeroing
// the reply is therefore not a stub - it is the correct encoding of "nothing
// has changed", which is the truth on every frame where no gamer signs in or
// out. Leaving the buffer untouched would instead re-read the command that was
// just written and walk off the end of it.
//
// A non-zero result makes the dispatcher skip the reply entirely, and other
// callers pass it to ThrowExceptionFromResult, so 0 is required either way.

extern "C" uint32_t xna_Net_GamerServices_Initialize_Entrypoint(
    int32_t reserved) {
  return 0;
}

namespace {

constexpr uint32_t kGamerServicesUpdateCommand = 6;
constexpr uint32_t kStorageBeginShowSelector = 27;
constexpr uint32_t kStorageEndShowSelector = 28;
constexpr uint32_t kStorageBeginOpenContainer = 69;
constexpr uint32_t kStorageEndOpenContainer = 70;
constexpr uint32_t kAvatarRendererCreate = 77;
constexpr uint32_t kAvatarRendererDispose = 78;
constexpr uint32_t kAvatarRendererState = 79;
constexpr uint32_t kAvatarRendererDraw = 80;
constexpr uint32_t kAvatarRendererBindPose = 81;
constexpr uint32_t kAvatarBeginGetFromGamer = 82;
constexpr uint32_t kAvatarEndGetFromGamer = 83;
constexpr uint32_t kAvatarCreateRandom = 84;
constexpr uint32_t kAvatarHeight = 85;
constexpr uint32_t kAvatarBodyType = 86;
constexpr uint32_t kAvatarAnimationCreate = 87;
constexpr uint32_t kAvatarAnimationDispose = 88;
constexpr uint32_t kAvatarAnimationUpdate = 89;
constexpr uint32_t kAvatarBoneCount = 71;
constexpr size_t kAvatarDescriptionBytes = 1020;
constexpr size_t kAvatarReplyCapacity = 4804;
constexpr size_t kAvatarDrawMatrices = 16;
constexpr size_t kAvatarDrawLights = 208;
constexpr size_t kAvatarDrawExpression = 244;
constexpr size_t kAvatarDrawBones = 264;
constexpr size_t kAvatarDrawBytes = 4808;
constexpr uint32_t kNetworkSessionBeginCreate = 43;
constexpr uint32_t kNetworkSessionEndCreate = 44;
constexpr uint32_t kNetworkSessionBeginJoinInvited = 45;
constexpr uint32_t kNetworkSessionEndJoinInvited = 46;
constexpr uint32_t kNetworkSessionUpdate = 47;
constexpr uint32_t kNetworkSessionDestroy = 48;
constexpr uint32_t kSessionFinderBeginFind = 50;
constexpr uint32_t kSessionFinderEndFind = 51;
constexpr uint32_t kSessionFinderBeginJoin = 52;
constexpr uint32_t kSessionFinderEndJoin = 53;
constexpr uint32_t kSessionFinderDestroy = 54;
constexpr uint32_t kSessionFinderQualityOfService = 55;
constexpr uint32_t kSessionPropertySlots = 8;
constexpr uint32_t kSessionBufferHeader = 4;
constexpr uint32_t kNetworkSessionGrowBuffer = 0x80040200u;
constexpr uint32_t kNetworkSessionFailed = 0x80004005u;
constexpr uint32_t kQualityProbes = 1;
constexpr uint32_t kQualityPingMs = 30;
constexpr uint32_t kGuideBeginShowMessageBox = 23;
constexpr uint32_t kGuideEndShowMessageBox = 24;
constexpr uint32_t kGuideBeginShowKeyboardInput = 25;
constexpr uint32_t kGuideEndShowKeyboardInput = 26;
constexpr uint32_t kGuideLocalSlots = 4;
constexpr uint32_t kGuideMaxButtons = 16;
constexpr uint32_t kGuideTextLength = 256;
constexpr uint32_t kStorageDeviceHandle = 1;

std::mutex guide_operations_mutex;
std::map<uint32_t, uint32_t> guide_operations;

void WatchGuideRequest(uint32_t operation, uint32_t request) {
  {
    std::lock_guard<std::mutex> lock(guide_operations_mutex);
    guide_operations[operation] = request;
  }
  std::thread([operation, request] {
    const auto* os = xe::kernel::xna::GetXnaOsTable();
    while (request && os->GuidePoll(request, nullptr, nullptr) ==
                          xe::kernel::xna::kXnaOsGuidePending) {
      xe::threading::Sleep(std::chrono::milliseconds(50));
    }
    xe::kernel::xna::XnaCompleteAsyncOperation(operation);
  }).detach();
}

uint32_t TakeGuideRequest(uint32_t operation) {
  std::lock_guard<std::mutex> lock(guide_operations_mutex);
  auto found = guide_operations.find(operation);
  if (found == guide_operations.end()) {
    return 0;
  }
  const uint32_t request = found->second;
  guide_operations.erase(found);
  return request;
}

uint8_t* LocateSessionBuffer(const uint8_t* command_buffer, uint32_t low,
                             uint32_t total, uint32_t current,
                             std::string* rejected) {
  constexpr size_t kArrayHeader = 2 * sizeof(void*);
  if (!low || !total) {
    *rejected = "no address";
    return nullptr;
  }
  const uint64_t high = reinterpret_cast<uintptr_t>(command_buffer) >> 32;
  for (const uint64_t candidate_high : {high, high - 1, high + 1}) {
    if (candidate_high > 0x7FFFu) {
      continue;
    }
    const uint64_t address = (candidate_high << 32) | low;
    if (address < kArrayHeader) {
      continue;
    }
    auto* data = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address));
    if (!xe::kernel::xna::XnaHostWritable(data - kArrayHeader, data + total)) {
      *rejected += fmt::format(" {:X}:pages", address);
      continue;
    }
    if (std::memcmp(data - kArrayHeader, command_buffer - kArrayHeader,
                    sizeof(void*)) != 0) {
      *rejected += fmt::format(" {:X}:type", address);
      continue;
    }
    uint32_t length = 0;
    uint32_t first = 0;
    std::memcpy(&length, data - sizeof(void*), sizeof(length));
    std::memcpy(&first, data, sizeof(first));
    if (length == total && first == current) {
      return data;
    }
    *rejected +=
        fmt::format(" {:X}:length={},first={}", address, length, first);
  }
  return nullptr;
}
constexpr uint32_t kEventPlayerSigninStatusChanged = 1;
constexpr size_t kGameDefaultsBytes = 48;

struct ReplyWriter {
  uint8_t* at;
  uint8_t* end;

  bool Fits(size_t bytes) const {
    return at && static_cast<size_t>(end - at) >= bytes;
  }
  void Word(uint32_t value) {
    if (!Fits(sizeof(value))) {
      at = nullptr;
      return;
    }
    std::memcpy(at, &value, sizeof(value));
    at += sizeof(value);
  }
  void String(const std::string& text) {
    const std::u16string wide = xe::to_utf16(text);
    const size_t span = ((wide.size() + 1) * 2 + 3) & ~size_t(3);
    if (!Fits(span)) {
      at = nullptr;
      return;
    }
    std::memset(at, 0, span);
    std::memcpy(at, wide.data(), wide.size() * 2);
    at += span;
  }
  void Zeros(size_t bytes) {
    if (!Fits(bytes)) {
      at = nullptr;
      return;
    }
    std::memset(at, 0, bytes);
    at += bytes;
  }
  void Bytes(const uint8_t* data, size_t bytes) {
    if (!Fits(bytes)) {
      at = nullptr;
      return;
    }
    std::memcpy(at, data, bytes);
    at += bytes;
  }
  void Float(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    Word(bits);
  }
  void IdentityMatrices(uint32_t count) {
    for (uint32_t matrix = 0; matrix < count; ++matrix) {
      for (uint32_t element = 0; element < 16; ++element) {
        Float(element % 5 == 0 ? 1.0f : 0.0f);
      }
    }
  }
};

std::mutex avatar_mutex;
std::map<uint32_t, uint32_t> avatar_gamer_operations;

size_t AvatarDescriptionSpan(const std::vector<uint8_t>& request,
                             size_t offset) {
  return offset < request.size()
             ? std::min(request.size() - offset, kAvatarDescriptionBytes)
             : 0;
}

const uint8_t* AvatarDescriptionAt(const std::vector<uint8_t>& request,
                                   size_t offset) {
  return AvatarDescriptionSpan(request, offset) ? request.data() + offset
                                                : nullptr;
}

struct SlotUser {
  uint32_t signin_state = 0;
  uint32_t is_live = 0;
  std::string gamertag;
};

bool LookupSlotUser(uint32_t slot, SlotUser* out) {
  auto* state = xe::kernel::kernel_state();
  if (!state || !state->xam_state()) {
    return false;
  }
  auto* profiles = state->xam_state()->profile_manager();
  auto* profile =
      profiles ? profiles->GetProfile(static_cast<uint8_t>(slot)) : nullptr;
  if (!profile) {
    out->signin_state = 0;
    out->is_live = 0;
    out->gamertag.clear();
    return true;
  }
  out->signin_state = static_cast<uint32_t>(profile->signin_state());
  out->is_live = profile->IsLiveEnabled() ? 1 : 0;
  out->gamertag = profile->name();
  return true;
}

std::mutex signin_mutex;
uint32_t reported_signin_state[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                     0xFFFFFFFFu};

}  // namespace

extern "C" uint32_t xna_Net_GamerServices_DispatchCommand_Entrypoint(
    void* buffer, int32_t size) {
  if (!buffer || size < 8) {
    return 0x80070057;
  }
  auto* bytes = static_cast<uint8_t*>(buffer);

  uint32_t command = 0;
  if (size >= 8) {
    std::memcpy(&command, bytes + 4, sizeof(command));
  }

  {
    static std::mutex seen_mutex;
    static std::map<uint32_t, bool> seen;
    std::lock_guard<std::mutex> lock(seen_mutex);
    if (!seen[command]) {
      seen[command] = true;
      XELOGI("[xna] GamerServices: command {} ({} byte buffer)", command, size);
    }
  }

  std::string request_name;
  uint32_t request_operation = 0;
  if (command == kStorageBeginOpenContainer && size > 16) {
    request_name = ReadUtf16(bytes + 16, bytes + size);
  } else if (command == kStorageEndOpenContainer && size >= 12) {
    std::memcpy(&request_operation, bytes + 8, sizeof(request_operation));
  }

  const std::vector<uint8_t> request_bytes(bytes, bytes + size);
  const auto word_at = [&request_bytes](size_t offset) {
    uint32_t value = 0;
    if (offset + sizeof(value) <= request_bytes.size()) {
      std::memcpy(&value, request_bytes.data() + offset, sizeof(value));
    }
    return value;
  };
  const auto text_at = [&request_bytes](size_t* offset) {
    std::u16string wide;
    size_t cursor = *offset;
    while (cursor + sizeof(uint16_t) <= request_bytes.size()) {
      uint16_t unit = 0;
      std::memcpy(&unit, request_bytes.data() + cursor, sizeof(unit));
      cursor += sizeof(unit);
      if (!unit) {
        break;
      }
      wide.push_back(static_cast<char16_t>(unit));
    }
    *offset += ((wide.size() + 1) * 2 + 3) & ~size_t(3);
    return xe::to_utf8(wide);
  };
  xe::kernel::xna::XnaSessionRequest session_request;
  uint32_t session_operation = 0;
  uint32_t session_handle = 0;
  uint32_t session_index = 0;
  uint32_t session_buffer_total = 0;
  uint32_t session_buffer_current = 0;
  uint32_t session_buffer_low = 0;
  if (command == kNetworkSessionBeginCreate ||
      command == kSessionFinderBeginFind) {
    session_request.type = word_at(8);
    session_request.first_local = word_at(12);
    session_request.local_mask = word_at(16);
    session_request.max_local = word_at(20);
    size_t at = 24;
    if (command == kNetworkSessionBeginCreate) {
      session_request.max_gamers = word_at(24);
      session_request.private_slots = word_at(28);
      at = 36;
    }
    for (uint32_t slot = 0; slot < kSessionPropertySlots; ++slot) {
      const uint32_t present = word_at(at);
      at += 4;
      session_request.properties.push_back(present);
      if (present) {
        session_request.properties.push_back(word_at(at));
        at += 4;
      }
    }
  } else if (command == kNetworkSessionEndCreate ||
             command == kNetworkSessionEndJoinInvited ||
             command == kSessionFinderEndFind ||
             command == kSessionFinderEndJoin) {
    session_operation = word_at(8);
  } else if (command == kNetworkSessionUpdate) {
    session_handle = word_at(8);
    session_buffer_total = word_at(12);
    session_buffer_current = word_at(16);
    session_buffer_low = word_at(20);
  } else if (command == kNetworkSessionDestroy ||
             command == kSessionFinderDestroy) {
    session_handle = word_at(8);
  } else if (command == kSessionFinderBeginJoin ||
             command == kSessionFinderQualityOfService) {
    session_handle = word_at(8);
    session_index = word_at(12);
  }

  constexpr uint32_t kReplyCapacity = 4096;
  ReplyWriter reply{bytes + 4, bytes + 4 + kReplyCapacity};
  std::memset(reply.at, 0, kReplyCapacity);

  if (command == kStorageBeginShowSelector) {
    reply.Word(0);
    reply.Word(BeginCompletedAsyncOperation());
    return 0;
  }

  if (command == kStorageEndShowSelector) {
    reply.Word(0);
    reply.Word(kStorageDeviceHandle);
    reply.Word(0);
    XELOGI("[xna] storage: selector returned device {}", kStorageDeviceHandle);
    return 0;
  }

  if (command == kAvatarBeginGetFromGamer) {
    const uint32_t operation = BeginCompletedAsyncOperation();
    {
      std::lock_guard<std::mutex> lock(avatar_mutex);
      avatar_gamer_operations[operation] = word_at(12);
    }
    reply.Word(operation);
    return 0;
  }

  if (command == kAvatarEndGetFromGamer) {
    uint32_t gamer = 0xFFFFFFFFu;
    {
      std::lock_guard<std::mutex> lock(avatar_mutex);
      auto found = avatar_gamer_operations.find(word_at(8));
      if (found != avatar_gamer_operations.end()) {
        gamer = found->second;
        avatar_gamer_operations.erase(found);
      }
    }
    const auto description =
        xe::kernel::xna::XnaAvatarDescriptionForGamer(gamer);
    ReplyWriter avatar_reply{bytes + 4, bytes + 4 + kAvatarReplyCapacity};
    avatar_reply.Bytes(description.data(), description.size());
    avatar_reply.Word(gamer < 4 ? gamer : 0xFFFFFFFFu);
    XELOGI("[xna] avatar: description for gamer {}", gamer);
    return 0;
  }

  if (command == kAvatarCreateRandom) {
    const auto description = xe::kernel::xna::XnaAvatarRandomDescription(
        static_cast<int32_t>(word_at(12)));
    ReplyWriter avatar_reply{bytes + 4, bytes + 4 + kAvatarReplyCapacity};
    avatar_reply.Bytes(description.data(), description.size());
    return 0;
  }

  if (command == kAvatarHeight) {
    reply.Float(xe::kernel::xna::XnaAvatarHeight(
        AvatarDescriptionAt(request_bytes, 12),
        AvatarDescriptionSpan(request_bytes, 12)));
    return 0;
  }

  if (command == kAvatarBodyType) {
    reply.Word(xe::kernel::xna::XnaAvatarBodyType(
        AvatarDescriptionAt(request_bytes, 12),
        AvatarDescriptionSpan(request_bytes, 12)));
    return 0;
  }

  if (command == kAvatarRendererCreate) {
    const uint32_t handle = xe::kernel::xna::XnaAvatarCreateRenderer(
        AvatarDescriptionAt(request_bytes, 16),
        AvatarDescriptionSpan(request_bytes, 16));
    const auto light = xe::kernel::xna::avatar::DefaultLighting();
    reply.Word(handle);
    for (float value : light.color) {
      reply.Float(value);
    }
    for (float value : light.direction) {
      reply.Float(value);
    }
    for (float value : light.ambient) {
      reply.Float(value);
    }
    XELOGI("[xna] avatar: renderer {} created", handle);
    return 0;
  }

  if (command == kAvatarRendererDispose) {
    if (!xe::kernel::xna::XnaAvatarDestroyRenderer(word_at(8))) {
      xe::kernel::xna::XnaAvatarDestroyRenderer(word_at(12));
    }
    return 0;
  }

  if (command == kAvatarRendererState) {
    reply.Word(xe::kernel::xna::XnaAvatarRendererReady(word_at(8)) ? 1 : 0);
    return 0;
  }

  if (command == kAvatarRendererBindPose) {
    std::vector<float> bones(size_t(kAvatarBoneCount) * 16);
    xe::kernel::xna::XnaAvatarBindPose(bones.data());
    ReplyWriter avatar_reply{bytes + 4, bytes + 4 + kAvatarReplyCapacity};
    avatar_reply.Bytes(reinterpret_cast<const uint8_t*>(bones.data()),
                       bones.size() * sizeof(float));
    return 0;
  }

  if (command == kAvatarRendererDraw) {
    if (request_bytes.size() < kAvatarDrawBytes) {
      XELOGW("[xna] avatar: draw request is {} bytes, {} expected",
             request_bytes.size(), kAvatarDrawBytes);
      return 0;
    }
    float matrices[3][16];
    float lights[9];
    uint32_t expression[5];
    std::vector<float> bones(size_t(kAvatarBoneCount) * 16);
    std::memcpy(matrices, request_bytes.data() + kAvatarDrawMatrices,
                sizeof(matrices));
    std::memcpy(lights, request_bytes.data() + kAvatarDrawLights,
                sizeof(lights));
    std::memcpy(expression, request_bytes.data() + kAvatarDrawExpression,
                sizeof(expression));
    std::memcpy(bones.data(), request_bytes.data() + kAvatarDrawBones,
                bones.size() * sizeof(float));
    xe::kernel::xna::XnaAvatarDraw(word_at(12), matrices[0], matrices[1],
                                   matrices[2], lights, lights + 3, lights + 6,
                                   expression, bones.data());
    return 0;
  }

  if (command == kAvatarAnimationCreate) {
    float length = 0.0f;
    const uint32_t handle =
        xe::kernel::xna::XnaAvatarCreateAnimation(word_at(12), &length);
    reply.Word(handle);
    reply.Float(length);
    XELOGI("[xna] avatar: animation {} for preset {}, {} s", handle,
           word_at(12), length);
    return 0;
  }

  if (command == kAvatarAnimationDispose) {
    if (!xe::kernel::xna::XnaAvatarDestroyAnimation(word_at(8))) {
      xe::kernel::xna::XnaAvatarDestroyAnimation(word_at(12));
    }
    return 0;
  }

  if (command == kAvatarAnimationUpdate) {
    const uint32_t seconds_bits = word_at(12);
    float seconds = 0.0f;
    std::memcpy(&seconds, &seconds_bits, sizeof(seconds));
    uint32_t expression[5];
    std::vector<float> bones(size_t(kAvatarBoneCount) * 16);
    xe::kernel::xna::XnaAvatarUpdateAnimation(word_at(8), seconds, expression,
                                              bones.data());
    ReplyWriter avatar_reply{bytes + 4, bytes + 4 + kAvatarReplyCapacity};
    for (uint32_t value : expression) {
      avatar_reply.Word(value);
    }
    avatar_reply.Bytes(reinterpret_cast<const uint8_t*>(bones.data()),
                       bones.size() * sizeof(float));
    return 0;
  }

  if (command == kGuideBeginShowMessageBox) {
    const uint32_t player = word_at(8);
    size_t at = 12;
    const std::string title = text_at(&at);
    const std::string text = text_at(&at);
    const uint32_t button_count = word_at(at);
    at += 4;
    std::vector<std::string> buttons;
    for (uint32_t i = 0; i < button_count && i < kGuideMaxButtons; ++i) {
      buttons.push_back(text_at(&at));
    }
    const uint32_t focus = word_at(at);
    std::vector<const char*> labels;
    for (const std::string& button : buttons) {
      labels.push_back(button.c_str());
    }
    const uint32_t request =
        xe::kernel::xna::GetXnaOsTable()->GuideBeginMessageBox(
            player < kGuideLocalSlots ? player : 0, title.c_str(), text.c_str(),
            labels.empty() ? nullptr : labels.data(),
            static_cast<uint32_t>(labels.size()), focus);
    const uint32_t operation = xe::kernel::xna::XnaReserveAsyncOperation();
    WatchGuideRequest(operation, request);
    reply.Word(operation);
    XELOGI(
        "[xna] guide: message box \"{}\" with {} button(s) as operation {}{}",
        title, buttons.size(), operation,
        request ? "" : " (no dialog could be shown)");
    return 0;
  }

  if (command == kGuideEndShowMessageBox) {
    const uint32_t operation = word_at(8);
    const uint32_t request = TakeGuideRequest(operation);
    int32_t button = -1;
    if (request) {
      const auto* os = xe::kernel::xna::GetXnaOsTable();
      if (os->GuidePoll(request, &button, nullptr) !=
          xe::kernel::xna::kXnaOsGuideCompleted) {
        button = -1;
      }
      os->GuideRelease(request);
    }
    reply.Word(static_cast<uint32_t>(button));
    XELOGI("[xna] guide: message box operation {} answered with button {}",
           operation, button);
    return 0;
  }

  if (command == kGuideBeginShowKeyboardInput) {
    const uint32_t player = word_at(8);
    size_t at = 12;
    const std::string title = text_at(&at);
    const std::string description = text_at(&at);
    const std::string default_text = text_at(&at);
    const bool password = word_at(at) != 0;
    const uint32_t request = xe::kernel::xna::XnaGuideBeginKeyboard(
        player < kGuideLocalSlots ? player : 0, title.c_str(),
        description.c_str(), default_text.c_str(), kGuideTextLength, password);
    const uint32_t operation = xe::kernel::xna::XnaReserveAsyncOperation();
    WatchGuideRequest(operation, request);
    reply.Word(operation);
    XELOGI("[xna] guide: keyboard input \"{}\" as operation {}{}", title,
           operation, request ? "" : " (no dialog could be shown)");
    return 0;
  }

  if (command == kGuideEndShowKeyboardInput) {
    const uint32_t operation = word_at(8);
    const uint32_t request = TakeGuideRequest(operation);
    bool cancelled = true;
    std::string text;
    if (request) {
      const auto* os = xe::kernel::xna::GetXnaOsTable();
      uint32_t text_bytes = 0;
      if (os->GuidePoll(request, nullptr, &text_bytes) ==
          xe::kernel::xna::kXnaOsGuideCompleted) {
        cancelled = false;
        std::vector<char> buffer(text_bytes ? text_bytes : 1, '\0');
        os->GuideGetText(request, buffer.data(),
                         static_cast<uint32_t>(buffer.size()));
        text = buffer.data();
      }
      os->GuideRelease(request);
    }
    reply.Word(cancelled ? 1 : 0);
    reply.String(text);
    XELOGI("[xna] guide: keyboard input operation {} {}", operation,
           cancelled ? "cancelled" : "answered");
    return 0;
  }

  if (command == kNetworkSessionBeginCreate) {
    const uint32_t id = xe::kernel::xna::XnaSessionBeginCreate(session_request);
    reply.Word(id);
    XELOGI(
        "[xna] network session: creating type {} for {} gamer(s), {} private, "
        "as operation {}",
        session_request.type, session_request.max_gamers,
        session_request.private_slots, id);
    return 0;
  }

  if (command == kNetworkSessionEndCreate || command == kSessionFinderEndJoin) {
    xe::kernel::xna::XnaSessionSummary summary;
    const uint32_t result =
        command == kNetworkSessionEndCreate
            ? xe::kernel::xna::XnaSessionEndCreate(session_operation, &summary)
            : xe::kernel::xna::XnaSessionEndJoin(session_operation, &summary);
    if (result) {
      return result;
    }
    reply.Word(summary.handle);
    reply.Word(summary.type);
    reply.Word(summary.max_gamers);
    reply.Word(summary.private_slots);
    for (uint32_t value : summary.properties) {
      reply.Word(value);
    }
    return 0;
  }

  if (command == kNetworkSessionBeginJoinInvited) {
    reply.Word(BeginCompletedAsyncOperation());
    return 0;
  }

  if (command == kNetworkSessionEndJoinInvited) {
    XELOGW("[xna] network session: joining by invite is not supported yet");
    return kNetworkSessionFailed;
  }

  if (command == kSessionFinderBeginFind) {
    reply.Word(xe::kernel::xna::XnaSessionBeginFind(session_request));
    return 0;
  }

  if (command == kSessionFinderEndFind) {
    std::vector<uint8_t> found;
    const uint32_t result =
        xe::kernel::xna::XnaSessionEndFind(session_operation, &found);
    if (result) {
      return result;
    }
    if (found.size() <= kReplyCapacity) {
      std::memcpy(reply.at, found.data(), found.size());
    }
    return 0;
  }

  if (command == kSessionFinderBeginJoin) {
    reply.Word(
        xe::kernel::xna::XnaSessionBeginJoin(session_handle, session_index));
    return 0;
  }

  if (command == kSessionFinderDestroy) {
    xe::kernel::xna::XnaSessionDestroyFinder(session_handle);
    return 0;
  }

  if (command == kSessionFinderQualityOfService) {
    reply.Word(kQualityProbes);
    reply.Word(kQualityProbes);
    reply.Word(kQualityPingMs);
    reply.Word(kQualityPingMs);
    return 0;
  }

  if (command == kNetworkSessionUpdate) {
    if (!xe::kernel::xna::XnaSessionPrepareUpdate(session_handle,
                                                  session_buffer_current)) {
      reply.Word(0);
      reply.Word(0);
      return 0;
    }

    std::string rejected;
    uint8_t* target =
        LocateSessionBuffer(bytes, session_buffer_low, session_buffer_total,
                            session_buffer_current, &rejected);
    if (!target) {
      XELOGW(
          "[xna] network session {}: session buffer {:08X} ({} bytes) not "
          "found, events held back; command buffer {}, rejected:{}",
          session_handle, session_buffer_low, session_buffer_total,
          static_cast<const void*>(bytes), rejected);
      reply.Word(0);
      reply.Word(0);
      return 0;
    }

    const uint32_t end = std::min(session_buffer_current, session_buffer_total);
    const uint32_t records =
        end > kSessionBufferHeader ? end - kSessionBufferHeader : 0;
    std::vector<uint8_t> events;
    uint32_t needed = 0;
    const uint32_t result = xe::kernel::xna::XnaSessionUpdate(
        session_handle, target + kSessionBufferHeader, records,
        session_buffer_total, &events, &needed);
    if (result == kNetworkSessionGrowBuffer) {
      reply.Word(0);
      reply.Word(needed);
      return result;
    }
    if (!events.empty()) {
      std::memcpy(target, events.data(), events.size());
    }
    reply.Word(0);
    reply.Word(static_cast<uint32_t>(events.size()));
    return 0;
  }

  if (command == kNetworkSessionDestroy) {
    xe::kernel::xna::XnaSessionDestroy(session_handle);
    reply.Word(0);
    reply.Word(0);
    return 0;
  }

  if (command == kStorageBeginOpenContainer) {
    const uint32_t id = BeginCompletedAsyncOperation();
    {
      std::lock_guard<std::mutex> lock(async_mutex);
      async_container_names[id] = request_name;
    }
    reply.Word(0);
    reply.Word(id);
    XELOGI("[xna] storage: opening container \"{}\" as operation {}",
           request_name, id);
    return 0;
  }

  if (command == kStorageEndOpenContainer) {
    std::string name;
    {
      std::lock_guard<std::mutex> lock(async_mutex);
      auto found = async_container_names.find(request_operation);
      if (found != async_container_names.end()) {
        name = found->second;
        async_container_names.erase(found);
      }
      open_containers[0] = name;
    }
    reply.Word(kStorageDeviceHandle);
    reply.Word(0);
    reply.String(name);
    XELOGI("[xna] storage: container \"{}\" opened", name);
    return 0;
  }

  if (command == kGamerServicesUpdateCommand) {
    std::lock_guard<std::mutex> lock(signin_mutex);
    for (uint32_t slot = 0; slot < 4; ++slot) {
      SlotUser user;
      if (!LookupSlotUser(slot, &user)) {
        continue;
      }
      if (user.signin_state == reported_signin_state[slot]) {
        continue;
      }
      reported_signin_state[slot] = user.signin_state;

      reply.Word(kEventPlayerSigninStatusChanged);
      reply.Word(slot);
      reply.Word(user.signin_state);
      reply.Word(user.is_live);
      reply.Word(0);
      reply.String(user.gamertag);
      reply.Word(0xFFFFFFFFu);
      if (user.signin_state != 0) {
        reply.Zeros(kGameDefaultsBytes);
      }
      if (!reply.at) {
        reported_signin_state[slot] = 0xFFFFFFFFu;
        std::memset(bytes + 4, 0, kReplyCapacity);
        return 0;
      }
      XELOGI("[xna] GamerServices: slot {} is now sign-in state {} ({})", slot,
             user.signin_state, user.gamertag);
    }
    constexpr uint32_t kGuideVisibleFlag = 1;
    auto* state = xe::kernel::kernel_state();
    const bool guide_visible =
        state && state->xam_state() && state->xam_state()->IsUIActive();
    reply.Word(0);
    reply.Word(guide_visible ? kGuideVisibleFlag : 0);
    return 0;
  }

  reply.Word(0);
  reply.Word(0);
  return 0;
}

// The matching getters. These return the value directly rather than an error
// code, so a generated default of zero would have been read as a real setting -
// a speed of sound of 0 makes every doppler calculation collapse. Reporting
// what was set is both correct and free.

extern "C" float xna_AUDIO_SoundEffectUnsafeNativeMethods__GetDistanceScale() {
  return distance_scale.load(std::memory_order_relaxed);
}

extern "C" float xna_AUDIO_SoundEffectUnsafeNativeMethods__GetDopplerScale() {
  return doppler_scale.load(std::memory_order_relaxed);
}

extern "C" float xna_AUDIO_SoundEffectUnsafeNativeMethods__GetSpeedOfSound() {
  return speed_of_sound.load(std::memory_order_relaxed);
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__GetMasterVolume(
    float* out_volume) {
  if (out_volume) {
    *out_volume = master_volume.load(std::memory_order_relaxed);
  }
  return 0;
}

// Stereo, and the mask is front left plus front right. XNA uses these to lay
// out 3D audio; a channel count of zero would divide by it.
extern "C" int32_t
xna_AUDIO_SoundEffectUnsafeNativeMethods__GetDestinationChannelCount() {
  return 2;
}

extern "C" int32_t
xna_AUDIO_SoundEffectUnsafeNativeMethods__GetDestinationChannelMask() {
  return 0x3;
}

namespace {

struct SoundEffectRecord {
  xe::kernel::xna::XnaSamples samples;
  uint32_t sample_rate = 0;
};

struct SoundInstanceRecord {
  uint32_t effect = 0;
  bool dynamic = false;
  bool played = false;
  xe::kernel::xna::XnaAudioFormat format;
  float volume = 1.0f;
  float pitch = 0.0f;
  float pan = 0.0f;
  int32_t loop_start = 0;
  int32_t loop_length = 0;
  int32_t loop_count = 0;
};

std::mutex sound_mutex;
uint32_t next_sound_handle = 1;
std::unordered_map<uint32_t, SoundEffectRecord> sound_effects;
std::unordered_map<uint32_t, SoundInstanceRecord> sound_instances;

constexpr uint32_t kSoundInvalidArg = 0x80070057;
constexpr uint32_t kVoiceStatePlaying = 1;
constexpr uint32_t kVoiceStateStopped = 4;
constexpr uint32_t kVoiceStatePaused = 8;

uint64_t SoundOwner(uint32_t instance) {
  return xe::kernel::xna::XnaVoiceOwner(xe::kernel::xna::kXnaVoiceSound,
                                        instance);
}

bool ParseWaveFormat(const uint8_t* bytes, uint32_t size,
                     xe::kernel::xna::XnaAudioFormat* out) {
  if (!bytes || size < 16 || !out) {
    return false;
  }
  const auto le16 = [bytes](uint32_t at) {
    return uint32_t(bytes[at] | (bytes[at + 1] << 8));
  };
  const auto be16 = [bytes](uint32_t at) {
    return uint32_t((bytes[at] << 8) | bytes[at + 1]);
  };
  const auto le32 = [bytes](uint32_t at) {
    return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) |
           (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
  };
  const auto be32 = [bytes](uint32_t at) {
    return (uint32_t(bytes[at]) << 24) | (uint32_t(bytes[at + 1]) << 16) |
           (uint32_t(bytes[at + 2]) << 8) | uint32_t(bytes[at + 3]);
  };
  const auto known = [](uint32_t tag) {
    return tag == 0x0001 || tag == 0x0002 || tag == 0x0161 || tag == 0x0162 ||
           tag == 0x0165 || tag == 0x0166;
  };
  const bool big = !known(le16(0)) && known(be16(0));
  const auto read16 = [&](uint32_t at) { return big ? be16(at) : le16(at); };
  const auto read32 = [&](uint32_t at) { return big ? be32(at) : le32(at); };
  const uint32_t tag = read16(0);
  out->channels = read16(2);
  out->sample_rate = read32(4);
  out->avg_bytes_per_second = read32(8);
  out->block_align = read16(12);
  out->bits_per_sample = read16(14);
  out->big_endian = big;
  switch (tag) {
    case 0x0001:
      out->codec = xe::kernel::xna::kXnaAudioPcm;
      break;
    case 0x0002:
      out->codec = xe::kernel::xna::kXnaAudioAdpcm;
      break;
    case 0x0161:
    case 0x0162:
      out->codec = xe::kernel::xna::kXnaAudioWma;
      break;
    case 0x0165:
    case 0x0166:
      out->codec = xe::kernel::xna::kXnaAudioXma;
      break;
    default:
      out->codec = xe::kernel::xna::kXnaAudioUnknown;
      XELOGW("[xna] SoundEffect: format tag {:04X} is not known", tag);
      break;
  }
  return out->codec != xe::kernel::xna::kXnaAudioUnknown;
}

xe::kernel::xna::XnaVoiceParams InstanceParams(
    const SoundInstanceRecord& instance) {
  xe::kernel::xna::XnaVoiceParams params;
  params.volume = instance.volume;
  params.pan = instance.pan;
  params.pitch = instance.pitch;
  params.loop = instance.loop_count != 0;
  params.loop_begin = uint32_t(std::max(instance.loop_start, 0));
  params.loop_length = uint32_t(std::max(instance.loop_length, 0));
  return params;
}

}  // namespace

extern "C" uint32_t
xna_AUDIO_SoundEffectUnsafeNativeMethods__CreateSoundEffectHandle(
    const uint8_t* format, uint32_t format_size, const uint8_t* data,
    uint32_t data_size, uint32_t* handle_out) {
  if (!handle_out) {
    return kSoundInvalidArg;
  }
  xe::kernel::xna::XnaAudioFormat parsed;
  SoundEffectRecord record;
  if (ParseWaveFormat(format, format_size, &parsed)) {
    record.samples = xe::kernel::xna::XnaDecodeAudio(parsed, data, data_size);
    record.sample_rate = parsed.sample_rate;
  }
  if (!record.samples) {
    XELOGW(
        "[xna] SoundEffect: {} byte(s) of codec {} ({} channel(s), {} Hz) did "
        "not decode and will be silent",
        data_size, parsed.codec, parsed.channels, parsed.sample_rate);
  }
  std::lock_guard<std::mutex> lock(sound_mutex);
  const uint32_t handle = next_sound_handle++;
  XELOGD("[xna] SoundEffect {}: codec {}, {} channel(s), {} Hz, {} frame(s)",
         handle, parsed.codec, parsed.channels, parsed.sample_rate,
         record.samples ? record.samples->size() / 2 : 0);
  sound_effects[handle] = std::move(record);
  *handle_out = handle;
  return 0;
}

extern "C" uint32_t
xna_AUDIO_SoundEffectUnsafeNativeMethods__ReleaseSoundEffectHandle(
    uint32_t effect) {
  std::lock_guard<std::mutex> lock(sound_mutex);
  sound_effects.erase(effect);
  return 0;
}

extern "C" uint32_t
xna_AUDIO_SoundEffectUnsafeNativeMethods__CreateSoundEffectInstance(
    uint32_t effect, uint32_t* instance_out) {
  if (!instance_out) {
    return kSoundInvalidArg;
  }
  std::lock_guard<std::mutex> lock(sound_mutex);
  if (!sound_effects.count(effect)) {
    return kSoundInvalidArg;
  }
  const uint32_t handle = next_sound_handle++;
  SoundInstanceRecord record;
  record.effect = effect;
  sound_instances[handle] = record;
  *instance_out = handle;
  return 0;
}

extern "C" uint32_t
xna_AUDIO_SoundEffectUnsafeNativeMethods__CreateDynamicSoundEffectInstance(
    const uint8_t* format, uint32_t format_size, uint32_t* instance_out) {
  if (!instance_out) {
    return kSoundInvalidArg;
  }
  SoundInstanceRecord record;
  record.dynamic = true;
  if (!ParseWaveFormat(format, format_size, &record.format)) {
    return kSoundInvalidArg;
  }
  std::lock_guard<std::mutex> lock(sound_mutex);
  const uint32_t handle = next_sound_handle++;
  sound_instances[handle] = record;
  *instance_out = handle;
  return 0;
}

extern "C" uint32_t
xna_AUDIO_SoundEffectUnsafeNativeMethods__DestroySoundEffectInstance(
    uint32_t instance) {
  xe::kernel::xna::XnaVoiceStop(SoundOwner(instance));
  std::lock_guard<std::mutex> lock(sound_mutex);
  sound_instances.erase(instance);
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__SubmitPacket(
    uint32_t instance, int32_t loop_start, int32_t loop_length,
    int32_t loop_count) {
  std::lock_guard<std::mutex> lock(sound_mutex);
  auto found = sound_instances.find(instance);
  if (found == sound_instances.end()) {
    return kSoundInvalidArg;
  }
  found->second.loop_start = loop_start;
  found->second.loop_length = loop_length;
  found->second.loop_count = loop_count;
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__SubmitBuffer(
    uint32_t instance, const uint8_t* data, uint32_t size) {
  std::lock_guard<std::mutex> lock(sound_mutex);
  auto found = sound_instances.find(instance);
  if (found == sound_instances.end() || !found->second.dynamic) {
    return kSoundInvalidArg;
  }
  auto samples =
      xe::kernel::xna::XnaDecodeAudio(found->second.format, data, size, false);
  auto params = InstanceParams(found->second);
  params.loop = false;
  params.paused = !found->second.played;
  xe::kernel::xna::XnaVoiceQueue(SoundOwner(instance), samples,
                                 found->second.format.sample_rate, params);
  return 0;
}

extern "C" uint32_t
xna_AUDIO_SoundEffectUnsafeNativeMethods__GetPendingBufferCount(
    uint32_t instance, int32_t* count_out) {
  if (!count_out) {
    return kSoundInvalidArg;
  }
  *count_out = int32_t(xe::kernel::xna::XnaVoicePending(SoundOwner(instance)));
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__Play(
    uint32_t instance) {
  std::lock_guard<std::mutex> lock(sound_mutex);
  auto found = sound_instances.find(instance);
  if (found == sound_instances.end()) {
    return kSoundInvalidArg;
  }
  SoundInstanceRecord& record = found->second;
  const uint64_t owner = SoundOwner(instance);
  if (record.dynamic) {
    record.played = true;
    auto params = InstanceParams(record);
    params.loop = false;
    xe::kernel::xna::XnaVoiceQueue(owner, nullptr, record.format.sample_rate,
                                   params);
    xe::kernel::xna::XnaVoiceSetPaused(owner, false);
    return 0;
  }
  const auto state = xe::kernel::xna::XnaVoiceQuery(owner, nullptr);
  if (state == xe::kernel::xna::XnaVoiceState::kPaused) {
    xe::kernel::xna::XnaVoiceSetPaused(owner, false);
    return 0;
  }
  if (state == xe::kernel::xna::XnaVoiceState::kPlaying) {
    return 0;
  }
  auto effect = sound_effects.find(record.effect);
  if (effect == sound_effects.end() || !effect->second.samples) {
    return 0;
  }
  record.played = true;
  xe::kernel::xna::XnaVoiceStart(owner, effect->second.samples,
                                 effect->second.sample_rate,
                                 InstanceParams(record));
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__Stop(
    uint32_t instance, uint32_t immediate) {
  std::lock_guard<std::mutex> lock(sound_mutex);
  auto found = sound_instances.find(instance);
  if (found == sound_instances.end()) {
    return kSoundInvalidArg;
  }
  if (immediate || found->second.dynamic) {
    xe::kernel::xna::XnaVoiceStop(SoundOwner(instance));
    found->second.played = false;
  } else {
    xe::kernel::xna::XnaVoiceSetLoop(SoundOwner(instance), false);
  }
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__Pause(
    uint32_t instance) {
  xe::kernel::xna::XnaVoiceSetPaused(SoundOwner(instance), true);
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__SetVolume(
    uint32_t instance, float volume) {
  std::lock_guard<std::mutex> lock(sound_mutex);
  auto found = sound_instances.find(instance);
  if (found != sound_instances.end()) {
    found->second.volume = volume;
  }
  xe::kernel::xna::XnaVoiceSetVolume(SoundOwner(instance), volume);
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__SetPitch(
    uint32_t instance, float pitch) {
  std::lock_guard<std::mutex> lock(sound_mutex);
  auto found = sound_instances.find(instance);
  if (found != sound_instances.end()) {
    found->second.pitch = pitch;
  }
  xe::kernel::xna::XnaVoiceSetPitch(SoundOwner(instance), pitch);
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__SetPan(
    uint32_t instance, float pan) {
  std::lock_guard<std::mutex> lock(sound_mutex);
  auto found = sound_instances.find(instance);
  if (found != sound_instances.end()) {
    found->second.pan = pan;
  }
  xe::kernel::xna::XnaVoiceSetPan(SoundOwner(instance), pan);
  return 0;
}

extern "C" uint32_t xna_AUDIO_SoundEffectUnsafeNativeMethods__GetState(
    uint32_t instance, uint32_t* state_out) {
  if (!state_out) {
    return kSoundInvalidArg;
  }
  std::lock_guard<std::mutex> lock(sound_mutex);
  auto found = sound_instances.find(instance);
  if (found == sound_instances.end() ||
      (found->second.dynamic && !found->second.played)) {
    *state_out = kVoiceStateStopped;
    return 0;
  }
  switch (xe::kernel::xna::XnaVoiceQuery(SoundOwner(instance), nullptr)) {
    case xe::kernel::xna::XnaVoiceState::kPlaying:
      *state_out = kVoiceStatePlaying;
      break;
    case xe::kernel::xna::XnaVoiceState::kPaused:
      *state_out = kVoiceStatePaused;
      break;
    default:
      *state_out = kVoiceStateStopped;
      break;
  }
  return 0;
}

// ---- XACT -------------------------------------------------------------------
//
// The bank layer, wired to the real files a title ships. Nexia already decodes
// XMA and already has an audio driver; what it lacked was everything above
// them, so this is where PlayCue("CoinsDropping") becomes a wave.
//
// The console runtime hands these calls a FILE PATH, not bank data:
// SoundBank..ctor opens the file, checks the SDBK signature, closes it and
// passes the full path with its length in CHARACTERS. A hosted title runs on
// the host CLR, so that path is an ordinary host path and the parsing belongs
// here. Strings arrive as UTF-16, like every other string on this boundary.

namespace {

// XACT handles never enter the HLCB packet stream, so unlike a graphics handle
// they carry no 24-bit ceiling and a plain counter is enough.
std::atomic<uint32_t> next_xact_handle{1};

std::mutex xact_mutex;
std::unordered_map<uint32_t, std::unique_ptr<xe::kernel::xna::XactSoundBank>>
    sound_banks;
std::unordered_map<uint32_t, std::unique_ptr<xe::kernel::xna::XactWaveBank>>
    wave_banks;

struct XactLiveCue {
  uint32_t sound_bank = 0;
  uint32_t cue_index = 0;
  uint32_t state = 0;
  // XACT variables are per instance, and a title that sets one expects to read
  // the same value back - so they are stored rather than discarded.
  std::unordered_map<std::string, float> variables;
};
std::unordered_map<uint32_t, XactLiveCue> live_cues;

// A category is named once with GetCategory and referred to by index after
// that, so the only thing the index has to be is CONSISTENT: the same name must
// always give the same number, and volume, pause and stop must land on the same
// category the title asked for. Indices are assigned on first sight, which
// holds until the global settings file is parsed and can name them itself.
struct XactCategory {
  std::string name;
  float volume = 1.0f;
  bool paused = false;
};
std::vector<XactCategory> categories;
std::unordered_map<std::string, uint16_t> category_indices;
// Engine-wide variables, the same deal as the per-cue ones.
std::unordered_map<std::string, float> global_variables;

std::unique_ptr<xe::kernel::xna::XactGlobalSettings> global_settings;

uint16_t CategoryIndex(const std::string& name) {
  auto found = category_indices.find(name);
  if (found != category_indices.end()) {
    return found->second;
  }
  // THE SETTINGS FILE OWNS THE NUMBERING. Assigning indices in the order the
  // title happens to ask for names produced ids unrelated to the ones the
  // sound records carry, so a stop aimed at Music landed somewhere else.
  uint16_t index;
  const uint32_t real =
      global_settings ? global_settings->FindCategory(name) : UINT32_MAX;
  if (real != UINT32_MAX) {
    index = static_cast<uint16_t>(real);
    if (categories.size() <= index) {
      categories.resize(static_cast<size_t>(index) + 1);
    }
    categories[index].name = name;
  } else {
    // A name the settings file does not carry still needs a stable id, and it
    // must not collide with a real one.
    index = static_cast<uint16_t>(std::max<size_t>(
        categories.size(),
        global_settings ? global_settings->category_names().size() : 0));
    if (categories.size() <= index) {
      categories.resize(static_cast<size_t>(index) + 1);
    }
    categories[index].name = name;
    XELOGW("[xna] XACT: category \"{}\" is not in the settings file", name);
  }
  category_indices[name] = index;
  return index;
}

// XACT_CUESTATE, read out of Cue::get_IsStopped and its siblings - each
// property is a single bit test against the value GetState returns.
constexpr uint32_t kCueCreated = 1;
constexpr uint32_t kCuePrepared = 4;
constexpr uint32_t kCuePlaying = 8;
constexpr uint32_t kCueStopped = 32;
constexpr uint32_t kCuePaused = 64;

// A bitmask: a cue stays Created and Prepared, so playing/stopped is set
// without dropping those.
constexpr uint32_t kCueResident = kCueCreated | kCuePrepared;
constexpr uint32_t kCuePlayingState = kCueResident | kCuePlaying;
constexpr uint32_t kCueStoppedState = kCueResident | kCueStopped;

// WaveBank::get_IsPrepared tests bit 4; get_IsInUse tests bit 128.
constexpr uint32_t kBankPrepared = 4;

// A managed String argument, which arrives as ANSI - one byte per character.
//
// NOT UTF-16, despite StringBuilder on the same boundary coming back wide. That
// asymmetry is real and both halves were measured: an out StringBuilder had to
// be written as UTF-16 or the effect parameter names came back as CJK, while an
// in String read as UTF-16 turns a file path into question marks. Reading these
// bytes in pairs gave 59 unprintable pairs and a trailing 's' - which is
// exactly ".xgs" plus its terminator read two bytes at a time - and every bank
// then failed to open.
//
// The count is in CHARACTERS, which for ANSI is also bytes.
std::string FromManagedString(const void* text, int32_t length) {
  if (!text || length <= 0) {
    return std::string();
  }
  const auto* bytes = static_cast<const char*>(text);
  std::string out;
  out.reserve(static_cast<size_t>(length));
  for (int32_t i = 0; i < length; ++i) {
    if (!bytes[i]) {
      break;
    }
    out.push_back(bytes[i]);
  }
  return out;
}

}  // namespace

extern "C" uint32_t xna_Audio_XActCue_Play(uint32_t cue);

extern "C" uint32_t xna_Audio_XActEngine_CreateAndInitEngine(
    const void* settings, int32_t length, uint32_t flags) {
  const std::string path = FromManagedString(settings, length);
  const std::string resolved = xe::kernel::xna::XnaResolveTitlePath(path);
  const uint32_t handle = next_xact_handle.fetch_add(1);
  if (resolved.empty()) {
    XELOGW("[xna] XACT engine {}: \"{}\" is not in the package", handle, path);
  } else {
    XELOGI("[xna] XACT engine {} from package \"{}\"", handle, resolved);
  }
  // The settings file names the categories, and the sound records index that
  // same table. Without it the numbering is invented here and cannot agree
  // with the banks.
  {
    auto settings_file =
        std::make_unique<xe::kernel::xna::XactGlobalSettings>();
    if (settings_file->Load(path)) {
      std::lock_guard<std::mutex> lock(xact_mutex);
      global_settings = std::move(settings_file);
    }
  }
  return handle;
}

extern "C" uint32_t xna_Audio_XActEngine_CreateSoundBank(uint32_t engine,
                                                         const void* name,
                                                         int32_t length) {
  const std::string path = FromManagedString(name, length);
  auto bank = std::make_unique<xe::kernel::xna::XactSoundBank>();
  if (!bank->Load(path)) {
    // The caller compares against 0xFFFFFFFF and raises
    // InvalidOperationException, which is the truth if the bank cannot be read.
    return UINT32_MAX;
  }
  const uint32_t handle = next_xact_handle.fetch_add(1);
  std::lock_guard<std::mutex> lock(xact_mutex);
  sound_banks[handle] = std::move(bank);
  return handle;
}

extern "C" uint32_t xna_Audio_XActEngine_CreateInMemoryWaveBank(
    uint32_t engine, const void* name, int32_t length) {
  const std::string path = FromManagedString(name, length);
  auto bank = std::make_unique<xe::kernel::xna::XactWaveBank>();
  if (!bank->Load(path)) {
    return UINT32_MAX;
  }
  const uint32_t handle = next_xact_handle.fetch_add(1);
  std::lock_guard<std::mutex> lock(xact_mutex);
  wave_banks[handle] = std::move(bank);
  return handle;
}

// GetCue(soundBank, name, length, out cue) - an ERROR result: it goes through
// ThrowExceptionFromErrorCode, so anything but zero throws inside the title.
extern "C" uint32_t xna_Audio_XActSoundBank_GetCue(uint32_t sound_bank,
                                                   const void* name,
                                                   int32_t length,
                                                   uint32_t* cue_out) {
  const std::string cue_name = FromManagedString(name, length);
  std::lock_guard<std::mutex> lock(xact_mutex);
  auto found = sound_banks.find(sound_bank);
  if (found == sound_banks.end() || !cue_out) {
    return 0x80070057;  // E_INVALIDARG
  }
  const uint32_t index = found->second->FindCue(cue_name);
  if (index == UINT32_MAX) {
    XELOGW("[xna] XACT has no cue named \"{}\"", cue_name);
    return 0x80070057;
  }
  const uint32_t handle = next_xact_handle.fetch_add(1);
  live_cues[handle] = {sound_bank, index, kCueCreated | kCuePrepared};
  *cue_out = handle;

  const auto* cue = found->second->cue(index);
  XELOGD("[xna] XACT cue \"{}\" -> handle {} (wave bank {} entry {}{})",
         cue_name, handle, cue->wave_bank, cue->wave_entry,
         cue->resolved ? "" : ", unresolved");
  return 0;
}

extern "C" uint32_t xna_Audio_XActSoundBank_PlayCue(uint32_t sound_bank,
                                                    const void* name,
                                                    int32_t length) {
  uint32_t cue = 0;
  const uint32_t result =
      xna_Audio_XActSoundBank_GetCue(sound_bank, name, length, &cue);
  if (result) {
    return result;
  }
  return xna_Audio_XActCue_Play(cue);
}

static const char* CueStateName(uint32_t state) {
  if (state & kCuePlaying) {
    return "Playing";
  }
  if (state & kCuePaused) {
    return "Paused";
  }
  if (state & kCueStopped) {
    return "Stopped";
  }
  if (state & kCuePrepared) {
    return "Prepared";
  }
  return "Created";
}

static void SetCueState(uint32_t cue, XactLiveCue* live, uint32_t state,
                        const char* why) {
  if (live->state == state) {
    return;
  }
  XELOGI("[xna] XACT cue {}: {} -> {} ({})", cue, CueStateName(live->state),
         CueStateName(state), why);
  live->state = state;
}

static uint32_t CueCategory(uint32_t cue) {
  auto found = live_cues.find(cue);
  if (found == live_cues.end()) {
    return UINT32_MAX;
  }
  auto bank = sound_banks.find(found->second.sound_bank);
  if (bank == sound_banks.end()) {
    return UINT32_MAX;
  }
  const auto* entry = bank->second->cue(found->second.cue_index);
  return entry ? entry->category : UINT32_MAX;
}

namespace {

std::unordered_map<uint64_t, uint32_t> variation_last;
std::mt19937 variation_random{std::random_device{}()};

const xe::kernel::xna::XactWaveRef& PickVariation(
    uint32_t sound_bank, uint32_t cue_index,
    const xe::kernel::xna::XactCue& cue) {
  const uint32_t count = uint32_t(cue.variations.size());
  if (count == 1) {
    return cue.variations[0];
  }
  const uint64_t key = (uint64_t(sound_bank) << 32) | cue_index;
  auto last = variation_last.find(key);
  uint32_t next = 0;
  if (cue.variation_mode <= 1) {
    next = last == variation_last.end() ? 0 : (last->second + 1) % count;
  } else {
    next =
        std::uniform_int_distribution<uint32_t>(0, count - 1)(variation_random);
    if (last != variation_last.end() && next == last->second) {
      next = (next + 1) % count;
    }
  }
  variation_last[key] = next;
  return cue.variations[next];
}

xe::kernel::xna::XnaAudioFormat WaveBankFormat(
    const xe::kernel::xna::XactWaveEntry& entry) {
  static constexpr uint32_t kWmaBlockAligns[] = {
      929,  1487, 1280, 2230, 8917, 8192, 4459, 5945, 2304,
      1536, 1485, 1008, 2731, 4096, 6827, 5462, 1280};
  static constexpr uint32_t kWmaAvgBytes[] = {12000, 24000, 4000, 6000,
                                              8000,  20000, 2500};
  xe::kernel::xna::XnaAudioFormat format;
  format.channels = entry.channels;
  format.sample_rate = entry.PlaybackRate();
  format.big_endian = true;
  switch (entry.format_tag) {
    case 0:
      format.codec = xe::kernel::xna::kXnaAudioPcm;
      format.bits_per_sample = entry.bits_per_sample ? 16 : 8;
      format.block_align = entry.block_align;
      break;
    case 1:
      format.codec = xe::kernel::xna::kXnaAudioXma;
      break;
    case 2:
      format.codec = xe::kernel::xna::kXnaAudioAdpcm;
      break;
    default: {
      const uint32_t align_index = entry.block_align & 0x1F;
      const uint32_t rate_index = entry.block_align >> 5;
      format.codec = xe::kernel::xna::kXnaAudioWma;
      format.bits_per_sample = 16;
      format.block_align = align_index < std::size(kWmaBlockAligns)
                               ? kWmaBlockAligns[align_index]
                               : 2230;
      format.avg_bytes_per_second = rate_index < std::size(kWmaAvgBytes)
                                        ? kWmaAvgBytes[rate_index]
                                        : 6000;
      break;
    }
  }
  return format;
}

}  // namespace

extern "C" uint32_t xna_Audio_XActCue_Play(uint32_t cue) {
  std::lock_guard<std::mutex> lock(xact_mutex);
  auto found = live_cues.find(cue);
  if (found == live_cues.end()) {
    return 0x80070057;
  }
  auto bank = sound_banks.find(found->second.sound_bank);
  if (bank == sound_banks.end()) {
    return 0x80070057;
  }
  const auto* entry = bank->second->cue(found->second.cue_index);
  if (!entry || !entry->resolved || entry->variations.empty()) {
    XELOGW("[xna] XACT cue \"{}\" resolves to no wave",
           entry ? entry->name : std::string());
    SetCueState(cue, &found->second, kCueStoppedState, "unresolved cue");
    return 0;
  }
  const auto& ref =
      PickVariation(found->second.sound_bank, found->second.cue_index, *entry);
  const xe::kernel::xna::XactWaveBank* wave = nullptr;
  const std::string& wanted = bank->second->wave_bank_name(ref.wave_bank);
  if (!wanted.empty()) {
    for (const auto& pair : wave_banks) {
      if (pair.second->name() == wanted) {
        wave = pair.second.get();
        break;
      }
    }
  }
  if (!wave) {
    uint32_t seen = 0;
    for (const auto& pair : wave_banks) {
      if (seen++ == ref.wave_bank) {
        wave = pair.second.get();
        break;
      }
    }
  }
  if (!wave) {
    XELOGW(
        "[xna] XACT cue \"{}\" wants wave bank {} \"{}\", which is not "
        "loaded",
        entry->name, ref.wave_bank, wanted);
    SetCueState(cue, &found->second, kCueStoppedState, "wave bank missing");
    return 0;
  }
  const auto* wave_entry = wave->entry(ref.wave_entry);
  uint32_t bytes = 0;
  const uint8_t* samples = wave->EntryData(ref.wave_entry, &bytes);
  if (!wave_entry || !samples) {
    XELOGW("[xna] XACT cue \"{}\" names wave entry {}, which is not in \"{}\"",
           entry->name, ref.wave_entry, wave->name());
    SetCueState(cue, &found->second, kCueStoppedState, "wave entry missing");
    return 0;
  }

  const auto format = WaveBankFormat(*wave_entry);
  SetCueState(cue, &found->second, kCuePlayingState, "Play");
  XELOGI(
      "[xna] XACT play \"{}\": {} entry {}, {} byte(s), codec {}, {} "
      "channel(s) at {} Hz, category {}",
      entry->name, wave->name(), ref.wave_entry, bytes, format.codec,
      format.channels, format.sample_rate, ref.category);
  if (!xe::kernel::xna::XactPlayWave(cue, ref.category, samples, bytes, format,
                                     1.0f)) {
    // Said out loud: a cue the title believes is playing but that produced no
    // samples is worth knowing about, and returning success here would hide it.
    XELOGW("[xna] XACT could not decode \"{}\"", entry->name);
    SetCueState(cue, &found->second, kCueStoppedState, "decode failed");
  }
  return 0;
}

extern "C" uint32_t xna_Audio_XActCue_Stop(uint32_t cue, uint32_t options) {
  XELOGI("[xna] XACT: the title called Cue.Stop on cue {} (options {:08X})",
         cue, options);
  xe::kernel::xna::XactStopCue(cue);
  std::lock_guard<std::mutex> lock(xact_mutex);
  auto found = live_cues.find(cue);
  if (found != live_cues.end()) {
    SetCueState(cue, &found->second, kCueStoppedState, "Cue.Stop");
  }
  return 0;
}

// Binds to Cue::ReleaseHandle, not a stop - Dispose() releases the handle and
// a fire-and-forget cue keeps playing.
extern "C" uint32_t xna_Audio_XActCue_Destroy(uint32_t cue) {
  XELOGI("[xna] XACT: the title released the handle for cue {}", cue);
  xe::kernel::xna::XactDetachCue(cue);
  std::lock_guard<std::mutex> lock(xact_mutex);
  live_cues.erase(cue);
  return 0;
}

// NOT an error code - Cue::get_IsPlaying and the rest each test one bit of it.
//
// A CUE HAS TO STOP ON ITS OWN. Nothing notifies anyone when a wave runs out:
// XNA polls IsPlaying, which is one bit of this. Answering from a flag set at
// Play time leaves a finished cue reading as playing forever, and a title that
// waits for a sound before moving on waits for good - so the mixer is asked
// whether it still has samples for this cue.
extern "C" uint32_t xna_Audio_XActCue_GetState(uint32_t cue) {
  std::lock_guard<std::mutex> lock(xact_mutex);
  auto found = live_cues.find(cue);
  if (found == live_cues.end()) {
    return kCueStopped;
  }
  if ((found->second.state & kCuePlaying) &&
      !xe::kernel::xna::XactIsCuePlaying(cue)) {
    SetCueState(cue, &found->second, kCueStoppedState, "samples exhausted");
  }
  return found->second.state;
}

// The bank is fully parsed by the time this can be asked, so PREPARED is the
// honest answer and a title waiting on it proceeds.
extern "C" uint32_t xna_Audio_XActWaveBank_GetState(uint32_t wave_bank) {
  std::lock_guard<std::mutex> lock(xact_mutex);
  return wave_banks.count(wave_bank) ? kBankPrepared : 0;
}

extern "C" uint32_t xna_Audio_XActSoundBank_GetState(uint32_t sound_bank) {
  std::lock_guard<std::mutex> lock(xact_mutex);
  return sound_banks.count(sound_bank) ? kBankPrepared : 0;
}

extern "C" uint32_t xna_Audio_XActWaveBank_Destroy(uint32_t wave_bank) {
  std::lock_guard<std::mutex> lock(xact_mutex);
  wave_banks.erase(wave_bank);
  return 0;
}

extern "C" uint32_t xna_Audio_XActSoundBank_Destroy(uint32_t sound_bank) {
  std::lock_guard<std::mutex> lock(xact_mutex);
  sound_banks.erase(sound_bank);
  return 0;
}

extern "C" uint32_t xna_Audio_XActEngine_Release(uint32_t engine) { return 0; }

// ---- media queue ------------------------------------------------------------
//
// MediaPlayer, which is how a title plays its music. Every one of these was a
// generated default, and that is what hung Arcadecraft: MediaPlayer.State reads
// Media_Queue_GetPlayState, the default answered Stopped forever, and a title
// that plays a song and waits for Playing waits for good. The thread sat in
// WaitSleepJoin with the log ending on "Media_Queue_GetPlayState answered with
// a default".
//
// So the queue keeps real state. No audio comes out of it yet - that is the
// same sample delivery XACT is waiting on - but the state machine is honest:
// what the title sets is what it reads back, and a song that was started
// reports as playing until something stops it.

namespace {

// Microsoft.Xna.Framework.Media.MediaState.
constexpr uint32_t kMediaStopped = 0;
constexpr uint32_t kMediaPlaying = 1;
constexpr uint32_t kMediaPaused = 2;

std::mutex media_mutex;
uint32_t media_state = kMediaStopped;
int32_t media_song_count = 0;
int32_t media_active_song = 0;
int32_t media_play_position = 0;
float media_volume = 1.0f;
bool media_muted = false;
bool media_repeat = false;
bool media_shuffle = false;
bool media_visualization = false;

struct SongRecord {
  std::string name;
  std::string path;
  int32_t duration_ms = 0;
};
std::unordered_map<uint32_t, SongRecord> songs;
uint32_t next_song_handle = 0x10000;
uint32_t media_current_song = 0;
const uint64_t kMusicOwner =
    xe::kernel::xna::XnaVoiceOwner(xe::kernel::xna::kXnaVoiceMusic, 1);

float MusicVolume() { return media_muted ? 0.0f : media_volume; }

}  // namespace

extern "C" uint32_t xna_MEDIA_Media_Song_CreateHandle(
    const char* name, uint32_t name_length, const char* path,
    uint32_t path_length, int32_t duration, uint32_t* handle_out) {
  if (!handle_out) {
    return 0x80070057;
  }
  SongRecord record;
  record.name = FromManagedString(name, int32_t(name_length));
  record.path = FromManagedString(path, int32_t(path_length));
  record.duration_ms = duration;
  std::string resolved = xe::kernel::xna::XnaResolveTitlePath(record.path);
  if (resolved.empty()) {
    resolved = xe::kernel::xna::XnaResolveTitlePath(record.path + ".wma");
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  const uint32_t handle = next_song_handle++;
  if (resolved.empty()) {
    XELOGW("[xna] Song {:08X}: \"{}\" - \"{}\" is not in the package", handle,
           record.name, record.path);
  } else {
    XELOGI("[xna] Song {:08X}: \"{}\" from package \"{}\", {} ms", handle,
           record.name, resolved, duration);
  }
  songs[handle] = std::move(record);
  *handle_out = handle;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Song_GetDuration(uint32_t song,
                                                     int32_t* out) {
  if (!out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  auto found = songs.find(song);
  *out = found != songs.end() ? found->second.duration_ms : 0;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Release(uint32_t item) {
  std::lock_guard<std::mutex> lock(media_mutex);
  songs.erase(item);
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_GetPlayState(uint32_t* state_out) {
  if (!state_out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  if (media_state == kMediaPlaying && media_current_song &&
      songs.count(media_current_song) &&
      xe::kernel::xna::XnaVoiceQuery(kMusicOwner, nullptr) ==
          xe::kernel::xna::XnaVoiceState::kNone) {
    media_state = kMediaStopped;
  }
  *state_out = media_state;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_PlaySong(uint32_t song) {
  std::lock_guard<std::mutex> lock(media_mutex);
  media_state = kMediaPlaying;
  media_song_count = 1;
  media_active_song = 0;
  media_play_position = 0;
  media_current_song = song;
  auto found = songs.find(song);
  if (found == songs.end()) {
    XELOGW("[xna] MediaPlayer: song {:08X} has no file behind it", song);
    return 0;
  }
  const std::string path = found->second.path;
  xe::kernel::xna::XnaVoiceParams params;
  params.volume = MusicVolume();
  params.loop = media_repeat;
  std::string resolved = xe::kernel::xna::XnaResolveTitlePath(path);
  if (resolved.empty()) {
    resolved = xe::kernel::xna::XnaResolveTitlePath(path + ".wma");
  }
  if (resolved.empty()) {
    XELOGW("[xna] MediaPlayer: \"{}\" is not in the package", path);
  } else {
    XELOGI("[xna] MediaPlayer: playing \"{}\" from package \"{}\"",
           found->second.name, resolved);
  }
  xe::kernel::xna::XnaVoiceStartAsync(
      kMusicOwner,
      [path](uint32_t* sample_rate) -> xe::kernel::xna::XnaSamples {
        std::vector<uint8_t> file;
        if (!xe::kernel::xna::XnaReadTitleFile(path, &file) &&
            !xe::kernel::xna::XnaReadTitleFile(path + ".wma", &file)) {
          XELOGW("[xna] MediaPlayer: no \"{}\" in the package", path);
          return nullptr;
        }
        return xe::kernel::xna::XnaDecodeMediaFile(file, sample_rate);
      },
      params);
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_PlaySongList(uint32_t songs,
                                                       int32_t index) {
  std::lock_guard<std::mutex> lock(media_mutex);
  media_state = kMediaPlaying;
  media_active_song = index;
  media_play_position = 0;
  if (media_song_count <= index) {
    media_song_count = index + 1;
  }
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_Stop() {
  std::lock_guard<std::mutex> lock(media_mutex);
  media_state = kMediaStopped;
  media_play_position = 0;
  xe::kernel::xna::XnaVoiceStop(kMusicOwner);
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_Pause() {
  std::lock_guard<std::mutex> lock(media_mutex);
  if (media_state == kMediaPlaying) {
    media_state = kMediaPaused;
    xe::kernel::xna::XnaVoiceSetPaused(kMusicOwner, true);
  }
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_Resume() {
  std::lock_guard<std::mutex> lock(media_mutex);
  if (media_state == kMediaPaused) {
    media_state = kMediaPlaying;
    xe::kernel::xna::XnaVoiceSetPaused(kMusicOwner, false);
  }
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_MoveNext() {
  std::lock_guard<std::mutex> lock(media_mutex);
  if (media_song_count > 0) {
    media_active_song = (media_active_song + 1) % media_song_count;
  }
  media_play_position = 0;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_MovePrev() {
  std::lock_guard<std::mutex> lock(media_mutex);
  if (media_song_count > 0) {
    media_active_song =
        (media_active_song + media_song_count - 1) % media_song_count;
  }
  media_play_position = 0;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_MoveTo(int32_t index) {
  std::lock_guard<std::mutex> lock(media_mutex);
  media_active_song = index;
  media_play_position = 0;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_GetActiveSongIndex(int32_t* out) {
  if (!out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  *out = media_active_song;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_GetSongCount(int32_t* out) {
  if (!out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  *out = media_song_count;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_GetPlayPosition(int32_t* out) {
  if (!out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  double seconds = 0.0;
  if (xe::kernel::xna::XnaVoiceQuery(kMusicOwner, &seconds) !=
      xe::kernel::xna::XnaVoiceState::kNone) {
    media_play_position = int32_t(seconds * 1000.0);
  }
  *out = media_play_position;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_GetVolume(float* out) {
  if (!out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  *out = media_volume;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_SetVolume(float volume) {
  std::lock_guard<std::mutex> lock(media_mutex);
  media_volume = volume;
  xe::kernel::xna::XnaVoiceSetVolume(kMusicOwner, MusicVolume());
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_IsMuted(uint32_t* out) {
  if (!out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  *out = media_muted ? 1 : 0;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_SetMute(uint32_t muted) {
  std::lock_guard<std::mutex> lock(media_mutex);
  media_muted = muted != 0;
  xe::kernel::xna::XnaVoiceSetVolume(kMusicOwner, MusicVolume());
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_GetRepeat(uint32_t* out) {
  if (!out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  *out = media_repeat ? 1 : 0;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_SetRepeat(uint32_t repeat) {
  std::lock_guard<std::mutex> lock(media_mutex);
  media_repeat = repeat != 0;
  xe::kernel::xna::XnaVoiceSetLoop(kMusicOwner, media_repeat);
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_GetShuffle(uint32_t* out) {
  if (!out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  *out = media_shuffle ? 1 : 0;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_SetShuffle(uint32_t shuffle) {
  std::lock_guard<std::mutex> lock(media_mutex);
  media_shuffle = shuffle != 0;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_IsVisualizationEnabled(
    uint32_t* out) {
  if (!out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  *out = media_visualization ? 1 : 0;
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Queue_EnableVisualization(
    uint32_t enabled) {
  std::lock_guard<std::mutex> lock(media_mutex);
  media_visualization = enabled != 0;
  return 0;
}

// A song is identified by an opaque handle the title only ever hands back to
// this API, so an index-derived one is a real identity rather than a stand-in.
// One-based, so zero stays "no song". Out of range fails, which is what ends a
// caller's enumeration.
extern "C" uint32_t xna_MEDIA_Media_Queue_GetSongAtIndex(int32_t index,
                                                         uint32_t* song_out) {
  if (!song_out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(media_mutex);
  if (index < 0 || index >= media_song_count) {
    return 0x80070057;
  }
  *song_out = index == 0 && media_current_song
                  ? media_current_song
                  : static_cast<uint32_t>(index) + 1;
  return 0;
}

// Visualization is the frequency and sample data behind a music visualiser.
// Nothing is being mixed here yet, so the honest answer is silence - zeroed
// buffers - rather than leaving the title's arrays holding whatever they held.
// IsVisualizationEnabled reports what the title set, so a visualiser that asks
// first will already know there is nothing to draw.
extern "C" uint32_t xna_MEDIA_Media_Queue_GetVisualizationData(
    float* frequencies, int32_t frequency_count, float* samples,
    int32_t sample_count) {
  if (frequencies && frequency_count > 0) {
    std::fill(frequencies, frequencies + frequency_count, 0.0f);
  }
  if (samples && sample_count > 0) {
    std::fill(samples, samples + sample_count, 0.0f);
  }
  return 0;
}

// A console always reports exactly one media source: its own local library.
//
// The record layout is not a guess - MediaSource::.ctor reads a 32-bit
// MediaSourceType at offset 0 and then PtrToStringUni for the name, and
// GetAvailableMediaSources allocates count * 132 bytes and walks it with the
// same stride. A zeroed record is therefore a valid LocalDevice source with an
// empty name, and XNA substitutes the localised local-library name itself when
// the name is empty and the type is zero.
static constexpr uint32_t kMediaSourceStride = 132;

extern "C" uint32_t xna_MEDIA_Media_Library_GetMediaSourceCount(
    int32_t* count) {
  if (count) {
    *count = 1;
  }
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Library_GetMediaSources(uint8_t* buffer,
                                                            uint32_t byte_count,
                                                            int32_t count) {
  if (!buffer || count <= 0) {
    return 0;
  }
  const uint64_t needed = uint64_t(count) * kMediaSourceStride;
  if (byte_count < needed) {
    return 0x8007007A;  // ERROR_INSUFFICIENT_BUFFER
  }
  std::memset(buffer, 0, static_cast<size_t>(needed));
  return 0;
}

// The game always has control here - nothing else on this host is playing the
// user's own music, which is what this asks about.
struct XDeviceData {
  uint32_t device_id;
  uint32_t device_type;
  uint64_t device_bytes;
  uint64_t device_free_bytes;
  uint64_t ignored;
};

// Reads one file out of the mounted STFS package. With no buffer it reports the
// size, so the caller can allocate exactly. Returns bytes read, or 0 when the
// package holds no such file - the title asks for optional files and a miss is
// an ordinary answer, not a failure.
extern "C" uint32_t Nexia_XnaReadTitleFile(const char* path, uint8_t* buffer,
                                           uint32_t capacity) {
  if (!path) {
    return 0;
  }
  std::vector<uint8_t> contents;
  if (!xe::kernel::xna::XnaReadTitleFile(std::string(path), &contents)) {
    return 0;
  }
  if (!buffer) {
    return static_cast<uint32_t>(contents.size());
  }
  const uint32_t copied =
      std::min<uint32_t>(capacity, static_cast<uint32_t>(contents.size()));
  std::memcpy(buffer, contents.data(), copied);
  return copied;
}

extern "C" uint32_t Nexia_XnaTitleDirectoryExists(const char* path) {
  return path && xe::kernel::xna::XnaTitleDirectoryExists(std::string(path))
             ? 1
             : 0;
}

extern "C" uint32_t Nexia_XnaIsTitlePath(const char* path) {
  return path && xe::kernel::xna::XnaIsTitlePath(std::string(path)) ? 1 : 0;
}

extern "C" uint32_t Nexia_XnaStatTitlePath(const char* path, uint64_t* size,
                                           uint32_t* is_directory) {
  uint64_t bytes = 0;
  bool directory = false;
  if (!path || !xe::kernel::xna::XnaStatTitlePath(std::string(path), &bytes,
                                                  &directory)) {
    return 0;
  }
  if (size) {
    *size = bytes;
  }
  if (is_directory) {
    *is_directory = directory ? 1 : 0;
  }
  return 1;
}

extern "C" uint32_t Nexia_XnaListTitleDirectory(const char* path,
                                                uint32_t directories,
                                                char* buffer,
                                                uint32_t capacity) {
  if (!path) {
    return 0;
  }
  std::vector<std::string> names;
  if (!xe::kernel::xna::XnaListTitleDirectory(std::string(path),
                                              directories != 0, &names)) {
    return 0;
  }
  std::string joined;
  for (const auto& name : names) {
    joined += name;
    joined.push_back('\n');
  }
  const uint32_t needed = static_cast<uint32_t>(joined.size()) + 1;
  if (!buffer) {
    return needed;
  }
  const uint32_t copied = std::min<uint32_t>(capacity, needed);
  if (copied) {
    std::memcpy(buffer, joined.c_str(), copied);
    buffer[copied - 1] = '\0';
  }
  return copied;
}

extern "C" uint32_t Nexia_XnaStorageRoot(uint32_t player, char* buffer,
                                         uint32_t capacity) {
  if (!buffer || capacity < 2) {
    return 0;
  }
  auto* state = xe::kernel::kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  if (!emulator) {
    return 0;
  }

  uint64_t xuid = 0;
  if (state->xam_state() && state->xam_state()->profile_manager()) {
    const uint8_t slot = player == 0xFF ? 0 : static_cast<uint8_t>(player);
    auto* profile = state->xam_state()->profile_manager()->GetProfile(slot);
    if (profile) {
      xuid = profile->xuid();
    }
  }

  const uint32_t title_id = emulator->title_id();
  std::filesystem::path root = emulator->content_root() /
                               fmt::format("{:016X}", xuid) /
                               fmt::format("{:08X}", title_id) / "00000001";
  if (player == 0xFF) {
    root /= "AllPlayers";
  } else {
    root /= fmt::format("Player{}", player);
  }

  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) {
    XELOGE("[xna] storage: could not create {}", root.string());
    return 0;
  }

  std::string text = root.string();
  if (!text.empty() && text.back() != '\\' && text.back() != '/') {
    text.push_back('\\');
  }
  if (text.size() + 1 > capacity) {
    return 0;
  }
  std::memcpy(buffer, text.data(), text.size());
  buffer[text.size()] = '\0';
  return static_cast<uint32_t>(text.size());
}

extern "C" uint32_t xna_STORAGE_STORAGE_CloseContent(uint32_t player) {
  std::lock_guard<std::mutex> lock(async_mutex);
  auto found = open_containers.find(player);
  if (found == open_containers.end()) {
    return 0;
  }
  XELOGI("[xna] storage: container \"{}\" closed for player {}", found->second,
         player);
  open_containers.erase(found);
  return 0;
}

extern "C" uint32_t xna_STORAGE_STORAGE_GetStorageData(uint32_t device,
                                                       void* out_data) {
  if (!device || device != kStorageDeviceHandle) {
    return 1;
  }
  if (out_data) {
    uint64_t capacity = 16ull * 1024 * 1024 * 1024;
    uint64_t available = capacity;
    const std::string root = xe::kernel::xna::XnaTitlePath();
    if (!root.empty()) {
      std::error_code error;
      const auto space = std::filesystem::space(
          std::filesystem::path(root).parent_path(), error);
      if (!error && space.capacity) {
        capacity = space.capacity;
        available = space.available;
      }
    }
    auto* data = static_cast<XDeviceData*>(out_data);
    data->device_id = device;
    data->device_type = 1;
    data->device_bytes = capacity;
    data->device_free_bytes = available;
    data->ignored = 0;
  }
  return 0;
}

extern "C" uint32_t xna_MEDIA_Media_Player_GameHasControl(uint32_t* out) {
  if (out) {
    *out = 1;
  }
  return 0;
}

// No media events ever fire, which is the truth: nothing here changes the
// user's playback from underneath the title.
extern "C" uint32_t xna_MEDIA_Media_Player_CheckForEvents(uint32_t* a,
                                                          uint32_t* b) {
  if (a) {
    *a = 0;
  }
  if (b) {
    *b = 0;
  }
  return 0;
}

// A streaming wave bank is the same file read the same way - the streaming part
// is how the console fed it to the hardware, not a different container - so it
// loads through the same parser. The extra arguments are the offset and packet
// size the console DVD reader wanted, which a host file does not need.
// CreateStreamingHandle(engine, path, path.Length, offset, packetSize) - the
// LENGTH comes second, not last. Read off the call in WaveBank..ctor:
//
//     ldloc.0                 // path
//     ldloc.0; get_Length()   // length
//     ldarg.3                 // offset
//     ldarg.s packetsize      // packet size
//
// Taking the last argument as the length truncated every path to the packet
// size - "F:\Xenia", exactly eight characters - and the bank never opened.
extern "C" uint32_t xna_Audio_XActEngine_CreateStreamingWaveBank(
    uint32_t engine, const void* name, uint32_t length, uint32_t offset,
    int16_t packet_size) {
  const std::string path =
      FromManagedString(name, static_cast<int32_t>(length));
  auto bank = std::make_unique<xe::kernel::xna::XactWaveBank>();
  if (!bank->Load(path)) {
    return UINT32_MAX;
  }
  const uint32_t handle = next_xact_handle.fetch_add(1);
  std::lock_guard<std::mutex> lock(xact_mutex);
  wave_banks[handle] = std::move(bank);
  return handle;
}

// Called every frame to let the engine advance. There is nothing to advance
// until sample delivery exists, and it returns void, so there is no result to
// misreport.
extern "C" void xna_Audio_XActEngine_DoWork(uint32_t engine) {}

// Categories are identified by index from here on, so the number matters more
// than what it is - see CategoryIndex.
extern "C" uint16_t xna_Audio_XActEngine_GetCategory(uint32_t engine,
                                                     const void* name,
                                                     int32_t length) {
  const std::string category = FromManagedString(name, length);
  std::lock_guard<std::mutex> lock(xact_mutex);
  return CategoryIndex(category);
}

extern "C" uint32_t xna_Audio_XActEngine_SetVolume(uint32_t engine,
                                                   uint16_t category,
                                                   float volume) {
  std::lock_guard<std::mutex> lock(xact_mutex);
  if (category >= categories.size()) {
    return 0x80070057;
  }
  categories[category].volume = volume;
  xe::kernel::xna::XnaVoiceSetCategoryVolume(category, volume);
  return 0;
}

extern "C" uint32_t xna_Audio_XActEngine_Pause(uint32_t engine,
                                               uint16_t category,
                                               uint32_t pause) {
  std::lock_guard<std::mutex> lock(xact_mutex);
  if (category >= categories.size()) {
    return 0x80070057;
  }
  categories[category].paused = pause != 0;
  xe::kernel::xna::XnaVoicePauseCategory(category, pause != 0);
  return 0;
}

extern "C" uint32_t xna_Audio_XActEngine_Stop(uint32_t engine,
                                              uint16_t category,
                                              uint32_t flags) {
  XELOGI("[xna] XACT: the title stopped category {} (flags {:08X})", category,
         flags);
  xe::kernel::xna::XactStopCategory(category);
  std::lock_guard<std::mutex> lock(xact_mutex);
  for (auto& pair : live_cues) {
    if (!(pair.second.state & kCuePlaying)) {
      continue;
    }
    if (CueCategory(pair.first) != category) {
      continue;
    }
    SetCueState(pair.first, &pair.second, kCueStoppedState, "category stop");
  }
  return 0;
}

extern "C" uint32_t xna_Audio_XActEngine_SetGlobalVariable(uint32_t engine,
                                                           const void* name,
                                                           int32_t length,
                                                           float value) {
  std::lock_guard<std::mutex> lock(xact_mutex);
  global_variables[FromManagedString(name, length)] = value;
  return 0;
}

extern "C" uint32_t xna_Audio_XActEngine_GetGlobalVariable(uint32_t engine,
                                                           const void* name,
                                                           int32_t length,
                                                           float* value_out) {
  if (!value_out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(xact_mutex);
  auto found = global_variables.find(FromManagedString(name, length));
  // A variable the title never set reads as zero rather than failing: XACT
  // defines every variable in the global settings, and refusing here would
  // throw inside a getter the title expects to always work.
  *value_out = found != global_variables.end() ? found->second : 0.0f;
  return 0;
}

extern "C" uint32_t xna_Audio_XActCue_SetVariable(uint32_t cue,
                                                  const void* name,
                                                  int32_t length, float value) {
  std::lock_guard<std::mutex> lock(xact_mutex);
  auto found = live_cues.find(cue);
  if (found == live_cues.end()) {
    return 0x80070057;
  }
  found->second.variables[FromManagedString(name, length)] = value;
  return 0;
}

extern "C" uint32_t xna_Audio_XActCue_GetVariable(uint32_t cue,
                                                  const void* name,
                                                  int32_t length,
                                                  float* value_out) {
  if (!value_out) {
    return 0x80070057;
  }
  std::lock_guard<std::mutex> lock(xact_mutex);
  auto found = live_cues.find(cue);
  if (found == live_cues.end()) {
    return 0x80070057;
  }
  auto variable = found->second.variables.find(FromManagedString(name, length));
  *value_out =
      variable != found->second.variables.end() ? variable->second : 0.0f;
  return 0;
}

extern "C" uint32_t xna_Audio_XActCue_Pause(uint32_t cue, uint32_t pause) {
  std::lock_guard<std::mutex> lock(xact_mutex);
  auto found = live_cues.find(cue);
  if (found == live_cues.end()) {
    return 0x80070057;
  }
  xe::kernel::xna::XnaVoiceSetPaused(
      xe::kernel::xna::XnaVoiceOwner(xe::kernel::xna::kXnaVoiceCue, cue),
      pause != 0);
  if (pause) {
    SetCueState(cue, &found->second,
                (found->second.state & ~kCuePlaying) | kCuePaused, "Pause");
  } else {
    SetCueState(cue, &found->second,
                (found->second.state & ~kCuePaused) | kCuePlaying, "Resume");
  }
  return 0;
}

// 3D positioning changes how a cue is mixed. Nothing is mixed yet, so the
// listener and emitter are accepted and have no effect - reported once so it is
// not mistaken for working spatial audio later.
extern "C" uint32_t xna_Audio_XActEngine_Apply3D(uint32_t engine, uint32_t cue,
                                                 const void* listener,
                                                 const void* emitter) {
  static std::once_flag announced;
  std::call_once(announced, []() {
    XELOGW("[xna] XACT 3D positioning is accepted but not applied");
  });
  return 0;
}
