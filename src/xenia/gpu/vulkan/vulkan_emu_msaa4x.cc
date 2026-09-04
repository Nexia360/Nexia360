/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_emu_msaa4x.h"

#include "xenia/base/logging.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/ui/vulkan/vulkan_util.h"

namespace xe {
namespace gpu {
namespace vulkan {

bool VulkanEmuMsaa4x::IsRequired(
    const ui::vulkan::VulkanDevice& vulkan_device) {
  const ui::vulkan::VulkanDevice::Properties& properties =
      vulkan_device.properties();
  // Only counts a sample count as usable if colour, depth and stencil
  // attachments all support it - a render pass mixes them, and a count that
  // only one of them can do is no use for emulating a guest surface.
  VkSampleCountFlags usable = properties.framebufferColorSampleCounts &
                              properties.framebufferDepthSampleCounts &
                              properties.framebufferStencilSampleCounts;
  return !(usable & (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT));
}

bool VulkanEmuMsaa4x::Initialize() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  if (!vulkan_device) {
    return false;
  }

  active_ = IsRequired(*vulkan_device);
  if (!active_) {
    return true;
  }

  // The box filter is a blit with a linear filter: reducing by exactly half
  // puts the destination texel centre at the meeting point of four source
  // texels, so the bilinear weights are all a quarter. That is the average of
  // the four samples, which is what a hardware resolve does. Without linear
  // filtering on the blit source the resolve would point-sample one of the
  // four and the emulation would be pointless, so check for it rather than
  // silently degrade.
  const ui::vulkan::VulkanInstance::Functions& ifn =
      vulkan_device->vulkan_instance()->functions();
  VkFormatProperties format_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(vulkan_device->physical_device(),
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          &format_properties);
  linear_blit_supported_ =
      (format_properties.optimalTilingFeatures &
       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;

  if (!linear_blit_supported_) {
    XELOGW(
        "VulkanEmuMsaa4x: the device can't linear-filter blits, so guest "
        "samples can't be averaged on resolve - multisampling will be "
        "reported as unsupported instead of emulated");
    active_ = false;
    return true;
  }

  XELOGI(
      "VulkanEmuMsaa4x: no multisampled attachments on this device - "
      "emulating 4x MSAA by rendering {}x{} host pixels per guest pixel",
      kScaleX, kScaleY);
  return true;
}

void VulkanEmuMsaa4x::Shutdown() {
  active_ = false;
  linear_blit_supported_ = false;
}

bool VulkanEmuMsaa4x::Resolve(VkImage source, VkImage dest, uint32_t dest_width,
                              uint32_t dest_height) {
  if (!active_ || source == VK_NULL_HANDLE || dest == VK_NULL_HANDLE ||
      !dest_width || !dest_height) {
    return false;
  }

  VkImageBlit region;
  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.mipLevel = 0;
  region.srcSubresource.baseArrayLayer = 0;
  region.srcSubresource.layerCount = 1;
  region.srcOffsets[0].x = 0;
  region.srcOffsets[0].y = 0;
  region.srcOffsets[0].z = 0;
  region.srcOffsets[1].x = int32_t(dest_width * kScaleX);
  region.srcOffsets[1].y = int32_t(dest_height * kScaleY);
  region.srcOffsets[1].z = 1;
  region.dstSubresource = region.srcSubresource;
  region.dstOffsets[0].x = 0;
  region.dstOffsets[0].y = 0;
  region.dstOffsets[0].z = 0;
  region.dstOffsets[1].x = int32_t(dest_width);
  region.dstOffsets[1].y = int32_t(dest_height);
  region.dstOffsets[1].z = 1;

  command_processor_.deferred_command_buffer().CmdVkBlitImage(
      source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
  return true;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
