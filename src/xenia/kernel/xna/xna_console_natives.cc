/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_console_natives.h"

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xna/xna_dependencies.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/virtual_file_system.h"

DEFINE_bool(xna_console_natives, true,
            "Hosted XNA titles call the console's own RuntimeHost natives "
            "(input, storage, audio...) instead of Nexia's re-implementations. "
            "RuntimeHost is loaded as a guest DLL; its CLR never starts.",
            "XNA");

namespace xe {
namespace kernel {
namespace xna {

namespace {

constexpr const char* kRuntimeDevice = "\\XnaConsoleRuntime";
// The most registers any native takes. PowerPC passes eight in r3-r10.
constexpr size_t kMaxArgs = 8;

std::mutex queue_mutex;
std::condition_variable queue_ready;
std::deque<std::function<void()>> queue;
object_ref<XHostThread> worker;
std::atomic<XThread*> worker_thread{nullptr};

// Console name -> guest function, built once at load and only read after.
std::unordered_map<std::string, uint32_t> natives;

// THE HOST THE NATIVES ARE GIVEN.
//
// RuntimeHost hands its natives a host object before any of them is called:
// 8A1AF7B8(host) - reached from its CLR host's own startup - runs eight
// per-subsystem setters, each of which QueryInterfaces the host, registers
// its native table through slot 3 (+0xC: iid, table, count), and keeps the
// interface for later. XInput's, for one, keeps it at 8A821254 and asks its
// +0x64 whether input may be read. Without a host that pointer is null and the
// first XInput call jumps to 0.
//
// This host is the least that satisfies them: one COM object in guest memory
// whose methods are guest trampolines into the functions below.
// QueryInterface answers with itself, so every interface shares the vtable;
// registering a table succeeds; every other method fails with E_NOTIMPL and
// leaves its outputs alone, which each caller checks - the input policy
// lookup failing means "no policy", and input is allowed. A method reached
// for the first time says so, which is how to learn what else they want.
constexpr uint32_t kHostSlots = 64;
constexpr uint32_t kInitNatives = 0x8A1AF7B8;  // The 4.0 RuntimeHost's.
// Its prologue, and its call to the XInput setter at +0x7C, so another build
// is never called at this address.
constexpr uint32_t kInitNativesPrologue[] = {
    0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FFA0, 0x7C7F1B78, 0x2B030000};
constexpr uint32_t kInitNativesInputCall = 0x480004B5;  // bl 8A1AFCE8

uint32_t host_trampolines[kHostSlots] = {};
std::atomic<uint64_t> host_slots_reported{0};

void HostQueryInterface(cpu::ppc::PPCContext* context, void*, void*) {
  const uint32_t out = uint32_t(context->r[5]);
  if (out) {
    xe::store_and_swap<uint32_t>(kernel_memory()->TranslateVirtual(out),
                                 uint32_t(context->r[3]));
  }
  context->r[3] = 0;
}

void HostAddRefRelease(cpu::ppc::PPCContext* context, void*, void*) {
  context->r[3] = 1;
}

void HostRegisterNatives(cpu::ppc::PPCContext* context, void*, void*) {
  XELOGI("XnaConsoleNatives: the host registered {} native(s) from {:08X}",
         uint32_t(context->r[6]), uint32_t(context->r[5]));
  context->r[3] = 0;
}

void HostUnimplemented(cpu::ppc::PPCContext* context, void* slot, void*) {
  const uint32_t index = uint32_t(reinterpret_cast<uintptr_t>(slot));
  if (!(host_slots_reported.fetch_or(uint64_t(1) << index) &
        (uint64_t(1) << index))) {
    XELOGW(
        "XnaConsoleNatives: host method +{:02X} is not implemented (called "
        "with {:08X} {:08X} {:08X}, lr {:08X})",
        index * 4, uint32_t(context->r[4]), uint32_t(context->r[5]),
        uint32_t(context->r[6]), uint32_t(context->lr));
  }
  context->r[3] = 0x80004001;  // E_NOTIMPL
}

// Builds the host object in guest memory and returns its address, or 0.
uint32_t CreateHostObject(KernelState* state) {
  auto* backend = state->processor()->backend();
  if (!host_trampolines[0]) {
    for (uint32_t i = 0; i < kHostSlots; ++i) {
      cpu::backend::GuestTrampolineProc proc = HostUnimplemented;
      if (i == 0) {
        proc = HostQueryInterface;
      } else if (i == 1 || i == 2) {
        proc = HostAddRefRelease;
      } else if (i == 3) {
        proc = HostRegisterNatives;
      }
      host_trampolines[i] = backend->CreateGuestTrampoline(
          proc, reinterpret_cast<void*>(uintptr_t(i)), nullptr, true);
      if (!host_trampolines[i]) {
        return 0;
      }
    }
  }
  // The object's first word is its vtable, which follows it.
  auto* memory = state->memory();
  const uint32_t object = memory->SystemHeapAlloc(8 + kHostSlots * 4);
  if (!object) {
    return 0;
  }
  auto* words = memory->TranslateVirtual<uint8_t*>(object);
  xe::store_and_swap<uint32_t>(words, object + 8);
  xe::store_and_swap<uint32_t>(words + 4, 0);
  for (uint32_t i = 0; i < kHostSlots; ++i) {
    xe::store_and_swap<uint32_t>(words + 8 + i * 4, host_trampolines[i]);
  }
  return object;
}

void WorkerMain() {
  worker_thread = XThread::GetCurrentThread();
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      queue_ready.wait(lock, [] { return !queue.empty(); });
      job = std::move(queue.front());
      queue.pop_front();
    }
    job();
  }
}

// Runs `job` on the natives' thread and waits for it. A native that calls
// back in from that thread runs inline rather than deadlocking on itself.
void RunOnWorker(const std::function<void()>& job) {
  if (XThread::IsInThread() &&
      XThread::GetCurrentThread() == worker_thread.load()) {
    job();
    return;
  }
  std::mutex done_mutex;
  std::condition_variable done_ready;
  bool done = false;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    queue.push_back([&] {
      job();
      std::lock_guard<std::mutex> done_lock(done_mutex);
      done = true;
      done_ready.notify_one();
    });
  }
  queue_ready.notify_one();
  std::unique_lock<std::mutex> lock(done_mutex);
  done_ready.wait(lock, [&] { return done; });
}

// A UTF-16BE identifier at `address` inside the image, or empty.
std::string ReadName(const uint8_t* image, uint32_t base, uint32_t size,
                     uint32_t address) {
  if (address < base || address >= base + size) {
    return {};
  }
  std::string name;
  for (uint32_t offset = address - base; offset + 1 < size; offset += 2) {
    const uint16_t c = xe::load_and_swap<uint16_t>(image + offset);
    if (!c) {
      break;
    }
    if (c < 0x20 || c >= 0x7F || name.size() >= 128) {
      return {};
    }
    name.push_back(char(c));
  }
  if (name.size() < 3 || !(std::isalpha(uint8_t(name[0])) || name[0] == '_')) {
    return {};
  }
  return name;
}

// RuntimeHost binds its natives through tables of big-endian
// {name, ordinal, function} records - three of them in the 4.0 runtime, for
// D3D, media/storage/input and audio/gamer services. They are found by shape
// rather than by address, so another runtime build is read the same way.
size_t IndexNatives(const uint8_t* image, uint32_t base, uint32_t size) {
  auto record = [&](uint32_t offset, std::string* name, uint32_t* function) {
    if (offset + 12 > size) {
      return false;
    }
    const uint32_t name_address = xe::load_and_swap<uint32_t>(image + offset);
    const uint32_t ordinal = xe::load_and_swap<uint32_t>(image + offset + 4);
    const uint32_t code = xe::load_and_swap<uint32_t>(image + offset + 8);
    if (ordinal > 0x10000 || code < base || code >= base + size || code & 3) {
      return false;
    }
    *name = ReadName(image, base, size, name_address);
    *function = code;
    return !name->empty();
  };
  size_t tables = 0;
  std::string name;
  uint32_t function = 0;
  for (uint32_t offset = 0; offset + 12 <= size;) {
    uint32_t run = 0;
    while (record(offset + run * 12, &name, &function)) {
      ++run;
    }
    if (run < 3) {
      offset += 4;
      continue;
    }
    for (uint32_t i = 0; i < run; ++i) {
      record(offset + i * 12, &name, &function);
      natives.emplace(name, function);
    }
    XELOGI("XnaConsoleNatives: {} native(s) in the table at {:08X}", run,
           base + offset);
    ++tables;
    offset += run * 12;
  }
  return tables;
}

}  // namespace

