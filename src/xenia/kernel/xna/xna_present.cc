/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_present.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xna/xna_exports.h"
#include "xenia/ui/d3d12/d3d12_presenter.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/d3d12/d3d12_util.h"

#include "xenia/kernel/xna/shaders/xna_sprite_ps.h"
#include "xenia/kernel/xna/shaders/xna_sprite_vs.h"

namespace xe {
namespace kernel {
namespace xna {

namespace {

using Microsoft::WRL::ComPtr;

// The console's own mode, which is what the device reports to the title.
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;

// Sized for a frame rather than for a title: a SpriteBatch flush is a few
// hundred sprites at most, and a frame that wanted more than this would be
// drawing something other than 2D.
constexpr uint32_t kMaxSpritesPerFrame = 16384;
constexpr uint32_t kMaxTextures = 512;

// The layout measured off a real command buffer. See the header.
constexpr uint32_t kSpriteRecordBytes = 56;

struct Vertex {
  float x, y;
  float u, v;
  float r, g, b, a;
};

struct SpriteBatch {
  uint32_t texture = 0;
  uint32_t texture_width = 0;
  uint32_t texture_height = 0;
  uint32_t count = 0;
  std::vector<uint8_t> records;
};

std::mutex present_mutex;
float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
std::vector<SpriteBatch> queued_sprites;

bool background_wanted = false;

// The resolved texture, taken on the GPU thread at the moment the present
// wrote its descriptor. Texture fetch constant 0 shares registers with vertex
// fetch constants 0 and 1, so the next frame's first vertex stream overwrites
// it - asking for the swap texture from the sprite thread later reads a
// constant that by then describes a vertex buffer.
ID3D12Resource* background_resource_cached = nullptr;
D3D12_SHADER_RESOURCE_VIEW_DESC background_srv_cached = {};

struct Objects {
  bool tried = false;
  bool usable = false;
  const ui::d3d12::D3D12Provider* provider = nullptr;
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  uint64_t fence_value = 0;
  HANDLE fence_event = nullptr;

  // Drawn into, then copied to the presenter's guest output. A separate target
  // rather than drawing straight into the guest image, because that image is
  // only promised to support a UAV - not a render target - and a copy between
  // two resources of the same format costs far less than finding out the hard
  // way on some other driver.
  ComPtr<ID3D12Resource> target;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  ComPtr<ID3D12DescriptorHeap> srv_heap;
  uint32_t srv_stride = 0;

  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> pipeline;

  ComPtr<ID3D12Resource> vertex_buffer;
  Vertex* vertices = nullptr;

  // The resolved scene, re-uploaded every frame because it is a new image
  // every frame - unlike the title's textures, which are uploaded once.
  uint32_t background_slot = UINT32_MAX;

