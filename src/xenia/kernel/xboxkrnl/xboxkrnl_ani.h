/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_ANI_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_ANI_H_

#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// The boot animation, as the kernel plays it: bootanim.xex loaded as a module
// and its ordinal 1 run on its own guest thread. These are the bodies behind
// AniStartBootAnimation and AniTerminateAnimation, exposed so the host can
// start the animation at boot without going through the guest export shim -
// there is no guest running at that point to call it.
X_STATUS StartBootAnimation();
// Waits for the animation to end on its own, with no timeout - the kernel's
// AniBlockOnAnimation. This is what decides when the dashboard follows, so
// nothing has to guess how long the animation runs.
X_STATUS BlockOnBootAnimation();
X_STATUS TerminateBootAnimation();

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_ANI_H_