std::filesystem::path FindConsoleRuntimeHost() {
  std::error_code ec;
  // Installed beside the console assemblies, if the user put it there.
  const auto installed = XnaOverlayPath() / "console" / "RuntimeHost.xex";
  if (std::filesystem::is_regular_file(installed, ec)) {
    return installed;
  }
  // Otherwise the XNA Indie Player's title update, as the launcher uses it.
  const auto runtime = FindIndiePlayerRuntime();
  if (runtime.empty()) {
    return {};
  }
  const auto candidate = runtime / "RuntimeHost.xex";
  return std::filesystem::is_regular_file(candidate, ec)
             ? candidate
             : std::filesystem::path();
}

XnaConsoleNatives& XnaConsoleNatives::Instance() {
  static XnaConsoleNatives instance;
  return instance;
}

bool XnaConsoleNatives::Load() {
  if (loaded_ || tried_) {
    return loaded_;
  }
  tried_ = true;
  if (!cvars::xna_console_natives) {
    XELOGI("XnaConsoleNatives: off (xna_console_natives = false)");
    return false;
  }
  auto* state = kernel_state();
  if (!state) {
    return false;
  }
  const auto path = FindConsoleRuntimeHost();
  if (path.empty()) {
    XELOGW(
        "XnaConsoleNatives: no RuntimeHost.xex - install the XNA Indie Player "
        "title update, or copy Runtime\\v4.0\\RuntimeHost.xex into "
        "xna\\console");
    return false;
  }

  // A host thread, so title teardown leaves it alone and the next hosted title
  // reuses it; RuntimeHost itself is reloaded per title (see Reset).
  if (!worker) {
    worker = object_ref<XHostThread>(new XHostThread(
        state, 256 * 1024, 0,
        [] {
          WorkerMain();
          return 0;
        },
        state->GetTitleProcess()));
    // It runs guest code, so the debugger must be able to suspend it.
    worker->set_can_debugger_suspend(true);
    worker->set_name("XNA Console Natives");
    worker->Create();
  }

  bool ok = false;
  RunOnWorker([&] {
    auto device = std::make_unique<vfs::HostPathDevice>(
        kRuntimeDevice, path.parent_path(), /*read_only=*/true);
    if (!device->Initialize()) {
      XELOGE("XnaConsoleNatives: cannot mount {}",
             xe::path_to_utf8(path.parent_path()));
      return;
    }
    state->file_system()->RegisterDevice(std::move(device));
    // DllMain runs here, on this thread: the C runtime starts and the static
    // constructors build what the natives lock and read. The CLR host is a
    // separate export, RuntimeHost_CreateInstance, and is never called.
    auto module = state->LoadUserModule(std::string(kRuntimeDevice) +
                                        "\\RuntimeHost.xex");
    if (!module || !module->xex_module()) {
      XELOGE("XnaConsoleNatives: RuntimeHost.xex did not load from {}",
             xe::path_to_utf8(path));
      return;
    }
    // LoadUserModule only reads the file in. This is the half XexLoadImage
    // does next: imports bound and code registered with the processor.
    const X_RESULT finished = state->FinishLoadingUserModule(module, false);
    if (XFAILED(finished)) {
      XELOGE(
          "XnaConsoleNatives: RuntimeHost.xex would not finish loading "
          "({:08X})",
          finished);
      return;
    }
    auto* xex = module->xex_module();
    const uint32_t base = xex->base_address();
    const uint32_t size = xex->image_size();
    const auto* image = state->memory()->TranslateVirtual<const uint8_t*>(base);

    // A hosted title has no executable module, and RuntimeHost's C runtime
    // reads both things only that sets up: _heap_init sizes its heap from the
    // executable's XEX header through XexExecutableModuleHandle, and _mtinit
    // takes a TLS slot from the title process. Under the launcher both come
    // from the launcher. Here RuntimeHost is the closest thing to the title's
    // executable - it carries the same title id - and its header asks for no
    // particular heap size, so the C runtime makes its default one.
    //
    // Attached first: any thread that starts from here on runs every attached
    // DLL's entry point with DLL_THREAD_ATTACH, and an unattached one gets
    // DLL_PROCESS_ATTACH - a second C runtime start-up racing this one. And
    // no dispatch thread: a hosted title never had one, and starting it here
    // is exactly such a thread.
    module->is_attached_ = true;
    if (!state->GetExecutableModule()) {
      state->SetExecutableModule(module, /*start_dispatch_thread=*/false);
      // RuntimeHost declares no TLS, so the title process would have no slots
      // and _mtinit's KeTlsAlloc fails. The launcher declares 64 and no static
      // data (its XEX_HEADER_TLS_INFO), which is what the runtime ran with.
      constexpr uint32_t kLauncherTlsSlots = 64;
      state->SetTitleProcessTlsSlots(kLauncherTlsSlots);
      XELOGI(
          "XnaConsoleNatives: RuntimeHost stands in as the executable "
          "module");
    }

    // DllMain(DLL_PROCESS_ATTACH), called here rather than inside
    // FinishLoadingUserModule so its answer can be read: the C runtime's heap,
    // threads, I/O and static constructors all start in it, and a failure
    // skips the constructors - the natives' locks are then never initialised
    // and the first native that takes one waits forever.
    uint64_t attach_args[] = {module->handle(), 1, 0};
    const uint32_t attached = uint32_t(state->processor()->Execute(
        XThread::GetCurrentThread()->thread_state(), module->entry_point(),
        attach_args, xe::countof(attach_args)));
    XELOGI("XnaConsoleNatives: DllMain(DLL_PROCESS_ATTACH) at {:08X} -> {}",
           module->entry_point(), attached);
    if (attached != 1) {
      // PROBE, for the 4.0 RuntimeHost (base 8A000000) only: what each step
      // of _CRT_INIT leaves behind - the heap handle _heap_init stores, the
      // count _CRT_INIT bumps on success, the onexit table the first C
      // initialiser allocates, and the XInput natives' critical section.
      if (base == 0x8A000000) {
        auto word = [&](uint32_t address) {
          return xe::load_and_swap<uint32_t>(image + (address - base));
        };
        XELOGE(
            "XnaConsoleNatives: C runtime start-up failed: heap {:08X}, "
            "init count {:08X}, onexit table {:08X}, XInput lock type "
            "{:02X}",
            word(0x8A860D80), word(0x8A81EDA0), word(0x8A860A4C),
            image[0x8A82129C - base]);
      }
      return;
    }
    const size_t tables = IndexNatives(image, base, size);
    XELOGI(
        "XnaConsoleNatives: RuntimeHost at {:08X}, {} native(s) in {} "
        "table(s), from {}",
        base, natives.size(), tables, xe::path_to_utf8(path));
    if (natives.empty()) {
      return;
    }

    // Give the natives their host (see CreateHostObject), at the address the
    // 4.0 runtime keeps the step in - checked, never assumed.
    bool known_build =
        kInitNatives >= base && kInitNatives + 0x80 <= base + size;
    for (size_t i = 0; known_build && i < xe::countof(kInitNativesPrologue);
         ++i) {
      known_build =
          xe::load_and_swap<uint32_t>(image + (kInitNatives - base) + i * 4) ==
          kInitNativesPrologue[i];
    }
    known_build = known_build &&
                  xe::load_and_swap<uint32_t>(image + (kInitNatives - base) +
                                              0x7C) == kInitNativesInputCall;
    if (!known_build) {
      XELOGE(
          "XnaConsoleNatives: this RuntimeHost is not the 4.0 build whose "
          "natives' host step is known; not using its natives");
      return;
    }
    const uint32_t host = CreateHostObject(state);
    if (!host) {
      XELOGE("XnaConsoleNatives: could not build the natives' host object");
      return;
    }
    uint64_t init_args[] = {host};
    const uint32_t init_result = uint32_t(state->processor()->Execute(
        XThread::GetCurrentThread()->thread_state(), kInitNatives, init_args,
        xe::countof(init_args)));
    XELOGI("XnaConsoleNatives: natives given their host {:08X} -> {:08X}", host,
           init_result);
    ok = int32_t(init_result) >= 0;
  });
  loaded_ = ok;
  return loaded_;
}