  // Uploaded once per handle. A title reuses its textures every frame, so
  // re-uploading them per frame would dominate everything else here.
  std::unordered_map<uint32_t, uint32_t> texture_slots;
  std::vector<ComPtr<ID3D12Resource>> texture_resources;
  std::vector<ComPtr<ID3D12Resource>> texture_uploads;
  // The version each uploaded slot was made from.
  std::vector<uint32_t> texture_versions;
};
Objects g;

// XNA SurfaceFormat to DXGI. The compressed formats map straight onto BC1-3,
// which is the whole reason a hosted title's textures can be used as they are.
DXGI_FORMAT ToDxgiFormat(uint32_t surface_format) {
  switch (surface_format) {
    case 1:
      return DXGI_FORMAT_B5G6R5_UNORM;
    case 2:
      return DXGI_FORMAT_B5G5R5A1_UNORM;
    case 3:
      return DXGI_FORMAT_B4G4R4A4_UNORM;
    case 4:
      return DXGI_FORMAT_BC1_UNORM;
    case 5:
      return DXGI_FORMAT_BC2_UNORM;
    case 6:
      return DXGI_FORMAT_BC3_UNORM;
    case 12:
      return DXGI_FORMAT_A8_UNORM;
    default:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

// How many bytes make up one value that was written as a single number on a
// big-endian console, and therefore have to be reversed here.
//
// THE COLOURS WERE NEVER A CHANNEL-ORDER PROBLEM. A Color pixel arrives as
// FF 70 B9 F7: read in that order it is opaque blue, which is what appeared on
// screen, and reversed it is F7 B9 70 FF - opaque orange, which is what the
// logo actually is. The console wrote a 32-bit colour big-endian, so the whole
// value is byte-reversed, and no choice of DXGI format expresses that. Swapping
// RGBA for BGRA only moved the wrong answer around: it turned purple into blue.
//
// The block-compressed formats have the same problem one level down - their
// colour endpoints and index words are 16-bit values written the same way.
uint32_t SwapUnitForFormat(uint32_t surface_format) {
  switch (surface_format) {
    case 0:   // Color
    case 9:   // Rgba1010102
    case 10:  // Rg32
    case 13:  // Single
    case 17:  // HalfVector2
      return 4;
    case 1:   // Bgr565
    case 2:   // Bgra5551
    case 3:   // Bgra4444
    case 7:   // NormalizedByte2
    case 16:  // HalfSingle
    case 4:   // Dxt1
    case 5:   // Dxt3
    case 6:   // Dxt5
    case 11:  // Rgba64
    case 14:  // Vector2
    case 18:  // HalfVector4
      return 2;
    case 12:  // Alpha8 - single bytes, nothing to reverse
      return 1;
    default:
      return 4;
  }
}

// Copies a row, undoing the console's byte order as it goes.
void CopySwapped(uint8_t* destination, const uint8_t* source, size_t bytes,
                 uint32_t unit) {
  if (unit <= 1) {
    std::memcpy(destination, source, bytes);
    return;
  }
  size_t at = 0;
  for (; at + unit <= bytes; at += unit) {
    for (uint32_t i = 0; i < unit; ++i) {
      destination[at + i] = source[at + unit - 1 - i];
    }
  }
  // A tail shorter than one value cannot be a value, so it is copied as it is
  // rather than reversed against nothing.
  if (at < bytes) {
    std::memcpy(destination + at, source + at, bytes - at);
  }
}

ui::d3d12::D3D12Presenter* GetPresenter() {
  auto* state = kernel_state();
  if (!state) {
    return nullptr;
  }
  auto* emulator = state->emulator();
  if (!emulator || !emulator->graphics_system()) {
    return nullptr;
  }
  return dynamic_cast<ui::d3d12::D3D12Presenter*>(
      emulator->graphics_system()->presenter());
}

bool CreatePipeline() {
  D3D12_DESCRIPTOR_RANGE srv_range = {};
  srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srv_range.NumDescriptors = 1;
  srv_range.BaseShaderRegister = 0;

  D3D12_ROOT_PARAMETER parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = 4;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  parameters[1].DescriptorTable.pDescriptorRanges = &srv_range;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
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
  root_desc.Flags =
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ID3D12RootSignature* root_signature =
      ui::d3d12::util::CreateRootSignature(*g.provider, root_desc);
  if (!root_signature) {
    XELOGE("[xna] could not create the sprite root signature");
    return false;
  }
  g.root_signature.Attach(root_signature);

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
  desc.pRootSignature = g.root_signature.Get();
  desc.VS.pShaderBytecode = kXnaSpriteVS;
  desc.VS.BytecodeLength = sizeof(kXnaSpriteVS);
  desc.PS.pShaderBytecode = kXnaSpritePS;
  desc.PS.BytecodeLength = sizeof(kXnaSpritePS);
  desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
  desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
  desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
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

  if (FAILED(g.device->CreateGraphicsPipelineState(
          &desc, IID_PPV_ARGS(&g.pipeline)))) {
    XELOGE("[xna] could not create the sprite pipeline state");
    return false;
  }
  return true;
}

// Reports once, either way. A title that shows nothing because the backend is
// not D3D12 should say so rather than leave a blank window unexplained.
bool EnsureObjects() {
  if (g.tried) {
    return g.usable;
  }
  g.tried = true;

  auto* state = kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  auto* graphics = emulator ? emulator->graphics_system() : nullptr;
  g.provider =
      graphics ? dynamic_cast<ui::d3d12::D3D12Provider*>(graphics->provider())
               : nullptr;
  if (!g.provider || !GetPresenter()) {
    XELOGW(
        "[xna] the host graphics backend is not D3D12, so a hosted title's "
        "output cannot be presented yet");
    return false;
  }
  g.device = g.provider->GetDevice();
  g.queue = g.provider->GetDirectQueue();
  if (!g.device || !g.queue) {
    return false;
  }

  if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&g.allocator))) ||
      FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         g.allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&g.list))) ||
      FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&g.fence)))) {
    XELOGE("[xna] could not create the D3D12 objects a frame needs");
    return false;
  }
  g.list->Close();
  g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!g.fence_event) {
    return false;
  }

  D3D12_RESOURCE_DESC target_desc = {};
  target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  target_desc.Width = kOutputWidth;
  target_desc.Height = kOutputHeight;
  target_desc.DepthOrArraySize = 1;
  target_desc.MipLevels = 1;
  target_desc.Format = ui::d3d12::D3D12Presenter::kGuestOutputFormat;
  target_desc.SampleDesc.Count = 1;
  target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear_value = {};
  clear_value.Format = target_desc.Format;
  if (FAILED(g.device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
          IID_PPV_ARGS(&g.target)))) {
    XELOGE("[xna] could not create the render target for hosted output");
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = 1;
  if (FAILED(g.device->CreateDescriptorHeap(&rtv_desc,
                                            IID_PPV_ARGS(&g.rtv_heap)))) {
    return false;
  }
  g.device->CreateRenderTargetView(
      g.target.Get(), nullptr,
      g.rtv_heap->GetCPUDescriptorHandleForHeapStart());

  D3D12_DESCRIPTOR_HEAP_DESC srv_desc = {};
  srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srv_desc.NumDescriptors = kMaxTextures;
  srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(g.device->CreateDescriptorHeap(&srv_desc,
                                            IID_PPV_ARGS(&g.srv_heap)))) {
    return false;
  }
  g.srv_stride = g.device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  D3D12_RESOURCE_DESC vertex_desc = {};
  vertex_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  vertex_desc.Width = sizeof(Vertex) * 6 * kMaxSpritesPerFrame;
  vertex_desc.Height = 1;
  vertex_desc.DepthOrArraySize = 1;
  vertex_desc.MipLevels = 1;
  vertex_desc.Format = DXGI_FORMAT_UNKNOWN;
  vertex_desc.SampleDesc.Count = 1;
  vertex_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(g.device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE,
          &vertex_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&g.vertex_buffer)))) {
    return false;
  }
  D3D12_RANGE no_read = {0, 0};
  if (FAILED(g.vertex_buffer->Map(0, &no_read,
                                  reinterpret_cast<void**>(&g.vertices)))) {
    return false;
  }

  if (!CreatePipeline()) {
    return false;
  }

  g.usable = true;
  XELOGI("[xna] hosted output goes to the screen through the D3D12 presenter");
  return true;
}

