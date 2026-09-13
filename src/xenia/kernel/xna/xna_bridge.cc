/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_bridge.h"

#include <string>

#include <cstring>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/hid/input.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {
namespace xna {

namespace {

// One guest context per host thread that calls in. Never freed: the thread
// outlives the calls, and tearing a context down under a half-finished export
// would be worse than leaking one per hosted title.
thread_local cpu::ThreadState* guest_thread_state = nullptr;

// The most registers any export we bridge takes; XamInputGetState uses three.
constexpr size_t kMaxArgs = 8;

}  // namespace

XnaBridge& XnaBridge::Instance() {
  static XnaBridge instance;
  return instance;
}

bool XnaBridge::EnsureGuestContext() {
  if (guest_thread_state) {
    return true;
  }
  auto* state = kernel_state();
  auto* processor = state ? state->processor() : nullptr;
  if (!processor) {
    XELOGE("XnaBridge: no processor - the emulator is not running a title");
    return false;
  }
  // UINT_MAX means "system thread": ThreadState gives it a synthetic id with
  // the high bit set, which is exactly what a host thread is.
  guest_thread_state = new cpu::ThreadState(processor, UINT_MAX);
  cpu::ThreadState::Bind(guest_thread_state);
  XELOGI("XnaBridge: bound a guest context to this host thread (id {:08X})",
         guest_thread_state->thread_id());
  return true;
}

cpu::Export* XnaBridge::Find(const std::string& module,
                             const std::string& name) {
  if (!indexed_) {
    auto* state = kernel_state();
    auto* processor = state ? state->processor() : nullptr;
    auto* resolver = processor ? processor->export_resolver() : nullptr;
    if (!resolver) {
      return nullptr;
    }
    for (const auto& table : resolver->tables()) {
      uint32_t from_this_table = 0;
      for (auto* entry : table.exports_by_name()) {
        if (entry && entry->name) {
          by_name_[table.module_name() + "!" + entry->name] = entry;
          // Also by bare name. Kernel export names are unique across the
          // modules in practice, and a caller that knows the name but guesses
          // the module should still find it rather than silently do nothing.
          bare_names_.emplace(entry->name, entry);
          ++from_this_table;
        }
      }
      XELOGI("XnaBridge:    {} export(s) from {}", from_this_table,
             table.module_name());
    }
    indexed_ = true;
    XELOGI("XnaBridge: indexed {} kernel exports", by_name_.size());
  }

  auto it = by_name_.find(module + "!" + name);
  if (it != by_name_.end()) {
    return it->second;
  }
  // The module the console names is not always the one the emulator registers
  // the export under, and the name is the part that is actually known.
  auto bare = bare_names_.find(name);
  return bare == bare_names_.end() ? nullptr : bare->second;
}

bool XnaBridge::Invoke(const std::string& module, const std::string& name,
                       const XnaArg* args, size_t arg_count,
                       uint64_t* out_result) {
  if (arg_count > kMaxArgs) {
    XELOGE("XnaBridge: {}!{} takes more arguments than the bridge passes",
           module, name);
    return false;
  }
  auto* entry = Find(module, name);
  if (!entry) {
    XELOGE(
        "XnaBridge: {}!{} is not a registered export, by module name or by "
        " name alone. {} key(s) and {} bare name(s) indexed",
        module, name, by_name_.size(), bare_names_.size());
    return false;
  }
  if (entry->get_type() != cpu::Export::Type::kFunction ||
      !entry->function_data.trampoline) {
    XELOGE("XnaBridge: {}!{} has no trampoline (unimplemented?)", module, name);
    return false;
  }
  if (!EnsureGuestContext()) {
    return false;
  }

  auto* memory = kernel_state()->memory();
  auto* context = guest_thread_state->context();

  // Guest scratch for anything passed by reference. Allocated per call rather
  // than pooled: these calls are not hot, and a pool would need a lifetime
  // rule for buffers an export might hold on to.
  std::vector<uint32_t> scratch(arg_count, 0);

  bool ok = true;
  for (size_t i = 0; i < arg_count && ok; ++i) {
    const auto& arg = args[i];
    if (arg.kind == XnaArgKind::kScalar) {
      context->r[3 + i] = arg.scalar;
      continue;
    }
    if (!arg.host || !arg.size) {
      // A null buffer is a legitimate argument - several exports treat it as
      // "do not report this".
      context->r[3 + i] = 0;
      continue;
    }
    scratch[i] = memory->SystemHeapAlloc(arg.size);
    if (!scratch[i]) {
      XELOGE("XnaBridge: out of guest scratch for {}!{} argument {}", module,
             name, i);
      ok = false;
      break;
    }
    if (arg.copy_in) {
      std::memcpy(memory->TranslateVirtual<void*>(scratch[i]), arg.host,
                  arg.size);
    } else {
      std::memset(memory->TranslateVirtual<void*>(scratch[i]), 0, arg.size);
    }
    context->r[3 + i] = scratch[i];
  }

  if (ok) {
    entry->function_data.trampoline(context);

    for (size_t i = 0; i < arg_count; ++i) {
      if (args[i].kind == XnaArgKind::kBuffer && args[i].copy_out &&
          scratch[i] && args[i].host) {
        std::memcpy(args[i].host, memory->TranslateVirtual<void*>(scratch[i]),
                    args[i].size);
      }
    }
    if (out_result) {
      *out_result = context->r[3];
    }
  }

  for (size_t i = 0; i < arg_count; ++i) {
    if (scratch[i]) {
      memory->SystemHeapFree(scratch[i]);
    }
  }
  return ok;
}

bool XnaBridge::RunSelfTest() {
  XELOGI("XnaBridge: self test starting");
  if (!EnsureGuestContext()) {
    return false;
  }

  // XamInputGetState is the right first target: it is what XNA's
  // XINPUT!XInput_GetState maps to, it takes both a scalar and an out-pointer,
  // and its answer is checkable without a controller - an empty slot must say
  // DEVICE_NOT_CONNECTED rather than failing.
  bool bridged = false;
  for (uint32_t slot = 0; slot < 4; ++slot) {
    xe::hid::X_INPUT_STATE state = {};
    const XnaArg args[] = {
        XnaArg::Scalar(slot),
        XnaArg::Scalar(0),
        XnaArg::Out(&state, sizeof(state)),
    };
    uint64_t result = 0;
    if (!Invoke("xam.xex", "XamInputGetState", args, 3, &result)) {
      return false;
    }
    bridged = true;
    XELOGI(
        "XnaBridge: XamInputGetState(slot {}) -> {:08X}, packet {}, buttons "
        "{:04X}",
        slot, static_cast<uint32_t>(result),
        static_cast<uint32_t>(state.packet_number),
        static_cast<uint16_t>(state.gamepad.buttons));
  }

  XELOGI("XnaBridge: self test {}", bridged ? "passed" : "did nothing");
  return bridged;
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
