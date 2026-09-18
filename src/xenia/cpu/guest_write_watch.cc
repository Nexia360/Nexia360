/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_write_watch.h"

#include <array>
#include <cstdlib>
#include <mutex>
#include <string>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/thread_state.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

DEFINE_string(
    guest_write_watch, "",
    "TEMPORARY: log every write to these guest addresses with the guest caller "
    "that made it, using hardware data breakpoints. Up to four, comma "
    "separated, hex. Empty disables. Example: 92331B9C,92331BA4,92331BA8",
    "CPU");

namespace xe {
namespace cpu {

#if XE_PLATFORM_WIN32

namespace {

// x86 gives us four debug registers, so four addresses is the hard ceiling.
constexpr size_t kMaxWatches = 4;

struct Watch {
  uint32_t guest_address = 0;
  uint32_t last_value = 0;
  bool have_last = false;
};

std::array<Watch, kMaxWatches> watches;
size_t watch_count = 0;
std::once_flag parse_once;
std::once_flag install_once;
std::mutex report_mutex;

void ParseWatches() {
  const std::string& text = cvars::guest_write_watch;
  size_t at = 0;
  while (at < text.size() && watch_count < kMaxWatches) {
    size_t comma = text.find(',', at);
    std::string piece = text.substr(
        at, comma == std::string::npos ? std::string::npos : comma - at);
    at = comma == std::string::npos ? text.size() : comma + 1;
    // Trim, and accept an optional 0x - the config writer round-trips strings
    // verbatim, so both spellings show up in practice.
    while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t')) {
      piece.erase(piece.begin());
    }
    while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t')) {
      piece.pop_back();
    }
    if (piece.empty()) {
      continue;
    }
    const uint32_t address =
        static_cast<uint32_t>(std::strtoull(piece.c_str(), nullptr, 16));
    if (!address) {
      continue;
    }
    watches[watch_count++].guest_address = address;
  }
}

bool WatchExceptionCallback(Exception* ex, void* data) {
  if (ex->code() != Exception::Code::kSingleStep) {
    return false;
  }
  auto* state = ThreadState::Get();
  auto* context = state ? state->context() : nullptr;
  if (!context) {
    // Not guest code - some other single step, leave it to whoever owns it.
    return false;
  }

  // DR6 would say which register fired, but the exception does not carry it.
  // Comparing each watched dword against its previous value identifies the
  // write just as well and costs nothing.
  std::lock_guard<std::mutex> lock(report_mutex);
  bool reported = false;
  for (size_t i = 0; i < watch_count; ++i) {
    Watch& watch = watches[i];
    const uint32_t stored = xe::load_and_swap<uint32_t>(
        context->virtual_membase + watch.guest_address);
    if (watch.have_last && stored == watch.last_value) {
      continue;
    }
    // When the value looks like a guest heap pointer, print the first dword at
    // that address too: for a C++ object that is the vtable pointer, which
    // names the class in a way the address alone never can.
    uint32_t vptr = 0;
    if (stored >= 0x30000000 && stored < 0x60000000) {
      vptr = xe::load_and_swap<uint32_t>(context->virtual_membase + stored);
    }
    XELOGW(
        "[write-watch] {:08X} <- {:08X} (was {:08X}) vptr {:08X}  by guest lr "
        "{:08X}  (host rip {:016X})",
        watch.guest_address, stored, watch.have_last ? watch.last_value : 0,
        vptr, static_cast<uint32_t>(context->lr), ex->pc());
    watch.last_value = stored;
    watch.have_last = true;
    reported = true;
  }
  if (!reported) {
    // A store that did not change the value still tells us someone wrote it.
    XELOGW("[write-watch] write of an unchanged value by guest lr {:08X}",
           static_cast<uint32_t>(context->lr));
  }
  return true;
}

}  // namespace

void ArmGuestWriteWatchForCurrentThread() {
  std::call_once(parse_once, ParseWatches);
  if (!watch_count) {
    return;
  }
  auto* state = ThreadState::Get();
  auto* context = state ? state->context() : nullptr;
  if (!context) {
    return;
  }

  std::call_once(install_once, []() {
    ExceptionHandler::Install(WatchExceptionCallback, nullptr);
    std::string list;
    for (size_t i = 0; i < watch_count; ++i) {
      list += fmt::format("{}{:08X}", i ? " " : "", watches[i].guest_address);
    }
    XELOGW("[write-watch] armed on guest {} - TEMPORARY instrumentation", list);
  });

  CONTEXT thread_context = {};
  thread_context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  if (!GetThreadContext(GetCurrentThread(), &thread_context)) {
    XELOGW("[write-watch] GetThreadContext failed; this thread is not watched");
    return;
  }

  uint64_t* const debug_address[kMaxWatches] = {
      &thread_context.Dr0, &thread_context.Dr1, &thread_context.Dr2,
      &thread_context.Dr3};
  for (size_t i = 0; i < watch_count; ++i) {
    const uint32_t guest_address = watches[i].guest_address;
    const uint64_t host_address =
        reinterpret_cast<uint64_t>(context->virtual_membase + guest_address);
    *debug_address[i] = host_address;
    // DR7 per slot i: L(i) at bit 2i enables it locally, RW(i) at bit 16+4i
    // (01 = break on data write), LEN(i) at bit 18+4i (11 = four bytes). A four
    // byte watch must be four byte aligned or the processor ignores it.
    thread_context.Dr7 &= ~(UINT64_C(0xF) << (16 + 4 * i));
    thread_context.Dr7 |= UINT64_C(1) << (2 * i);
    thread_context.Dr7 |= UINT64_C(0b01) << (16 + 4 * i);
    thread_context.Dr7 |= UINT64_C(0b11) << (18 + 4 * i);
    if (host_address & 3) {
      XELOGW(
          "[write-watch] guest {:08X} is not 4 byte aligned - the processor "
          "will not report it",
          guest_address);
    }
  }
  thread_context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  if (!SetThreadContext(GetCurrentThread(), &thread_context)) {
    XELOGW("[write-watch] SetThreadContext failed; this thread is not watched");
  }
}

#else

void ArmGuestWriteWatchForCurrentThread() {}

#endif  // XE_PLATFORM_WIN32

}  // namespace cpu
}  // namespace xe
