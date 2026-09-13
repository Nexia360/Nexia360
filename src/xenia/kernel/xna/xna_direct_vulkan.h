/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_DIRECT_VULKAN_H_
#define XENIA_KERNEL_XNA_XNA_DIRECT_VULKAN_H_

#include <cstdint>

#include "xenia/kernel/xna/xna_gpu.h"

namespace xe {
namespace kernel {
namespace xna {

bool XnaVulkanDirectActive();

bool XnaVulkanDirectDraw(const XnaGpuDraw& draw, const XnaGpuStream* streams,
                         uint32_t stream_count);

void XnaVulkanDirectClear(const XnaGpuTarget& target, const float* color,
                          bool clear_color, bool clear_depth, float depth);

void XnaVulkanDirectPresent();

bool XnaVulkanDirectReadBackBuffer(uint8_t* out, uint32_t bytes);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_DIRECT_VULKAN_H_
