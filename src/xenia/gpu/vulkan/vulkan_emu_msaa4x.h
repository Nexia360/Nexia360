/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_EMU_MSAA4X_H_
#define XENIA_GPU_VULKAN_VULKAN_EMU_MSAA4X_H_

#include <cstdint>

#include "xenia/ui/vulkan/vulkan_device.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

// Multisampling substitute for devices whose framebuffers only ever offer one
// sample.
//
// The guest asks for 2x or 4x MSAA and the EDRAM emulation expects to be able
// to store a value per guest sample. A device that advertises only
// VK_SAMPLE_COUNT_1_BIT for colour, depth and stencil attachments cannot do
// that with a multisampled image, so the samples are given somewhere to live
// instead: every guest pixel is rendered as a 2x2 block of host pixels, one
// host pixel per guest sample, and the block is averaged back down when the
// guest resolves.
//
// This is supersampling, and it differs from hardware MSAA in ways that matter:
// the pixel shader runs per sample rather than per covered pixel, so it costs
// four shader invocations per guest pixel rather than one, and shading is not
// shared between samples of the same primitive. What it does reproduce is the
// thing titles depend on - a guest pixel carrying four independently covered
// samples that average on resolve, which is what makes alpha to mask coverage
// and geometry edges resolve smoothly rather than collapsing to one bit.
//
// The 2x2 block is arranged to match the Direct3D 10.1 standard 4x sample
// positions the Xenos ordering maps onto, so shaders indexing a sample by its
// guest index address the same corner they would on hardware:
//
//     guest sample 0 -> block (0, 0)      guest sample 1 -> block (1, 0)
//     guest sample 2 -> block (0, 1)      guest sample 3 -> block (1, 1)
class VulkanEmuMsaa4x {
 public:
  // One host pixel per guest sample, on each axis.
  static constexpr uint32_t kScaleX = 2;
  static constexpr uint32_t kScaleY = 2;

  // Whether the device can't provide real multisampled attachments and needs
  // this substitute. True when neither 2x nor 4x is available for colour,
  // depth and stencil attachments simultaneously - a device missing only 2x
  // still has 4x to fall back on and doesn't need emulation.
  static bool IsRequired(const ui::vulkan::VulkanDevice& vulkan_device);

  explicit VulkanEmuMsaa4x(VulkanCommandProcessor& command_processor)
      : command_processor_(command_processor) {}
  VulkanEmuMsaa4x(const VulkanEmuMsaa4x&) = delete;
  VulkanEmuMsaa4x& operator=(const VulkanEmuMsaa4x&) = delete;
  ~VulkanEmuMsaa4x() { Shutdown(); }

  bool Initialize();
  void Shutdown();

  bool is_active() const { return active_; }

  // Scale to apply to a guest surface to hold its samples, 1 when inactive so
  // callers can multiply unconditionally.
  uint32_t scale_x() const { return active_ ? kScaleX : 1; }
  uint32_t scale_y() const { return active_ ? kScaleY : 1; }

  // Averages each 2x2 block of source into one texel of dest. Both images must
  // already be in TRANSFER_SRC_OPTIMAL / TRANSFER_DST_OPTIMAL respectively, and
  // source must be exactly dest_width * kScaleX by dest_height * kScaleY.
  // Records into the command processor's deferred command buffer.
  bool Resolve(VkImage source, VkImage dest, uint32_t dest_width,
               uint32_t dest_height);

 private:
  VulkanCommandProcessor& command_processor_;
  bool active_ = false;
  // Whether a linear-filtered blit is usable for the box filter, decided per
  // format at Initialize.
  bool linear_blit_supported_ = false;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_EMU_MSAA4X_H_
