/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_HOST_H_
#define XENIA_KERNEL_XNA_XNA_HOST_H_

#include <filesystem>
#include <string>

namespace xe {
namespace kernel {
namespace xna {

// Runs an XNA title's managed code on the host CLR, with Nexia underneath it as
// the operating system.
//
// The title's IL is JITted once, by the host - there is no second runtime and
// no PPC emulation involved. What Nexia provides is everything the console did
// and a desktop does not, handed over as the XnaOsTable (see xna_os.h).
//
// The runtime is loaded through hostfxr resolved at RUN TIME rather than linked
// against, so nothing here adds a build dependency on the .NET SDK: a machine
// without .NET simply cannot start XNA titles, and says so.
class XnaHost {
 public:
  static XnaHost& Instance();

  XnaHost(const XnaHost&) = delete;
  XnaHost& operator=(const XnaHost&) = delete;

  // `bootstrap_assembly` is the managed side of this boundary; its
  // .runtimeconfig.json must sit beside it. `game_assembly` is the title's own
  // entry assembly, which the bootstrap loads into its own context.
  // Returns false and logs the reason on any failure.
  bool Start(const std::filesystem::path& bootstrap_assembly,
             const std::filesystem::path& game_assembly);

  void Stop();

  bool running() const { return running_; }

  // Why the last Start failed, for showing a user something better than "no".
  const std::string& last_error() const { return last_error_; }

 private:
  XnaHost() = default;
  ~XnaHost();

  bool LoadHostFxr();
  void Fail(std::string message);

  void* hostfxr_module_ = nullptr;
  void* host_context_ = nullptr;
  bool running_ = false;
  std::string last_error_;
};

bool XnaHostWritable(const void* begin, const void* end);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_HOST_H_
