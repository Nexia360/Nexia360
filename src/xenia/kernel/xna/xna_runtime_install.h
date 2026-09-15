/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_RUNTIME_INSTALL_H_
#define XENIA_KERNEL_XNA_XNA_RUNTIME_INSTALL_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace xe {
namespace kernel {
namespace xna {

extern const uint8_t kXnaInstallScript[];
extern const size_t kXnaInstallScriptSize;

bool XnaRuntimeInstalled(std::string* out_missing);

void RequestXnaRuntimeInstall(const std::string& missing);

bool XnaRuntimeInstallPending();

bool InstallXnaRuntime(std::string* out_error);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_RUNTIME_INSTALL_H_