void XnaConsoleNatives::Reset() {
  tried_ = false;
  if (!loaded_) {
    return;
  }
  RunOnWorker([&] {
    loaded_ = false;
    natives.clear();
    if (auto* state = kernel_state()) {
      state->file_system()->UnregisterDevice(kRuntimeDevice);
    }
  });
}

uint32_t XnaConsoleNatives::Find(std::string_view name) const {
  if (!loaded_) {
    return 0;
  }
  auto it = natives.find(std::string(name));
  return it == natives.end() ? 0 : it->second;
}

bool XnaConsoleNatives::Call(uint32_t function, const XnaArg* args,
                             size_t arg_count, uint64_t* out_result) {
  if (!loaded_ || !function || arg_count > kMaxArgs) {
    return false;
  }
  bool ok = true;
  RunOnWorker([&] {
    // Teardown may have forgotten RuntimeHost since the caller looked it up.
    if (!loaded_) {
      ok = false;
      return;
    }
    auto* state = kernel_state();
    auto* memory = state->memory();
    // Guest copies of anything passed by reference, as XnaBridge makes them.
    std::vector<uint32_t> scratch(arg_count, 0);
    uint64_t registers[kMaxArgs] = {};
    for (size_t i = 0; i < arg_count && ok; ++i) {
      const auto& arg = args[i];
      if (arg.kind == XnaArgKind::kScalar) {
        registers[i] = arg.scalar;
        continue;
      }
      if (!arg.host || !arg.size) {
        continue;
      }
      scratch[i] = memory->SystemHeapAlloc(arg.size);
      if (!scratch[i]) {
        XELOGE("XnaConsoleNatives: out of guest scratch for {:08X} argument {}",
               function, i);
        ok = false;
        break;
      }
      auto* guest = memory->TranslateVirtual<void*>(scratch[i]);
      if (arg.copy_in) {
        std::memcpy(guest, arg.host, arg.size);
      } else {
        std::memset(guest, 0, arg.size);
      }
      registers[i] = scratch[i];
    }
    // Execute answers an unresolvable address with a made-up result, which
    // the managed side would read as an error code from the native.
    if (ok && !state->processor()->ResolveFunction(function)) {
      XELOGE("XnaConsoleNatives: no guest code at {:08X}", function);
      ok = false;
    }
    if (ok) {
      auto* thread_state = XThread::GetCurrentThread()->thread_state();
      const uint64_t result = state->processor()->Execute(
          thread_state, function, registers, arg_count);
      for (size_t i = 0; i < arg_count; ++i) {
        if (scratch[i] && args[i].copy_out && args[i].host) {
          std::memcpy(args[i].host, memory->TranslateVirtual<void*>(scratch[i]),
                      args[i].size);
        }
      }
      if (out_result) {
        *out_result = result;
      }
    }
    for (uint32_t block : scratch) {
      if (block) {
        memory->SystemHeapFree(block);
      }
    }
  });
  return ok;
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
