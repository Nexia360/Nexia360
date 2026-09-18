/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_PRIVATE_H_
#define XENIA_KERNEL_XAM_XAM_PRIVATE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/xam/xam_ordinals.h"

namespace xe {
namespace kernel {
namespace xam {

xe::cpu::Export* RegisterExport_xam(xe::cpu::Export* export_entry);

void RecordLaunchOrigin();
void ReloadForLaunch();
void CloseOpenedContentFiles();

// Builds the 0x10C launch blob the Avatar Editor expects, in the exact shape
// the real XamLaunchAvatarEditor produces: 'EPIX' then the item string at
// +0x0C. Defined in xam_info.cc, used there and by XamLaunchAvatarEditor.
std::vector<uint8_t> BuildAvatarEditorLaunchData(const std::u16string& item);

// Registration functions, one per file.
#define XE_MODULE_EXPORT_GROUP(m, n)                                  \
  void Register##n##Exports(xe::cpu::ExportResolver* export_resolver, \
                            KernelState* kernel_state);
#include "xam_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_PRIVATE_H_
