// xenia/cpu/backend/x64/x64_function.cc
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Fiber execution experiment with DEBUG LOGS (Windows-only fast path).
 * Conservative version:
 *  - Skip fiber if RA looks poison (e.g., 0xBCBCBCBC, 0xCCCCCCCC, …)
 *  - Skip fiber for hot dispatcher stubs (denylist)
 *  - Watchdog burst limiter
 *  - Keeps RA verbatim (never mutates it)
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/x64_function.h"

#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"

#include "xenia/base/logging.h"

#if XE_PLATFORM_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <chrono>
#include <cstdint>
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {
DEFINE_bool(fiber_poison_checks, false,
            "Checks for poison addresses and skips fiber", "CPU");

DEFINE_bool(replace_thunk_call_with_fibers, false,
            "Use Windows Fibers for thread encapsulation, (Experimental)",
            "CPU");

#if XE_PLATFORM_WIN32
// Reasonable stack size for a leaf-ish worker that just calls the thunk.
static constexpr SIZE_T kFiberCommitBytes = 1ull << 21;   // 4 MB
static constexpr SIZE_T kFiberReserveBytes = 1ull << 25;  // 64 MB

// Stubs that are extremely hot and/or spinny in your trace — avoid fibers.
static constexpr uint32_t kDenylistedFns[] = {
    0x8209CC88,  // seen hammering
    0x82487FD0,  // seen hammering
                 // add more here if needed
};

struct FiberCallCtx {
  uint8_t* code_ptr = nullptr;
  void* thunk_ptr = nullptr;  // X64Backend::host_to_guest_thunk
  xe::cpu::ppc::PPCContext* ctx = nullptr;
  uint32_t return_address = 0;  // forwarded verbatim
  bool has_work = false;
};

struct ThreadFiberEnv {
  void* main_fiber = nullptr;
  void* worker_fiber = nullptr;
};

struct CallWatchdog {
  uint32_t last_fn = 0;
  uint64_t count = 0;
  std::chrono::steady_clock::time_point window_start{};
};

static thread_local ThreadFiberEnv tl_env{};
static thread_local FiberCallCtx tl_call_ctx{};
static thread_local CallWatchdog tl_watchdog{};

static inline bool LooksPoison(uint32_t ra) {
  if (!cvars::fiber_poison_checks) return false;
  switch (ra) {
    case 0xBCBCBCBCu:
    case 0xCDCDCDCDu:
    case 0xCCCCCCCCu:
    case 0xFEEEFEEEu:
    case 0xDEADF00Du:
    case 0xBAADF00Du:
    case 0xDEADBABEu:
    case 0xDEADBAB3u:
      return true;
    default:
      return false;
  }
}

static inline bool IsDenylisted(uint32_t fn) {
  for (uint32_t d : kDenylistedFns) {
    if (d == fn) return true;
  }
  return false;
}

// Worker runs the thunk once per posted call and yields back.
static VOID WINAPI WorkerFiberEntry(void* /*param*/) {
  XELOGD("[FBR ] entry worker={} main={} tid={}", tl_env.worker_fiber,
         tl_env.main_fiber, static_cast<unsigned long>(GetCurrentThreadId()));

  using ThunkFn = void (*)(uint8_t*, xe::cpu::ppc::PPCContext*, void*);
  for (;;) {
    if (!tl_env.main_fiber) {
      XELOGD("[FBR ] main fiber lost; returning worker={} tid={}",
             tl_env.worker_fiber,
             static_cast<unsigned long>(GetCurrentThreadId()));
      return;
    }
    if (!tl_call_ctx.has_work) {
      SwitchToFiber(tl_env.main_fiber);
      continue;
    }

    auto code = tl_call_ctx.code_ptr;
    auto thunk = reinterpret_cast<ThunkFn>(tl_call_ctx.thunk_ptr);
    auto ctx = tl_call_ctx.ctx;
    uint32_t ra = tl_call_ctx.return_address;

    if (LooksPoison(ra)) {
      // Should be rare now — we prefilter in CallImpl — but keep telemetry.
      XELOGD("[FBR ] NOTE: suspicious RA observed: {:#010X} (forwarding as-is)",
             ra);
    }

    XELOGD("[FBR ] call  code={} worker={} tid={} ra={:#010X}",
           static_cast<const void*>(code), tl_env.worker_fiber,
           static_cast<unsigned long>(GetCurrentThreadId()), ra);

    thunk(code, ctx, reinterpret_cast<void*>(uintptr_t(ra)));

    XELOGD("[FBR ] ret   code={} worker={} tid={}",
           static_cast<const void*>(code), tl_env.worker_fiber,
           static_cast<unsigned long>(GetCurrentThreadId()));

    tl_call_ctx.has_work = false;
    SwitchToFiber(tl_env.main_fiber);
  }
}
#endif  // XE_PLATFORM_WIN32

// -----------------------------------------------------------------------------
// X64Function
// -----------------------------------------------------------------------------

X64Function::X64Function(Module* module, uint32_t address)
    : GuestFunction(module, address) {}

X64Function::~X64Function() {
  // Fibers are thread-affine and cleaned up on thread exit.
}

void X64Function::Setup(uint8_t* machine_code, size_t machine_code_length) {
  machine_code_ = machine_code;
  machine_code_length_ = machine_code_length;
}

bool X64Function::CallImpl(ThreadState* thread_state, uint32_t return_address) {
  auto backend =
      reinterpret_cast<X64Backend*>(thread_state->processor()->backend());
  auto thunk = backend->host_to_guest_thunk();
  auto* ctx = thread_state->context();

#if XE_PLATFORM_WIN32
  if (cvars::replace_thunk_call_with_fibers) {
    const uint32_t fn_addr = address();

    // Conservative prefilter: if RA is poison or function is denylisted, skip
    // fiber.
    if (LooksPoison(return_address) || IsDenylisted(fn_addr)) {
      if (LooksPoison(return_address)) {
        XELOGD("[CALL] poison RA {:#010X} -> direct thunk (fn={:#010X})",
               return_address, fn_addr);
      } else {
        XELOGD("[CALL] denylisted fn={:#010X} -> direct thunk", fn_addr);
      }
      thunk(machine_code_, ctx,
            reinterpret_cast<void*>(uintptr_t(return_address)));
      return true;
    }

    // Prepare main fiber (per-thread).
    void* current_fiber = IsThreadAFiber() ? GetCurrentFiber() : nullptr;
    if (!tl_env.main_fiber) {
      if (current_fiber) {
        tl_env.main_fiber = current_fiber;
        XELOGD("[CALL] adopt main fiber cur={} tid={}", tl_env.main_fiber,
               static_cast<unsigned long>(GetCurrentThreadId()));
      } else {
        tl_env.main_fiber =
            ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        XELOGD("[CALL] convert thread to fiber rc={} tid={}", tl_env.main_fiber,
               static_cast<unsigned long>(GetCurrentThreadId()));
        if (!tl_env.main_fiber) {
          XELOGD("[CALL] convert failed, falling back to direct thunk");
          thunk(machine_code_, ctx,
                reinterpret_cast<void*>(uintptr_t(return_address)));
          return true;
        }
      }
    }

    // If already on a non-main fiber (re-entrant from worker), just call
    // directly.
    if (current_fiber && current_fiber != tl_env.main_fiber) {
      XELOGD("[CALL] on foreign fiber cur={} main={} tid={}, direct thunk",
             current_fiber, tl_env.main_fiber,
             static_cast<unsigned long>(GetCurrentThreadId()));
      thunk(machine_code_, ctx,
            reinterpret_cast<void*>(uintptr_t(return_address)));
      return true;
    }

    // Create per-thread worker fiber once.
    if (!tl_env.worker_fiber) {
      tl_env.worker_fiber =
          CreateFiberEx(kFiberCommitBytes, kFiberReserveBytes,
                        FIBER_FLAG_FLOAT_SWITCH, &WorkerFiberEntry, nullptr);
      XELOGD("[CALL] create worker fiber={} tid={}", tl_env.worker_fiber,
             static_cast<unsigned long>(GetCurrentThreadId()));
      if (!tl_env.worker_fiber) {
        XELOGD("[CALL] CreateFiberEx failed, direct thunk");
        thunk(machine_code_, ctx,
              reinterpret_cast<void*>(uintptr_t(return_address)));
        return true;
      }
    }

    // Watchdog (per thread): if the same fn hammers quickly, use direct call.
    const auto now = std::chrono::steady_clock::now();
    if (tl_watchdog.last_fn != fn_addr ||
        now - tl_watchdog.window_start > std::chrono::milliseconds(5)) {
      tl_watchdog.last_fn = fn_addr;
      tl_watchdog.window_start = now;
      tl_watchdog.count = 0;
    }
    if (++tl_watchdog.count > 64) {  // slightly higher threshold
      XELOGD("[CALL] watchdog: spinning fn={:#010X} cnt={} -> direct thunk",
             fn_addr, static_cast<unsigned long long>(tl_watchdog.count));
      thunk(machine_code_, ctx,
            reinterpret_cast<void*>(uintptr_t(return_address)));
      // Reset for next window.
      tl_watchdog.window_start = now;
      tl_watchdog.count = 0;
      return true;
    }

    // Post work for worker fiber and switch.
    tl_call_ctx.code_ptr = machine_code_;
    tl_call_ctx.thunk_ptr = reinterpret_cast<void*>(thunk);
    tl_call_ctx.ctx = ctx;
    tl_call_ctx.return_address = return_address;
    tl_call_ctx.has_work = true;

    XELOGD(
        "[CALL] enter fn={:#010X} tid={} cur={} main={} worker={} ra={:#010X}",
        fn_addr, static_cast<unsigned long>(GetCurrentThreadId()),
        current_fiber, tl_env.main_fiber, tl_env.worker_fiber, return_address);

    SwitchToFiber(tl_env.worker_fiber);

    XELOGD("[CALL] exit  fn={:#010X} tid={} cur={} main={} worker={}", fn_addr,
           static_cast<unsigned long>(GetCurrentThreadId()), GetCurrentFiber(),
           tl_env.main_fiber, tl_env.worker_fiber);

    return true;

#else
  // Linux: no fiber support, direct thunk call.
  thunk(machine_code_, thread_state->context(),
        reinterpret_cast<void*>(uintptr_t(return_address)));
  return true;
#endif
#if XE_PLATFORM_WIN32
  }  // closes if (cvars::replace_thunk_call_with_fibers)

  // Fallback when fibers are disabled.
  thunk(machine_code_, thread_state->context(),
        reinterpret_cast<void*>(uintptr_t(return_address)));
  return true;
#endif
}

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
