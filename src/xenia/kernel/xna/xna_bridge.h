/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_BRIDGE_H_
#define XENIA_KERNEL_XNA_XNA_BRIDGE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace xe {
namespace cpu {
class Export;
}  // namespace cpu

namespace kernel {
namespace xna {

// Calls Nexia's own kernel exports from host code, generically.
//
// XNA's native surface is very nearly XAM with a prefix - XInput_GetState is
// XamInputGetState, XAM_IsGuideVisible is XamIsUIActive, STORAGE_ShowDeviceUI
// is XamShowDeviceSelectorUI. Writing a C++ shim per function would mean
// hundreds of them; instead this invokes any registered export by name, driven
// by a description of its arguments. What maps to what is then data, not code.
//
// The mechanics being bridged: a kernel export is an ExportTrampoline taking a
// PPCContext, reading its arguments from r3 upward, and any pointer it is given
// is a GUEST address. So a host caller needs a bound guest thread context and a
// scratch buffer inside guest memory for anything passed by reference.

enum class XnaArgKind : uint32_t {
  // Passed by value in a register.
  kScalar,
  // Host memory that the export sees as a guest pointer. Bounced through
  // guest scratch: copied in before the call if `copy_in`, back out after if
  // `copy_out`.
  kBuffer,
};

struct XnaArg {
  XnaArgKind kind = XnaArgKind::kScalar;

  uint64_t scalar = 0;  // kScalar

  void* host = nullptr;   // kBuffer
  uint32_t size = 0;      // kBuffer
  bool copy_in = false;   // kBuffer
  bool copy_out = false;  // kBuffer

  static XnaArg Scalar(uint64_t value) {
    XnaArg arg;
    arg.kind = XnaArgKind::kScalar;
    arg.scalar = value;
    return arg;
  }

  static XnaArg Out(void* host, uint32_t size) {
    XnaArg arg;
    arg.kind = XnaArgKind::kBuffer;
    arg.host = host;
    arg.size = size;
    arg.copy_out = true;
    return arg;
  }

  static XnaArg In(void* host, uint32_t size) {
    XnaArg arg;
    arg.kind = XnaArgKind::kBuffer;
    arg.host = host;
    arg.size = size;
    arg.copy_in = true;
    return arg;
  }

  static XnaArg InOut(void* host, uint32_t size) {
    XnaArg arg;
    arg.kind = XnaArgKind::kBuffer;
    arg.host = host;
    arg.size = size;
    arg.copy_in = true;
    arg.copy_out = true;
    return arg;
  }
};

class XnaBridge {
 public:
  static XnaBridge& Instance();

  XnaBridge(const XnaBridge&) = delete;
  XnaBridge& operator=(const XnaBridge&) = delete;

  // Gives the calling host thread a guest context, once. Every Invoke needs
  // one; calling it again on the same thread does nothing.
  bool EnsureGuestContext();

  // `module` is a registered table name such as "xam.xex". Returns false if the
  // export is unknown, unimplemented, or the call could not be set up - and
  // says which in the log, once per export.
  bool Invoke(const std::string& module, const std::string& name,
              const XnaArg* args, size_t arg_count, uint64_t* out_result);

  // Proves the whole path against a real export rather than a mock: binds a
  // context, calls xam.xex!XamInputGetState for every slot, and reports what
  // came back. Logs its findings; returns false if the bridge itself failed.
  bool RunSelfTest();

 private:
  XnaBridge() = default;

  cpu::Export* Find(const std::string& module, const std::string& name);

  // Built on first use - the resolver's tables do not change after launch.
  std::unordered_map<std::string, cpu::Export*> by_name_;
  // The same exports keyed on name alone, for callers that know the export but
  // not which module the emulator registered it under.
  std::unordered_map<std::string, cpu::Export*> bare_names_;
  bool indexed_ = false;
};

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_BRIDGE_H_
