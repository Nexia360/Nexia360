/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_direct_vulkan.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/string_buffer.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xna/xna_exports.h"
#include "xenia/kernel/xna/xna_present.h"
#include "xenia/kernel/xna/xna_runtimehost.h"
#include "xenia/memory.h"
#include "xenia/ui/vulkan/vulkan_presenter.h"
#include "xenia/ui/vulkan/vulkan_provider.h"
#include "xenia/ui/vulkan/vulkan_util.h"

#include "xenia/ui/shaders/bytecode/vulkan_spirv/guest_output_bilinear_ps.h"
#include "xenia/ui/shaders/bytecode/vulkan_spirv/guest_output_triangle_strip_rect_vs.h"

DECLARE_bool(xna_direct_d3d12);

namespace xe {
namespace kernel {
namespace xna {

namespace {

using gpu::Shader;
using gpu::SpirvShader;
using gpu::SpirvShaderTranslator;
namespace xenos = gpu::xenos;
namespace reg = gpu::reg;
namespace uivk = ui::vulkan;

constexpr uint32_t kBackBufferWidth = 1280;
constexpr uint32_t kBackBufferHeight = 720;
constexpr VkFormat kBackBufferFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t kUploadBytes = 64u << 20;
constexpr uint32_t kZeroBytes = 1024;
constexpr uint32_t kSetsPerPool = 8192;
constexpr uint32_t kNullVertexFetchConstant = 80;
constexpr uint32_t kFetchConstantDwords = 192;
constexpr uint32_t kNull2DArray = 0;
constexpr uint32_t kNull3D = 1;
constexpr uint32_t kNullCube = 2;

struct Target {
  VkImage color = VK_NULL_HANDLE;
  VkDeviceMemory color_memory = VK_NULL_HANDLE;
  VkImageView attachment_view = VK_NULL_HANDLE;
  VkImageView sampled_view = VK_NULL_HANDLE;
  VkImage depth = VK_NULL_HANDLE;
  VkDeviceMemory depth_memory = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageLayout color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct HostTexture {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint32_t version = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t levels = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
};

struct Retired {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct TextureFormatInfo {
  VkFormat format;
  VkComponentMapping mapping;
  uint32_t block;
  uint32_t bytes;
};

struct PipelineKey {
  uint64_t render_pass;
  uint64_t layout;
  const void* vertex;
  const void* pixel;
  uint32_t target_count;
  uint32_t blend[6];
  uint32_t write[4];
  uint32_t depth_enable;
  uint32_t depth_write;
  uint32_t depth_function;
  uint32_t topology;
  bool operator<(const PipelineKey& other) const {
    return std::memcmp(this, &other, sizeof(*this)) < 0;
  }
};

struct State {
  std::mutex mutex;
  bool tried = false;
  bool usable = false;
  uivk::VulkanProvider* provider = nullptr;
  uivk::VulkanPresenter* presenter = nullptr;
  uivk::VulkanDevice* device = nullptr;
  VkDevice vk = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool recording = false;

  VkBuffer upload = VK_NULL_HANDLE;
  VkDeviceMemory upload_memory = VK_NULL_HANDLE;
  uint32_t upload_memory_type = 0;
  VkDeviceSize upload_memory_size = 0;
  uint8_t* upload_mapped = nullptr;
  uint32_t upload_used = kZeroBytes;

  VkDescriptorSetLayout shared_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout constants_layout = VK_NULL_HANDLE;
  std::map<uint32_t, VkDescriptorSetLayout> texture_layouts;
  std::map<uint64_t, VkPipelineLayout> pipeline_layouts;
  VkDescriptorPool static_pool = VK_NULL_HANDLE;
  VkDescriptorPool frame_pool = VK_NULL_HANDLE;
  VkDescriptorSet shared_set = VK_NULL_HANDLE;
  VkDescriptorSet blit_set = VK_NULL_HANDLE;
  uint32_t sets_used = 0;
  uint32_t shared_binding_count = 1;

  std::unordered_map<uint32_t, Target> targets;
  std::unordered_map<uint32_t, HostTexture> textures;
  std::unordered_map<uint64_t, std::unique_ptr<SpirvShader>> shaders;
  std::unordered_map<const void*, VkShaderModule> modules;
  std::map<PipelineKey, VkPipeline> pipelines;
  std::map<std::array<uint32_t, 5>, VkRenderPass> render_passes;
  std::map<std::array<uint64_t, 7>, VkFramebuffer> framebuffers;
  std::unordered_map<uint64_t, VkSampler> samplers;
  std::vector<Retired> retired;
  std::vector<VkFramebuffer> retired_framebuffers;

  std::unique_ptr<SpirvShaderTranslator> translator;
  bool image_view_format_swizzle = true;
  bool independent_blend = false;
  bool anisotropy = false;
  float max_anisotropy = 1.0f;
  bool mirror_clamp_to_edge = false;
  StringBuffer disasm;

  VkRenderPass open_render_pass = VK_NULL_HANDLE;
  VkFramebuffer open_framebuffer = VK_NULL_HANDLE;

  VkImage null_images[3] = {};
  VkDeviceMemory null_memory[3] = {};
  VkImageView null_views[3] = {};
  VkImageLayout null_layouts[3] = {VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_UNDEFINED};

  VkRenderPass blit_render_pass = VK_NULL_HANDLE;
  VkDescriptorSetLayout blit_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout blit_layout = VK_NULL_HANDLE;
  VkPipeline blit_pipeline = VK_NULL_HANDLE;
  VkSampler blit_sampler = VK_NULL_HANDLE;
  VkShaderModule blit_vs = VK_NULL_HANDLE;
  VkShaderModule blit_ps = VK_NULL_HANDLE;
  std::map<uint64_t, VkFramebuffer> blit_framebuffers;

