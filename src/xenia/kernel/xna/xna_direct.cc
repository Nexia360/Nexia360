/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_direct.h"

#include <algorithm>
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
#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/dxbc_shader_translator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/base/xxhash.h"
#include "xenia/kernel/xna/xna_direct_vulkan.h"
#include "xenia/kernel/xna/xna_exports.h"
#include "xenia/kernel/xna/xna_present.h"
#include "xenia/kernel/xna/xna_runtimehost.h"
#include "xenia/memory.h"
#include "xenia/ui/d3d12/d3d12_presenter.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/d3d12/d3d12_util.h"

#include "xenia/kernel/xna/shaders/xna_avatar_ps.h"
#include "xenia/kernel/xna/shaders/xna_avatar_vs.h"
#include "xenia/kernel/xna/shaders/xna_sprite_ps.h"
#include "xenia/kernel/xna/shaders/xna_sprite_vs.h"
#include "xenia/kernel/xna/xna_avatar.h"

DEFINE_bool(xna_direct_d3d12, true,
            "Draw a hosted XNA title straight to D3D12 instead of through the "
            "Xenos register, EDRAM and resolve path.",
            "XNA");

namespace xe {
namespace kernel {
namespace xna {

namespace {

using Microsoft::WRL::ComPtr;
using gpu::DxbcShader;
using gpu::DxbcShaderTranslator;
using gpu::Shader;
namespace xenos = gpu::xenos;
namespace reg = gpu::reg;

constexpr uint32_t kBackBufferWidth = 1280;
constexpr uint32_t kBackBufferHeight = 720;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
constexpr uint32_t kUploadBytes = 64u << 20;
constexpr uint32_t kZeroBytes = 256;
constexpr uint32_t kViewHeapSize = 65536;
constexpr uint32_t kReservedViews = 2;
constexpr uint32_t kSamplerHeapSize = 2048;
constexpr uint32_t kStagingViews = 8192;
constexpr uint32_t kNullSrv2DArray = 0;
constexpr uint32_t kNullSrv3D = 1;
constexpr uint32_t kNullSrvCube = 2;
constexpr uint32_t kFirstStagingView = 3;
constexpr uint32_t kTargetViews = 1024;
constexpr uint32_t kNullVertexFetchConstant = 80;
constexpr uint32_t kFetchConstantDwords = 192;

const D3D12_RESOURCE_STATES kReadState = D3D12_RESOURCE_STATES(
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

struct Target {
  ComPtr<ID3D12Resource> color;
  ComPtr<ID3D12Resource> depth;
  uint32_t width = 0;
  uint32_t height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t rtv = UINT32_MAX;
  uint32_t dsv = UINT32_MAX;
  uint32_t srv = UINT32_MAX;
  D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
};

struct HostTexture {
  ComPtr<ID3D12Resource> resource;
  uint32_t version = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t levels = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t srv = UINT32_MAX;
};

struct PipelineKey {
  const void* vertex;
  const void* pixel;
  uint32_t target_count;
  uint32_t target_formats[4];
  uint32_t blend[6];
  uint32_t write[4];
  uint32_t depth_enable;
  uint32_t depth_write;
  uint32_t depth_function;
  uint32_t stencil[7];
  uint32_t topology_type;
  bool operator<(const PipelineKey& other) const {
    return std::memcmp(this, &other, sizeof(*this)) < 0;
  }
};

struct BlitVertex {
  float x, y;
  float u, v;
  float r, g, b, a;
};

struct State {
  std::mutex mutex;
  bool tried = false;
  bool usable = false;
  const ui::d3d12::D3D12Provider* provider = nullptr;
  ui::d3d12::D3D12Presenter* presenter = nullptr;
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  uint64_t fence_value = 0;
  HANDLE fence_event = nullptr;
  bool recording = false;

  ComPtr<ID3D12Resource> upload;
  uint8_t* upload_mapped = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS upload_gpu = 0;
  uint32_t upload_used = kZeroBytes;

  ComPtr<ID3D12DescriptorHeap> view_heap;
  uint32_t view_used = kReservedViews;
  ComPtr<ID3D12DescriptorHeap> sampler_heap;
  uint32_t sampler_used = 0;
  std::map<std::vector<uint64_t>, uint32_t> sampler_sets;

  ComPtr<ID3D12DescriptorHeap> staging_heap;
  uint32_t staging_next = kFirstStagingView;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  uint32_t rtv_next = 0;
  ComPtr<ID3D12DescriptorHeap> dsv_heap;
  uint32_t dsv_next = 0;

  std::unordered_map<uint32_t, Target> targets;
  std::unordered_map<uint32_t, HostTexture> textures;
  std::unordered_map<uint64_t, std::unique_ptr<DxbcShader>> shaders;
  std::map<PipelineKey, ComPtr<ID3D12PipelineState>> pipelines;
  std::unordered_map<uint32_t, ComPtr<ID3D12RootSignature>> root_signatures;
  std::vector<ComPtr<ID3D12Resource>> retired;

  std::unique_ptr<DxbcShaderTranslator> translator;
  StringBuffer disasm;

  ComPtr<ID3D12Resource> present_target;
  uint32_t present_rtv = UINT32_MAX;
  ComPtr<ID3D12RootSignature> blit_root_signature;
  ComPtr<ID3D12PipelineState> blit_pipeline;
  ComPtr<ID3D12Resource> readback;
  uint64_t readback_bytes = 0;

  ComPtr<ID3D12RootSignature> avatar_root_signature;
  std::map<std::vector<uint32_t>, ComPtr<ID3D12PipelineState>>
      avatar_pipelines;
  std::unordered_map<uint64_t, HostTexture> avatar_textures;
};

State s;

D3D12_CPU_DESCRIPTOR_HANDLE ViewCpu(uint32_t index) {
  D3D12_CPU_DESCRIPTOR_HANDLE handle =
      s.view_heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(index) * s.provider->GetViewDescriptorSize();
  return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE ViewGpu(uint32_t index) {
  D3D12_GPU_DESCRIPTOR_HANDLE handle =
      s.view_heap->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += UINT64(index) * s.provider->GetViewDescriptorSize();
  return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE SamplerCpu(uint32_t index) {
  D3D12_CPU_DESCRIPTOR_HANDLE handle =
      s.sampler_heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(index) * s.provider->GetSamplerDescriptorSize();
  return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE SamplerGpu(uint32_t index) {
  D3D12_GPU_DESCRIPTOR_HANDLE handle =
      s.sampler_heap->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += UINT64(index) * s.provider->GetSamplerDescriptorSize();
  return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE StagingCpu(uint32_t index) {
  D3D12_CPU_DESCRIPTOR_HANDLE handle =
      s.staging_heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(index) * s.provider->GetViewDescriptorSize();
  return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE RtvCpu(uint32_t index) {
  D3D12_CPU_DESCRIPTOR_HANDLE handle =
      s.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(index) * s.provider->GetRTVDescriptorSize();
  return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DsvCpu(uint32_t index) {
  D3D12_CPU_DESCRIPTOR_HANDLE handle =
      s.dsv_heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(index) * s.provider->GetDSVDescriptorSize();
  return handle;
}

uint32_t AllocateStaging() {
  if (s.staging_next >= kStagingViews) {
    XELOGW("[xna] direct: out of staging descriptors");
    return UINT32_MAX;
  }
  return s.staging_next++;
}

void MoveTo(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
            D3D12_RESOURCE_STATES wanted) {
  if (!resource || state == wanted) {
    return;
  }
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = state;
  barrier.Transition.StateAfter = wanted;
  s.list->ResourceBarrier(1, &barrier);
  state = wanted;
}

bool BeginRecording() {
  if (s.recording) {
    return true;
  }
  if (FAILED(s.allocator->Reset()) ||
      FAILED(s.list->Reset(s.allocator.Get(), nullptr))) {
    XELOGE("[xna] direct: could not reset the command list");
    return false;
  }
  s.recording = true;
  return true;
}

void Submit() {
  if (!s.recording) {
    return;
  }
  s.recording = false;
  if (FAILED(s.list->Close())) {
    XELOGE("[xna] direct: the command list failed to close");
  } else {
    ID3D12CommandList* lists[] = {s.list.Get()};
    s.queue->ExecuteCommandLists(1, lists);
  }
  const uint64_t value = ++s.fence_value;
  if (SUCCEEDED(s.queue->Signal(s.fence.Get(), value)) &&
      s.fence->GetCompletedValue() < value &&
      SUCCEEDED(s.fence->SetEventOnCompletion(value, s.fence_event))) {
    WaitForSingleObject(s.fence_event, INFINITE);
  }
  s.upload_used = kZeroBytes;
  s.view_used = kReservedViews;
  s.retired.clear();
}

bool EnsureRoom(uint64_t upload_bytes, uint32_t views, uint32_t samplers) {
  if (upload_bytes > kUploadBytes - kZeroBytes ||
      views > kViewHeapSize - kReservedViews || samplers > kSamplerHeapSize) {
    return false;
  }
  const bool upload_full =
      uint64_t(xe::align(s.upload_used, 512u)) + upload_bytes > kUploadBytes;
  const bool views_full = s.view_used + views > kViewHeapSize;
  const bool samplers_full = s.sampler_used + samplers > kSamplerHeapSize;
  if (!upload_full && !views_full && !samplers_full) {
    return true;
  }
  Submit();
  if (samplers_full) {
    s.sampler_sets.clear();
    s.sampler_used = 0;
  }
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

DXGI_FORMAT TextureFormatFor(uint32_t surface_format) {
  switch (surface_format) {
    case 1:  return DXGI_FORMAT_B5G6R5_UNORM;
    case 2:  return DXGI_FORMAT_B5G5R5A1_UNORM;
    case 3:  return DXGI_FORMAT_B4G4R4A4_UNORM;
    case 4:  return DXGI_FORMAT_BC1_UNORM;
    case 5:  return DXGI_FORMAT_BC2_UNORM;
    case 6:  return DXGI_FORMAT_BC3_UNORM;
    case 7:  return DXGI_FORMAT_R8G8_SNORM;
    case 8:  return DXGI_FORMAT_R8G8B8A8_SNORM;
    case 9:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 10: return DXGI_FORMAT_R16G16_UNORM;
    case 11: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 12: return DXGI_FORMAT_A8_UNORM;
    case 13: return DXGI_FORMAT_R32_FLOAT;
    case 14: return DXGI_FORMAT_R32G32_FLOAT;
    case 15: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 16: return DXGI_FORMAT_R16_FLOAT;
    case 17: return DXGI_FORMAT_R16G16_FLOAT;
    case 18:
    case 19: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

DXGI_FORMAT TargetFormatFor(uint32_t surface_format) {
  switch (surface_format) {
    case 9:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 10: return DXGI_FORMAT_R16G16_UNORM;
    case 11: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 13: return DXGI_FORMAT_R32_FLOAT;
    case 14: return DXGI_FORMAT_R32G32_FLOAT;
    case 15: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 16: return DXGI_FORMAT_R16_FLOAT;
    case 17: return DXGI_FORMAT_R16G16_FLOAT;
    case 18:
    case 19: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

bool IsBlockCompressed(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC2_UNORM ||
         format == DXGI_FORMAT_BC3_UNORM;
}

void WriteTextureSrv(ID3D12Resource* resource, DXGI_FORMAT format,
                     uint32_t levels, uint32_t index) {
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = format;
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Texture2DArray.MostDetailedMip = 0;
  desc.Texture2DArray.MipLevels = levels;
  desc.Texture2DArray.FirstArraySlice = 0;
  desc.Texture2DArray.ArraySize = 1;
  s.device->CreateShaderResourceView(resource, &desc, StagingCpu(index));
}

Target* EnsureTarget(uint32_t key, uint32_t width, uint32_t height,
                     uint32_t surface_format) {
  DXGI_FORMAT format = kBackBufferFormat;
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
  if (target.color && target.width == width && target.height == height &&
      target.format == format) {
    return &target;
  }
  if (target.color) {
    s.retired.push_back(target.color);
    s.retired.push_back(target.depth);
    target.color.Reset();
    target.depth.Reset();
  }
  if (target.rtv == UINT32_MAX) {
    if (s.rtv_next >= kTargetViews || s.dsv_next >= kTargetViews) {
      XELOGW("[xna] direct: out of render target descriptors");
      return nullptr;
    }
    target.rtv = s.rtv_next++;
    target.dsv = s.dsv_next++;
  }
  if (target.srv == UINT32_MAX) {
    target.srv = AllocateStaging();
    if (target.srv == UINT32_MAX) {
      return nullptr;
    }
  }

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if (FAILED(s.device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
          IID_PPV_ARGS(&target.color)))) {
    XELOGE("[xna] direct: could not create a {}x{} render target (format {})",
           width, height, uint32_t(format));
    return nullptr;
  }
  D3D12_RESOURCE_DESC depth_desc = desc;
  depth_desc.Format = kDepthFormat;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                     D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
  D3D12_CLEAR_VALUE depth_clear = {};
  depth_clear.Format = kDepthFormat;
  depth_clear.DepthStencil.Depth = 1.0f;
  if (FAILED(s.device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
          IID_PPV_ARGS(&target.depth)))) {
    XELOGE("[xna] direct: could not create a {}x{} depth buffer", width,
           height);
    target.color.Reset();
    return nullptr;
  }
  target.width = width;
  target.height = height;
  target.format = format;
  target.color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  target.depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  s.device->CreateRenderTargetView(target.color.Get(), nullptr,
                                   RtvCpu(target.rtv));
  s.device->CreateDepthStencilView(target.depth.Get(), nullptr,
                                   DsvCpu(target.dsv));
  WriteTextureSrv(target.color.Get(), format, 1, target.srv);
  const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  s.list->ClearRenderTargetView(RtvCpu(target.rtv), zero, 0, nullptr);
  s.list->ClearDepthStencilView(
      DsvCpu(target.dsv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
      1.0f, 0, 0, nullptr);
  XELOGD("[xna] direct target {:08X}: {}x{} format {}", key, width, height,
         uint32_t(format));
  return &target;
}

uint32_t EnsureTexture(const XnaGpuTextureBinding& binding) {
  if (!binding.handle) {
    return UINT32_MAX;
  }
  XnaTextureView base;
  if (!XnaLookupTexture(binding.handle, 0, &base) || !base.width ||
      !base.height || !base.data) {
    return UINT32_MAX;
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
  const DXGI_FORMAT format = TextureFormatFor(base.format);

  HostTexture& texture = s.textures[binding.handle];
  const bool same_shape = texture.resource && texture.width == base.width &&
                          texture.height == base.height &&
                          texture.levels == levels && texture.format == format;
  if (same_shape && texture.version == base.version) {
    return texture.srv;
  }
  if (IsBlockCompressed(format) && ((base.width & 3) || (base.height & 3))) {
    XELOGW("[xna] direct: texture {:08X} is {}x{} block compressed, not a "
           "multiple of 4",
           binding.handle, base.width, base.height);
    return UINT32_MAX;
  }

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = base.width;
  desc.Height = base.height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = UINT16(levels);
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(levels);
  std::vector<UINT> rows(levels);
  std::vector<UINT64> row_bytes(levels);
  UINT64 total = 0;
  s.device->GetCopyableFootprints(&desc, 0, levels, 0, footprints.data(),
                                  rows.data(), row_bytes.data(), &total);
  if (!EnsureRoom(total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, 0, 0)) {
    XELOGW("[xna] direct: texture {:08X} needs {} upload bytes",
           binding.handle, total);
    return UINT32_MAX;
  }
  uint32_t offset = 0;
  uint8_t* mapped = AllocateUpload(uint32_t(total),
                                   D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,
                                   &offset);
  if (!mapped) {
    return UINT32_MAX;
  }

  if (same_shape) {
    D3D12_RESOURCE_STATES state = kReadState;
    MoveTo(texture.resource.Get(), state, D3D12_RESOURCE_STATE_COPY_DEST);
  } else {
    if (texture.resource) {
      s.retired.push_back(texture.resource);
      texture.resource.Reset();
    }
    if (FAILED(s.device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&texture.resource)))) {
      XELOGE("[xna] direct: could not create texture {:08X} {}x{} format {}",
             binding.handle, base.width, base.height, base.format);
      return UINT32_MAX;
    }
  }

  const uint32_t unit = XnaTextureSwapUnit(base.format);
  for (uint32_t level = 0; level < levels; ++level) {
    const XnaTextureView& view = views[level];
    const uint64_t source_pitch = view.size / (rows[level] ? rows[level] : 1);
    for (UINT row = 0; row < rows[level]; ++row) {
      const uint64_t copy = std::min<uint64_t>(row_bytes[level], source_pitch);
      XnaCopyTextureRow(mapped + footprints[level].Offset +
                            uint64_t(row) * footprints[level].Footprint.RowPitch,
                        view.data + uint64_t(row) * source_pitch, size_t(copy),
                        unit);
    }
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = texture.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = level;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = s.upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprints[level];
    source.PlacedFootprint.Offset += offset;
    s.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  }
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
  MoveTo(texture.resource.Get(), state, kReadState);

  if (texture.srv == UINT32_MAX) {
    texture.srv = AllocateStaging();
    if (texture.srv == UINT32_MAX) {
      return UINT32_MAX;
    }
  }
  WriteTextureSrv(texture.resource.Get(), format, levels, texture.srv);
  texture.version = base.version;
  texture.width = base.width;
  texture.height = base.height;
  texture.levels = levels;
  texture.format = format;
  return texture.srv;
}

D3D12_TEXTURE_ADDRESS_MODE AddressModeFor(uint32_t clamp) {
  switch (xenos::ClampMode(clamp & 7)) {
    case xenos::ClampMode::kRepeat:
      return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case xenos::ClampMode::kMirroredRepeat:
      return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
    case xenos::ClampMode::kMirrorClampToBorder:
      return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    case xenos::ClampMode::kClampToBorder:
      return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    default:
      return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  }
}

uint64_t SamplerKeyFor(const DxbcShader::SamplerBinding& binding,
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

void WriteSampler(uint64_t key, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const uint32_t mag = uint32_t(key & 3);
  const uint32_t min = uint32_t((key >> 2) & 3);
  const uint32_t mip = uint32_t((key >> 4) & 3);
  const uint32_t aniso = uint32_t((key >> 6) & 7);
  const D3D12_FILTER_TYPE mag_type = mag == uint32_t(xenos::TextureFilter::kLinear)
                                         ? D3D12_FILTER_TYPE_LINEAR
                                         : D3D12_FILTER_TYPE_POINT;
  const D3D12_FILTER_TYPE min_type = min == uint32_t(xenos::TextureFilter::kLinear)
                                         ? D3D12_FILTER_TYPE_LINEAR
                                         : D3D12_FILTER_TYPE_POINT;
  const D3D12_FILTER_TYPE mip_type = mip == uint32_t(xenos::TextureFilter::kLinear)
                                         ? D3D12_FILTER_TYPE_LINEAR
                                         : D3D12_FILTER_TYPE_POINT;
  D3D12_SAMPLER_DESC desc = {};
  desc.MaxAnisotropy = 1;
  if (aniso >= 2 && aniso <= 5) {
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (aniso - 1);
  } else {
    desc.Filter = D3D12_ENCODE_BASIC_FILTER(min_type, mag_type, mip_type,
                                            D3D12_FILTER_REDUCTION_TYPE_STANDARD);
  }
  desc.AddressU = AddressModeFor(uint32_t((key >> 9) & 7));
  desc.AddressV = AddressModeFor(uint32_t((key >> 12) & 7));
  desc.AddressW = desc.AddressV;
  desc.MipLODBias = 0.0f;
  desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  desc.MinLOD = 0.0f;
  desc.MaxLOD = mip == uint32_t(xenos::TextureFilter::kBaseMap)
                    ? 0.0f
                    : D3D12_FLOAT32_MAX;
  s.device->CreateSampler(&desc, handle);
}

uint32_t SamplerTable(const std::vector<uint64_t>& keys) {
  auto found = s.sampler_sets.find(keys);
  if (found != s.sampler_sets.end()) {
    return found->second;
  }
  if (s.sampler_used + keys.size() > kSamplerHeapSize) {
    return UINT32_MAX;
  }
  const uint32_t base = s.sampler_used;
  s.sampler_used += uint32_t(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    WriteSampler(keys[i], SamplerCpu(base + uint32_t(i)));
  }
  s.sampler_sets.emplace(keys, base);
  return base;
}

ID3D12RootSignature* RootSignatureFor(uint32_t textures_pixel,
                                      uint32_t samplers_pixel,
                                      uint32_t textures_vertex,
                                      uint32_t samplers_vertex) {
  const uint32_t key = textures_pixel | (samplers_pixel << 8) |
                       (textures_vertex << 16) | (samplers_vertex << 24);
  auto found = s.root_signatures.find(key);
  if (found != s.root_signatures.end()) {
    return found->second.Get();
  }
  D3D12_ROOT_PARAMETER parameters[10] = {};
  uint32_t count = 0;
  const auto cbv = [&](DxbcShaderTranslator::CbufferRegister reg_index,
                       D3D12_SHADER_VISIBILITY visibility) {
    D3D12_ROOT_PARAMETER& parameter = parameters[count++];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister = uint32_t(reg_index);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = visibility;
  };
  cbv(DxbcShaderTranslator::CbufferRegister::kFetchConstants,
      D3D12_SHADER_VISIBILITY_ALL);
  cbv(DxbcShaderTranslator::CbufferRegister::kFloatConstants,
      D3D12_SHADER_VISIBILITY_VERTEX);
  cbv(DxbcShaderTranslator::CbufferRegister::kFloatConstants,
      D3D12_SHADER_VISIBILITY_PIXEL);
  cbv(DxbcShaderTranslator::CbufferRegister::kSystemConstants,
      D3D12_SHADER_VISIBILITY_ALL);
  cbv(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants,
      D3D12_SHADER_VISIBILITY_ALL);

  D3D12_DESCRIPTOR_RANGE shared_ranges[2] = {};
  shared_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  shared_ranges[0].NumDescriptors = 1;
  shared_ranges[0].BaseShaderRegister =
      uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
  shared_ranges[0].RegisterSpace =
      uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
  shared_ranges[0].OffsetInDescriptorsFromTableStart = 0;
  shared_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  shared_ranges[1].NumDescriptors = 1;
  shared_ranges[1].BaseShaderRegister =
      uint32_t(DxbcShaderTranslator::UAVRegister::kSharedMemory);
  shared_ranges[1].RegisterSpace = 0;
  shared_ranges[1].OffsetInDescriptorsFromTableStart = 1;
  {
    D3D12_ROOT_PARAMETER& parameter = parameters[count++];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = shared_ranges;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  D3D12_DESCRIPTOR_RANGE ranges[4] = {};
  const auto table = [&](D3D12_DESCRIPTOR_RANGE& range,
                         D3D12_DESCRIPTOR_RANGE_TYPE type, uint32_t n,
                         uint32_t base_register, uint32_t space,
                         D3D12_SHADER_VISIBILITY visibility) {
    range.RangeType = type;
    range.NumDescriptors = n;
    range.BaseShaderRegister = base_register;
    range.RegisterSpace = space;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& parameter = parameters[count++];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = visibility;
  };
  const uint32_t textures_start =
      uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
  const uint32_t main_space = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
  if (textures_pixel) {
    table(ranges[0], D3D12_DESCRIPTOR_RANGE_TYPE_SRV, textures_pixel,
          textures_start, main_space, D3D12_SHADER_VISIBILITY_PIXEL);
  }
  if (samplers_pixel) {
    table(ranges[1], D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, samplers_pixel, 0, 0,
          D3D12_SHADER_VISIBILITY_PIXEL);
  }
  if (textures_vertex) {
    table(ranges[2], D3D12_DESCRIPTOR_RANGE_TYPE_SRV, textures_vertex,
          textures_start, main_space, D3D12_SHADER_VISIBILITY_VERTEX);
  }
  if (samplers_vertex) {
    table(ranges[3], D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, samplers_vertex, 0,
          0, D3D12_SHADER_VISIBILITY_VERTEX);
  }

  D3D12_ROOT_SIGNATURE_DESC desc = {};
  desc.NumParameters = count;
  desc.pParameters = parameters;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  ID3D12RootSignature* root_signature =
      ui::d3d12::util::CreateRootSignature(*s.provider, desc);
  if (!root_signature) {
    XELOGE("[xna] direct: no root signature for {} pixel textures, {} pixel "
           "samplers, {} vertex textures, {} vertex samplers",
           textures_pixel, samplers_pixel, textures_vertex, samplers_vertex);
    return nullptr;
  }
  ComPtr<ID3D12RootSignature> owned;
  owned.Attach(root_signature);
  s.root_signatures.emplace(key, owned);
  return root_signature;
}

D3D12_BLEND BlendFor(uint32_t xna, bool alpha) {
  switch (xna) {
    case 0:  return D3D12_BLEND_ONE;
    case 1:  return D3D12_BLEND_ZERO;
    case 2:  return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case 3:  return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 4:  return D3D12_BLEND_SRC_ALPHA;
    case 5:  return D3D12_BLEND_INV_SRC_ALPHA;
    case 6:  return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 7:  return alpha ? D3D12_BLEND_INV_DEST_ALPHA
                          : D3D12_BLEND_INV_DEST_COLOR;
    case 8:  return D3D12_BLEND_DEST_ALPHA;
    case 9:  return D3D12_BLEND_INV_DEST_ALPHA;
    case 10: return D3D12_BLEND_BLEND_FACTOR;
    case 11: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 12: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
  }
}

D3D12_BLEND_OP BlendOpFor(uint32_t xna) {
  switch (xna) {
    case 1:  return D3D12_BLEND_OP_SUBTRACT;
    case 2:  return D3D12_BLEND_OP_REV_SUBTRACT;
    case 3:  return D3D12_BLEND_OP_MIN;
    case 4:  return D3D12_BLEND_OP_MAX;
    default: return D3D12_BLEND_OP_ADD;
  }
}

D3D12_COMPARISON_FUNC CompareFor(uint32_t xna) {
  switch (xna) {
    case 1:  return D3D12_COMPARISON_FUNC_NEVER;
    case 2:  return D3D12_COMPARISON_FUNC_LESS;
    case 3:  return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 4:  return D3D12_COMPARISON_FUNC_EQUAL;
    case 5:  return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case 6:  return D3D12_COMPARISON_FUNC_GREATER;
    case 7:  return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    default: return D3D12_COMPARISON_FUNC_ALWAYS;
  }
}

D3D12_STENCIL_OP StencilOpFor(uint32_t xna) {
  switch (xna) {
    case 1:  return D3D12_STENCIL_OP_ZERO;
    case 2:  return D3D12_STENCIL_OP_REPLACE;
    case 3:  return D3D12_STENCIL_OP_INCR;
    case 4:  return D3D12_STENCIL_OP_DECR;
    case 5:  return D3D12_STENCIL_OP_INCR_SAT;
    case 6:  return D3D12_STENCIL_OP_DECR_SAT;
    case 7:  return D3D12_STENCIL_OP_INVERT;
    default: return D3D12_STENCIL_OP_KEEP;
  }
}

void ApplyStencil(const uint32_t* stencil, D3D12_DEPTH_STENCIL_DESC* desc) {
  desc->StencilEnable = stencil[0] ? TRUE : FALSE;
  desc->StencilReadMask = UINT8(stencil[5]);
  desc->StencilWriteMask = UINT8(stencil[6]);
  D3D12_DEPTH_STENCILOP_DESC face;
  face.StencilFunc = CompareFor(stencil[1]);
  face.StencilPassOp = StencilOpFor(stencil[2]);
  face.StencilFailOp = StencilOpFor(stencil[3]);
  face.StencilDepthFailOp = StencilOpFor(stencil[4]);
  desc->FrontFace = face;
  desc->BackFace = face;
}

void StencilKey(const XnaGpuDraw& draw, uint32_t* stencil) {
  if (!draw.stencil_enable) {
    for (uint32_t i = 0; i < 7; ++i) {
      stencil[i] = 0;
    }
    return;
  }
  stencil[0] = 1;
  stencil[1] = draw.stencil_function & 7;
  stencil[2] = draw.stencil_pass & 7;
  stencil[3] = draw.stencil_fail & 7;
  stencil[4] = draw.stencil_depth_fail & 7;
  stencil[5] = draw.stencil_read_mask & 0xFF;
  stencil[6] = draw.stencil_write_mask & 0xFF;
}

bool TopologyFor(xenos::PrimitiveType primitive,
                 D3D_PRIMITIVE_TOPOLOGY* topology,
                 D3D12_PRIMITIVE_TOPOLOGY_TYPE* topology_type) {
  switch (primitive) {
    case xenos::PrimitiveType::kPointList:
      *topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      *topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      return true;
    case xenos::PrimitiveType::kLineList:
      *topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      *topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      return true;
    case xenos::PrimitiveType::kLineStrip:
      *topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      *topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      return true;
    case xenos::PrimitiveType::kTriangleList:
      *topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      *topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      return true;
    case xenos::PrimitiveType::kTriangleStrip:
      *topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      *topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      return true;
    default:
      return false;
  }
}

DxbcShader* HostShaderFor(Shader* original) {
  const uint64_t hash = original->ucode_data_hash();
  auto found = s.shaders.find(hash);
  if (found != s.shaders.end()) {
    return found->second.get();
  }
  auto shader = std::make_unique<DxbcShader>(
      original->type(), hash, original->ucode_dwords(),
      original->ucode_dword_count(), std::endian::native);
  shader->AnalyzeUcode(s.disasm);
  if (!shader->is_ucode_analyzed()) {
    XELOGW("[xna] direct: shader {:016X} did not analyze", hash);
    return nullptr;
  }
  DxbcShader* result = shader.get();
  s.shaders.emplace(hash, std::move(shader));
  return result;
}

Shader::Translation* Translate(DxbcShader* shader, uint64_t modification) {
  Shader::Translation* translation =
      shader->GetOrCreateTranslation(modification);
  if (!translation->is_translated()) {
    s.translator->TranslateAnalyzedShader(*translation);
    if (!translation->is_valid()) {
      XELOGW("[xna] direct: {} shader {:016X} failed to translate: {}",
             shader->type() == xenos::ShaderType::kVertex ? "vertex" : "pixel",
             shader->ucode_data_hash(),
             translation->errors().empty()
                 ? std::string("no error given")
                 : translation->errors().front().message);
    }
  }
  return translation->is_valid() ? translation : nullptr;
}

bool TranslatePair(DxbcShader* vertex, DxbcShader* pixel,
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

  DxbcShaderTranslator::Modification vertex_modification(
      s.translator->GetDefaultVertexShaderModification(
          vertex->GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg),
          Shader::HostVertexShaderType::kVertex));
  vertex_modification.vertex.interpolator_mask = mask;
  vertex_modification.vertex.user_clip_plane_count = 0;
  vertex_modification.vertex.user_clip_plane_cull = 0;
  vertex_modification.vertex.vertex_kill_and = 0;
  vertex_modification.vertex.output_point_size = 0;
  *vertex_out = Translate(vertex, vertex_modification.value);
  if (!*vertex_out) {
    return false;
  }
  *pixel_out = nullptr;
  if (!pixel) {
    return true;
  }

  DxbcShaderTranslator::Modification pixel_modification(
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
      DxbcShaderTranslator::Modification::DepthStencilMode::kNoModifiers;
  pixel_modification.pixel.rt0_blend_rgb_factor_for_premult =
      xenos::BlendFactor::kOne;
  pixel_modification.pixel.rt0_blend_a_factor_for_premult =
      xenos::BlendFactor::kOne;
  *pixel_out = Translate(pixel, pixel_modification.value);
  return *pixel_out != nullptr;
}

ID3D12PipelineState* PipelineFor(const PipelineKey& key,
                                 Shader::Translation* vertex,
                                 Shader::Translation* pixel,
                                 ID3D12RootSignature* root_signature) {
  auto found = s.pipelines.find(key);
  if (found != s.pipelines.end()) {
    return found->second.Get();
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = root_signature;
  desc.VS.pShaderBytecode = vertex->translated_binary().data();
  desc.VS.BytecodeLength = vertex->translated_binary().size();
  if (pixel) {
    desc.PS.pShaderBytecode = pixel->translated_binary().data();
    desc.PS.BytecodeLength = pixel->translated_binary().size();
  }
  desc.BlendState.AlphaToCoverageEnable = FALSE;
  desc.BlendState.IndependentBlendEnable = TRUE;
  const bool color_blend = !(key.blend[0] == 0 && key.blend[1] == 1 &&
                             key.blend[2] == 0);
  const bool alpha_blend = !(key.blend[3] == 0 && key.blend[4] == 1 &&
                             key.blend[5] == 0);
  for (uint32_t i = 0; i < key.target_count; ++i) {
    D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[i];
    blend.BlendEnable = (color_blend || alpha_blend) ? TRUE : FALSE;
    blend.SrcBlend = BlendFor(key.blend[0], false);
    blend.DestBlend = BlendFor(key.blend[1], false);
    blend.BlendOp = BlendOpFor(key.blend[2]);
    blend.SrcBlendAlpha = BlendFor(key.blend[3], true);
    blend.DestBlendAlpha = BlendFor(key.blend[4], true);
    blend.BlendOpAlpha = BlendOpFor(key.blend[5]);
    blend.LogicOpEnable = FALSE;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = UINT8(key.write[i] & 0xF);
  }
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.FrontCounterClockwise = FALSE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.DepthStencilState.DepthEnable = key.depth_enable ? TRUE : FALSE;
  desc.DepthStencilState.DepthWriteMask =
      (key.depth_enable && key.depth_write) ? D3D12_DEPTH_WRITE_MASK_ALL
                                            : D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.DepthStencilState.DepthFunc = CompareFor(key.depth_function);
  ApplyStencil(key.stencil, &desc.DepthStencilState);
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology_type);
  desc.NumRenderTargets = key.target_count;
  for (uint32_t i = 0; i < key.target_count; ++i) {
    desc.RTVFormats[i] = DXGI_FORMAT(key.target_formats[i]);
  }
  desc.DSVFormat = kDepthFormat;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pipeline;
  if (FAILED(s.device->CreateGraphicsPipelineState(&desc,
                                                   IID_PPV_ARGS(&pipeline)))) {
    XELOGE("[xna] direct: pipeline creation failed for vs {:016X} ps {:016X}",
           vertex->shader().ucode_data_hash(),
           pixel ? pixel->shader().ucode_data_hash() : uint64_t(0));
  }
  s.pipelines.emplace(key, pipeline);
  return pipeline.Get();
}

uint32_t StrideForFetch(const DxbcShader* shader, uint32_t fetch_constant) {
  for (const auto& binding : shader->vertex_bindings()) {
    if (binding.fetch_constant == fetch_constant) {
      return binding.stride_words * 4;
    }
  }
  return 0;
}

bool PackFloats(const DxbcShader* shader, const float* registers,
                D3D12_GPU_VIRTUAL_ADDRESS* address) {
  if (!shader) {
    *address = s.upload_gpu;
    return true;
  }
  const auto& map = shader->constant_register_map();
  uint32_t count = 0;
  for (uint32_t word = 0; word < 4; ++word) {
    count += xe::bit_count(map.float_bitmap[word]);
  }
  uint32_t offset = 0;
  uint8_t* out = AllocateUpload(std::max<uint32_t>(count, 1) * 16,
                                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
                                &offset);
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
  *address = s.upload_gpu + offset;
  return true;
}

uint32_t FloatCount(const DxbcShader* shader) {
  if (!shader) {
    return 0;
  }
  uint32_t count = 0;
  for (uint32_t word = 0; word < 4; ++word) {
    count += xe::bit_count(shader->constant_register_map().float_bitmap[word]);
  }
  return count;
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
    XELOGW("[xna] direct: no conversion for XNA PrimitiveType {}",
           draw.primitive_type);
    return false;
  }
  D3D_PRIMITIVE_TOPOLOGY topology;
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
  if (!TopologyFor(primitive, &topology, &topology_type)) {
    XELOGW("[xna] direct: Xenos primitive {} has no D3D12 topology",
           uint32_t(primitive));
    return false;
  }
  uint32_t index_count = draw.primitive_count * 3;
  XenosPrimitiveIndexCount(primitive, draw.primitive_count, &index_count);
  if (!index_count) {
    return false;
  }

  DxbcShader* vertex = HostShaderFor(original_vertex);
  DxbcShader* pixel = original_pixel ? HostShaderFor(original_pixel) : nullptr;
  if (!vertex || (original_pixel && !pixel)) {
    return false;
  }
  Shader::Translation* vertex_translation = nullptr;
  Shader::Translation* pixel_translation = nullptr;
  if (!TranslatePair(vertex, pixel, &vertex_translation, &pixel_translation)) {
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
        XELOGW("[xna] direct: render target slot {} has no guest memory", i);
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
      [&](const DxbcShader::TextureBinding& binding) -> uint32_t {
    const uint32_t null_view =
        binding.dimension == xenos::FetchOpDimension::k3DOrStacked
            ? kNullSrv3D
            : (binding.dimension == xenos::FetchOpDimension::kCube
                   ? kNullSrvCube
                   : kNullSrv2DArray);
    const XnaGpuTextureBinding& texture = texture_for(binding.fetch_constant);
    if (!texture.guest_address || null_view != kNullSrv2DArray) {
      return null_view;
    }
    auto target = s.targets.find(texture.guest_address);
    if (target != s.targets.end() && target->second.color) {
      for (uint32_t i = 0; i < bound; ++i) {
        if (targets[i] == &target->second) {
          return null_view;
        }
      }
      sampled.push_back(&target->second);
      return target->second.srv;
    }
    if (texture.type != 0) {
      return null_view;
    }
    const uint32_t srv = EnsureTexture(texture);
    return srv != UINT32_MAX ? srv : null_view;
  };

  std::vector<uint32_t> views;
  uint32_t textures_pixel = 0;
  if (pixel) {
    for (const auto& binding : pixel->GetTextureBindingsAfterTranslation()) {
      views.push_back(resolve_texture(binding));
    }
    textures_pixel = uint32_t(views.size());
  }
  for (const auto& binding : vertex->GetTextureBindingsAfterTranslation()) {
    views.push_back(resolve_texture(binding));
  }
  const uint32_t textures_vertex = uint32_t(views.size()) - textures_pixel;

  std::vector<uint64_t> samplers_pixel;
  if (pixel) {
    for (const auto& binding : pixel->GetSamplerBindingsAfterTranslation()) {
      samplers_pixel.push_back(
          SamplerKeyFor(binding, texture_for(binding.fetch_constant)));
    }
  }
  std::vector<uint64_t> samplers_vertex;
  for (const auto& binding : vertex->GetSamplerBindingsAfterTranslation()) {
    samplers_vertex.push_back(
        SamplerKeyFor(binding, texture_for(binding.fetch_constant)));
  }

  uint32_t min_index = 0;
  uint32_t max_index = index_count - 1;
  std::vector<uint8_t> indices;
  if (draw.indexed) {
    const uint32_t stride = draw.index_32bit ? 4u : 2u;
    const uint64_t start = uint64_t(draw.start_index) * stride;
    if (!draw.index_guest_address || start >= draw.index_size_bytes) {
      XELOGD("[xna] direct: start index {} is past the {} byte index buffer",
             draw.start_index, draw.index_size_bytes);
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
      XELOGD("[xna] direct: vertex {} at stride {} is past the {} byte "
             "stream at {:08X}",
             uint64_t(first_vertex) + min_index, stride, stream.size_bytes,
             stream.guest_address);
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
  upload_bytes += sizeof(DxbcShaderTranslator::SystemConstants) + 256;
  upload_bytes += kFetchConstantDwords * sizeof(uint32_t) + 256;

  const bool pixel_samplers_cached =
      samplers_pixel.empty() || s.sampler_sets.count(samplers_pixel);
  const bool vertex_samplers_cached =
      samplers_vertex.empty() || s.sampler_sets.count(samplers_vertex);
  const uint32_t sampler_room =
      (pixel_samplers_cached ? 0 : uint32_t(samplers_pixel.size())) +
      (vertex_samplers_cached ? 0 : uint32_t(samplers_vertex.size()));
  if (!EnsureRoom(upload_bytes, uint32_t(views.size()),
                  sampler_room ? uint32_t(samplers_pixel.size() +
                                          samplers_vertex.size())
                               : 0)) {
    XELOGW("[xna] direct: draw needs {} upload bytes and {} views, more than "
           "a frame holds",
           upload_bytes, views.size());
    return false;
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
  uint32_t fetch_offset = 0;
  uint8_t* fetch_out = AllocateUpload(
      sizeof(fetch), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
      &fetch_offset);
  if (!fetch_out) {
    return false;
  }
  std::memcpy(fetch_out, fetch, sizeof(fetch));

  D3D12_INDEX_BUFFER_VIEW index_view = {};
  if (draw.indexed) {
    uint32_t offset = 0;
    uint8_t* out = AllocateUpload(uint32_t(indices.size()), 256, &offset);
    if (!out) {
      return false;
    }
    std::memcpy(out, indices.data(), indices.size());
    index_view.BufferLocation = s.upload_gpu + offset;
    index_view.SizeInBytes = UINT(indices.size());
    index_view.Format =
        draw.index_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
  }

  float registers[512][4] = {};
  for (const XnaGpuConstant& constant : draw.constants) {
    if (constant.register_index < 512) {
      std::memcpy(registers[constant.register_index], constant.value,
                  sizeof(constant.value));
    }
  }
  D3D12_GPU_VIRTUAL_ADDRESS float_vertex = 0;
  D3D12_GPU_VIRTUAL_ADDRESS float_pixel = 0;
  if (!PackFloats(vertex, &registers[0][0], &float_vertex) ||
      !PackFloats(pixel, &registers[256][0], &float_pixel)) {
    return false;
  }

  DxbcShaderTranslator::SystemConstants system;
  std::memset(&system, 0, sizeof(system));
  system.flags = DxbcShaderTranslator::kSysFlag_WNotReciprocal |
                 (uint32_t(xenos::CompareFunction::kAlways)
                  << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift);
  if (topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) {
    system.flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  } else if (topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE) {
    system.flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }
  system.vertex_index_endian = xenos::Endian::kNone;
  system.vertex_index_offset = 0;
  system.vertex_index_min = 0;
  system.vertex_index_max = 0xFFFFFF;
  system.ndc_scale[0] = 1.0f;
  system.ndc_scale[1] = 1.0f;
  system.ndc_scale[2] = 1.0f;
  for (uint32_t i = 0; i < 4; ++i) {
    system.color_exp_bias[i] = 1.0f;
  }
  uint32_t system_offset = 0;
  uint8_t* system_out = AllocateUpload(
      sizeof(system), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
      &system_offset);
  if (!system_out) {
    return false;
  }
  std::memcpy(system_out, &system, sizeof(system));

  const uint32_t view_base = s.view_used;
  s.view_used += uint32_t(views.size());
  for (size_t i = 0; i < views.size(); ++i) {
    s.device->CopyDescriptorsSimple(1, ViewCpu(view_base + uint32_t(i)),
                                    StagingCpu(views[i]),
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
  const uint32_t sampler_pixel_base =
      samplers_pixel.empty() ? 0 : SamplerTable(samplers_pixel);
  const uint32_t sampler_vertex_base =
      samplers_vertex.empty() ? 0 : SamplerTable(samplers_vertex);
  if (sampler_pixel_base == UINT32_MAX || sampler_vertex_base == UINT32_MAX) {
    return false;
  }

  ID3D12RootSignature* root_signature = RootSignatureFor(
      textures_pixel, uint32_t(samplers_pixel.size()), textures_vertex,
      uint32_t(samplers_vertex.size()));
  if (!root_signature) {
    return false;
  }
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vertex = vertex_translation;
  key.pixel = pixel_translation;
  key.target_count = bound;
  for (uint32_t i = 0; i < bound; ++i) {
    key.target_formats[i] = uint32_t(targets[i]->format);
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
  StencilKey(draw, key.stencil);
  key.topology_type = uint32_t(topology_type);
  ID3D12PipelineState* pipeline =
      PipelineFor(key, vertex_translation, pixel_translation, root_signature);
  if (!pipeline) {
    return false;
  }

  for (uint32_t i = 0; i < bound; ++i) {
    MoveTo(targets[i]->color.Get(), targets[i]->color_state,
           D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  MoveTo(targets[0]->depth.Get(), targets[0]->depth_state,
         D3D12_RESOURCE_STATE_DEPTH_WRITE);
  for (Target* target : sampled) {
    MoveTo(target->color.Get(), target->color_state, kReadState);
  }

  ID3D12DescriptorHeap* heaps[] = {s.view_heap.Get(), s.sampler_heap.Get()};
  s.list->SetDescriptorHeaps(2, heaps);
  s.list->SetGraphicsRootSignature(root_signature);
  s.list->SetGraphicsRootConstantBufferView(0, s.upload_gpu + fetch_offset);
  s.list->SetGraphicsRootConstantBufferView(1, float_vertex);
  s.list->SetGraphicsRootConstantBufferView(2, float_pixel);
  s.list->SetGraphicsRootConstantBufferView(3, s.upload_gpu + system_offset);
  s.list->SetGraphicsRootConstantBufferView(4, s.upload_gpu);
  s.list->SetGraphicsRootDescriptorTable(5, ViewGpu(0));
  uint32_t parameter = 6;
  if (textures_pixel) {
    s.list->SetGraphicsRootDescriptorTable(parameter++, ViewGpu(view_base));
  }
  if (!samplers_pixel.empty()) {
    s.list->SetGraphicsRootDescriptorTable(parameter++,
                                           SamplerGpu(sampler_pixel_base));
  }
  if (textures_vertex) {
    s.list->SetGraphicsRootDescriptorTable(
        parameter++, ViewGpu(view_base + textures_pixel));
  }
  if (!samplers_vertex.empty()) {
    s.list->SetGraphicsRootDescriptorTable(parameter++,
                                           SamplerGpu(sampler_vertex_base));
  }

  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
  for (uint32_t i = 0; i < bound; ++i) {
    rtvs[i] = RtvCpu(targets[i]->rtv);
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = DsvCpu(targets[0]->dsv);
  s.list->OMSetRenderTargets(bound, rtvs, FALSE, &dsv);

  const float target_width = float(targets[0]->width);
  const float target_height = float(targets[0]->height);
  D3D12_VIEWPORT viewport;
  viewport.TopLeftX = float(draw.viewport_x);
  viewport.TopLeftY = float(draw.viewport_y);
  viewport.Width =
      draw.viewport_width ? float(draw.viewport_width) : target_width;
  viewport.Height =
      draw.viewport_height ? float(draw.viewport_height) : target_height;
  viewport.MinDepth = std::clamp(draw.viewport_min_depth, 0.0f, 1.0f);
  viewport.MaxDepth = std::clamp(draw.viewport_max_depth, 0.0f, 1.0f);
  s.list->RSSetViewports(1, &viewport);
  const D3D12_RECT scissor = {0, 0, LONG(targets[0]->width),
                              LONG(targets[0]->height)};
  s.list->RSSetScissorRects(1, &scissor);
  const float blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  s.list->OMSetBlendFactor(blend_factor);
  s.list->IASetPrimitiveTopology(topology);
  s.list->SetPipelineState(pipeline);
  s.list->OMSetStencilRef(draw.stencil_reference & 0xFF);
  if (draw.indexed) {
    s.list->IASetIndexBuffer(&index_view);
    s.list->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
  } else {
    s.list->DrawInstanced(index_count, 1, 0, 0);
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
  if (clear_color) {
    const float rgba[4] = {color ? color[0] : 0.0f, color ? color[1] : 0.0f,
                           color ? color[2] : 0.0f, color ? color[3] : 0.0f};
    MoveTo(host->color.Get(), host->color_state,
           D3D12_RESOURCE_STATE_RENDER_TARGET);
    s.list->ClearRenderTargetView(RtvCpu(host->rtv), rgba, 0, nullptr);
  }
  if (clear_depth) {
    MoveTo(host->depth.Get(), host->depth_state,
           D3D12_RESOURCE_STATE_DEPTH_WRITE);
    s.list->ClearDepthStencilView(
        DsvCpu(host->dsv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        std::clamp(depth, 0.0f, 1.0f), 0, 0, nullptr);
  }
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
        auto& d3d_context = static_cast<
            ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(
            context);
        ID3D12Resource* guest_output = d3d_context.resource_uav_capable();
        if (!guest_output) {
          return false;
        }
        if (!EnsureRoom(sizeof(BlitVertex) * 6 + 256, 1, 0)) {
          return false;
        }
        const float w = float(kBackBufferWidth);
        const float h = float(kBackBufferHeight);
        const BlitVertex quad[6] = {
            {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            {w, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            {0.0f, h, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            {w, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            {w, h, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            {0.0f, h, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        };
        uint32_t vertex_offset = 0;
        uint8_t* vertices = AllocateUpload(sizeof(quad), 256, &vertex_offset);
        if (!vertices) {
          return false;
        }
        std::memcpy(vertices, quad, sizeof(quad));
        const uint32_t view = s.view_used++;
        s.device->CopyDescriptorsSimple(1, ViewCpu(view), StagingCpu(back->srv),
                                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        MoveTo(back->color.Get(), back->color_state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvCpu(s.present_rtv);
        s.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT viewport = {0.0f, 0.0f, w, h, 0.0f, 1.0f};
        s.list->RSSetViewports(1, &viewport);
        const D3D12_RECT scissor = {0, 0, LONG(kBackBufferWidth),
                                    LONG(kBackBufferHeight)};
        s.list->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* heaps[] = {s.view_heap.Get(),
                                         s.sampler_heap.Get()};
        s.list->SetDescriptorHeaps(2, heaps);
        s.list->SetGraphicsRootSignature(s.blit_root_signature.Get());
        s.list->SetPipelineState(s.blit_pipeline.Get());
        const float constants[4] = {1.0f / w, 1.0f / h, 0.0f, 0.0f};
        s.list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
        s.list->SetGraphicsRootDescriptorTable(1, ViewGpu(view));
        s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
        vertex_view.BufferLocation = s.upload_gpu + vertex_offset;
        vertex_view.SizeInBytes = UINT(sizeof(quad));
        vertex_view.StrideInBytes = UINT(sizeof(BlitVertex));
        s.list->IASetVertexBuffers(0, 1, &vertex_view);
        s.list->DrawInstanced(6, 1, 0, 0);

        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = s.present_target.Get();
        barriers[0].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = guest_output;
        barriers[1].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Transition.StateBefore =
            ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        s.list->ResourceBarrier(2, barriers);
        s.list->CopyResource(guest_output, s.present_target.Get());
        std::swap(barriers[0].Transition.StateBefore,
                  barriers[0].Transition.StateAfter);
        std::swap(barriers[1].Transition.StateBefore,
                  barriers[1].Transition.StateAfter);
        s.list->ResourceBarrier(2, barriers);
        Submit();
        return true;
      });
  Submit();
}

bool ReadBackLocked(uint8_t* out, uint32_t bytes) {
  const uint32_t row = kBackBufferWidth * 4;
  if (!out || bytes < row * kBackBufferHeight) {
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
  const D3D12_RESOURCE_DESC desc = back->color->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rows = 0;
  UINT64 row_bytes = 0;
  UINT64 total = 0;
  s.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows,
                                  &row_bytes, &total);
  if (!s.readback || s.readback_bytes < total) {
    s.readback.Reset();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, total,
                                            D3D12_RESOURCE_FLAG_NONE);
    if (FAILED(s.device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE,
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&s.readback)))) {
      XELOGE("[xna] direct: could not create the readback buffer");
      Submit();
      return false;
    }
    s.readback_bytes = total;
  }
  MoveTo(back->color.Get(), back->color_state,
         D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION destination = {};
  destination.pResource = s.readback.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint = footprint;
  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = back->color.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  source.SubresourceIndex = 0;
  s.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  Submit();

  D3D12_RANGE read_range = {0, SIZE_T(total)};
  uint8_t* mapped = nullptr;
  if (FAILED(s.readback->Map(0, &read_range,
                             reinterpret_cast<void**>(&mapped)))) {
    return false;
  }
  for (uint32_t y = 0; y < kBackBufferHeight; ++y) {
    std::memcpy(out + size_t(y) * row,
                mapped + footprint.Offset +
                    uint64_t(y) * footprint.Footprint.RowPitch,
                row);
  }
  D3D12_RANGE written = {0, 0};
  s.readback->Unmap(0, &written);
  return true;
}

bool CreateBlit() {
  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;
  D3D12_ROOT_PARAMETER parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = 4;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  parameters[1].DescriptorTable.pDescriptorRanges = &range;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = 2;
  root_desc.pParameters = parameters;
  root_desc.NumStaticSamplers = 1;
  root_desc.pStaticSamplers = &sampler;
  root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ID3D12RootSignature* root_signature =
      ui::d3d12::util::CreateRootSignature(*s.provider, root_desc);
  if (!root_signature) {
    return false;
  }
  s.blit_root_signature.Attach(root_signature);

  D3D12_INPUT_ELEMENT_DESC elements[3] = {};
  elements[0].SemanticName = "POSITION";
  elements[0].Format = DXGI_FORMAT_R32G32_FLOAT;
  elements[0].AlignedByteOffset = 0;
  elements[1].SemanticName = "TEXCOORD";
  elements[1].Format = DXGI_FORMAT_R32G32_FLOAT;
  elements[1].AlignedByteOffset = 8;
  elements[2].SemanticName = "COLOR";
  elements[2].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  elements[2].AlignedByteOffset = 16;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = s.blit_root_signature.Get();
  desc.VS.pShaderBytecode = kXnaSpriteVS;
  desc.VS.BytecodeLength = sizeof(kXnaSpriteVS);
  desc.PS.pShaderBytecode = kXnaSpritePS;
  desc.PS.BytecodeLength = sizeof(kXnaSpritePS);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.InputLayout.pInputElementDescs = elements;
  desc.InputLayout.NumElements = 3;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = ui::d3d12::D3D12Presenter::kGuestOutputFormat;
  desc.SampleDesc.Count = 1;
  return SUCCEEDED(s.device->CreateGraphicsPipelineState(
      &desc, IID_PPV_ARGS(&s.blit_pipeline)));
}

bool CreateAvatarRootSignature() {
  if (s.avatar_root_signature) {
    return true;
  }
  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = avatar::kLayerCount;
  range.BaseShaderRegister = 0;
  D3D12_ROOT_PARAMETER parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[0].Descriptor.ShaderRegister = 0;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  parameters[1].DescriptorTable.pDescriptorRanges = &range;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    const D3D12_TEXTURE_ADDRESS_MODE mode =
        i ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[i].AddressU = mode;
    samplers[i].AddressV = mode;
    samplers[i].AddressW = mode;
    samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[i].ShaderRegister = i;
    samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  D3D12_ROOT_SIGNATURE_DESC desc = {};
  desc.NumParameters = 2;
  desc.pParameters = parameters;
  desc.NumStaticSamplers = 2;
  desc.pStaticSamplers = samplers;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ID3D12RootSignature* root_signature =
      ui::d3d12::util::CreateRootSignature(*s.provider, desc);
  if (!root_signature) {
    XELOGE("[xna] direct: could not create the avatar root signature");
    return false;
  }
  s.avatar_root_signature.Attach(root_signature);
  return true;
}

ID3D12PipelineState* AvatarPipelineFor(DXGI_FORMAT format,
                                       const XnaGpuDraw& state) {
  uint32_t stencil[7];
  StencilKey(state, stencil);
  std::vector<uint32_t> key(stencil, stencil + 7);
  key.push_back(uint32_t(format));
  auto found = s.avatar_pipelines.find(key);
  if (found != s.avatar_pipelines.end()) {
    return found->second.Get();
  }
  if (!CreateAvatarRootSignature()) {
    return nullptr;
  }
  D3D12_INPUT_ELEMENT_DESC elements[2 + avatar::kLayerCount] = {};
  elements[0].SemanticName = "POSITION";
  elements[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
  elements[0].AlignedByteOffset = 0;
  elements[1].SemanticName = "NORMAL";
  elements[1].Format = DXGI_FORMAT_R32G32B32_FLOAT;
  elements[1].AlignedByteOffset = 12;
  for (uint32_t k = 0; k < avatar::kLayerCount; ++k) {
    elements[2 + k].SemanticName = "TEXCOORD";
    elements[2 + k].SemanticIndex = k;
    elements[2 + k].Format = DXGI_FORMAT_R32G32_FLOAT;
    elements[2 + k].AlignedByteOffset = 24 + 8 * k;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = s.avatar_root_signature.Get();
  desc.VS.pShaderBytecode = kXnaAvatarVS;
  desc.VS.BytecodeLength = sizeof(kXnaAvatarVS);
  desc.PS.pShaderBytecode = kXnaAvatarPS;
  desc.PS.BytecodeLength = sizeof(kXnaAvatarPS);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.DepthStencilState.DepthEnable = TRUE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  ApplyStencil(stencil, &desc.DepthStencilState);
  desc.InputLayout.pInputElementDescs = elements;
  desc.InputLayout.NumElements = 2 + avatar::kLayerCount;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.DSVFormat = kDepthFormat;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pipeline;
  if (FAILED(s.device->CreateGraphicsPipelineState(&desc,
                                                   IID_PPV_ARGS(&pipeline)))) {
    XELOGE("[xna] direct: could not create the avatar pipeline for format {}",
           uint32_t(format));
  }
  s.avatar_pipelines.emplace(key, pipeline);
  return pipeline.Get();
}

uint32_t EnsureAvatarTexture(uint64_t id, const avatar::Texture& texture) {
  HostTexture& host = s.avatar_textures[id];
  if (host.resource && host.srv != UINT32_MAX) {
    return host.srv;
  }
  if (!texture.width || !texture.height || !texture.slices ||
      texture.rgba.size() <
          size_t(texture.width) * texture.height * 4 * texture.slices) {
    return UINT32_MAX;
  }
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = texture.width;
  desc.Height = texture.height;
  desc.DepthOrArraySize = UINT16(texture.slices);
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(texture.slices);
  std::vector<UINT> rows(texture.slices);
  std::vector<UINT64> row_bytes(texture.slices);
  UINT64 total = 0;
  s.device->GetCopyableFootprints(&desc, 0, texture.slices, 0,
                                  footprints.data(), rows.data(),
                                  row_bytes.data(), &total);
  if (!EnsureRoom(total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, 0, 0)) {
    XELOGW("[xna] direct: avatar texture {:X} needs {} upload bytes", id,
           total);
    return UINT32_MAX;
  }
  uint32_t offset = 0;
  uint8_t* mapped = AllocateUpload(uint32_t(total),
                                   D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,
                                   &offset);
  if (!mapped) {
    return UINT32_MAX;
  }
  ComPtr<ID3D12Resource> resource;
  if (FAILED(s.device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&resource)))) {
    XELOGE("[xna] direct: could not create avatar texture {:X} {}x{}x{}", id,
           texture.width, texture.height, texture.slices);
    return UINT32_MAX;
  }
  const size_t source_row = size_t(texture.width) * 4;
  const size_t source_slice = source_row * texture.height;
  for (uint32_t slice = 0; slice < texture.slices; ++slice) {
    for (UINT row = 0; row < rows[slice]; ++row) {
      std::memcpy(mapped + footprints[slice].Offset +
                      uint64_t(row) * footprints[slice].Footprint.RowPitch,
                  texture.rgba.data() + slice * source_slice + row * source_row,
                  source_row);
    }
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = slice;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = s.upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprints[slice];
    source.PlacedFootprint.Offset += offset;
    s.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  }
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
  MoveTo(resource.Get(), state, kReadState);
  if (host.srv == UINT32_MAX) {
    host.srv = AllocateStaging();
    if (host.srv == UINT32_MAX) {
      return UINT32_MAX;
    }
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
  view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Texture2DArray.MipLevels = 1;
  view.Texture2DArray.ArraySize = texture.slices;
  s.device->CreateShaderResourceView(resource.Get(), &view,
                                     StagingCpu(host.srv));
  host.resource = resource;
  host.width = texture.width;
  host.height = texture.height;
  host.levels = 1;
  host.format = DXGI_FORMAT_R8G8B8A8_UNORM;
  return host.srv;
}

bool DrawAvatarLocked(const XnaGpuDraw& state,
                      const XnaAvatarDrawBatch* batches, uint32_t count) {
  if (!BeginRecording()) {
    return false;
  }
  Target* target =
      state.target_count && state.target_addresses[0]
          ? EnsureTarget(state.target_addresses[0], state.target_width,
                         state.target_height, state.target_formats[0])
          : EnsureTarget(0, 0, 0, 0);
  if (!target) {
    return false;
  }
  ID3D12PipelineState* pipeline = AvatarPipelineFor(target->format, state);
  if (!pipeline) {
    return false;
  }
  for (uint32_t b = 0; b < count; ++b) {
    const XnaAvatarDrawBatch& batch = batches[b];
    if (!batch.vertex_count || !batch.index_count) {
      continue;
    }
    uint32_t views[avatar::kLayerCount];
    for (uint32_t i = 0; i < avatar::kLayerCount; ++i) {
      views[i] = kNullSrv2DArray;
      if (batch.textures[i]) {
        const uint32_t srv =
            EnsureAvatarTexture(batch.texture_ids[i], *batch.textures[i]);
        if (srv != UINT32_MAX) {
          views[i] = srv;
        }
      }
    }
    const uint32_t vertex_bytes =
        batch.vertex_count * uint32_t(sizeof(avatar::GpuVertex));
    const uint32_t index_bytes = batch.index_count * uint32_t(sizeof(uint16_t));
    if (!EnsureRoom(uint64_t(vertex_bytes) + index_bytes +
                        sizeof(avatar::GpuConstants) + 1024,
                    avatar::kLayerCount, 0)) {
      XELOGW("[xna] direct: avatar batch of {} vertices does not fit a frame",
             batch.vertex_count);
      continue;
    }
    uint32_t vertex_offset = 0;
    uint8_t* vertex_out = AllocateUpload(vertex_bytes, 256, &vertex_offset);
    uint32_t index_offset = 0;
    uint8_t* index_out = AllocateUpload(index_bytes, 256, &index_offset);
    uint32_t constant_offset = 0;
    uint8_t* constant_out = AllocateUpload(
        uint32_t(sizeof(avatar::GpuConstants)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, &constant_offset);
    if (!vertex_out || !index_out || !constant_out) {
      return false;
    }
    std::memcpy(vertex_out, batch.vertices, vertex_bytes);
    std::memcpy(index_out, batch.indices, index_bytes);
    std::memcpy(constant_out, &batch.constants, sizeof(batch.constants));
    const uint32_t view_base = s.view_used;
    s.view_used += avatar::kLayerCount;
    for (uint32_t i = 0; i < avatar::kLayerCount; ++i) {
      s.device->CopyDescriptorsSimple(1, ViewCpu(view_base + i),
                                      StagingCpu(views[i]),
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    MoveTo(target->color.Get(), target->color_state,
           D3D12_RESOURCE_STATE_RENDER_TARGET);
    MoveTo(target->depth.Get(), target->depth_state,
           D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ID3D12DescriptorHeap* heaps[] = {s.view_heap.Get(), s.sampler_heap.Get()};
    s.list->SetDescriptorHeaps(2, heaps);
    s.list->SetGraphicsRootSignature(s.avatar_root_signature.Get());
    s.list->SetPipelineState(pipeline);
    s.list->OMSetStencilRef(state.stencil_reference & 0xFF);
    s.list->SetGraphicsRootConstantBufferView(0,
                                              s.upload_gpu + constant_offset);
    s.list->SetGraphicsRootDescriptorTable(1, ViewGpu(view_base));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvCpu(target->rtv);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = DsvCpu(target->dsv);
    s.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    D3D12_VIEWPORT viewport;
    viewport.TopLeftX = float(state.viewport_x);
    viewport.TopLeftY = float(state.viewport_y);
    viewport.Width = state.viewport_width ? float(state.viewport_width)
                                          : float(target->width);
    viewport.Height = state.viewport_height ? float(state.viewport_height)
                                            : float(target->height);
    viewport.MinDepth = std::clamp(state.viewport_min_depth, 0.0f, 1.0f);
    viewport.MaxDepth = std::clamp(state.viewport_max_depth, 0.0f, 1.0f);
    if (viewport.MaxDepth <= viewport.MinDepth) {
      viewport.MinDepth = 0.0f;
      viewport.MaxDepth = 1.0f;
    }
    s.list->RSSetViewports(1, &viewport);
    const D3D12_RECT scissor = {0, 0, LONG(target->width),
                                LONG(target->height)};
    s.list->RSSetScissorRects(1, &scissor);
    s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    vertex_view.BufferLocation = s.upload_gpu + vertex_offset;
    vertex_view.SizeInBytes = vertex_bytes;
    vertex_view.StrideInBytes = UINT(sizeof(avatar::GpuVertex));
    s.list->IASetVertexBuffers(0, 1, &vertex_view);
    D3D12_INDEX_BUFFER_VIEW index_view = {};
    index_view.BufferLocation = s.upload_gpu + index_offset;
    index_view.SizeInBytes = index_bytes;
    index_view.Format = DXGI_FORMAT_R16_UINT;
    s.list->IASetIndexBuffer(&index_view);
    s.list->DrawIndexedInstanced(batch.index_count, 1, 0, 0, 0);
  }
  return true;
}

bool Initialize() {
  auto* state = kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  auto* graphics = emulator ? emulator->graphics_system() : nullptr;
  if (!graphics) {
    return false;
  }
  s.provider = dynamic_cast<ui::d3d12::D3D12Provider*>(graphics->provider());
  s.presenter =
      dynamic_cast<ui::d3d12::D3D12Presenter*>(graphics->presenter());
  if (!s.provider || !s.presenter) {
    XELOGW("[xna] direct D3D12 path unavailable: the host backend is not "
           "D3D12");
    return false;
  }
  s.device = s.provider->GetDevice();
  s.queue = s.provider->GetDirectQueue();
  if (!s.device || !s.queue) {
    return false;
  }
  if (FAILED(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&s.allocator))) ||
      FAILED(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         s.allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&s.list))) ||
      FAILED(s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&s.fence)))) {
    XELOGE("[xna] direct: could not create the command objects");
    return false;
  }
  s.list->Close();
  s.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!s.fence_event) {
    return false;
  }

  D3D12_RESOURCE_DESC upload_desc;
  ui::d3d12::util::FillBufferResourceDesc(upload_desc, kUploadBytes,
                                          D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(s.device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE,
          &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&s.upload)))) {
    XELOGE("[xna] direct: could not create the upload ring");
    return false;
  }
  D3D12_RANGE no_read = {0, 0};
  if (FAILED(s.upload->Map(0, &no_read,
                           reinterpret_cast<void**>(&s.upload_mapped)))) {
    return false;
  }
  s.upload_gpu = s.upload->GetGPUVirtualAddress();
  std::memset(s.upload_mapped, 0, kZeroBytes);

  const auto make_heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count,
                             bool shader_visible,
                             ComPtr<ID3D12DescriptorHeap>& heap) {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    return SUCCEEDED(
        s.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)));
  };
  if (!make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewHeapSize, true,
                 s.view_heap) ||
      !make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize, true,
                 s.sampler_heap) ||
      !make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kStagingViews, false,
                 s.staging_heap) ||
      !make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kTargetViews + 1, false,
                 s.rtv_heap) ||
      !make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kTargetViews, false,
                 s.dsv_heap)) {
    XELOGE("[xna] direct: could not create the descriptor heaps");
    return false;
  }

  ui::d3d12::util::CreateBufferRawSRV(s.device, ViewCpu(0), s.upload.Get(),
                                      kUploadBytes);
  D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav = {};
  null_uav.Format = DXGI_FORMAT_R32_TYPELESS;
  null_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  null_uav.Buffer.NumElements = 1;
  null_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  s.device->CreateUnorderedAccessView(nullptr, nullptr, &null_uav,
                                      ViewCpu(1));

  D3D12_SHADER_RESOURCE_VIEW_DESC null_srv = {};
  null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  null_srv.Texture2DArray.MipLevels = 1;
  null_srv.Texture2DArray.ArraySize = 1;
  s.device->CreateShaderResourceView(nullptr, &null_srv,
                                     StagingCpu(kNullSrv2DArray));
  null_srv = {};
  null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  null_srv.Texture3D.MipLevels = 1;
  s.device->CreateShaderResourceView(nullptr, &null_srv,
                                     StagingCpu(kNullSrv3D));
  null_srv = {};
  null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  null_srv.TextureCube.MipLevels = 1;
  s.device->CreateShaderResourceView(nullptr, &null_srv,
                                     StagingCpu(kNullSrvCube));

  s.translator = std::make_unique<DxbcShaderTranslator>(
      s.provider->GetAdapterVendorID(), false, false, true, true, 1, 1);

  D3D12_RESOURCE_DESC present_desc = {};
  present_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  present_desc.Width = kBackBufferWidth;
  present_desc.Height = kBackBufferHeight;
  present_desc.DepthOrArraySize = 1;
  present_desc.MipLevels = 1;
  present_desc.Format = ui::d3d12::D3D12Presenter::kGuestOutputFormat;
  present_desc.SampleDesc.Count = 1;
  present_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if (FAILED(s.device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &present_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
          IID_PPV_ARGS(&s.present_target)))) {
    XELOGE("[xna] direct: could not create the present target");
    return false;
  }
  s.present_rtv = kTargetViews;
  s.device->CreateRenderTargetView(s.present_target.Get(), nullptr,
                                   RtvCpu(s.present_rtv));

  if (!CreateBlit()) {
    XELOGE("[xna] direct: could not create the present pipeline");
    return false;
  }
  XELOGI("[xna] hosted title draws straight to D3D12");
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

struct ShaderRegistry {
  std::mutex mutex;
  std::unordered_map<uint64_t, std::unique_ptr<DxbcShader>> shaders;
  Shader* active_vertex = nullptr;
  Shader* active_pixel = nullptr;
};

ShaderRegistry& Registry() {
  static ShaderRegistry registry;
  return registry;
}

}  // namespace

bool XnaDirectActive() {
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (Ready()) {
      return true;
    }
  }
  return XnaVulkanDirectActive();
}

bool XnaDirectDraw(const XnaGpuDraw& draw, const XnaGpuStream* streams,
                   uint32_t stream_count) {
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (Ready()) {
      return DrawLocked(draw, streams, stream_count);
    }
  }
  return XnaVulkanDirectDraw(draw, streams, stream_count);
}

void XnaDirectClear(const XnaGpuTarget& target, const float* color,
                    bool clear_color, bool clear_depth, float depth) {
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (Ready()) {
      ClearLocked(target, color, clear_color, clear_depth, depth);
      return;
    }
  }
  XnaVulkanDirectClear(target, color, clear_color, clear_depth, depth);
}

void XnaDirectPresent() {
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (Ready()) {
      PresentLocked();
      return;
    }
  }
  XnaVulkanDirectPresent();
}

bool XnaDirectReadBackBuffer(uint8_t* out, uint32_t bytes) {
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (Ready()) {
      return ReadBackLocked(out, bytes);
    }
  }
  return XnaVulkanDirectReadBackBuffer(out, bytes);
}

bool XnaDirectDrawAvatar(const XnaGpuDraw& target,
                         const XnaAvatarDrawBatch* batches, uint32_t count) {
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!Ready()) {
    XELOGD("[xna] avatar draw skipped: the direct D3D12 path is not active");
    return false;
  }
  return DrawAvatarLocked(target, batches, count);
}

void XnaDirectClearStencil(const XnaGpuTarget& target, uint32_t value) {
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!Ready() || !BeginRecording()) {
    return;
  }
  Target* host = EnsureTarget(target.guest_address, target.width,
                              target.height, target.format);
  if (!host) {
    return;
  }
  MoveTo(host->depth.Get(), host->depth_state,
         D3D12_RESOURCE_STATE_DEPTH_WRITE);
  s.list->ClearDepthStencilView(DsvCpu(host->dsv), D3D12_CLEAR_FLAG_STENCIL,
                                1.0f, UINT8(value), 0, nullptr);
}

Shader* XnaDirectLoadShader(xenos::ShaderType type, const uint32_t* ucode,
                            uint32_t dword_count) {
  if (!ucode || !dword_count) {
    return nullptr;
  }
  const uint64_t hash =
      XXH3_64bits(ucode, size_t(dword_count) * sizeof(uint32_t));
  ShaderRegistry& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto found = registry.shaders.find(hash);
  Shader* shader = nullptr;
  if (found != registry.shaders.end()) {
    shader = found->second.get();
  } else {
    auto created = std::make_unique<DxbcShader>(type, hash, ucode, dword_count,
                                                std::endian::native);
    StringBuffer disasm;
    created->AnalyzeUcode(disasm);
    if (!created->is_ucode_analyzed()) {
      XELOGW("[xna] direct: shader {:016X} did not analyze", hash);
    }
    shader = created.get();
    registry.shaders.emplace(hash, std::move(created));
  }
  if (type == xenos::ShaderType::kVertex) {
    registry.active_vertex = shader;
  } else if (type == xenos::ShaderType::kPixel) {
    registry.active_pixel = shader;
  }
  return shader;
}

void XnaDirectSetActiveShaders(Shader* vertex_shader, Shader* pixel_shader) {
  ShaderRegistry& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.active_vertex = vertex_shader;
  registry.active_pixel = pixel_shader;
}

Shader* XnaDirectActiveShader(bool vertex) {
  ShaderRegistry& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  return vertex ? registry.active_vertex : registry.active_pixel;
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