// Uploads a texture the first time it is drawn with and returns its descriptor
// slot, or UINT32_MAX. Must be called with the command list open.
// Binds the command processor's own resolved texture - no upload, no copy.
// It hands it over in NON_PIXEL_SHADER_RESOURCE and expects it back that way,
// and both paths submit to the same direct queue, so ordering needs no fence.
uint32_t EnsureBackground(ID3D12Resource** bound) {
  *bound = nullptr;
  if (!background_wanted || !background_resource_cached) {
    XELOGD("[xna] composite skipped: wanted {}, resolved texture {}",
           background_wanted, background_resource_cached ? "yes" : "no");
    return UINT32_MAX;
  }
  ID3D12Resource* resource = background_resource_cached;
  const D3D12_SHADER_RESOURCE_VIEW_DESC srv = background_srv_cached;
  if (g.background_slot == UINT32_MAX) {
    g.background_slot = kMaxTextures - 1;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE handle =
      g.srv_heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(g.background_slot) * g.srv_stride;
  g.device->CreateShaderResourceView(resource, &srv, handle);

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  g.list->ResourceBarrier(1, &barrier);
  *bound = resource;
  return g.background_slot;
}

uint32_t EnsureTexture(uint32_t handle) {
  XnaTextureView view;
  if (!XnaLookupTexture(handle, 0, &view) || !view.width || !view.height) {
    return UINT32_MAX;
  }
  auto found = g.texture_slots.find(handle);
  if (found != g.texture_slots.end()) {
    // Already uploaded - but only reusable if the title has not written to it
    // since. Keeping the first upload forever is right for content that never
    // changes and wrong for everything else, and a texture bound before it was
    // filled would otherwise stay blank for the life of the title.
    if (g.texture_versions[found->second] == view.version) {
      return found->second;
    }
    g.texture_slots.erase(found);
  }
  const uint32_t slot = static_cast<uint32_t>(g.texture_resources.size());
  if (slot >= kMaxTextures) {
    return UINT32_MAX;
  }

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = view.width;
  desc.Height = view.height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = ToDxgiFormat(view.format);
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12Resource> resource;
  if (FAILED(g.device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)))) {
    return UINT32_MAX;
  }

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes,
                                  &total);

  D3D12_RESOURCE_DESC upload_desc = {};
  upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  upload_desc.Width = total;
  upload_desc.Height = 1;
  upload_desc.DepthOrArraySize = 1;
  upload_desc.MipLevels = 1;
  upload_desc.Format = DXGI_FORMAT_UNKNOWN;
  upload_desc.SampleDesc.Count = 1;
  upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> upload;
  if (FAILED(g.device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE,
          &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&upload)))) {
    return UINT32_MAX;
  }
  uint8_t* mapped = nullptr;
  D3D12_RANGE no_read = {0, 0};
  if (FAILED(upload->Map(0, &no_read, reinterpret_cast<void**>(&mapped)))) {
    return UINT32_MAX;
  }
  // Row by row: the source is packed and D3D12 wants each row aligned, so the
  // two pitches only match by accident.
  const uint64_t source_pitch = view.size / (rows ? rows : 1);
  const uint32_t swap_unit = SwapUnitForFormat(view.format);
  for (UINT row = 0; row < rows; ++row) {
    const uint64_t copy = std::min<uint64_t>(row_bytes, source_pitch);
    CopySwapped(mapped + footprint.Offset + row * footprint.Footprint.RowPitch,
                view.data + row * source_pitch, static_cast<size_t>(copy),
                swap_unit);
  }
  upload->Unmap(0, nullptr);

  D3D12_TEXTURE_COPY_LOCATION destination = {};
  destination.pResource = resource.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  destination.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = upload.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint = footprint;
  g.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  g.list->ResourceBarrier(1, &barrier);

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = desc.Format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE handle_cpu =
      g.srv_heap->GetCPUDescriptorHandleForHeapStart();
  handle_cpu.ptr += static_cast<SIZE_T>(slot) * g.srv_stride;
  g.device->CreateShaderResourceView(resource.Get(), &srv, handle_cpu);

  // WHAT WAS ACTUALLY UPLOADED. A logo that is invisible on a black screen is
  // either tinted to nothing or made of nothing, and those are different bugs
  // in different places. A texture whose pixels are entirely zero never had its
  // data reach us; one with real content did, and the fault is downstream.
  if (slot < 6) {
    uint64_t sum = 0;
    uint32_t non_zero = 0;
    for (uint32_t i = 0; i < view.size; ++i) {
      sum += view.data[i];
      non_zero += view.data[i] != 0 ? 1 : 0;
    }
    // SAMPLED FROM THE MIDDLE, NOT THE CORNER. A logo's corner is transparent
    // background, and "E9 00 00 00" there reads equally well as a red with no
    // alpha or an alpha over black - which is exactly the ambiguity that made
    // the last guess a coin toss. The centre row is where the artwork is, and a
    // pixel with three different non-zero channels settles the order outright.
    //
    // The two candidates differ predictably: if the bytes are A,R,G,B then the
    // first byte of an opaque pixel is FF and the colour follows; if they are
    // R,G,B,A then the LAST byte is FF.
    std::string first;
    const uint32_t centre_row = view.height / 2;
    const uint32_t bytes_per_row = view.size / (view.height ? view.height : 1);
    const uint32_t centre = centre_row * bytes_per_row + bytes_per_row / 2;
    for (uint32_t i = 0; i < 24 && centre + i < view.size; ++i) {
      char byte[4];
      std::snprintf(byte, sizeof(byte), "%02X ", view.data[centre + i]);
      first += byte;
    }
    XELOGI(
        "[xna] texture {} -> slot {}: {}x{} format {} ({}), {} byte(s), {} "
        "non-zero, checksum {}, centre pixels {}",
        handle, slot, view.width, view.height, view.format,
        static_cast<uint32_t>(desc.Format), view.size, non_zero, sum, first);
  }

  g.texture_resources.push_back(resource);
  g.texture_uploads.push_back(upload);
  g.texture_versions.push_back(view.version);
  g.texture_slots[handle] = slot;
  return slot;
}

}  // namespace

