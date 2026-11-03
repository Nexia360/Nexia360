/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_CONSOLE_NATIVES_H_
#define XENIA_KERNEL_XNA_XNA_CONSOLE_NATIVES_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include "xenia/base/cvar.h"
#include "xenia/kernel/xna/xna_bridge.h"

DECLARE_bool(xna_console_natives);

namespace xe {
namespace kernel {
namespace xna {

// THE CONSOLE'S OWN XNA NATIVES, WITHOUT ITS CLR.
//
// RuntimeHost.xex is two things: the host of the Compact Framework runtime
// (NetCFUserMode.dll, which JITs IL to PowerPC), and the native half of XNA -
// every P/Invoke the console's MXF*.dlx assemblies make lands in one of its
// three {name, ordinal, function} tables. A hosted title already runs those
// assemblies on the host CLR, so the IL is JITted once; this loads RuntimeHost
// as an ordinary guest DLL and calls its natives directly, so the real
// implementation answers instead of a C++ re-implementation, and NetCF never
// starts. RuntimeHost_CreateInstance, which builds the CLR host, is never
// called.
//
// Every call runs on one guest thread of its own: the natives take critical
// sections, wait on kernel objects and call XAM, which all need a real XThread
// rather than a bare guest context.
class XnaConsoleNatives {
 public:
  static XnaConsoleNatives& Instance();

  XnaConsoleNatives(const XnaConsoleNatives&) = delete;
  XnaConsoleNatives& operator=(const XnaConsoleNatives&) = delete;

  // Finds RuntimeHost.xex, loads it (DllMain runs; the CLR does not) and
  // indexes its natives. Safe to call again; it loads once. Returns false, and
  // says why in the log, when the cvar is off or RuntimeHost cannot be loaded -
  // the hosted entry points then answer from their own implementations.
  bool Load();
  bool loaded() const { return loaded_; }

  // Title teardown unloads RuntimeHost with every other user module. This
  // forgets it first, on the natives' thread so a call already running
  // finishes, and lets the next hosted title load it again.
  void Reset();

  // The guest address of the native the console assemblies know by `name`
  // (e.g. "XInput_GetState"), or 0 when RuntimeHost has none.
  uint32_t Find(std::string_view name) const;

  // Runs the native at `function` on the natives' guest thread and waits for
  // it. Arguments are XnaBridge's: scalars go in registers, buffers are copied
  // through guest memory. The bytes are copied as they are - converting their
  // fields between host and console order is the caller's job, because only
  // the caller knows the layout. False if the call could not be made.
  bool Call(uint32_t function, const XnaArg* args, size_t arg_count,
            uint64_t* out_result);

 private:
  XnaConsoleNatives() = default;

  std::atomic<bool> loaded_{false};
  bool tried_ = false;
};

// Where the 4.0 runtime's RuntimeHost.xex was found, or empty.
std::filesystem::path FindConsoleRuntimeHost();

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_CONSOLE_NATIVES_H_