  VkBuffer readback = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;
  uint8_t* readback_mapped = nullptr;
  VkDeviceSize readback_bytes = 0;
};

State s;

const uivk::VulkanDevice::Functions& F() { return s.device->functions(); }

bool BeginRecording() {
  if (s.recording) {
    return true;
  }
  if (F().vkResetCommandPool(s.vk, s.pool, 0) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferBeginInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (F().vkBeginCommandBuffer(s.cmd, &info) != VK_SUCCESS) {
    XELOGE("[xna] direct vulkan: could not begin the command buffer");
    return false;
  }
  s.recording = true;
  return true;
}

void EndRenderPass() {
  if (s.open_render_pass != VK_NULL_HANDLE) {
    F().vkCmdEndRenderPass(s.cmd);
    s.open_render_pass = VK_NULL_HANDLE;
    s.open_framebuffer = VK_NULL_HANDLE;
  }
}

void DestroyRetired() {
  for (const Retired& item : s.retired) {
    if (item.view != VK_NULL_HANDLE) {
      F().vkDestroyImageView(s.vk, item.view, nullptr);
    }
    if (item.image != VK_NULL_HANDLE) {
      F().vkDestroyImage(s.vk, item.image, nullptr);
    }
    if (item.memory != VK_NULL_HANDLE) {
      F().vkFreeMemory(s.vk, item.memory, nullptr);
    }
  }
  s.retired.clear();
  for (VkFramebuffer framebuffer : s.retired_framebuffers) {
    F().vkDestroyFramebuffer(s.vk, framebuffer, nullptr);
  }
  s.retired_framebuffers.clear();
}

void Submit() {
  if (!s.recording) {
    return;
  }
  EndRenderPass();
  s.recording = false;
  bool submitted = false;
  if (F().vkEndCommandBuffer(s.cmd) == VK_SUCCESS) {
    uivk::util::FlushMappedMemoryRange(s.device, s.upload_memory,
                                       s.upload_memory_type, 0,
                                       s.upload_memory_size, s.upload_used);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s.cmd;
    auto queue = s.device->AcquireQueue(s.queue_family, 0);
    submitted = s.device->SubmitAndUpdateLost(queue.queue(), 1, &submit,
                                              s.fence) == VK_SUCCESS;
  }
  if (submitted) {
    F().vkWaitForFences(s.vk, 1, &s.fence, VK_TRUE, UINT64_MAX);
    F().vkResetFences(s.vk, 1, &s.fence);
  } else {
    XELOGE("[xna] direct vulkan: the command buffer failed to submit");
  }
  F().vkResetDescriptorPool(s.vk, s.frame_pool, 0);
  s.sets_used = 0;
  s.upload_used = kZeroBytes;
  DestroyRetired();
}

bool EnsureRoom(uint64_t upload_bytes, uint32_t sets) {
  if (upload_bytes > kUploadBytes - kZeroBytes || sets > kSetsPerPool) {
    return false;
  }
  const bool upload_full =
      uint64_t(xe::align(s.upload_used, 256u)) + upload_bytes > kUploadBytes;
  const bool sets_full = s.sets_used + sets > kSetsPerPool;
  if (!upload_full && !sets_full) {
    return true;
  }
  Submit();
  return BeginRecording();
}

uint8_t* AllocateUpload(uint32_t bytes, uint32_t alignment, uint32_t* offset) {
  const uint32_t at = xe::align(s.upload_used, alignment);
  if (uint64_t(at) + bytes > kUploadBytes) {
    return nullptr;
  }
  s.upload_used = at + bytes;
  *offset = at;
  return s.upload_mapped + at;
}

void ImageBarrier(VkImage image, VkImageAspectFlags aspect,
                  VkImageLayout& layout, VkImageLayout wanted) {
  if (layout == wanted) {
    return;
  }
  EndRenderPass();
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask =
      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = layout;
  barrier.newLayout = wanted;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = uivk::util::InitializeSubresourceRange(aspect);
  F().vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);
  layout = wanted;
}

bool CreateImage(VkImageType type, VkFormat format, uint32_t width,
                 uint32_t height, uint32_t depth, uint32_t levels,
                 uint32_t layers, VkImageUsageFlags usage,
                 VkImageCreateFlags flags, VkImage* image,
                 VkDeviceMemory* memory) {
  VkImageCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.flags = flags;
  info.imageType = type;
  info.format = format;
  info.extent.width = width;
  info.extent.height = height;
  info.extent.depth = depth;
  info.mipLevels = levels;
  info.arrayLayers = layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return uivk::util::CreateDedicatedAllocationImage(
      s.device, info, uivk::util::MemoryPurpose::kDeviceLocal, *image, *memory);
}

VkImageView CreateView(VkImage image, VkImageViewType type, VkFormat format,
                       VkImageAspectFlags aspect, uint32_t levels,
                       uint32_t layers, const VkComponentMapping& mapping) {
  VkImageViewCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = image;
  info.viewType = type;
  info.format = format;
  info.components = mapping;
  info.subresourceRange =
      uivk::util::InitializeSubresourceRange(aspect, 0, levels, 0, layers);
  VkImageView view = VK_NULL_HANDLE;
  if (F().vkCreateImageView(s.vk, &info, nullptr, &view) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return view;
}

VkFormat TargetFormatFor(uint32_t surface_format) {
  switch (surface_format) {
    case 9:  return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 10: return VK_FORMAT_R16G16_UNORM;
    case 11: return VK_FORMAT_R16G16B16A16_UNORM;
    case 13: return VK_FORMAT_R32_SFLOAT;
    case 14: return VK_FORMAT_R32G32_SFLOAT;
    case 15: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 16: return VK_FORMAT_R16_SFLOAT;
    case 17: return VK_FORMAT_R16G16_SFLOAT;
    case 18:
    case 19: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

TextureFormatInfo TextureFormatFor(uint32_t surface_format) {
  const VkComponentMapping id = {};
  switch (surface_format) {
    case 1:
      return {VK_FORMAT_R5G6B5_UNORM_PACK16, id, 1, 2};
    case 2:
      return {VK_FORMAT_A1R5G5B5_UNORM_PACK16, id, 1, 2};
    case 3:
      return {VK_FORMAT_R4G4B4A4_UNORM_PACK16,
              {VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
               VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R},
              1, 2};
    case 4:
      return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, id, 4, 8};
    case 5:
      return {VK_FORMAT_BC2_UNORM_BLOCK, id, 4, 16};
    case 6:
      return {VK_FORMAT_BC3_UNORM_BLOCK, id, 4, 16};
    case 7:
      return {VK_FORMAT_R8G8_SNORM, id, 1, 2};
    case 8:
      return {VK_FORMAT_R8G8B8A8_SNORM, id, 1, 4};
    case 9:
      return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, id, 1, 4};
    case 10:
      return {VK_FORMAT_R16G16_UNORM, id, 1, 4};
    case 11:
      return {VK_FORMAT_R16G16B16A16_UNORM, id, 1, 8};
    case 12:
      return {VK_FORMAT_R8_UNORM,
              {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
               VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R},
              1, 1};
    case 13:
      return {VK_FORMAT_R32_SFLOAT, id, 1, 4};
    case 14:
      return {VK_FORMAT_R32G32_SFLOAT, id, 1, 8};
    case 15:
      return {VK_FORMAT_R32G32B32A32_SFLOAT, id, 1, 16};
    case 16:
      return {VK_FORMAT_R16_SFLOAT, id, 1, 2};
    case 17:
      return {VK_FORMAT_R16G16_SFLOAT, id, 1, 4};
    case 18:
    case 19:
      return {VK_FORMAT_R16G16B16A16_SFLOAT, id, 1, 8};
    default:
      return {VK_FORMAT_R8G8B8A8_UNORM, id, 1, 4};
  }
}

void RetireFramebuffers() {
  for (auto& entry : s.framebuffers) {
    s.retired_framebuffers.push_back(entry.second);
  }
  s.framebuffers.clear();
}

void RetireTarget(Target& target) {
  EndRenderPass();
  s.retired.push_back({VK_NULL_HANDLE, target.attachment_view, VK_NULL_HANDLE});
  s.retired.push_back({VK_NULL_HANDLE, target.sampled_view, VK_NULL_HANDLE});
  s.retired.push_back({target.color, VK_NULL_HANDLE, target.color_memory});
  s.retired.push_back({VK_NULL_HANDLE, target.depth_view, VK_NULL_HANDLE});
  s.retired.push_back({target.depth, VK_NULL_HANDLE, target.depth_memory});
  target = Target();
  RetireFramebuffers();
}

Target* EnsureTarget(uint32_t key, uint32_t width, uint32_t height,
                     uint32_t surface_format) {
  VkFormat format = kBackBufferFormat;
  if (!key) {
    width = kBackBufferWidth;
    height = kBackBufferHeight;
  } else {
    format = TargetFormatFor(surface_format);
  }
  if (!width || !height) {
    return nullptr;
  }
  Target& target = s.targets[key];
  if (target.color != VK_NULL_HANDLE && target.width == width &&
      target.height == height && target.format == format) {
    return &target;
  }
  if (target.color != VK_NULL_HANDLE) {
    RetireTarget(target);
  }
  const VkComponentMapping id = {};
  if (!CreateImage(VK_IMAGE_TYPE_2D, format, width, height, 1, 1, 1,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   0, &target.color, &target.color_memory)) {
    XELOGE("[xna] direct vulkan: could not create a {}x{} render target",
           width, height);
    target = Target();
    return nullptr;
  }
  target.attachment_view = CreateView(target.color, VK_IMAGE_VIEW_TYPE_2D,
                                      format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                                      id);
  target.sampled_view = CreateView(target.color, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                   format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, id);
  if (!CreateImage(VK_IMAGE_TYPE_2D, kDepthFormat, width, height, 1, 1, 1,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   0, &target.depth, &target.depth_memory)) {
    XELOGE("[xna] direct vulkan: could not create a {}x{} depth buffer", width,
           height);
    RetireTarget(target);
    return nullptr;
  }
  target.depth_view = CreateView(target.depth, VK_IMAGE_VIEW_TYPE_2D,
                                 kDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1,
                                 id);
  target.width = width;
  target.height = height;
  target.format = format;
  target.color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  target.depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (target.attachment_view == VK_NULL_HANDLE ||
      target.sampled_view == VK_NULL_HANDLE ||
      target.depth_view == VK_NULL_HANDLE) {
    RetireTarget(target);
    return nullptr;
  }
  ImageBarrier(target.color, VK_IMAGE_ASPECT_COLOR_BIT, target.color_layout,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkClearColorValue zero = {};
  const VkImageSubresourceRange color_range =
      uivk::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  F().vkCmdClearColorImage(s.cmd, target.color,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                           &color_range);
  ImageBarrier(target.depth, VK_IMAGE_ASPECT_DEPTH_BIT, target.depth_layout,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkClearDepthStencilValue far_depth = {1.0f, 0};
  const VkImageSubresourceRange depth_range =
      uivk::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT);
  F().vkCmdClearDepthStencilImage(s.cmd, target.depth,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &far_depth, 1, &depth_range);
  RetireFramebuffers();
  XELOGD("[xna] direct vulkan target {:08X}: {}x{} format {}", key, width,
         height, uint32_t(format));
  return &target;
}

VkImageView EnsureTexture(const XnaGpuTextureBinding& binding) {
  if (!binding.handle) {
    return VK_NULL_HANDLE;
  }
  XnaTextureView base;
  if (!XnaLookupTexture(binding.handle, 0, &base) || !base.width ||
      !base.height || !base.data) {
    return VK_NULL_HANDLE;
  }
  std::vector<XnaTextureView> views(1, base);
  const uint32_t wanted_levels = std::max<uint32_t>(binding.levels, 1);
  for (uint32_t level = 1; level < wanted_levels; ++level) {
    XnaTextureView view;
    if (!XnaLookupTexture(binding.handle, level, &view) || !view.width ||
        !view.height || !view.data) {
      break;
    }
    views.push_back(view);
  }
  const uint32_t levels = uint32_t(views.size());
  const TextureFormatInfo info = TextureFormatFor(base.format);

  HostTexture& texture = s.textures[binding.handle];
  const bool same_shape = texture.image != VK_NULL_HANDLE &&
                          texture.width == base.width &&
                          texture.height == base.height &&
                          texture.levels == levels &&
                          texture.format == info.format;
  if (same_shape && texture.version == base.version) {
    return texture.view;
  }

  uint64_t total = 0;
  for (const XnaTextureView& view : views) {
    total += xe::align<uint64_t>(view.size, 16) + 16;
  }
  if (!EnsureRoom(total, 0)) {
    XELOGW("[xna] direct vulkan: texture {:08X} needs {} upload bytes",
           binding.handle, total);
    return VK_NULL_HANDLE;
  }

  if (!same_shape) {
    if (texture.image != VK_NULL_HANDLE) {
      EndRenderPass();
      s.retired.push_back({VK_NULL_HANDLE, texture.view, VK_NULL_HANDLE});
      s.retired.push_back({texture.image, VK_NULL_HANDLE, texture.memory});
      texture = HostTexture();
    }
    if (!CreateImage(VK_IMAGE_TYPE_2D, info.format, base.width, base.height, 1,
                     levels, 1,
                     VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     0, &texture.image, &texture.memory)) {
      XELOGE("[xna] direct vulkan: could not create texture {:08X} {}x{} "
             "format {}",
             binding.handle, base.width, base.height, base.format);
      texture = HostTexture();
      return VK_NULL_HANDLE;
    }
    texture.view = CreateView(texture.image, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                              info.format, VK_IMAGE_ASPECT_COLOR_BIT, levels, 1,
                              info.mapping);
    texture.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (texture.view == VK_NULL_HANDLE) {
      s.retired.push_back({texture.image, VK_NULL_HANDLE, texture.memory});
      texture = HostTexture();
      return VK_NULL_HANDLE;
    }
  }

  const uint32_t unit = XnaTextureSwapUnit(base.format);
  std::vector<VkBufferImageCopy> regions;
  for (uint32_t level = 0; level < levels; ++level) {
    const XnaTextureView& view = views[level];
    uint32_t offset = 0;
    uint8_t* out = AllocateUpload(view.size, 16, &offset);
    if (!out) {
      return VK_NULL_HANDLE;
    }
    XnaCopyTextureRow(out, view.data, view.size, unit);
    const uint32_t blocks_wide = (view.width + info.block - 1) / info.block;
    const uint32_t rows = (view.height + info.block - 1) / info.block;
    const uint32_t tight_row = blocks_wide * info.bytes;
    const uint32_t source_row = rows ? view.size / rows : tight_row;
    VkBufferImageCopy region = {};
    region.bufferOffset = offset;
    region.bufferRowLength =
        (source_row != tight_row && source_row % info.bytes == 0)
            ? (source_row / info.bytes) * info.block
            : 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = level;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = view.width;
    region.imageExtent.height = view.height;
    region.imageExtent.depth = 1;
    regions.push_back(region);
  }
  ImageBarrier(texture.image, VK_IMAGE_ASPECT_COLOR_BIT, texture.layout,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  F().vkCmdCopyBufferToImage(s.cmd, s.upload, texture.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             uint32_t(regions.size()), regions.data());
  ImageBarrier(texture.image, VK_IMAGE_ASPECT_COLOR_BIT, texture.layout,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  texture.version = base.version;
  texture.width = base.width;
  texture.height = base.height;
  texture.levels = levels;
  texture.format = info.format;
  return texture.view;
}

VkSamplerAddressMode AddressModeFor(uint32_t clamp) {
  switch (xenos::ClampMode(clamp & 7)) {
    case xenos::ClampMode::kRepeat:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case xenos::ClampMode::kMirroredRepeat:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
    case xenos::ClampMode::kMirrorClampToBorder:
      return s.mirror_clamp_to_edge
                 ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                 : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case xenos::ClampMode::kClampToBorder:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  }
}

uint64_t SamplerKeyFor(const SpirvShader::SamplerBinding& binding,
                       const XnaGpuTextureBinding& texture) {
  const uint32_t mag =
      binding.mag_filter == xenos::TextureFilter::kUseFetchConst
          ? texture.mag_filter
          : uint32_t(binding.mag_filter);
  const uint32_t min =
      binding.min_filter == xenos::TextureFilter::kUseFetchConst
          ? texture.min_filter
          : uint32_t(binding.min_filter);
  const uint32_t mip =
      binding.mip_filter == xenos::TextureFilter::kUseFetchConst
          ? texture.mip_filter
          : uint32_t(binding.mip_filter);
  const uint32_t aniso =
      binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
          ? texture.aniso
          : uint32_t(binding.aniso_filter);
  return uint64_t(mag & 3) | (uint64_t(min & 3) << 2) |
         (uint64_t(mip & 3) << 4) | (uint64_t(aniso & 7) << 6) |
         (uint64_t(texture.address_u & 7) << 9) |
         (uint64_t(texture.address_v & 7) << 12);
}

VkSampler SamplerFor(uint64_t key) {
  auto found = s.samplers.find(key);
  if (found != s.samplers.end()) {
    return found->second;
  }
  const uint32_t mag = uint32_t(key & 3);
  const uint32_t min = uint32_t((key >> 2) & 3);
  const uint32_t mip = uint32_t((key >> 4) & 3);
  const uint32_t aniso = uint32_t((key >> 6) & 7);
  VkSamplerCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter = mag == uint32_t(xenos::TextureFilter::kLinear)
                       ? VK_FILTER_LINEAR
                       : VK_FILTER_NEAREST;
  info.minFilter = min == uint32_t(xenos::TextureFilter::kLinear)
                       ? VK_FILTER_LINEAR
                       : VK_FILTER_NEAREST;
  info.mipmapMode = mip == uint32_t(xenos::TextureFilter::kLinear)
                        ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                        : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = AddressModeFor(uint32_t((key >> 9) & 7));
  info.addressModeV = AddressModeFor(uint32_t((key >> 12) & 7));
  info.addressModeW = info.addressModeV;
  info.mipLodBias = 0.0f;
  if (s.anisotropy && aniso >= 2 && aniso <= 5) {
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy =
        std::min(float(1u << (aniso - 1)), s.max_anisotropy);
  } else {
    info.anisotropyEnable = VK_FALSE;
    info.maxAnisotropy = 1.0f;
  }
  info.compareEnable = VK_FALSE;
  info.compareOp = VK_COMPARE_OP_NEVER;
  info.minLod = 0.0f;
  info.maxLod = mip == uint32_t(xenos::TextureFilter::kBaseMap)
                    ? 0.25f
                    : VK_LOD_CLAMP_NONE;
  info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  info.unnormalizedCoordinates = VK_FALSE;
  VkSampler sampler = VK_NULL_HANDLE;
  if (F().vkCreateSampler(s.vk, &info, nullptr, &sampler) != VK_SUCCESS) {
    sampler = VK_NULL_HANDLE;
  }
  s.samplers.emplace(key, sampler);
  return sampler;
}

VkDescriptorSetLayout TextureLayoutFor(bool is_vertex, uint32_t textures,
                                       uint32_t samplers) {
  const uint32_t key =
      (is_vertex ? 1u : 0u) | (textures << 1) | (samplers << 16);
  auto found = s.texture_layouts.find(key);
  if (found != s.texture_layouts.end()) {
    return found->second;
  }
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  const VkShaderStageFlags stage =
      is_vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (uint32_t i = 0; i < textures; ++i) {
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = i;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = stage;
    bindings.push_back(binding);
  }
  for (uint32_t i = 0; i < samplers; ++i) {
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = textures + i;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = stage;
    bindings.push_back(binding);
  }
  VkDescriptorSetLayoutCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = uint32_t(bindings.size());
  info.pBindings = bindings.empty() ? nullptr : bindings.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  if (F().vkCreateDescriptorSetLayout(s.vk, &info, nullptr, &layout) !=
      VK_SUCCESS) {
    layout = VK_NULL_HANDLE;
  }
  s.texture_layouts.emplace(key, layout);
  return layout;
}

VkPipelineLayout PipelineLayoutFor(uint32_t textures_pixel,
                                   uint32_t samplers_pixel,
                                   uint32_t textures_vertex,
                                   uint32_t samplers_vertex) {
  const uint64_t key = uint64_t(textures_pixel) |
                       (uint64_t(samplers_pixel) << 16) |
                       (uint64_t(textures_vertex) << 32) |
                       (uint64_t(samplers_vertex) << 48);
  auto found = s.pipeline_layouts.find(key);
  if (found != s.pipeline_layouts.end()) {
    return found->second;
  }
  VkDescriptorSetLayout layouts[SpirvShaderTranslator::kDescriptorSetCount];
  layouts[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
      s.shared_layout;
  layouts[SpirvShaderTranslator::kDescriptorSetConstants] = s.constants_layout;
  layouts[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
      TextureLayoutFor(true, textures_vertex, samplers_vertex);
  layouts[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
      TextureLayoutFor(false, textures_pixel, samplers_pixel);
  for (VkDescriptorSetLayout layout : layouts) {
    if (layout == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
  }
  VkPipelineLayoutCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  info.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
  info.pSetLayouts = layouts;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (F().vkCreatePipelineLayout(s.vk, &info, nullptr, &layout) !=
      VK_SUCCESS) {
    layout = VK_NULL_HANDLE;
  }
  s.pipeline_layouts.emplace(key, layout);
  return layout;
}

VkRenderPass RenderPassFor(const VkFormat* formats, uint32_t count) {
  std::array<uint32_t, 5> key = {count, 0, 0, 0, 0};
  for (uint32_t i = 0; i < count; ++i) {
    key[1 + i] = uint32_t(formats[i]);
  }
  auto found = s.render_passes.find(key);
  if (found != s.render_passes.end()) {
    return found->second;
  }
  VkAttachmentDescription attachments[5] = {};
  VkAttachmentReference color_refs[4] = {};
  for (uint32_t i = 0; i < count; ++i) {
    attachments[i].format = formats[i];
    attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[i].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_refs[i].attachment = i;
    color_refs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  VkAttachmentDescription& depth = attachments[count];
  depth.format = kDepthFormat;
  depth.samples = VK_SAMPLE_COUNT_1_BIT;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference depth_ref = {};
  depth_ref.attachment = count;
  depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = count;
  subpass.pColorAttachments = color_refs;
  subpass.pDepthStencilAttachment = &depth_ref;
  VkRenderPassCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = count + 1;
  info.pAttachments = attachments;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (F().vkCreateRenderPass(s.vk, &info, nullptr, &render_pass) !=
      VK_SUCCESS) {
    render_pass = VK_NULL_HANDLE;
  }
  s.render_passes.emplace(key, render_pass);
  return render_pass;
}

VkFramebuffer FramebufferFor(VkRenderPass render_pass, Target* const* targets,
                             uint32_t count) {
  std::array<uint64_t, 7> key = {};
  key[0] = uint64_t(render_pass);
  for (uint32_t i = 0; i < count; ++i) {
    key[1 + i] = uint64_t(targets[i]->attachment_view);
  }
  key[5] = uint64_t(targets[0]->depth_view);
  key[6] = (uint64_t(targets[0]->width) << 32) | targets[0]->height;
  auto found = s.framebuffers.find(key);
  if (found != s.framebuffers.end()) {
    return found->second;
  }
  VkImageView views[5];
  for (uint32_t i = 0; i < count; ++i) {
    views[i] = targets[i]->attachment_view;
  }
  views[count] = targets[0]->depth_view;
  VkFramebufferCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  info.renderPass = render_pass;
  info.attachmentCount = count + 1;
  info.pAttachments = views;
  info.width = targets[0]->width;
  info.height = targets[0]->height;
  info.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  if (F().vkCreateFramebuffer(s.vk, &info, nullptr, &framebuffer) !=
      VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  s.framebuffers.emplace(key, framebuffer);
  return framebuffer;
}

VkBlendFactor BlendFor(uint32_t xna, bool alpha) {
  switch (xna) {
    case 0:  return VK_BLEND_FACTOR_ONE;
    case 1:  return VK_BLEND_FACTOR_ZERO;
    case 2:  return alpha ? VK_BLEND_FACTOR_SRC_ALPHA
                          : VK_BLEND_FACTOR_SRC_COLOR;
    case 3:  return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                          : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 4:  return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6:  return alpha ? VK_BLEND_FACTOR_DST_ALPHA
                          : VK_BLEND_FACTOR_DST_COLOR;
    case 7:  return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA
                          : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 8:  return VK_BLEND_FACTOR_DST_ALPHA;
    case 9:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 10: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 12: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp BlendOpFor(uint32_t xna) {
  switch (xna) {
    case 1:  return VK_BLEND_OP_SUBTRACT;
    case 2:  return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 3:  return VK_BLEND_OP_MIN;
    case 4:  return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
  }
}

VkCompareOp CompareFor(uint32_t xna) {
  switch (xna) {
    case 1:  return VK_COMPARE_OP_NEVER;
    case 2:  return VK_COMPARE_OP_LESS;
    case 3:  return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 4:  return VK_COMPARE_OP_EQUAL;
    case 5:  return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case 6:  return VK_COMPARE_OP_GREATER;
    case 7:  return VK_COMPARE_OP_NOT_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
  }
}

bool TopologyFor(xenos::PrimitiveType primitive,
                 VkPrimitiveTopology* topology, bool* polygonal,
                 bool* line) {
  *polygonal = false;
  *line = false;
  switch (primitive) {
    case xenos::PrimitiveType::kPointList:
      *topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      return true;
    case xenos::PrimitiveType::kLineList:
      *topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      *line = true;
      return true;
    case xenos::PrimitiveType::kLineStrip:
      *topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      *line = true;
      return true;
    case xenos::PrimitiveType::kTriangleList:
      *topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      *polygonal = true;
      return true;
    case xenos::PrimitiveType::kTriangleStrip:
      *topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      *polygonal = true;
      return true;
    default:
      return false;
  }
}

SpirvShader* HostShaderFor(Shader* original) {
  const uint64_t hash = original->ucode_data_hash();
  auto found = s.shaders.find(hash);
  if (found != s.shaders.end()) {
    return found->second.get();
  }
  auto shader = std::make_unique<SpirvShader>(
      original->type(), hash, original->ucode_dwords(),
      original->ucode_dword_count(), std::endian::native);
  shader->AnalyzeUcode(s.disasm);
  if (!shader->is_ucode_analyzed()) {
    XELOGW("[xna] direct vulkan: shader {:016X} did not analyze", hash);
    return nullptr;
  }
  SpirvShader* result = shader.get();
  s.shaders.emplace(hash, std::move(shader));
  return result;
}

Shader::Translation* Translate(SpirvShader* shader, uint64_t modification) {
  Shader::Translation* translation =
      shader->GetOrCreateTranslation(modification);
  if (!translation->is_translated()) {
    s.translator->TranslateAnalyzedShader(*translation);
    if (!translation->is_valid()) {
      XELOGW("[xna] direct vulkan: {} shader {:016X} failed to translate: {}",
             shader->type() == xenos::ShaderType::kVertex ? "vertex" : "pixel",
             shader->ucode_data_hash(),
             translation->errors().empty()
                 ? std::string("no error given")
                 : translation->errors().front().message);
    }
  }
  return translation->is_valid() ? translation : nullptr;
}

VkShaderModule ModuleFor(Shader::Translation* translation) {
  auto found = s.modules.find(translation);
  if (found != s.modules.end()) {
    return found->second;
  }
  const auto& binary = translation->translated_binary();
  VkShaderModule module = uivk::util::CreateShaderModule(
      s.device, reinterpret_cast<const uint32_t*>(binary.data()),
      binary.size());
  if (module == VK_NULL_HANDLE) {
    XELOGE("[xna] direct vulkan: could not create a module for shader {:016X}",
           translation->shader().ucode_data_hash());
  }
  s.modules.emplace(translation, module);
  return module;
}

bool TranslatePair(SpirvShader* vertex, SpirvShader* pixel,
                   Shader::Translation** vertex_out,
                   Shader::Translation** pixel_out) {
  reg::SQ_PROGRAM_CNTL program_cntl;
  program_cntl.value = 0;
  program_cntl.vs_export_mode = xenos::VertexShaderExportMode::kPosition1Vector;
  reg::SQ_CONTEXT_MISC context_misc;
  context_misc.value = 0;
  uint32_t param_gen_pos = UINT32_MAX;
  uint32_t mask = 0;
  if (pixel) {
    mask = vertex->writes_interpolators() &
           pixel->GetInterpolatorInputMask(program_cntl, context_misc,
                                           param_gen_pos);
  }
  SpirvShaderTranslator::Modification vertex_modification(
      s.translator->GetDefaultVertexShaderModification(
          vertex->GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg),
          Shader::HostVertexShaderType::kVertex));
  vertex_modification.vertex.interpolator_mask = mask;
  vertex_modification.vertex.output_point_parameters = 0;
  vertex_modification.vertex.user_clip_plane_count = 0;
  vertex_modification.vertex.user_clip_plane_cull = 0;
  *vertex_out = Translate(vertex, vertex_modification.value);
  if (!*vertex_out) {
    return false;
  }
  *pixel_out = nullptr;
  if (!pixel) {
    return true;
  }
  SpirvShaderTranslator::Modification pixel_modification(
      s.translator->GetDefaultPixelShaderModification(
          pixel->GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg)));
  pixel_modification.pixel.interpolator_mask = mask;
  pixel_modification.pixel.interpolators_centroid =
      mask & ~xenos::GetInterpolatorSamplingPattern(
                 xenos::MsaaSamples::k1X, context_misc.sc_sample_cntl, 0);
  if (param_gen_pos < xenos::kMaxInterpolators) {
    pixel_modification.pixel.param_gen_enable = 1;
    pixel_modification.pixel.param_gen_interpolator = param_gen_pos;
  } else {
    pixel_modification.pixel.param_gen_enable = 0;
    pixel_modification.pixel.param_gen_interpolator = 0;
  }
  pixel_modification.pixel.param_gen_point = 0;
  pixel_modification.pixel.depth_stencil_mode =
      SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers;
  pixel_modification.pixel.rt0_blend_rgb_factor_for_premult =
      xenos::BlendFactor::kOne;
  pixel_modification.pixel.rt0_blend_a_factor_for_premult =
      xenos::BlendFactor::kOne;
  *pixel_out = Translate(pixel, pixel_modification.value);
  return *pixel_out != nullptr;
}

VkPipeline PipelineFor(const PipelineKey& key, VkShaderModule vertex,
                       VkShaderModule pixel, VkPipelineLayout layout,
                       VkRenderPass render_pass, VkPrimitiveTopology topology) {
  auto found = s.pipelines.find(key);
  if (found != s.pipelines.end()) {
    return found->second;
  }
  VkPipelineShaderStageCreateInfo stages[2] = {};
  uint32_t stage_count = 0;
  stages[stage_count].sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[stage_count].module = vertex;
  stages[stage_count].pName = "main";
  ++stage_count;
  if (pixel != VK_NULL_HANDLE) {
    stages[stage_count].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stage_count].module = pixel;
    stages[stage_count].pName = "main";
    ++stage_count;
  }
  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = topology;
  VkPipelineViewportStateCreateInfo viewport = {};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rasterization = {};
  rasterization.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization.cullMode = VK_CULL_MODE_NONE;
  rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
  depth_stencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil.depthTestEnable = key.depth_enable ? VK_TRUE : VK_FALSE;
  depth_stencil.depthWriteEnable =
      (key.depth_enable && key.depth_write) ? VK_TRUE : VK_FALSE;
  depth_stencil.depthCompareOp = CompareFor(key.depth_function);
  const bool color_blend =
      !(key.blend[0] == 0 && key.blend[1] == 1 && key.blend[2] == 0);
  const bool alpha_blend =
      !(key.blend[3] == 0 && key.blend[4] == 1 && key.blend[5] == 0);
  VkPipelineColorBlendAttachmentState attachments[4] = {};
  for (uint32_t i = 0; i < key.target_count; ++i) {
    VkPipelineColorBlendAttachmentState& blend = attachments[i];
    blend.blendEnable = (color_blend || alpha_blend) ? VK_TRUE : VK_FALSE;
    blend.srcColorBlendFactor = BlendFor(key.blend[0], false);
    blend.dstColorBlendFactor = BlendFor(key.blend[1], false);
    blend.colorBlendOp = BlendOpFor(key.blend[2]);
    blend.srcAlphaBlendFactor = BlendFor(key.blend[3], true);
    blend.dstAlphaBlendFactor = BlendFor(key.blend[4], true);
    blend.alphaBlendOp = BlendOpFor(key.blend[5]);
    blend.colorWriteMask =
        VkColorComponentFlags(key.write[s.independent_blend ? i : 0] & 0xF);
  }
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount = key.target_count;
  color_blend_state.pAttachments = attachments;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                           VK_DYNAMIC_STATE_SCISSOR,
                                           VK_DYNAMIC_STATE_BLEND_CONSTANTS};
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = uint32_t(xe::countof(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;
  VkGraphicsPipelineCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &rasterization;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth_stencil;
  info.pColorBlendState = &color_blend_state;
  info.pDynamicState = &dynamic;
  info.layout = layout;
  info.renderPass = render_pass;
  info.subpass = 0;
  info.basePipelineIndex = -1;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (F().vkCreateGraphicsPipelines(s.vk, VK_NULL_HANDLE, 1, &info, nullptr,
                                    &pipeline) != VK_SUCCESS) {
    XELOGE("[xna] direct vulkan: pipeline creation failed");
    pipeline = VK_NULL_HANDLE;
  }
  s.pipelines.emplace(key, pipeline);
  return pipeline;
}

uint32_t StrideForFetch(const SpirvShader* shader, uint32_t fetch_constant) {
  for (const auto& binding : shader->vertex_bindings()) {
    if (binding.fetch_constant == fetch_constant) {
      return binding.stride_words * 4;
    }
  }
  return 0;
}

uint32_t FloatCount(const SpirvShader* shader) {
  if (!shader) {
    return 0;
  }
  uint32_t count = 0;
  for (uint32_t word = 0; word < 4; ++word) {
    count += xe::bit_count(shader->constant_register_map().float_bitmap[word]);
  }
  return count;
}

bool PackFloats(const SpirvShader* shader, const float* registers,
                VkDescriptorBufferInfo* info) {
  info->buffer = s.upload;
  if (!shader) {
    info->offset = 0;
    info->range = 16;
    return true;
  }
  const auto& map = shader->constant_register_map();
  const uint32_t count = std::max<uint32_t>(FloatCount(shader), 1);
  uint32_t offset = 0;
  uint8_t* out = AllocateUpload(count * 16, 256, &offset);
  if (!out) {
    return false;
  }
  std::memset(out, 0, 16);
  for (uint32_t word = 0; word < 4; ++word) {
    uint64_t bits = map.float_bitmap[word];
    uint32_t index;
    while (xe::bit_scan_forward(bits, &index)) {
      bits = xe::clear_lowest_bit(bits);
      std::memcpy(out, registers + size_t(word * 64 + index) * 4, 16);
      out += 16;
    }
  }
  info->offset = offset;
  info->range = count * 16;
  return true;
}

bool AllocateSets(VkDescriptorSetLayout* layouts, uint32_t count,
                  VkDescriptorSet* sets) {
  if (!count) {
    return true;
  }
  VkDescriptorSetAllocateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  info.descriptorPool = s.frame_pool;
  info.descriptorSetCount = count;
  info.pSetLayouts = layouts;
  if (F().vkAllocateDescriptorSets(s.vk, &info, sets) != VK_SUCCESS) {
    return false;
  }
  s.sets_used += count;
  return true;
}

bool DrawLocked(const XnaGpuDraw& draw, const XnaGpuStream* streams,
                uint32_t stream_count) {
  auto* memory = kernel_state() ? kernel_state()->memory() : nullptr;
  auto* original_vertex = static_cast<Shader*>(draw.vertex_shader);
  auto* original_pixel = static_cast<Shader*>(draw.pixel_shader);
  if (!memory || !original_vertex || !streams || !stream_count) {
    return false;
  }

  xenos::PrimitiveType primitive = xenos::PrimitiveType::kTriangleList;
  if (!XnaPrimitiveToXenos(draw.primitive_type, &primitive)) {
    XELOGW("[xna] direct vulkan: no conversion for XNA PrimitiveType {}",
           draw.primitive_type);
    return false;
  }
  VkPrimitiveTopology topology;
  bool polygonal = false;
  bool line = false;
  if (!TopologyFor(primitive, &topology, &polygonal, &line)) {
    XELOGW("[xna] direct vulkan: Xenos primitive {} has no Vulkan topology",
           uint32_t(primitive));
    return false;
  }
  uint32_t index_count = draw.primitive_count * 3;
  XenosPrimitiveIndexCount(primitive, draw.primitive_count, &index_count);
  if (!index_count) {
    return false;
  }

  SpirvShader* vertex = HostShaderFor(original_vertex);
  SpirvShader* pixel = original_pixel ? HostShaderFor(original_pixel) : nullptr;
  if (!vertex || (original_pixel && !pixel)) {
    return false;
  }
  Shader::Translation* vertex_translation = nullptr;
  Shader::Translation* pixel_translation = nullptr;
  if (!TranslatePair(vertex, pixel, &vertex_translation, &pixel_translation)) {
    return false;
  }
  VkShaderModule vertex_module = ModuleFor(vertex_translation);
  VkShaderModule pixel_module =
      pixel_translation ? ModuleFor(pixel_translation) : VK_NULL_HANDLE;
  if (vertex_module == VK_NULL_HANDLE ||
      (pixel_translation && pixel_module == VK_NULL_HANDLE)) {
    return false;
  }

  if (!BeginRecording()) {
    return false;
  }

  Target* targets[4] = {};
  uint32_t bound = 0;
  const uint32_t target_count = std::min<uint32_t>(draw.target_count, 4);
  if (!target_count) {
    targets[bound++] = EnsureTarget(0, 0, 0, 0);
  } else {
    for (uint32_t i = 0; i < target_count; ++i) {
      if (!draw.target_addresses[i]) {
        XELOGW("[xna] direct vulkan: render target slot {} has no guest memory",
               i);
        return false;
      }
      targets[bound++] =
          EnsureTarget(draw.target_addresses[i], draw.target_width,
                       draw.target_height, draw.target_formats[i]);
    }
  }
  for (uint32_t i = 0; i < bound; ++i) {
    if (!targets[i]) {
      return false;
    }
  }

  static const XnaGpuTextureBinding kUnbound{};
  const auto texture_for = [&](uint32_t fetch_constant)
      -> const XnaGpuTextureBinding& {
    return fetch_constant < XnaGpuDraw::kMaxTextureSlots
               ? draw.textures[fetch_constant]
               : kUnbound;
  };
  std::vector<Target*> sampled;
  const auto resolve_texture =
      [&](const SpirvShader::TextureBinding& binding) -> VkImageView {
    const xenos::FetchOpDimension dimension = binding.dimension;
    const uint32_t null_index =
        dimension == xenos::FetchOpDimension::k3DOrStacked
            ? kNull3D
            : (dimension == xenos::FetchOpDimension::kCube ? kNullCube
                                                           : kNull2DArray);
    const VkImageView null_view = s.null_views[null_index];
    const XnaGpuTextureBinding& texture = texture_for(binding.fetch_constant);
    if (!texture.guest_address || null_index != kNull2DArray) {
      return null_view;
    }
    auto target = s.targets.find(texture.guest_address);
    if (target != s.targets.end() && target->second.color != VK_NULL_HANDLE) {
      for (uint32_t i = 0; i < bound; ++i) {
        if (targets[i] == &target->second) {
          return null_view;
        }
      }
      sampled.push_back(&target->second);
      return target->second.sampled_view;
    }
    if (texture.type != 0) {
      return null_view;
    }
    const VkImageView view = EnsureTexture(texture);
    return view != VK_NULL_HANDLE ? view : null_view;
  };

  std::vector<VkImageView> pixel_views;
  if (pixel) {
    for (const auto& binding : pixel->GetTextureBindingsAfterTranslation()) {
      pixel_views.push_back(resolve_texture(binding));
    }
  }
  std::vector<VkImageView> vertex_views;
  for (const auto& binding : vertex->GetTextureBindingsAfterTranslation()) {
    vertex_views.push_back(resolve_texture(binding));
  }
  std::vector<VkSampler> pixel_samplers;
  if (pixel) {
    for (const auto& binding : pixel->GetSamplerBindingsAfterTranslation()) {
      pixel_samplers.push_back(
          SamplerFor(SamplerKeyFor(binding, texture_for(binding.fetch_constant))));
    }
  }
  std::vector<VkSampler> vertex_samplers;
  for (const auto& binding : vertex->GetSamplerBindingsAfterTranslation()) {
    vertex_samplers.push_back(
        SamplerFor(SamplerKeyFor(binding, texture_for(binding.fetch_constant))));
  }

  uint32_t min_index = 0;
  uint32_t max_index = index_count - 1;
  std::vector<uint8_t> indices;
  if (draw.indexed) {
    const uint32_t stride = draw.index_32bit ? 4u : 2u;
    const uint64_t start = uint64_t(draw.start_index) * stride;
    if (!draw.index_guest_address || start >= draw.index_size_bytes) {
      return false;
    }
    index_count = std::min<uint32_t>(
        index_count, uint32_t((draw.index_size_bytes - start) / stride));
    if (!index_count) {
      return false;
    }
    const uint8_t* source = memory->TranslateVirtual<const uint8_t*>(
        draw.index_guest_address + uint32_t(start));
    if (!source) {
      return false;
    }
    std::vector<uint32_t> values(index_count);
    min_index = UINT32_MAX;
    max_index = 0;
    for (uint32_t i = 0; i < index_count; ++i) {
      const uint32_t value =
          draw.index_32bit ? xe::load_and_swap<uint32_t>(source + i * 4)
                           : xe::load_and_swap<uint16_t>(source + i * 2);
      values[i] = value;
      min_index = std::min(min_index, value);
      max_index = std::max(max_index, value);
    }
    indices.resize(size_t(index_count) * stride);
    for (uint32_t i = 0; i < index_count; ++i) {
      const uint32_t rebased = values[i] - min_index;
      if (draw.index_32bit) {
        std::memcpy(indices.data() + size_t(i) * 4, &rebased, 4);
      } else {
        const uint16_t narrow = uint16_t(rebased);
        std::memcpy(indices.data() + size_t(i) * 2, &narrow, 2);
      }
    }
  }
  const uint64_t vertex_span = uint64_t(max_index) - min_index + 1;

  uint32_t fetch_slots[32];
  uint32_t fetch_slot_count = 0;
  bool wants_null_fetch = false;
  const auto& vertex_map = vertex->constant_register_map();
  for (uint32_t i = 0; i < 96 && fetch_slot_count < 32; ++i) {
    const uint32_t slot = 95 - i;
    if (!(vertex_map.vertex_fetch_bitmap[slot / 32] &
          (uint32_t(1) << (slot % 32)))) {
      continue;
    }
    if (slot == kNullVertexFetchConstant) {
      wants_null_fetch = true;
      continue;
    }
    fetch_slots[fetch_slot_count++] = slot;
  }
  if (!fetch_slot_count) {
    for (uint32_t slot = 0; slot < stream_count && slot < 32; ++slot) {
      fetch_slots[fetch_slot_count++] = slot;
    }
  }
  struct StreamCopy {
    uint32_t slot;
    const uint8_t* source;
    uint32_t bytes;
  };
  std::vector<StreamCopy> copies;
  const uint32_t first_vertex =
      draw.indexed ? draw.base_vertex : draw.start_vertex;
  uint64_t upload_bytes = 0;
  for (uint32_t slot_index = 0; slot_index < fetch_slot_count; ++slot_index) {
    const uint32_t i = std::min(slot_index, stream_count - 1);
    const XnaGpuStream& stream = streams[i];
    const uint32_t stride =
        stream.stride ? stream.stride
                      : StrideForFetch(vertex, fetch_slots[slot_index]);
    if (!stream.guest_address || !stream.size_bytes || !stride) {
      continue;
    }
    const uint64_t begin = (uint64_t(first_vertex) + min_index) * stride;
    if (begin >= stream.size_bytes) {
      return false;
    }
    uint64_t bytes = vertex_span * stride;
    if (begin + bytes > stream.size_bytes) {
      bytes = stream.size_bytes - begin;
    }
    const uint8_t* source = memory->TranslateVirtual<const uint8_t*>(
        stream.guest_address + uint32_t(begin));
    if (!source) {
      return false;
    }
    copies.push_back({fetch_slots[slot_index], source, uint32_t(bytes)});
    upload_bytes += bytes + 256;
  }
  upload_bytes += indices.size() + 256;
  upload_bytes += (uint64_t(std::max<uint32_t>(FloatCount(vertex), 1)) +
                   std::max<uint32_t>(FloatCount(pixel), 1)) *
                      16 +
                  512;
  upload_bytes += sizeof(SpirvShaderTranslator::SystemConstants) + 256;
  upload_bytes += kFetchConstantDwords * sizeof(uint32_t) + 256;

  const uint32_t textures_pixel = uint32_t(pixel_views.size());
  const uint32_t textures_vertex = uint32_t(vertex_views.size());
  VkPipelineLayout pipeline_layout = PipelineLayoutFor(
      textures_pixel, uint32_t(pixel_samplers.size()), textures_vertex,
      uint32_t(vertex_samplers.size()));
  if (pipeline_layout == VK_NULL_HANDLE) {
    return false;
  }
  VkDescriptorSetLayout set_layouts[3];
  uint32_t set_count = 0;
  set_layouts[set_count++] = s.constants_layout;
  const bool vertex_set = textures_vertex || !vertex_samplers.empty();
  const bool pixel_set = textures_pixel || !pixel_samplers.empty();
  if (vertex_set) {
    set_layouts[set_count++] = TextureLayoutFor(
        true, textures_vertex, uint32_t(vertex_samplers.size()));
  }
  if (pixel_set) {
    set_layouts[set_count++] = TextureLayoutFor(
        false, textures_pixel, uint32_t(pixel_samplers.size()));
  }
  if (!EnsureRoom(upload_bytes, set_count)) {
    XELOGW("[xna] direct vulkan: draw needs {} upload bytes, more than a "
           "frame holds",
           upload_bytes);
    return false;
  }
  VkDescriptorSet sets[3] = {};
  if (!AllocateSets(set_layouts, set_count, sets)) {
    Submit();
    if (!BeginRecording() || !AllocateSets(set_layouts, set_count, sets)) {
      XELOGE("[xna] direct vulkan: could not allocate descriptor sets");
      return false;
    }
  }

  uint32_t fetch[kFetchConstantDwords] = {};
  for (const StreamCopy& copy : copies) {
    uint32_t offset = 0;
    uint8_t* out = AllocateUpload(copy.bytes, 256, &offset);
    if (!out) {
      return false;
    }
    std::memcpy(out, copy.source, copy.bytes);
    xenos::xe_gpu_vertex_fetch_t vertex_fetch;
    vertex_fetch.dword_0 = 0;
    vertex_fetch.dword_1 = 0;
    vertex_fetch.type = xenos::FetchConstantType::kVertex;
    vertex_fetch.address = offset >> 2;
    vertex_fetch.endian = xenos::Endian::k8in32;
    vertex_fetch.size = copy.bytes >> 2;
    fetch[copy.slot * 2 + 0] = vertex_fetch.dword_0;
    fetch[copy.slot * 2 + 1] = vertex_fetch.dword_1;
  }
  if (wants_null_fetch) {
    xenos::xe_gpu_vertex_fetch_t null_fetch;
    null_fetch.dword_0 = 0;
    null_fetch.dword_1 = 0;
    null_fetch.type = xenos::FetchConstantType::kVertex;
    null_fetch.address = 0;
    null_fetch.endian = xenos::Endian::k8in32;
    null_fetch.size = kZeroBytes >> 2;
    fetch[kNullVertexFetchConstant * 2 + 0] = null_fetch.dword_0;
    fetch[kNullVertexFetchConstant * 2 + 1] = null_fetch.dword_1;
  }
  const uint32_t lowest_stream_constant =
      fetch_slot_count ? fetch_slots[fetch_slot_count - 1] : 96;
  for (uint32_t slot = 0; slot < XnaGpuDraw::kMaxTextureSlots; ++slot) {
    const XnaGpuTextureBinding& binding = draw.textures[slot];
    if (!binding.guest_address || !binding.width || !binding.height) {
      continue;
    }
    if (slot * 6 + 5 >= lowest_stream_constant * 2 ||
        (wants_null_fetch && slot == kNullVertexFetchConstant / 3)) {
      continue;
    }
    xenos::xe_gpu_texture_fetch_t texture = {};
    texture.type = xenos::FetchConstantType::kTexture;
    texture.format = XnaGpuTextureFormatFor(binding.format);
    texture.dimension = xenos::DataDimension::k2DOrStacked;
    texture.size_2d.width = binding.width - 1;
    texture.size_2d.height = binding.height - 1;
    texture.size_2d.stack_depth = 0;
    texture.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    texture.clamp_x = static_cast<xenos::ClampMode>(binding.address_u & 7);
    texture.clamp_y = static_cast<xenos::ClampMode>(binding.address_v & 7);
    texture.clamp_z = static_cast<xenos::ClampMode>(binding.address_v & 7);
    texture.mip_max_level = std::max<uint32_t>(binding.levels, 1) - 1;
    std::memcpy(fetch + slot * 6, &texture, sizeof(texture));
  }
  uint32_t fetch_offset = 0;
  uint8_t* fetch_out = AllocateUpload(sizeof(fetch), 256, &fetch_offset);
  if (!fetch_out) {
    return false;
  }
  std::memcpy(fetch_out, fetch, sizeof(fetch));

  uint32_t index_offset = 0;
  if (draw.indexed) {
    uint8_t* out = AllocateUpload(uint32_t(indices.size()), 256, &index_offset);
    if (!out) {
      return false;
    }
    std::memcpy(out, indices.data(), indices.size());
  }

  float registers[512][4] = {};
  for (const XnaGpuConstant& constant : draw.constants) {
    if (constant.register_index < 512) {
      std::memcpy(registers[constant.register_index], constant.value,
                  sizeof(constant.value));
    }
  }
  VkDescriptorBufferInfo float_vertex = {};
  VkDescriptorBufferInfo float_pixel = {};
  if (!PackFloats(vertex, &registers[0][0], &float_vertex) ||
      !PackFloats(pixel, &registers[256][0], &float_pixel)) {
    return false;
  }

  SpirvShaderTranslator::SystemConstants system;
  std::memset(&system, 0, sizeof(system));
  system.flags = SpirvShaderTranslator::kSysFlag_WNotReciprocal |
                 (uint32_t(xenos::CompareFunction::kAlways)
                  << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift);
  if (polygonal) {
    system.flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (line) {
    system.flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  system.vertex_index_endian = xenos::Endian::kNone;
  system.vertex_base_index = 0;
  system.ndc_scale[0] = 1.0f;
  system.ndc_scale[1] = -1.0f;
  system.ndc_scale[2] = 1.0f;
  if (!s.image_view_format_swizzle) {
    const uint32_t identity = uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
    for (uint32_t i = 0; i < 16; ++i) {
      system.texture_swizzles[i] = identity | (identity << 12);
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    system.color_exp_bias[i] = 1.0f;
  }
  uint32_t system_offset = 0;
  uint8_t* system_out = AllocateUpload(sizeof(system), 256, &system_offset);
  if (!system_out) {
    return false;
  }
  std::memcpy(system_out, &system, sizeof(system));

  VkDescriptorBufferInfo buffers[SpirvShaderTranslator::kConstantBufferCount];
  for (auto& buffer : buffers) {
    buffer.buffer = s.upload;
    buffer.offset = 0;
    buffer.range = kZeroBytes;
  }
  buffers[SpirvShaderTranslator::kConstantBufferSystem].offset = system_offset;
  buffers[SpirvShaderTranslator::kConstantBufferSystem].range = sizeof(system);
  buffers[SpirvShaderTranslator::kConstantBufferFloatVertex] = float_vertex;
  buffers[SpirvShaderTranslator::kConstantBufferFloatPixel] = float_pixel;
  buffers[SpirvShaderTranslator::kConstantBufferFetch].offset = fetch_offset;
  buffers[SpirvShaderTranslator::kConstantBufferFetch].range = sizeof(fetch);
  std::vector<VkWriteDescriptorSet> writes;
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets[0];
    write.dstBinding = i;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffers[i];
    writes.push_back(write);
  }
  std::vector<VkDescriptorImageInfo> image_infos;
  image_infos.reserve(vertex_views.size() + vertex_samplers.size() +
                      pixel_views.size() + pixel_samplers.size());
  const auto add_texture_writes = [&](VkDescriptorSet set,
                                      const std::vector<VkImageView>& views,
                                      const std::vector<VkSampler>& samplers) {
    for (size_t i = 0; i < views.size(); ++i) {
      VkDescriptorImageInfo& info = image_infos.emplace_back();
      info.sampler = VK_NULL_HANDLE;
      info.imageView = views[i];
      info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkWriteDescriptorSet write = {};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = set;
      write.dstBinding = uint32_t(i);
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &info;
      writes.push_back(write);
    }
    for (size_t i = 0; i < samplers.size(); ++i) {
      VkDescriptorImageInfo& info = image_infos.emplace_back();
      info.sampler = samplers[i];
      info.imageView = VK_NULL_HANDLE;
      info.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VkWriteDescriptorSet write = {};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = set;
      write.dstBinding = uint32_t(views.size() + i);
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      write.pImageInfo = &info;
      writes.push_back(write);
    }
  };
  uint32_t next_set = 1;
  const VkDescriptorSet vertex_texture_set =
      vertex_set ? sets[next_set++] : VK_NULL_HANDLE;
  const VkDescriptorSet pixel_texture_set =
      pixel_set ? sets[next_set++] : VK_NULL_HANDLE;
  if (vertex_set) {
    add_texture_writes(vertex_texture_set, vertex_views, vertex_samplers);
  }
  if (pixel_set) {
    add_texture_writes(pixel_texture_set, pixel_views, pixel_samplers);
  }
  F().vkUpdateDescriptorSets(s.vk, uint32_t(writes.size()), writes.data(), 0,
                             nullptr);

  VkFormat formats[4];
  for (uint32_t i = 0; i < bound; ++i) {
    formats[i] = targets[i]->format;
  }
  VkRenderPass render_pass = RenderPassFor(formats, bound);
  if (render_pass == VK_NULL_HANDLE) {
    return false;
  }
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.render_pass = uint64_t(render_pass);
  key.layout = uint64_t(pipeline_layout);
  key.vertex = vertex_translation;
  key.pixel = pixel_translation;
  key.target_count = bound;
  for (uint32_t i = 0; i < bound; ++i) {
    key.write[i] = draw.color_write[i] & 0xF;
  }
  key.blend[0] = draw.color_src;
  key.blend[1] = draw.color_dst;
  key.blend[2] = draw.color_op;
  key.blend[3] = draw.alpha_src;
  key.blend[4] = draw.alpha_dst;
  key.blend[5] = draw.alpha_op;
  key.depth_enable = draw.depth_enable ? 1 : 0;
  key.depth_write = draw.depth_write_enable ? 1 : 0;
  key.depth_function = draw.depth_function;
  key.topology = uint32_t(topology);
  VkPipeline pipeline = PipelineFor(key, vertex_module, pixel_module,
                                    pipeline_layout, render_pass, topology);
  if (pipeline == VK_NULL_HANDLE) {
    return false;
  }

  for (Target* target : sampled) {
    ImageBarrier(target->color, VK_IMAGE_ASPECT_COLOR_BIT, target->color_layout,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  for (uint32_t i = 0; i < bound; ++i) {
    ImageBarrier(targets[i]->color, VK_IMAGE_ASPECT_COLOR_BIT,
                 targets[i]->color_layout,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  }
  ImageBarrier(targets[0]->depth, VK_IMAGE_ASPECT_DEPTH_BIT,
               targets[0]->depth_layout,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  VkFramebuffer framebuffer = FramebufferFor(render_pass, targets, bound);
  if (framebuffer == VK_NULL_HANDLE) {
    return false;
  }
  if (s.open_framebuffer != framebuffer) {
    EndRenderPass();
    VkRenderPassBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = render_pass;
    begin.framebuffer = framebuffer;
    begin.renderArea.extent.width = targets[0]->width;
    begin.renderArea.extent.height = targets[0]->height;
    F().vkCmdBeginRenderPass(s.cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    s.open_render_pass = render_pass;
    s.open_framebuffer = framebuffer;
  }

  F().vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  const VkDescriptorSet fixed_sets[2] = {s.shared_set, sets[0]};
  F().vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline_layout, 0, 2, fixed_sets, 0, nullptr);
  if (vertex_set) {
    F().vkCmdBindDescriptorSets(
        s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
        SpirvShaderTranslator::kDescriptorSetTexturesVertex, 1,
        &vertex_texture_set, 0, nullptr);
  }
  if (pixel_set) {
    F().vkCmdBindDescriptorSets(
        s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
        SpirvShaderTranslator::kDescriptorSetTexturesPixel, 1,
        &pixel_texture_set, 0, nullptr);
  }
  const float target_width = float(targets[0]->width);
  const float target_height = float(targets[0]->height);
  VkViewport viewport;
  viewport.x = float(draw.viewport_x);
  viewport.y = float(draw.viewport_y);
  viewport.width =
      draw.viewport_width ? float(draw.viewport_width) : target_width;
  viewport.height =
      draw.viewport_height ? float(draw.viewport_height) : target_height;
  viewport.minDepth = std::clamp(draw.viewport_min_depth, 0.0f, 1.0f);
  viewport.maxDepth = std::clamp(draw.viewport_max_depth, 0.0f, 1.0f);
  F().vkCmdSetViewport(s.cmd, 0, 1, &viewport);
  VkRect2D scissor = {};
  scissor.extent.width = targets[0]->width;
  scissor.extent.height = targets[0]->height;
  F().vkCmdSetScissor(s.cmd, 0, 1, &scissor);
  const float blend_constants[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  F().vkCmdSetBlendConstants(s.cmd, blend_constants);
  if (draw.indexed) {
    F().vkCmdBindIndexBuffer(
        s.cmd, s.upload, index_offset,
        draw.index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    F().vkCmdDrawIndexed(s.cmd, index_count, 1, 0, 0, 0);
  } else {
    F().vkCmdDraw(s.cmd, index_count, 1, 0, 0);
  }
  return true;
}

void ClearLocked(const XnaGpuTarget& target, const float* color,
                 bool clear_color, bool clear_depth, float depth) {
  if (!BeginRecording()) {
    return;
  }
  Target* host = EnsureTarget(target.guest_address, target.width,
                              target.height, target.format);
  if (!host) {
    return;
  }
  EndRenderPass();
  if (clear_color) {
    ImageBarrier(host->color, VK_IMAGE_ASPECT_COLOR_BIT, host->color_layout,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue value = {};
    for (uint32_t i = 0; i < 4; ++i) {
      value.float32[i] = color ? color[i] : 0.0f;
    }
    const VkImageSubresourceRange range =
        uivk::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
    F().vkCmdClearColorImage(s.cmd, host->color,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                             &range);
  }
  if (clear_depth) {
    ImageBarrier(host->depth, VK_IMAGE_ASPECT_DEPTH_BIT, host->depth_layout,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearDepthStencilValue value = {std::clamp(depth, 0.0f, 1.0f), 0};
    const VkImageSubresourceRange range =
        uivk::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT);
    F().vkCmdClearDepthStencilImage(s.cmd, host->depth,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &value, 1, &range);
  }
}

VkFramebuffer BlitFramebufferFor(VkImageView view, uint64_t version) {
  auto found = s.blit_framebuffers.find(version);
  if (found != s.blit_framebuffers.end()) {
    return found->second;
  }
  while (s.blit_framebuffers.size() >=
         uivk::VulkanPresenter::kMaxActiveGuestOutputImageVersions) {
    s.retired_framebuffers.push_back(s.blit_framebuffers.begin()->second);
    s.blit_framebuffers.erase(s.blit_framebuffers.begin());
  }
  VkFramebufferCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  info.renderPass = s.blit_render_pass;
  info.attachmentCount = 1;
  info.pAttachments = &view;
  info.width = kBackBufferWidth;
  info.height = kBackBufferHeight;
  info.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  if (F().vkCreateFramebuffer(s.vk, &info, nullptr, &framebuffer) !=
      VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  s.blit_framebuffers.emplace(version, framebuffer);
  return framebuffer;
}

void PresentLocked() {
  if (!BeginRecording()) {
    return;
  }
  Target* back = EnsureTarget(0, 0, 0, 0);
  if (!back) {
    Submit();
    return;
  }
  s.presenter->RefreshGuestOutput(
      kBackBufferWidth, kBackBufferHeight, kBackBufferWidth, kBackBufferHeight,
      [&](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vulkan_context = static_cast<
            uivk::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        context.SetIs8bpc(true);
        if (!BeginRecording()) {
          return false;
        }
        VkFramebuffer framebuffer = BlitFramebufferFor(
            vulkan_context.image_view(), vulkan_context.image_version());
        if (framebuffer == VK_NULL_HANDLE) {
          return false;
        }
        ImageBarrier(back->color, VK_IMAGE_ASPECT_COLOR_BIT, back->color_layout,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EndRenderPass();

        VkImageMemoryBarrier acquire = {};
        acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire.srcAccessMask =
            uivk::VulkanPresenter::kGuestOutputInternalAccessMask;
        acquire.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        acquire.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire.image = vulkan_context.image();
        acquire.subresourceRange = uivk::util::InitializeSubresourceRange();
        F().vkCmdPipelineBarrier(
            s.cmd, uivk::VulkanPresenter::kGuestOutputInternalStageMask,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
            nullptr, 1, &acquire);

        VkDescriptorImageInfo image_info = {};
        image_info.imageView = back->attachment_view;
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo sampler_info = {};
        sampler_info.sampler = s.blit_sampler;
        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = s.blit_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &image_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = s.blit_set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].pImageInfo = &sampler_info;
        F().vkUpdateDescriptorSets(s.vk, 2, writes, 0, nullptr);

        VkRenderPassBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = s.blit_render_pass;
        begin.framebuffer = framebuffer;
        begin.renderArea.extent.width = kBackBufferWidth;
        begin.renderArea.extent.height = kBackBufferHeight;
        F().vkCmdBeginRenderPass(s.cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        F().vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              s.blit_pipeline);
        VkViewport viewport = {0.0f, 0.0f, float(kBackBufferWidth),
                               float(kBackBufferHeight), 0.0f, 1.0f};
        F().vkCmdSetViewport(s.cmd, 0, 1, &viewport);
        VkRect2D scissor = {};
        scissor.extent.width = kBackBufferWidth;
        scissor.extent.height = kBackBufferHeight;
        F().vkCmdSetScissor(s.cmd, 0, 1, &scissor);
        F().vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    s.blit_layout, 0, 1, &s.blit_set, 0,
                                    nullptr);
        const float rect[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
        F().vkCmdPushConstants(s.cmd, s.blit_layout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(rect), rect);
        struct {
          int32_t offset[2];
          float size_inv[2];
        } pixel_constants = {{0, 0},
                             {1.0f / float(kBackBufferWidth),
                              1.0f / float(kBackBufferHeight)}};
        F().vkCmdPushConstants(s.cmd, s.blit_layout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 16,
                               sizeof(pixel_constants), &pixel_constants);
        F().vkCmdDraw(s.cmd, 4, 1, 0, 0);
        F().vkCmdEndRenderPass(s.cmd);

        VkImageMemoryBarrier release = acquire;
        release.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        release.dstAccessMask =
            uivk::VulkanPresenter::kGuestOutputInternalAccessMask;
        release.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        release.newLayout = uivk::VulkanPresenter::kGuestOutputInternalLayout;
        F().vkCmdPipelineBarrier(
            s.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            uivk::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0,
            nullptr, 0, nullptr, 1, &release);
        Submit();
        return true;
      });
  Submit();
}

bool ReadBackLocked(uint8_t* out, uint32_t bytes) {
  const uint32_t row = kBackBufferWidth * 4;
  const uint32_t total = row * kBackBufferHeight;
  if (!out || bytes < total) {
    return false;
  }
  if (!BeginRecording()) {
    return false;
  }
  Target* back = EnsureTarget(0, 0, 0, 0);
  if (!back) {
    Submit();
    return false;
  }
  if (s.readback == VK_NULL_HANDLE) {
    if (!uivk::util::CreateDedicatedAllocationBuffer(
            s.device, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            uivk::util::MemoryPurpose::kReadback, s.readback,
            s.readback_memory)) {
      XELOGE("[xna] direct vulkan: could not create the readback buffer");
      Submit();
      return false;
    }
    if (F().vkMapMemory(s.vk, s.readback_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void**>(&s.readback_mapped)) !=
        VK_SUCCESS) {
      Submit();
      return false;
    }
    s.readback_bytes = total;
  }
  ImageBarrier(back->color, VK_IMAGE_ASPECT_COLOR_BIT, back->color_layout,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy region = {};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent.width = kBackBufferWidth;
  region.imageExtent.height = kBackBufferHeight;
  region.imageExtent.depth = 1;
  F().vkCmdCopyImageToBuffer(s.cmd, back->color,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.readback,
                             1, &region);
  VkMemoryBarrier host = {};
  host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  F().vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr,
                           0, nullptr);
  Submit();
  VkMappedMemoryRange range = {};
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.memory = s.readback_memory;
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  F().vkInvalidateMappedMemoryRanges(s.vk, 1, &range);
  std::memcpy(out, s.readback_mapped, total);
  return true;
}

bool CreateNullImages() {
  const VkComponentMapping id = {};
  if (!CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 1,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   0, &s.null_images[kNull2DArray],
                   &s.null_memory[kNull2DArray]) ||
      !CreateImage(VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 1,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   0, &s.null_images[kNull3D], &s.null_memory[kNull3D]) ||
      !CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 6,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                   &s.null_images[kNullCube], &s.null_memory[kNullCube])) {
    return false;
  }
  s.null_views[kNull2DArray] =
      CreateView(s.null_images[kNull2DArray], VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                 VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, id);
  s.null_views[kNull3D] =
      CreateView(s.null_images[kNull3D], VK_IMAGE_VIEW_TYPE_3D,
                 VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, id);
  s.null_views[kNullCube] =
      CreateView(s.null_images[kNullCube], VK_IMAGE_VIEW_TYPE_CUBE,
                 VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6, id);
  for (VkImageView view : s.null_views) {
    if (view == VK_NULL_HANDLE) {
      return false;
    }
  }
  if (!BeginRecording()) {
    return false;
  }
  const VkClearColorValue zero = {};
  const VkImageSubresourceRange range =
      uivk::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  for (uint32_t i = 0; i < 3; ++i) {
    ImageBarrier(s.null_images[i], VK_IMAGE_ASPECT_COLOR_BIT, s.null_layouts[i],
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    F().vkCmdClearColorImage(s.cmd, s.null_images[i],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                             &range);
    ImageBarrier(s.null_images[i], VK_IMAGE_ASPECT_COLOR_BIT, s.null_layouts[i],
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  Submit();
  return true;
}

bool CreateBlit() {
  VkAttachmentDescription attachment = {};
  attachment.format = uivk::VulkanPresenter::kGuestOutputFormat;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference color_ref = {};
  color_ref.attachment = 0;
  color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;
  VkRenderPassCreateInfo render_pass_info = {};
  render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_info.attachmentCount = 1;
  render_pass_info.pAttachments = &attachment;
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;
  if (F().vkCreateRenderPass(s.vk, &render_pass_info, nullptr,
                             &s.blit_render_pass) != VK_SUCCESS) {
    return false;
  }

  VkDescriptorSetLayoutBinding bindings[2] = {};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 2;
  set_layout_info.pBindings = bindings;
  if (F().vkCreateDescriptorSetLayout(s.vk, &set_layout_info, nullptr,
                                      &s.blit_set_layout) != VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange ranges[2] = {};
  ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  ranges[0].offset = 0;
  ranges[0].size = 16;
  ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  ranges[1].offset = 16;
  ranges[1].size = 16;
  VkPipelineLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &s.blit_set_layout;
  layout_info.pushConstantRangeCount = 2;
  layout_info.pPushConstantRanges = ranges;
  if (F().vkCreatePipelineLayout(s.vk, &layout_info, nullptr,
                                 &s.blit_layout) != VK_SUCCESS) {
    return false;
  }

  VkDescriptorSetAllocateInfo allocate = {};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = s.static_pool;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &s.blit_set_layout;
  if (F().vkAllocateDescriptorSets(s.vk, &allocate, &s.blit_set) !=
      VK_SUCCESS) {
    return false;
  }

  s.blit_vs = uivk::util::CreateShaderModule(
      s.device, guest_output_triangle_strip_rect_vs,
      sizeof(guest_output_triangle_strip_rect_vs));
  s.blit_ps = uivk::util::CreateShaderModule(s.device, guest_output_bilinear_ps,
                                             sizeof(guest_output_bilinear_ps));
  if (s.blit_vs == VK_NULL_HANDLE || s.blit_ps == VK_NULL_HANDLE) {
    return false;
  }

  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxAnisotropy = 1.0f;
  sampler_info.maxLod = 0.25f;
  if (F().vkCreateSampler(s.vk, &sampler_info, nullptr, &s.blit_sampler) !=
      VK_SUCCESS) {
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = s.blit_vs;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = s.blit_ps;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkPipelineViewportStateCreateInfo viewport = {};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rasterization = {};
  rasterization.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization.cullMode = VK_CULL_MODE_NONE;
  rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterization.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend = {};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo color_blend = {};
  color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend.attachmentCount = 1;
  color_blend.pAttachments = &blend;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                           VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkGraphicsPipelineCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &rasterization;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &color_blend;
  info.pDynamicState = &dynamic;
  info.layout = s.blit_layout;
  info.renderPass = s.blit_render_pass;
  info.basePipelineIndex = -1;
  return F().vkCreateGraphicsPipelines(s.vk, VK_NULL_HANDLE, 1, &info, nullptr,
                                       &s.blit_pipeline) == VK_SUCCESS;
}

bool Initialize() {
  auto* state = kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  auto* graphics = emulator ? emulator->graphics_system() : nullptr;
  if (!graphics) {
    return false;
  }
  s.provider = dynamic_cast<uivk::VulkanProvider*>(graphics->provider());
  s.presenter = dynamic_cast<uivk::VulkanPresenter*>(graphics->presenter());
  if (!s.provider || !s.presenter || !s.provider->vulkan_device()) {
    return false;
  }
  s.device = s.provider->vulkan_device();
  s.vk = s.device->device();
  s.queue_family = s.device->queue_family_graphics_compute();
  const auto& properties = s.device->properties();
  s.independent_blend = properties.independentBlend;
  s.anisotropy = properties.samplerAnisotropy;
  s.max_anisotropy = properties.maxSamplerAnisotropy;
  s.mirror_clamp_to_edge = properties.samplerMirrorClampToEdge;

  VkCommandPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = s.queue_family;
  if (F().vkCreateCommandPool(s.vk, &pool_info, nullptr, &s.pool) !=
      VK_SUCCESS) {
    return false;
  }
  VkCommandBufferAllocateInfo buffer_info = {};
  buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  buffer_info.commandPool = s.pool;
  buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  buffer_info.commandBufferCount = 1;
  if (F().vkAllocateCommandBuffers(s.vk, &buffer_info, &s.cmd) != VK_SUCCESS) {
    return false;
  }
  VkFenceCreateInfo fence_info = {};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (F().vkCreateFence(s.vk, &fence_info, nullptr, &s.fence) != VK_SUCCESS) {
    return false;
  }

  if (!uivk::util::CreateDedicatedAllocationBuffer(
          s.device, kUploadBytes,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
              VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          uivk::util::MemoryPurpose::kUpload, s.upload, s.upload_memory,
          &s.upload_memory_type, &s.upload_memory_size)) {
    XELOGE("[xna] direct vulkan: could not create the upload ring");
    return false;
  }
  if (F().vkMapMemory(s.vk, s.upload_memory, 0, VK_WHOLE_SIZE, 0,
                      reinterpret_cast<void**>(&s.upload_mapped)) !=
      VK_SUCCESS) {
    return false;
  }
  std::memset(s.upload_mapped, 0, kZeroBytes);

  SpirvShaderTranslator::Features features(s.device);
  s.image_view_format_swizzle = features.image_view_format_swizzle;
  s.shared_binding_count =
      1u << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
          features.max_storage_buffer_range);
  s.translator = std::make_unique<SpirvShaderTranslator>(features, false,
                                                         false, false);

  const VkShaderStageFlags guest_stages =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutBinding shared_binding = {};
  shared_binding.binding = 0;
  shared_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_binding.descriptorCount = s.shared_binding_count;
  shared_binding.stageFlags = guest_stages;
  VkDescriptorSetLayoutCreateInfo shared_info = {};
  shared_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_info.bindingCount = 1;
  shared_info.pBindings = &shared_binding;
  if (F().vkCreateDescriptorSetLayout(s.vk, &shared_info, nullptr,
                                      &s.shared_layout) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayoutBinding
      constant_bindings[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_bindings[i].binding = i;
    constant_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constant_bindings[i].descriptorCount = 1;
    constant_bindings[i].stageFlags = guest_stages;
  }
  VkDescriptorSetLayoutCreateInfo constants_info = {};
  constants_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  constants_info.bindingCount = SpirvShaderTranslator::kConstantBufferCount;
  constants_info.pBindings = constant_bindings;
  if (F().vkCreateDescriptorSetLayout(s.vk, &constants_info, nullptr,
                                      &s.constants_layout) != VK_SUCCESS) {
    return false;
  }

  VkDescriptorPoolSize static_sizes[3] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, s.shared_binding_count},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
  VkDescriptorPoolCreateInfo static_pool_info = {};
  static_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  static_pool_info.maxSets = 2;
  static_pool_info.poolSizeCount = 3;
  static_pool_info.pPoolSizes = static_sizes;
  if (F().vkCreateDescriptorPool(s.vk, &static_pool_info, nullptr,
                                 &s.static_pool) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorPoolSize frame_sizes[3] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
       kSetsPerPool * SpirvShaderTranslator::kConstantBufferCount},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerPool * 8},
      {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerPool * 8}};
  VkDescriptorPoolCreateInfo frame_pool_info = {};
  frame_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  frame_pool_info.maxSets = kSetsPerPool;
  frame_pool_info.poolSizeCount = 3;
  frame_pool_info.pPoolSizes = frame_sizes;
  if (F().vkCreateDescriptorPool(s.vk, &frame_pool_info, nullptr,
                                 &s.frame_pool) != VK_SUCCESS) {
    return false;
  }

  VkDescriptorSetAllocateInfo shared_allocate = {};
  shared_allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  shared_allocate.descriptorPool = s.static_pool;
  shared_allocate.descriptorSetCount = 1;
  shared_allocate.pSetLayouts = &s.shared_layout;
  if (F().vkAllocateDescriptorSets(s.vk, &shared_allocate, &s.shared_set) !=
      VK_SUCCESS) {
    return false;
  }
  std::vector<VkDescriptorBufferInfo> shared_buffers(s.shared_binding_count);
  for (auto& buffer : shared_buffers) {
    buffer.buffer = s.upload;
    buffer.offset = 0;
    buffer.range = VK_WHOLE_SIZE;
  }
  VkWriteDescriptorSet shared_write = {};
  shared_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  shared_write.dstSet = s.shared_set;
  shared_write.dstBinding = 0;
  shared_write.descriptorCount = s.shared_binding_count;
  shared_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_write.pBufferInfo = shared_buffers.data();
  F().vkUpdateDescriptorSets(s.vk, 1, &shared_write, 0, nullptr);

  if (!CreateNullImages()) {
    XELOGE("[xna] direct vulkan: could not create the null textures");
    return false;
  }
  if (!CreateBlit()) {
    XELOGE("[xna] direct vulkan: could not create the present pipeline");
    return false;
  }
  XELOGI("[xna] hosted title draws straight to Vulkan");
  return true;
}

bool Ready() {
  if (!cvars::xna_direct_d3d12) {
    return false;
  }
  if (!s.tried) {
    s.tried = true;
    s.usable = Initialize();
  }
  return s.usable;
}

}  // namespace

bool XnaVulkanDirectActive() {
  std::lock_guard<std::mutex> lock(s.mutex);
  return Ready();
}

bool XnaVulkanDirectDraw(const XnaGpuDraw& draw, const XnaGpuStream* streams,
                         uint32_t stream_count) {
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!Ready()) {
    return false;
  }
  return DrawLocked(draw, streams, stream_count);
}

void XnaVulkanDirectClear(const XnaGpuTarget& target, const float* color,
                          bool clear_color, bool clear_depth, float depth) {
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!Ready()) {
    return;
  }
  ClearLocked(target, color, clear_color, clear_depth, depth);
}

void XnaVulkanDirectPresent() {
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!Ready()) {
    return;
  }
  PresentLocked();
}

bool XnaVulkanDirectReadBackBuffer(uint8_t* out, uint32_t bytes) {
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!Ready()) {
    return false;
  }
  return ReadBackLocked(out, bytes);
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