uint32_t XnaTextureSwapUnit(uint32_t surface_format) {
  return SwapUnitForFormat(surface_format);
}

void XnaCopyTextureRow(uint8_t* destination, const uint8_t* source,
                       size_t bytes, uint32_t unit) {
  CopySwapped(destination, source, bytes, unit);
}

void XnaSetClearColor(const float rgba[4]) {
  if (!rgba) {
    return;
  }
  std::lock_guard<std::mutex> lock(present_mutex);
  std::copy(rgba, rgba + 4, clear_color);
}

void XnaQueueSprites(uint32_t texture_handle, uint32_t texture_width,
                     uint32_t texture_height, uint32_t count,
                     const uint8_t* records, uint32_t bytes) {
  if (!records || !count || bytes < count * kSpriteRecordBytes) {
    return;
  }
  std::lock_guard<std::mutex> lock(present_mutex);
  SpriteBatch batch;
  batch.texture = texture_handle;
  batch.texture_width = texture_width;
  batch.texture_height = texture_height;
  batch.count = count;
  batch.records.assign(records, records + count * kSpriteRecordBytes);
  queued_sprites.push_back(std::move(batch));
}

// Called on the GPU thread, immediately after the present writes texture fetch
// constant 0 - which is the only moment that constant still describes the front
// buffer. The resource is resolved here rather than when the sprites are drawn.
void XnaSetBackgroundFromSwapTexture() {
  auto* state = kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  auto* graphics = emulator ? emulator->graphics_system() : nullptr;
  auto* processor = graphics
                        ? dynamic_cast<xe::gpu::d3d12::D3D12CommandProcessor*>(
                              graphics->command_processor())
                        : nullptr;
  if (!processor) {
    return;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  ID3D12Resource* resource = processor->HostedRequestSwapTexture(srv);
  if (!resource) {
    return;
  }
  std::lock_guard<std::mutex> lock(present_mutex);
  background_resource_cached = resource;
  background_srv_cached = srv;
  background_wanted = true;
}

void XnaPresentFrame() {
  std::lock_guard<std::mutex> lock(present_mutex);
  std::vector<SpriteBatch> batches;
  batches.swap(queued_sprites);

  if (!EnsureObjects()) {
    return;
  }
  auto* presenter = GetPresenter();
  if (!presenter) {
    return;
  }
  float color[4];
  std::copy(clear_color, clear_color + 4, color);

  const bool refreshed = presenter->RefreshGuestOutput(
      kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight,
      [&](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& d3d_context = static_cast<
            ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(
            context);
        ID3D12Resource* guest_output = d3d_context.resource_uav_capable();
        if (!guest_output) {
          return false;
        }
        if (FAILED(g.allocator->Reset()) ||
            FAILED(g.list->Reset(g.allocator.Get(), nullptr))) {
          return false;
        }

        // Textures first, while nothing is bound: uploading records a copy and
        // a barrier, which cannot happen inside a render pass.
        std::vector<uint32_t> slots(batches.size(), UINT32_MAX);
        for (size_t i = 0; i < batches.size(); ++i) {
          slots[i] = EnsureTexture(batches[i].texture);
        }
        ID3D12Resource* background_resource = nullptr;
        const uint32_t background_slot = EnsureBackground(&background_resource);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g.list->ClearRenderTargetView(rtv, color, 0, nullptr);

        D3D12_VIEWPORT viewport = {};
        viewport.Width = float(kOutputWidth);
        viewport.Height = float(kOutputHeight);
        viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor = {0, 0, LONG(kOutputWidth), LONG(kOutputHeight)};
        g.list->RSSetViewports(1, &viewport);
        g.list->RSSetScissorRects(1, &scissor);

        g.list->SetGraphicsRootSignature(g.root_signature.Get());
        g.list->SetPipelineState(g.pipeline.Get());
        ID3D12DescriptorHeap* heaps[] = {g.srv_heap.Get()};
        g.list->SetDescriptorHeaps(1, heaps);
        const float constants[4] = {1.0f / float(kOutputWidth),
                                    1.0f / float(kOutputHeight), 0.0f, 0.0f};
        g.list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        uint32_t vertex_count = 0;
        if (background_slot != UINT32_MAX) {
          const Vertex quad[6] = {
              {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
              {float(kOutputWidth), 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
              {0.0f, float(kOutputHeight), 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
              {float(kOutputWidth), 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
              {float(kOutputWidth), float(kOutputHeight), 1.0f, 1.0f, 1.0f,
               1.0f, 1.0f, 1.0f},
              {0.0f, float(kOutputHeight), 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
          };
          std::memcpy(g.vertices, quad, sizeof(quad));
          D3D12_GPU_DESCRIPTOR_HANDLE table =
              g.srv_heap->GetGPUDescriptorHandleForHeapStart();
          table.ptr += UINT64(background_slot) * g.srv_stride;
          g.list->SetGraphicsRootDescriptorTable(1, table);
          D3D12_VERTEX_BUFFER_VIEW view = {};
          view.BufferLocation = g.vertex_buffer->GetGPUVirtualAddress();
          view.SizeInBytes = sizeof(quad);
          view.StrideInBytes = sizeof(Vertex);
          g.list->IASetVertexBuffers(0, 1, &view);
          g.list->DrawInstanced(6, 1, 0, 0);
          vertex_count = 6;
        }
        for (size_t i = 0; i < batches.size(); ++i) {
          const SpriteBatch& batch = batches[i];
          if (slots[i] == UINT32_MAX) {
            continue;
          }
          const float texture_width =
              float(batch.texture_width ? batch.texture_width : 1);
          const float texture_height =
              float(batch.texture_height ? batch.texture_height : 1);
          const uint32_t first_vertex = vertex_count;
          for (uint32_t s = 0; s < batch.count; ++s) {
            if (vertex_count + 6 > 6 * kMaxSpritesPerFrame) {
              break;
            }
            const uint8_t* record =
                batch.records.data() + size_t(s) * kSpriteRecordBytes;
            float f[8];
            std::memcpy(f, record, sizeof(f));
            uint32_t packed = 0;
            std::memcpy(&packed, record + 52, sizeof(packed));
            // XNA packs a Color as R in the low byte through A in the high one.
            const float cr = float(packed & 0xFF) / 255.0f;
            const float cg = float((packed >> 8) & 0xFF) / 255.0f;
            const float cb = float((packed >> 16) & 0xFF) / 255.0f;
            const float ca = float((packed >> 24) & 0xFF) / 255.0f;
            const float u0 = f[0] / texture_width;
            const float v0 = f[1] / texture_height;
            const float u1 = (f[0] + f[2]) / texture_width;
            const float v1 = (f[1] + f[3]) / texture_height;
            const float x0 = f[4], y0 = f[5];
            const float x1 = f[4] + f[6], y1 = f[5] + f[7];

            // THE RECORD, AS IT ACTUALLY ARRIVED. Sprites are reaching the
            // renderer with textures bound and still nothing is visible, so
            // either they are tinted to nothing or they are being placed
            // somewhere that is not on screen. Both are wrong readings of the
            // same 56 bytes, and printing a few of them says which - the
            // remaining five words are printed too, in case the colour is not
            // where it was assumed to be.
            static uint32_t records_reported = 0;
            if (records_reported < 8) {
              ++records_reported;
              uint32_t rest[5];
              std::memcpy(rest, record + 32, sizeof(rest));
              XELOGI(
                  "[xna] sprite {}: src ({:.0f},{:.0f} {:.0f}x{:.0f}) dest "
                  "({:.0f},{:.0f} {:.0f}x{:.0f}) colour {:08X} rest "
                  "{:08X} {:08X} {:08X} {:08X} {:08X} texture {}x{}",
                  records_reported, f[0], f[1], f[2], f[3], f[4], f[5], f[6],
                  f[7], packed, rest[0], rest[1], rest[2], rest[3], rest[4],
                  batch.texture_width, batch.texture_height);
            }
            const Vertex quad[6] = {
                {x0, y0, u0, v0, cr, cg, cb, ca},
                {x1, y0, u1, v0, cr, cg, cb, ca},
                {x0, y1, u0, v1, cr, cg, cb, ca},
                {x1, y0, u1, v0, cr, cg, cb, ca},
                {x1, y1, u1, v1, cr, cg, cb, ca},
                {x0, y1, u0, v1, cr, cg, cb, ca},
            };
            std::memcpy(g.vertices + vertex_count, quad, sizeof(quad));
            vertex_count += 6;
          }
          if (vertex_count == first_vertex) {
            continue;
          }
          D3D12_GPU_DESCRIPTOR_HANDLE table =
              g.srv_heap->GetGPUDescriptorHandleForHeapStart();
          table.ptr += UINT64(slots[i]) * g.srv_stride;
          g.list->SetGraphicsRootDescriptorTable(1, table);
          D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
          vertex_view.BufferLocation = g.vertex_buffer->GetGPUVirtualAddress() +
                                       UINT64(first_vertex) * sizeof(Vertex);
          vertex_view.SizeInBytes =
              UINT((vertex_count - first_vertex) * sizeof(Vertex));
          vertex_view.StrideInBytes = sizeof(Vertex);
          g.list->IASetVertexBuffers(0, 1, &vertex_view);
          g.list->DrawInstanced(vertex_count - first_vertex, 1, 0, 0);
        }

        // Both images end the callback in the state they started in, which is
        // what the presenter promises its next user.
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = g.target.Get();
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
        g.list->ResourceBarrier(2, barriers);

        g.list->CopyResource(guest_output, g.target.Get());

        std::swap(barriers[0].Transition.StateBefore,
                  barriers[0].Transition.StateAfter);
        std::swap(barriers[1].Transition.StateBefore,
                  barriers[1].Transition.StateAfter);
        g.list->ResourceBarrier(2, barriers);

        // WHAT THE FRAME ACTUALLY CONTAINED. A white window is either the
        // clear colour or a full-screen white sprite covering everything, and
        // those need different fixes - so the first few frames say which,
        // rather than leaving it to be guessed from the picture.
        // The FIRST frames are the least informative ones - a title presents
        // before it has loaded anything, so they are empty by definition and
        // reporting them says nothing about whether drawing works. The first
        // frame is reported to prove the path runs at all, and after that only
        // frames that actually carried sprites.
        static uint32_t frames_seen = 0;
        static uint32_t frames_with_sprites = 0;
        ++frames_seen;
        const bool worth_reporting =
            frames_seen == 1 || (!batches.empty() && frames_with_sprites < 3);
        if (!batches.empty()) {
          ++frames_with_sprites;
        }
        if (worth_reporting) {
          uint32_t resolved = 0;
          for (uint32_t slot : slots) {
            resolved += slot != UINT32_MAX ? 1 : 0;
          }
          XELOGI(
              "[xna] frame {}: clear ({:.2f} {:.2f} {:.2f} {:.2f}), {} sprite "
              "batch(es), {} with a texture, {} vertices",
              frames_seen, color[0], color[1], color[2], color[3],
              batches.size(), resolved, vertex_count);
        }

        if (background_resource) {
          D3D12_RESOURCE_BARRIER back = {};
          back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          back.Transition.pResource = background_resource;
          back.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          back.Transition.StateBefore =
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
          back.Transition.StateAfter =
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
          g.list->ResourceBarrier(1, &back);
        }
        if (FAILED(g.list->Close())) {
          return false;
        }
        ID3D12CommandList* lists[] = {g.list.Get()};
        g.queue->ExecuteCommandLists(1, lists);

        // Waited on before returning: the presenter may consume the image as
        // soon as the callback ends, and the allocator is reset at the top of
        // the next frame.
        const uint64_t value = ++g.fence_value;
        if (FAILED(g.queue->Signal(g.fence.Get(), value))) {
          return false;
        }
        if (g.fence->GetCompletedValue() < value) {
          if (FAILED(g.fence->SetEventOnCompletion(value, g.fence_event))) {
            return false;
          }
          WaitForSingleObject(g.fence_event, INFINITE);
        }
        return true;
      });

  // Reported once. A window showing something other than what was just drawn -
  // white, when the frame cleared to black - means the image never reached the
  // presenter, and that is a different problem from drawing the wrong thing.
  static bool reported_refresh = false;
  if (!reported_refresh) {
    reported_refresh = true;
    XELOGI("[xna] the presenter {} the first hosted frame",
           refreshed ? "accepted" : "REFUSED");
  }
}

void XnaShutdownPresent() {
  std::lock_guard<std::mutex> lock(present_mutex);
  if (g.fence_event) {
    CloseHandle(g.fence_event);
    g.fence_event = nullptr;
  }
  g.texture_slots.clear();
  g.texture_resources.clear();
  g.texture_uploads.clear();
  g.texture_versions.clear();
  g.pipeline.Reset();
  g.root_signature.Reset();
  g.vertex_buffer.Reset();
  g.srv_heap.Reset();
  g.rtv_heap.Reset();
  g.target.Reset();
  g.list.Reset();
  g.allocator.Reset();
  g.fence.Reset();
  g.usable = false;
  g.tried = false;
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
