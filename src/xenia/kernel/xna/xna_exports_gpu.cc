/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The console's graphics resources: textures and the four immutable state
// objects. XNA creates these once and then refers to them by handle inside the
// command packets it hands to D3D_Device_ReceivePackets, so nothing here draws
// anything - it is the object table the packet stream indexes into.
//
// Every signature and every return value below was read out of the console
// assemblies with tools/xna/dump-il.ps1 rather than guessed. Two rules came out
// of that, and they are not interchangeable:
//
//   * Create* returns a HANDLE, and the caller compares it against 0xFFFFFFFF
//     to raise OutOfMemoryException. Any other value is accepted.
//   * CopyData returns an ERROR CODE through
//     GraphicsHelpers::ThrowExceptionFromResult, where anything but 0 throws.
//
// Returning the wrong kind of value passes silently in one direction and kills
// the title in the other.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xna/xna_exports.h"
#include "xenia/kernel/xna/xna_effect.h"
#include "xenia/kernel/xna/xna_direct.h"
#include "xenia/kernel/xna/xna_gpu.h"
#include "xenia/kernel/xna/xna_guest_heap.h"
#include "xenia/kernel/xna/xna_packets.h"
#include "xenia/kernel/xna/xna_present.h"
#include "xenia/kernel/xna/xna_runtimehost.h"

#include <cmath>

extern "C" void Nexia_XnaDrawUserPrimitives(int32_t primitive_type,
                                            int32_t primitive_count,
                                            const void* vertex_data,
                                            uint32_t vertex_data_size,
                                            uint32_t vertex_stride);

DEFINE_bool(
    xna_geometry, true,
    "Route a hosted XNA title's 3D draws through the command processor. "
    "Off falls back to the sprite-only 2D path.",
    "XNA");

namespace {

// TEXTURE_CREATION_PARAMS, as declared in MXF.Graphics.dlx: six sequential
// 32-bit fields. XNA passes it BY VALUE, and the x64 convention passes a
// 24-byte struct by hidden reference, so it arrives here as a pointer.
struct TextureCreationParams {
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  // 0 means "a full mip chain, you pick the count" - the constructor writes
  // `mipMap ? 0 : 1` - and the count actually created goes back out through
  // the out parameter, where it becomes Texture.LevelCount.
  uint32_t levels;
  uint32_t format;
  uint32_t is_video;
};

// TEXTURE_COPYDATA_INFO: four sequential 32-bit fields, also by value and so
// also by reference at 16 bytes.
struct TextureCopyDataInfo {
  uint32_t level;
  uint32_t data_size;
  uint32_t element_size;
  uint32_t element_count;
};

// TEXTURECUBE_COPYDATA_INFO, from MXF.Graphics.dlx. Same shape as the 2D form
// with the FACE wedged into the middle - so the fields cannot simply be reused
// positionally, which is why it is written out rather than aliased.
struct TextureCubeCopyDataInfo {
  uint32_t level;
  uint32_t data_size;
  uint32_t face;  // CubeMapFace: +X, -X, +Y, -Y, +Z, -Z
  uint32_t element_size;
  uint32_t element_count;
};

// Microsoft.Xna.Framework.Rectangle - four ints. This one is a real pointer,
// and null means the whole level.
struct Rect {
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
};

struct TextureLevel {
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t bytes_per_pixel = 0;
};

struct Texture {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  uint32_t levels = 0;
  uint32_t format = 0;
  bool is_video = false;
  // Matches Microsoft.Xna.Framework.Graphics.TextureType: 0 Texture2D,
  // 1 Texture3D, 2 TextureCube. The readback path switches on it to decide
  // which managed wrapper to build.
  uint32_t type = 0;
  // The handle owns the pixels, in guest memory: the texture cache fetches
  // from there, so a host-side copy would only have to be pushed back across.
  uint32_t handle = 0;
  std::string name;
  std::vector<TextureLevel> level_data;
  // Bumped by every write. The presenter uploads a texture once and then keeps
  // it, which is right for content that never changes and WRONG for everything
  // else - a texture bound before it was filled, or refilled per frame, would
  // stay whatever it was the first time anyone drew with it. This is how the
  // upload knows its copy is stale.
  uint32_t version = 0;
  uint32_t uploads = 0;
  uint32_t discards = 0;
  uint32_t binds = 0;
  uint32_t resolves = 0;
};

std::mutex resource_mutex;
std::unordered_map<uint32_t, Texture> textures;

// What a sampler and the texture it samples have in common once the naming
// decoration is gone: "_DiffuseMapTexture" and "DiffuseMapSampler" both reduce
// to "DiffuseMap", "_SceneDepthMap" and "SceneDepthSampler" to "SceneDepth".
// ONE suffix is removed, first match wins - stripping more would take
// "DiffuseMapSampler" down to "Diffuse" and stop it matching.
std::string TextureNameStem(const std::string& name) {
  std::string stem = name;
  if (!stem.empty() && stem.front() == '_') {
    stem.erase(0, 1);
  }
  for (const char* suffix : {"Sampler", "Texture", "Map"}) {
    const size_t length = std::strlen(suffix);
    if (stem.size() > length &&
        stem.compare(stem.size() - length, length, suffix) == 0) {
      stem.resize(stem.size() - length);
      break;
    }
  }
  return stem;
}

// A handle is a GUEST ADDRESS. Not a counter, not a host pointer.
//
// The console runtime declares every one of these as System.UInt32 -
// GraphicsDevice::pComPtr, GraphicsResource::pComPtr - because on hardware it
// is a 32-bit pointer into the console address space. Honouring that is what
// lets a resource be drawn: the payload can then live in guest memory, where
// the command processor, the texture cache and the vertex fetch already look,
// instead of in a host container that would have to be copied across on every
// draw.
//
// THE TOP BYTE IS NOT OURS. Every command packet is a 32-bit word built as
// `(packetType << 24) | pComPtr`, and AddSetStreamSource does NOT mask the
// handle first - so anything in the top byte becomes the packet type. A
// handle is therefore a 24-bit index into a table of descriptors in guest
// memory, and the descriptor carries the payload address. See
// xna_guest_heap.h.
uint32_t AllocateHandle(uint32_t type = xe::kernel::xna::XnaGuestResource::kNone) {
  const uint32_t handle = xe::kernel::xna::XnaGuestResourceCreate(type);
  if (!handle) {
    // Every caller compares against this and raises OutOfMemoryException, which
    // is the truth: guest memory for the resource could not be had.
    return UINT32_MAX;
  }
  return handle;
}

// How a SurfaceFormat is laid out in memory. Uncompressed formats are one
// pixel per block; the DXT formats pack a 4x4 block into 8 or 16 bytes.
//
// This is read from the format the texture was CREATED with, never inferred by
// dividing the byte count by the pixel count. That inference is what refused
// every compressed texture: DXT1 is half a byte per pixel, so the division has
// no whole answer and the copy was rejected as "no whole bytes per pixel" -
// which killed Arcadecraft the moment it loaded its first DXT asset.
struct SurfaceLayout {
  uint32_t block_width;
  uint32_t block_height;
  uint32_t bytes_per_block;
};

SurfaceLayout FormatLayout(uint32_t format) {
  switch (format) {
    case 4:  return {4, 4, 8};    // Dxt1
    case 5:  return {4, 4, 16};   // Dxt3
    case 6:  return {4, 4, 16};   // Dxt5
    case 12: return {1, 1, 1};    // Alpha8
    case 1:                       // Bgr565
    case 2:                       // Bgra5551
    case 3:                       // Bgra4444
    case 7:                       // NormalizedByte2
    case 16: return {1, 1, 2};    // HalfSingle
    case 11: return {1, 1, 8};    // Rgba64
    case 14: return {1, 1, 8};    // Vector2
    case 18: return {1, 1, 8};    // HalfVector4
    case 19: return {1, 1, 8};    // HdrBlendable
    case 15: return {1, 1, 16};   // Vector4
    default: return {1, 1, 4};    // Color, Rgba1010102, Rg32, Single, ...
  }
}

// One mip level. Dimensions halve and never go below one.
uint32_t LevelSize(uint32_t base, uint32_t level) {
  return std::max<uint32_t>(1, base >> level);
}

// A full chain runs until both dimensions reach one.
uint32_t FullMipCount(uint32_t width, uint32_t height) {
  uint32_t count = 1;
  while (width > 1 || height > 1) {
    width = std::max<uint32_t>(1, width >> 1);
    height = std::max<uint32_t>(1, height >> 1);
    ++count;
  }
  return count;
}

constexpr uint32_t kInvalidArg = 0x80070057;   // E_INVALIDARG
constexpr uint32_t kNotImplemented = 0x80004001;  // E_NOTIMPL

}  // namespace

// ---- state objects ----------------------------------------------------------
//
// BlendState, DepthStencilState, RasterizerState and SamplerState are immutable
// descriptions the title builds once and then binds by handle. The settings
// block is deliberately not read here: the packet stream names the handle, so
// the state has to be resolved when the packets are translated, not now.
// Handing back a live handle is what lets the title get that far.
//
// Each caller does `pComPtr = Create...(device, ref settings)` and throws
// OutOfMemoryException if the result is 0xFFFFFFFF.

// BlendState/Settings, in declaration order (all int32): ColorSourceBlend,
// ColorDestinationBlend, ColorBlendFunction, AlphaSourceBlend,
// AlphaDestinationBlend, AlphaBlendFunction, then four ColorWriteChannels, a
// packed Color BlendFactor and MultiSampleMask. Only the six factor/function
// fields are kept - the default Opaque state reads One/Zero/Add, which is what
// the draw path assumed unconditionally before. XNA Blend: 0 One, 1 Zero, 2
// SourceColor ... ; XNA BlendFunction: 0 Add, 1 Subtract, 2 ReverseSubtract, 3
// Min, 4 Max.
struct BlendSettings {
  uint32_t color_src = 0;
  uint32_t color_dst = 1;
  uint32_t color_op = 0;
  uint32_t alpha_src = 0;
  uint32_t alpha_dst = 1;
  uint32_t alpha_op = 0;
  uint32_t color_write[4] = {0xF, 0xF, 0xF, 0xF};
};

std::map<uint32_t, BlendSettings> blend_states;

extern "C" uint32_t xna_D3D_D3D_Device_CreateBlendState(uint32_t device,
                                                        const void* settings) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  const uint32_t handle = AllocateHandle();
  BlendSettings state = {};
  if (settings) {
    const auto* words = static_cast<const int32_t*>(settings);
    state.color_src = static_cast<uint32_t>(words[0]);
    state.color_dst = static_cast<uint32_t>(words[1]);
    state.color_op = static_cast<uint32_t>(words[2]);
    state.alpha_src = static_cast<uint32_t>(words[3]);
    state.alpha_dst = static_cast<uint32_t>(words[4]);
    state.alpha_op = static_cast<uint32_t>(words[5]);
    for (uint32_t i = 0; i < 4; ++i) {
      state.color_write[i] = static_cast<uint32_t>(words[6 + i]) & 0xF;
    }
  }
  blend_states[handle] = state;
  XELOGD(
      "[xna] blend state {:08X}: colour src {} dst {} op {}, alpha src {} dst "
      "{} op {}, write {:X} {:X} {:X} {:X}",
      handle, state.color_src, state.color_dst, state.color_op, state.alpha_src,
      state.alpha_dst, state.alpha_op, state.color_write[0],
      state.color_write[1], state.color_write[2], state.color_write[3]);
  return handle;
}

// DepthStencilState/Settings, in declaration order: sixteen int32 fields
// beginning DepthBufferEnable, DepthBufferWriteEnable, DepthBufferFunction.
struct DepthStencilSettings {
  int32_t depth_enable;
  int32_t depth_write_enable;
  int32_t depth_function;
};

std::map<uint32_t, DepthStencilSettings> depth_states;

extern "C" uint32_t xna_D3D_D3D_Device_CreateDepthStencilState(
    uint32_t device, const void* settings) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  const uint32_t handle = AllocateHandle();
  DepthStencilSettings state = {};
  if (settings) {
    const auto* words = static_cast<const int32_t*>(settings);
    state.depth_enable = words[0];
    state.depth_write_enable = words[1];
    state.depth_function = words[2];
  }
  depth_states[handle] = state;
  XELOGD("[xna] depth stencil state {:08X}: enable {} write {} func {}", handle,
         state.depth_enable, state.depth_write_enable, state.depth_function);
  return handle;
}

extern "C" uint32_t xna_D3D_D3D_Device_CreateRasterizerState(
    uint32_t device, const void* settings) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  return AllocateHandle();
}

// The decoded SamplerState.Settings a handle stands for. The struct on the wire
// is { TextureFilter Filter; TextureAddressMode AddressU, V, W; int
// MaxAnisotropy; int MaxMipLevel; float LODBias; } - all 4 bytes, in that order.
struct SamplerParams {
  uint32_t address_u = 2;
  uint32_t address_v = 2;
  uint32_t mag_filter = 0;
  uint32_t min_filter = 0;
  uint32_t mip_filter = 0;
  uint32_t aniso = 0;
};

static std::mutex sampler_params_mutex;
static std::map<uint32_t, SamplerParams> sampler_params;

// XNA TextureAddressMode -> Xenos ClampMode. Wrap 0 -> kRepeat 0, Clamp 1 ->
// kClampToEdge 2, Mirror 2 -> kMirroredRepeat 1.
static uint32_t XnaAddressToXenos(uint32_t mode) {
  switch (mode) {
    case 0: return 0;  // Wrap -> kRepeat
    case 1: return 2;  // Clamp -> kClampToEdge
    case 2: return 1;  // Mirror -> kMirroredRepeat
    default: return 2;
  }
}

extern "C" uint32_t xna_D3D_D3D_Device_CreateSamplerState(
    uint32_t device, const void* settings) {
  SamplerParams params;
  if (settings) {
    const auto* s = static_cast<const uint32_t*>(settings);
    const uint32_t filter = s[0];
    params.address_u = XnaAddressToXenos(s[1]);
    params.address_v = XnaAddressToXenos(s[2]);
    // XNA TextureFilter: 0 Linear, 1 Point, 2 Anisotropic, then the mixed
    // min/mag/mip permutations 3..8. Xenos wants each of the three filters, and
    // its point/linear are 0/1. Only the three the title actually uses appear,
    // so the permutations are folded onto the nearest whole point/linear rather
    // than decoded bit by bit.
    const bool linear = filter != 1;  // anything but Point has linear somewhere
    params.mag_filter = linear ? 1 : 0;
    params.min_filter = linear ? 1 : 0;
    params.mip_filter = (filter == 1 || filter == 4 || filter == 6 ||
                         filter == 8)
                            ? 0
                            : 1;
    params.aniso = filter == 2 ? 4 : 0;  // kMax_8_1 when anisotropic
  }
  std::lock_guard<std::mutex> lock(resource_mutex);
  const uint32_t handle = AllocateHandle();
  if (handle != UINT32_MAX) {
    std::lock_guard<std::mutex> plock(sampler_params_mutex);
    sampler_params[handle] = params;
  }
  return handle;
}

// Lays every level (and cube face) out back to back in ONE guest allocation and
// records where each one starts. From here on nothing copies the pixels again:
// the presenter and the texture cache are both handed a pointer into this.
// `resolve_target` pads the allocation out to what a RESOLVE will write, which
// is not what the image measures - see the tail of this function.
static bool AllocateTextureStorage(uint32_t handle, Texture& texture,
                                   uint32_t faces,
                                   bool resolve_target = false) {
  const SurfaceLayout layout = FormatLayout(texture.format);
  const auto blocks = [](uint32_t pixels, uint32_t size) {
    return (pixels + size - 1) / size;
  };
  uint32_t offset = 0;
  for (uint32_t face = 0; face < faces; ++face) {
    for (uint32_t mip = 0; mip < texture.levels; ++mip) {
      const uint32_t width = LevelSize(texture.width, mip);
      const uint32_t height = LevelSize(texture.height, mip);
      const uint32_t bytes = blocks(width, layout.block_width) *
                             blocks(height, layout.block_height) *
                             layout.bytes_per_block;
      const uint32_t slot = face * texture.levels + mip;
      if (slot >= texture.level_data.size()) {
        return false;
      }
      texture.level_data[slot].offset = offset;
      texture.level_data[slot].size = bytes;
      texture.level_data[slot].bytes_per_pixel = layout.bytes_per_block;
      offset += bytes;
    }
  }
  texture.handle = handle;

  // BIG ENOUGH FOR WHAT THE RESOLVE WRITES, NOT FOR WHAT THE IMAGE MEASURES.
  //
  // A resolve destination is filled with its pitch and height rounded up to
  // the GPU's tile size, so a 200x120 target is written as though it were
  // 256x128 - 131072 bytes against the 96000 the image needs. The surplus
  // lands in whatever the guest heap handed out next, and what it landed on
  // was a resource DESCRIPTOR: a vertex declaration's size field came back as
  // 0xB01050FF, which is a clear colour, so the declaration reported zero
  // elements, the shader's vfetch was left blank, no position was produced and
  // every draw was refused. The front buffer already learned this; a render
  // target is the same case.
  if (resolve_target) {
    const uint32_t padded = xe::round_up(texture.width, 128u) *
                            xe::round_up(texture.height, 128u) *
                            layout.bytes_per_block;
    if (padded > offset) {
      offset = padded;
    }
  }
  return offset != 0 && xe::kernel::xna::XnaGuestResourceResize(handle, offset);
}

// ---- textures ---------------------------------------------------------------

extern "C" uint32_t xna_D3D_D3D_Texture2D_CreateHandle(
    uint32_t device, const TextureCreationParams* params,
    uint32_t* out_levels) {
  if (!params) {
    return UINT32_MAX;
  }

  Texture texture;
  texture.width = params->width;
  texture.height = params->height;
  texture.depth = params->depth;
  texture.format = params->format;
  texture.is_video = params->is_video != 0;
  texture.levels = params->levels != 0
                       ? params->levels
                       : FullMipCount(params->width, params->height);
  texture.level_data.resize(texture.levels);
  const uint32_t levels = texture.levels;

  std::lock_guard<std::mutex> lock(resource_mutex);
  const uint32_t handle = AllocateHandle(
      xe::kernel::xna::XnaGuestResource::kTexture);
  if (handle == UINT32_MAX) {
    return handle;
  }
  textures[handle] = std::move(texture);
  if (!AllocateTextureStorage(handle, textures[handle], 1)) {
    textures.erase(handle);
    xe::kernel::xna::XnaGuestResourceDestroy(handle);
    return UINT32_MAX;
  }

  // Becomes Texture.LevelCount, so it has to be the count actually created and
  // not the zero that asked for a full chain.
  if (out_levels) {
    *out_levels = levels;
  }
  return handle;
}

extern "C" void xna_D3D_D3D_Texture2D_ReleaseHandle(uint32_t device,
                                                    uint32_t texture) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  textures.erase(texture);
}

// SetData and GetData both land here, and `read` says which way the copy runs -
// NOT which way it is set. The IL names it outright:
//
//     IL_0007: ldarg.s read
//     call InteropCopyData(..., Rectangle*, System.Byte)
//
// so a ZERO means write and a one means read back. Reading it as "is set" ran
// every SetData backwards: the empty texture was copied OUT over the title's
// own pixel buffer, which both left every texture blank and destroyed the data
// on the way past. Every texture that reached the renderer was zero, and the
// logos drew as blank quads on a black screen. The same byte, with the same
// name, ends VertexBuffer, IndexBuffer and TextureCube - so all four were
// inverted, and no vertex or index buffer was ever filled either.
// The result goes through ThrowExceptionFromResult, so anything but 0 becomes
// an exception inside the title.
//
// The copy is laid out from the format the texture was CREATED with, via
// FormatLayout. Deriving bytes per pixel from the byte count divided by the
// rectangle area looked like it avoided a format table, but it cannot express
// a compressed format at all - DXT1 is half a byte per pixel - so every DXT
// asset was refused, which is what stopped Arcadecraft loading content.
// Shared by Texture2D and TextureCube, because the only thing that differs
// between them is which slot of level_data a (face, level) pair names. The
// block arithmetic below is the part worth having once.
//
// `slot` indexes level_data; `mip` is the mip level the dimensions come from -
// for a cube they are not the same number, since six faces share one chain.

static uint32_t GuestSwapWidth(uint32_t reported_element_size,
                               uint32_t field_width) {
  return reported_element_size <= 1 ? 1 : field_width;
}

static uint32_t GuestSwapWidthForFormat(uint32_t xna_surface_format) {
  switch (xe::kernel::xna::XnaGpuTextureEndianFor(
      xe::kernel::xna::XnaGpuTextureFormatFor(xna_surface_format))) {
    case xe::gpu::xenos::Endian::kNone:
      return 1;
    case xe::gpu::xenos::Endian::k8in16:
      return 2;
    default:
      return 4;
  }
}

static void CopyGuest(void* dest, const void* src, uint32_t bytes,
               uint32_t element_size) {
  switch (element_size) {
    case 2:
      xe::copy_and_swap(static_cast<uint16_t*>(dest),
                        static_cast<const uint16_t*>(src), bytes / 2);
      return;
    case 4:
      xe::copy_and_swap(static_cast<uint32_t*>(dest),
                        static_cast<const uint32_t*>(src), bytes / 4);
      return;
    case 8:
      xe::copy_and_swap(static_cast<uint64_t*>(dest),
                        static_cast<const uint64_t*>(src), bytes / 8);
      return;
    default:
      std::memcpy(dest, src, bytes);
      return;
  }
}

static uint32_t CopyTextureLevel(Texture& texture, uint32_t slot, uint32_t mip,
                                 void* data, uint32_t data_size,
                                 uint32_t element_size, const Rect* rect,
                                 uint8_t read) {
  if (slot >= texture.level_data.size()) {
    XELOGW(
        "[xna] texture {:08X} discarded a copy: level slot {} but only {} "
        "level(s) exist ({}x{} format {}, {} mip(s))",
        texture.handle, slot, texture.level_data.size(), texture.width,
        texture.height, texture.format, texture.levels);
    ++texture.discards;
    return kInvalidArg;
  }
  TextureLevel& level = texture.level_data[slot];
  auto* base = xe::kernel::xna::XnaGuestResourceData(texture.handle);
  if (!base) {
    XELOGW(
        "[xna] texture {:08X} discarded a copy: {}x{} format {} has no guest "
        "payload",
        texture.handle, texture.width, texture.height, texture.format);
    ++texture.discards;
    return kInvalidArg;
  }

  const uint32_t level_width = LevelSize(texture.width, mip);
  const uint32_t level_height = LevelSize(texture.height, mip);
  const SurfaceLayout layout = FormatLayout(texture.format);

  const uint32_t copy_width =
      rect ? static_cast<uint32_t>(rect->width) : level_width;
  const uint32_t copy_height =
      rect ? static_cast<uint32_t>(rect->height) : level_height;
  const uint32_t left = rect ? static_cast<uint32_t>(rect->x) : 0;
  const uint32_t top = rect ? static_cast<uint32_t>(rect->y) : 0;
  if (!copy_width || !copy_height ||
      left + copy_width > level_width || top + copy_height > level_height) {
    ++texture.discards;
    return kInvalidArg;
  }
  // A compressed level is addressed in whole blocks, so a rectangle that starts
  // or ends inside one cannot be expressed. Refusing is right: writing the
  // nearest block would corrupt the pixels around it.
  if (layout.block_width > 1 &&
      ((left % layout.block_width) || (top % layout.block_height) ||
       (copy_width % layout.block_width &&
        left + copy_width != level_width) ||
       (copy_height % layout.block_height &&
        top + copy_height != level_height))) {
    xe::kernel::xna::XnaExportUnimplemented(
        "D3D!D3D_Texture2D_CopyData with a rectangle that does not fall on "
        "compressed block boundaries");
    ++texture.discards;
    return kNotImplemented;
  }

  // Rounded up, because a level narrower than a block still costs a whole one -
  // the 1x1 tail of a mip chain is a full 4x4 block on disk.
  const auto blocks = [](uint32_t pixels, uint32_t size) {
    return (pixels + size - 1) / size;
  };
  const uint32_t level_blocks_wide = blocks(level_width, layout.block_width);
  const uint32_t level_blocks_high = blocks(level_height, layout.block_height);
  const uint32_t copy_blocks_wide = blocks(copy_width, layout.block_width);
  const uint32_t copy_blocks_high = blocks(copy_height, layout.block_height);

  const size_t level_bytes = static_cast<size_t>(level_blocks_wide) *
                             level_blocks_high * layout.bytes_per_block;
  if (level.size != level_bytes) {
    XELOGW(
        "[xna] texture {:08X} discarded a copy: level {} was allocated {} "
        "bytes but {}x{} of format {} is {} bytes ({}x{} blocks of {})",
        texture.handle, slot, level.size, level_width, level_height,
        texture.format, level_bytes, level_blocks_wide, level_blocks_high,
        layout.bytes_per_block);
    ++texture.discards;
    return kInvalidArg;
  }
  level.bytes_per_pixel = layout.bytes_per_block;
  const size_t needed = static_cast<size_t>(copy_blocks_wide) *
                        copy_blocks_high * layout.bytes_per_block;
  if (data_size < needed) {
    XELOGW(
        "[xna] Texture2D_CopyData was given {} bytes for a {}x{} region of "
        "format {}, which needs {}",
        data_size, copy_width, copy_height, texture.format, needed);
    ++texture.discards;
    return kInvalidArg;
  }

  const uint32_t row_bytes = copy_blocks_wide * layout.bytes_per_block;
  const uint32_t level_pitch = level_blocks_wide * layout.bytes_per_block;
  const uint32_t left_blocks = left / layout.block_width;
  const uint32_t top_blocks = top / layout.block_height;
  auto* caller = static_cast<uint8_t*>(data);

  // READING A RESOLVED RENDER TARGET NEEDS THE CONTRACT SPRUNG FIRST.
  //
  // A resolve does not write guest memory - it holds the pixels in a GPU
  // readback buffer and arms a one-shot watch on the destination, and the
  // bytes only materialise when a GUEST access trips it. The copy below is a
  // host memcpy, which is not one, so a render target read back this way
  // returns zeroes however well the resolve worked. That is what made every
  // GetData in the probe report 00000000.
  if (read) {
    auto* resource =
        xe::kernel::xna::XnaGuestResourceLookup(texture.handle);
    if (resource && resource->data) {
      xe::kernel::xna::XnaGuestRangeRead(
          resource->data + static_cast<uint32_t>(level.offset),
          static_cast<uint32_t>(level.size));
    }
  }
  for (uint32_t row = 0; row < copy_blocks_high; ++row) {
    uint8_t* stored = base + level.offset +
                      static_cast<size_t>(top_blocks + row) * level_pitch +
                      static_cast<size_t>(left_blocks) * layout.bytes_per_block;
    uint8_t* theirs = caller + static_cast<size_t>(row) * row_bytes;
    const uint32_t width =
        GuestSwapWidth(element_size, GuestSwapWidthForFormat(texture.format));
    if (read) {
      CopyGuest(theirs, stored, row_bytes, width);
    } else {
      CopyGuest(stored, theirs, row_bytes, width);
    }
  }
  if (!read) {
    ++texture.version;
    ++texture.uploads;
  }
  return 0;
}

extern "C" uint32_t xna_D3D_D3D_Texture2D_CopyData(
    uint32_t device, uint32_t handle, void* data,
    const TextureCopyDataInfo* info, const Rect* rect, uint8_t read) {
  if (!data || !info) {
    return kInvalidArg;
  }
  uint32_t written_address = 0;
  uint32_t written_bytes = 0;
  uint32_t result = kInvalidArg;
  uint32_t texture_format = 0;
  {
    std::lock_guard<std::mutex> lock(resource_mutex);
    auto found_locked = textures.find(handle);
    if (found_locked == textures.end()) {
      xe::kernel::xna::XnaExportUnimplemented(
          "D3D!D3D_Texture2D_CopyData on an unknown texture");
      return kInvalidArg;
    }
    texture_format = found_locked->second.format;
    result = CopyTextureLevel(found_locked->second, info->level, info->level,
                              data, info->data_size, info->element_size, rect,
                              read);
    if (!read && !result) {
      auto* descriptor = xe::kernel::xna::XnaGuestResourceLookup(
          found_locked->second.handle);
      const uint32_t slot = info->level;
      if (descriptor && slot < found_locked->second.level_data.size()) {
        written_address =
            descriptor->data +
            static_cast<uint32_t>(found_locked->second.level_data[slot].offset);
        written_bytes = static_cast<uint32_t>(
            found_locked->second.level_data[slot].size);
      }
    }
  }
  // Outside the mutex - the invalidation callbacks take the global critical
  // region and reach into the GPU, and holding resource_mutex across that
  // inverts the lock order and hangs the emulator.
  if (written_bytes) {
    xe::kernel::xna::XnaGuestRangeWritten(written_address, written_bytes);
  }
  // DID THE PIXELS ARRIVE, AND FOR WHICH TEXTURE. Every texture reaching the
  // renderer is entirely zero while the sprites that use them are correct, so
  // the data is being lost between SetData and the upload. Whether it was ever
  // handed over decides which half to look in, and the handle says whether the
  // title even fills the textures it later binds.
  // The non-zero scan walks the whole upload, so it is worth doing only when
  // the line it feeds will actually be written.
  if (xe::logging::ShouldLog(xe::LogLevel::Debug)) {
    uint32_t non_zero = 0;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < info->data_size; ++i) {
      non_zero += bytes[i] != 0 ? 1 : 0;
    }
    XELOGD(
        "[xna] Texture2D_CopyData texture {} level {}: {} byte(s) in, {} "
        "non-zero, element size {} -> swap width {}, read {}, rect {}, "
        "result {:08X}",
        handle, info->level, info->data_size, non_zero, info->element_size,
        GuestSwapWidth(info->element_size,
                       GuestSwapWidthForFormat(texture_format)),
        read ? 1 : 0, rect ? "yes" : "no", result);
  }
  return result;
}

// ---- cube maps ---------------------------------------------------------------
//
// A cube is six faces sharing one mip chain, so it is stored as a Texture whose
// level_data holds `levels * 6` slots and a (face, level) pair names one of
// them. Nothing else about it differs, which is why the copy itself is shared.
//
// These were stubs until now, and Arcadecraft's SunBurn renderer loads an
// environment map: the create returned E_NOTIMPL, the title kept the null it
// got back, and the NullReferenceException finally surfaced at shutdown in
// UnloadGlobalAssets - a long way from the call that actually failed.

extern "C" uint32_t xna_D3D_D3D_TextureCube_CreateHandle(
    uint32_t device, const TextureCreationParams* params,
    uint32_t* out_levels) {
  if (!params) {
    return UINT32_MAX;
  }
  Texture texture;
  // A cube face is square, and the creation params carry the edge length.
  texture.width = params->width;
  texture.height = params->width;
  texture.depth = 1;
  texture.type = 2;
  texture.format = params->format;
  texture.is_video = params->is_video != 0;
  texture.levels = params->levels != 0
                       ? params->levels
                       : FullMipCount(params->width, params->width);
  texture.level_data.resize(static_cast<size_t>(texture.levels) * 6);
  const uint32_t levels = texture.levels;

  std::lock_guard<std::mutex> lock(resource_mutex);
  const uint32_t handle = AllocateHandle(
      xe::kernel::xna::XnaGuestResource::kTexture);
  if (handle == UINT32_MAX) {
    return handle;
  }
  textures[handle] = std::move(texture);
  if (!AllocateTextureStorage(handle, textures[handle], 6)) {
    textures.erase(handle);
    xe::kernel::xna::XnaGuestResourceDestroy(handle);
    return UINT32_MAX;
  }

  // Becomes TextureCube.LevelCount - the count per face, not the total.
  if (out_levels) {
    *out_levels = levels;
  }
  return handle;
}

extern "C" void xna_D3D_D3D_TextureCube_ReleaseHandle(uint32_t device,
                                                      uint32_t texture) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  textures.erase(texture);
}

extern "C" uint32_t xna_D3D_D3D_TextureCube_CopyData(
    uint32_t device, uint32_t handle, void* data,
    const TextureCubeCopyDataInfo* info, const Rect* rect, uint8_t read) {
  if (!data || !info) {
    return kInvalidArg;
  }
  if (info->face >= 6) {
    return kInvalidArg;
  }
  std::lock_guard<std::mutex> lock(resource_mutex);
  auto found = textures.find(handle);
  if (found == textures.end()) {
    xe::kernel::xna::XnaExportUnimplemented(
        "D3D!D3D_TextureCube_CopyData on an unknown texture");
    return kInvalidArg;
  }
  Texture& texture = found->second;
  // Faces are laid out one whole chain after another, so a face's level 0 is at
  // face * levels.
  const uint32_t slot = info->face * texture.levels + info->level;
  return CopyTextureLevel(texture, slot, info->level, data, info->data_size,
                          info->element_size, rect, read);
}

namespace {

struct Texture3DCopyDataInfo {
  void* data;
  uint32_t level;
  uint32_t data_size;
  uint32_t element_size;
  uint32_t element_count;
  uint32_t left;
  uint32_t top;
  uint32_t right;
  uint32_t bottom;
  uint32_t front;
  uint32_t back;
};

}  // namespace

extern "C" uint32_t xna_D3D_D3D_Texture3D_CreateHandle(
    uint32_t device, const TextureCreationParams* params,
    uint32_t* out_levels) {
  if (!params || !params->width || !params->height || !params->depth) {
    return UINT32_MAX;
  }
  Texture texture;
  texture.width = params->width;
  texture.height = params->height;
  texture.depth = params->depth;
  texture.type = 1;
  texture.format = params->format;
  texture.is_video = params->is_video != 0;
  texture.levels =
      params->levels != 0
          ? params->levels
          : FullMipCount(std::max(params->width, params->depth),
                         std::max(params->height, params->depth));
  texture.level_data.resize(texture.levels);
  const SurfaceLayout layout = FormatLayout(texture.format);
  uint32_t offset = 0;
  for (uint32_t mip = 0; mip < texture.levels; ++mip) {
    const uint32_t blocks_wide =
        (LevelSize(texture.width, mip) + layout.block_width - 1) /
        layout.block_width;
    const uint32_t blocks_high =
        (LevelSize(texture.height, mip) + layout.block_height - 1) /
        layout.block_height;
    const uint32_t bytes = blocks_wide * blocks_high * layout.bytes_per_block *
                           LevelSize(texture.depth, mip);
    texture.level_data[mip].offset = offset;
    texture.level_data[mip].size = bytes;
    texture.level_data[mip].bytes_per_pixel = layout.bytes_per_block;
    offset += bytes;
  }
  const uint32_t levels = texture.levels;

  std::lock_guard<std::mutex> lock(resource_mutex);
  const uint32_t handle = AllocateHandle(
      xe::kernel::xna::XnaGuestResource::kTexture);
  if (handle == UINT32_MAX) {
    return handle;
  }
  texture.handle = handle;
  textures[handle] = std::move(texture);
  if (!offset || !xe::kernel::xna::XnaGuestResourceResize(handle, offset)) {
    textures.erase(handle);
    xe::kernel::xna::XnaGuestResourceDestroy(handle);
    return UINT32_MAX;
  }
  if (out_levels) {
    *out_levels = levels;
  }
  return handle;
}

extern "C" void xna_D3D_D3D_Texture3D_ReleaseHandle(uint32_t device,
                                                    uint32_t texture) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  textures.erase(texture);
}

extern "C" uint32_t xna_D3D_D3D_Texture3D_CopyData(
    uint32_t device, uint32_t handle, const Texture3DCopyDataInfo* info,
    uint8_t read) {
  if (!info || !info->data) {
    return kInvalidArg;
  }
  uint32_t written_address = 0;
  uint32_t written_bytes = 0;
  {
    std::lock_guard<std::mutex> lock(resource_mutex);
    auto found = textures.find(handle);
    if (found == textures.end()) {
      xe::kernel::xna::XnaExportUnimplemented(
          "D3D!D3D_Texture3D_CopyData on an unknown texture");
      return kInvalidArg;
    }
    Texture& texture = found->second;
    if (info->level >= texture.level_data.size()) {
      ++texture.discards;
      return kInvalidArg;
    }
    TextureLevel& level = texture.level_data[info->level];
    auto* base = xe::kernel::xna::XnaGuestResourceData(texture.handle);
    if (!base) {
      ++texture.discards;
      return kInvalidArg;
    }
    const uint32_t level_width = LevelSize(texture.width, info->level);
    const uint32_t level_height = LevelSize(texture.height, info->level);
    const uint32_t level_depth = LevelSize(texture.depth, info->level);
    if (info->left >= info->right || info->top >= info->bottom ||
        info->front >= info->back || info->right > level_width ||
        info->bottom > level_height || info->back > level_depth) {
      XELOGW(
          "[xna] Texture3D_CopyData texture {:08X} level {}: box {},{},{} - "
          "{},{},{} is outside {}x{}x{}",
          handle, info->level, info->left, info->top, info->front, info->right,
          info->bottom, info->back, level_width, level_height, level_depth);
      ++texture.discards;
      return kInvalidArg;
    }
    const SurfaceLayout layout = FormatLayout(texture.format);
    const uint32_t box_width = info->right - info->left;
    const uint32_t box_height = info->bottom - info->top;
    if (layout.block_width > 1 &&
        ((info->left % layout.block_width) ||
         (info->top % layout.block_height) ||
         (box_width % layout.block_width && info->right != level_width) ||
         (box_height % layout.block_height && info->bottom != level_height))) {
      xe::kernel::xna::XnaExportUnimplemented(
          "D3D!D3D_Texture3D_CopyData with a box that does not fall on "
          "compressed block boundaries");
      ++texture.discards;
      return kNotImplemented;
    }
    const auto blocks = [](uint32_t pixels, uint32_t size) {
      return (pixels + size - 1) / size;
    };
    const uint32_t level_pitch =
        blocks(level_width, layout.block_width) * layout.bytes_per_block;
    const uint32_t slice_bytes =
        level_pitch * blocks(level_height, layout.block_height);
    const uint32_t row_bytes =
        blocks(box_width, layout.block_width) * layout.bytes_per_block;
    const uint32_t rows = blocks(box_height, layout.block_height);
    const uint32_t slices = info->back - info->front;
    const size_t needed = static_cast<size_t>(row_bytes) * rows * slices;
    if (info->data_size < needed ||
        static_cast<size_t>(slice_bytes) * level_depth > level.size) {
      XELOGW(
          "[xna] Texture3D_CopyData texture {:08X} level {}: given {} bytes, "
          "box needs {}, level holds {} of {}",
          handle, info->level, info->data_size, needed, level.size,
          static_cast<size_t>(slice_bytes) * level_depth);
      ++texture.discards;
      return kInvalidArg;
    }
    if (read) {
      auto* resource = xe::kernel::xna::XnaGuestResourceLookup(texture.handle);
      if (resource && resource->data) {
        xe::kernel::xna::XnaGuestRangeRead(
            resource->data + static_cast<uint32_t>(level.offset),
            static_cast<uint32_t>(level.size));
      }
    }
    const uint32_t swap = GuestSwapWidth(
        info->element_size, GuestSwapWidthForFormat(texture.format));
    auto* caller = static_cast<uint8_t*>(info->data);
    const uint32_t left_blocks = info->left / layout.block_width;
    const uint32_t top_blocks = info->top / layout.block_height;
    for (uint32_t slice = 0; slice < slices; ++slice) {
      for (uint32_t row = 0; row < rows; ++row) {
        uint8_t* stored =
            base + level.offset +
            static_cast<size_t>(info->front + slice) * slice_bytes +
            static_cast<size_t>(top_blocks + row) * level_pitch +
            static_cast<size_t>(left_blocks) * layout.bytes_per_block;
        uint8_t* theirs =
            caller + (static_cast<size_t>(slice) * rows + row) * row_bytes;
        if (read) {
          CopyGuest(theirs, stored, row_bytes, swap);
        } else {
          CopyGuest(stored, theirs, row_bytes, swap);
        }
      }
    }
    if (!read) {
      ++texture.version;
      ++texture.uploads;
      auto* descriptor =
          xe::kernel::xna::XnaGuestResourceLookup(texture.handle);
      if (descriptor) {
        written_address =
            descriptor->data + static_cast<uint32_t>(level.offset);
        written_bytes = static_cast<uint32_t>(level.size);
      }
    }
  }
  if (written_bytes) {
    xe::kernel::xna::XnaGuestRangeWritten(written_address, written_bytes);
  }
  return 0;
}

// ---- buffers, declarations, render targets, queries --------------------------
//
// Implemented as a batch rather than one per run: each of these is the next
// wall behind the last, and tools/xna/classify-returns.ps1 settles the one
// question that matters for each - whether the caller treats the result as a
// handle it compares against 0xFFFFFFFF, or as an error code it hands to
// ThrowExceptionFromResult. Guessing is what makes these expensive; the
// classifier makes it mechanical.
//
// Contents are stored the same way textures are, so the packet stream has real
// bytes to bind once it is translated.

namespace {

// Buffers hold nothing here. The bytes live in guest memory, reached through
// the descriptor the handle names, so a draw can point the GPU straight at them
// instead of staging a copy per draw.
uint32_t CreateBuffer(uint32_t type, uint32_t stride, uint32_t byte_size,
                      uint32_t usage) {
  const uint32_t handle = AllocateHandle(type);
  if (handle == UINT32_MAX) {
    return handle;
  }
  auto* resource = xe::kernel::xna::XnaGuestResourceLookup(handle);
  if (!resource) {
    return UINT32_MAX;
  }
  resource->stride = stride;
  resource->usage = usage;
  if (byte_size && !xe::kernel::xna::XnaGuestResourceResize(handle, byte_size)) {
    xe::kernel::xna::XnaGuestResourceDestroy(handle);
    return UINT32_MAX;
  }
  XELOGD("[xna] buffer: type {} stride {} size {} -> handle {:08X}", type,
         stride, byte_size, handle);
  return handle;
}

uint32_t BufferCopyLocked(uint32_t handle, void* data, uint32_t offset,
                          uint32_t bytes, uint32_t element_size, uint8_t read,
                          uint32_t* written_address, uint32_t* written_bytes);

// Both buffer kinds copy the same way: a byte offset, a length, a direction.
uint32_t BufferCopy(uint32_t handle, void* data, uint32_t offset,
                    uint32_t bytes, uint32_t element_size, uint8_t read) {
  if (!data) {
    return kInvalidArg;
  }
  // Announced AFTER the lock is dropped. TriggerPhysicalMemoryCallbacks takes
  // the global critical region and runs the GPU's own invalidation callbacks;
  // doing that while holding resource_mutex puts the two locks in the opposite
  // order to every other path and hangs the emulator outright.
  uint32_t written_address = 0;
  uint32_t written_bytes = 0;
  const uint32_t result =
      BufferCopyLocked(handle, data, offset, bytes, element_size, read,
                       &written_address, &written_bytes);
  if (written_bytes) {
    xe::kernel::xna::XnaGuestRangeWritten(written_address, written_bytes);
  }
  return result;
}

uint32_t BufferCopyLocked(uint32_t handle, void* data, uint32_t offset,
                          uint32_t bytes, uint32_t element_size, uint8_t read,
                          uint32_t* written_address, uint32_t* written_bytes) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  auto* resource = xe::kernel::xna::XnaGuestResourceLookup(handle);
  if (!resource) {
    return kInvalidArg;
  }
  // A buffer created with a size of zero is sized by its first write: XNA
  // computes the byte count from the element count and the element size, and
  // those two do not always reach the creation call together.
  const uint32_t needed = offset + bytes;
  if (resource->size < needed) {
    if (read) {
      return kInvalidArg;
    }
    if (!xe::kernel::xna::XnaGuestResourceResize(handle, needed)) {
      return kInvalidArg;
    }
  }
  auto* storage = xe::kernel::xna::XnaGuestResourceData(handle);
  if (!storage) {
    return kInvalidArg;
  }
  if (read) {
    CopyGuest(data, storage + offset, bytes, element_size);
  } else {
    CopyGuest(storage + offset, data, bytes, element_size);
    // The GPU learns a range is dirty from the invalidation callbacks, and a
    // write from here raises none - see XnaGuestRangeWritten. Recorded, not
    // raised: the caller does it once this mutex is gone.
    *written_address = resource->data + offset;
    *written_bytes = bytes;
  }
  return 0;
}

}  // namespace

// InteropCreateVertexBuffer(device, byteSize, usage) - HANDLE.
extern "C" uint32_t xna_D3D_D3D_VertexBuffer_CreateHandle(uint32_t device,
                                                          uint32_t byte_size,
                                                          int32_t usage) {
  return CreateBuffer(xe::kernel::xna::XnaGuestResource::kVertexBuffer, 0,
                      byte_size, static_cast<uint32_t>(usage));
}

extern "C" void xna_D3D_D3D_VertexBuffer_ReleaseHandle(uint32_t device,
                                                       uint32_t handle) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  xe::kernel::xna::XnaGuestResourceDestroy(handle);
}

// InteropCopyData(device, handle, data, offsetInBytes, info, isSet) - ERROR.
// VERTEX_COPYDATA_INFO is four sequential 32-bit fields: stride, element count,
// element size, options. The byte count is the count times the size; the stride
// only differs from the element size for an interleaved copy, which no path
// reached so far performs - so that case is refused rather than written wrong.
extern "C" uint32_t xna_D3D_D3D_VertexBuffer_CopyData(
    uint32_t device, uint32_t handle, void* data, uint32_t offset,
    const uint32_t* info, uint8_t read) {
  if (!info) {
    return kInvalidArg;
  }
  const uint32_t stride = info[0];
  const uint32_t element_count = info[1];
  const uint32_t element_size = info[2];
  if (stride != 0 && element_size != 0 && stride != element_size) {
    xe::kernel::xna::XnaExportUnimplemented(
        "D3D!D3D_VertexBuffer_CopyData with a stride that is not the element "
        "size");
    return kNotImplemented;
  }
  return BufferCopy(handle, data, offset, element_count * element_size,
                    GuestSwapWidth(element_size, 4), read);
}

// InteropCreateIndexBuffer(device, byteSize, sixteenBit, isDynamic) - HANDLE.
//
// The third argument is the index width, not the usage. IndexBuffer::CreateBuffer
// compares its element size against 2 and pushes 1 for a sixteen bit buffer,
// then pushes isDynamic last. Reading those two the other way round made every
// static buffer - which is all of them - report 32 bit indices, halving the
// index count the buffer was thought to hold and dropping any draw that started
// past it.
extern "C" uint32_t xna_D3D_D3D_IndexBuffer_CreateHandle(uint32_t device,
                                                         uint32_t byte_size,
                                                         int32_t sixteen_bit,
                                                         int32_t is_dynamic) {
  return CreateBuffer(xe::kernel::xna::XnaGuestResource::kIndexBuffer,
                      sixteen_bit ? 2 : 4, byte_size,
                      static_cast<uint32_t>(is_dynamic));
}

extern "C" void xna_D3D_D3D_IndexBuffer_ReleaseHandle(uint32_t device,
                                                      uint32_t handle) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  xe::kernel::xna::XnaGuestResourceDestroy(handle);
}

// InteropCopyData(device, handle, data, byteCount, offsetInBytes, options,
// isSet) - ERROR.
//
// NOT (offset, elementCount, elementSize). IndexBuffer::SetData<T> pushes the
// return of ValidateParameters<T> - which is sizeof(T) * elementCount, a BYTE
// COUNT - then offsetInBytes, then SetDataOptions, then the direction. Read as
// an element count and an element size, the byte count became the destination
// offset and the size came from SetDataOptions, which is zero for every
// non-dynamic buffer: `count * size` was zero and EVERY index buffer copy
// wrote nothing.
//
// The whole title then drew from index buffers full of zeros - every triangle
// (v0, v0, v0), degenerate, invisible, while the draw counters reported
// hundreds of healthy draws a frame.
extern "C" uint32_t xna_D3D_D3D_IndexBuffer_CopyData(
    uint32_t device, uint32_t handle, void* data, uint32_t bytes,
    uint32_t offset, uint32_t options, uint8_t read) {
  uint32_t element_size = 1;
  {
    std::lock_guard<std::mutex> lock(resource_mutex);
    auto* resource = xe::kernel::xna::XnaGuestResourceLookup(handle);
    if (resource && resource->element_size) {
      element_size = resource->element_size;
    }
  }
  return BufferCopy(handle, data, offset, bytes, element_size, read);
}

extern "C" void Nexia_XnaIndexElementSize(uint32_t handle,
                                          uint32_t element_size) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  auto* resource = xe::kernel::xna::XnaGuestResourceLookup(handle);
  if (resource) {
    resource->element_size = element_size;
  }
}

// THE VERTEX DECLARATION, AS THE MANAGED SIDE HANDED IT OVER.
//
// UnsafeNativeStructures.VERTEX_ELEMENT: five Int32s, SequentialLayout, default
// packing, and both enums are Int32-backed - so twenty bytes with no padding on
// the 32-bit console and on this host alike. DeclarationManager
// ::CreateNativeDeclaration fills it in this order, one run covering every
// bound stream, on a little-endian host.
struct XnaVertexElement {
  uint32_t stream;
  uint32_t offset;
  uint32_t format;      // XNA VertexElementFormat
  uint32_t usage;       // XNA VertexElementUsage
  uint32_t usage_index;
};
static_assert(sizeof(XnaVertexElement) == 20,
              "VERTEX_ELEMENT is five Int32s; anything else means the console "
              "struct was read wrong and every element after the first is "
              "taken from the wrong offset");

// InteropCreateHandle(device, elements, count) - HANDLE.
//
// THE THIRD ARGUMENT IS A COUNT, NOT A SIZE. DeclarationManager
// ::CreateNativeDeclaration sums the element counts of every bound declaration
// into one local and pushes THAT alongside the pointer, so seven elements
// arrive as "7". Taking it for a byte count allocated seven bytes and kept the
// first seven bytes of the first element, which is not even one whole element -
// and every reader since has seen a declaration with nothing in it.
extern "C" uint32_t xna_D3D_D3D_Decl_CreateHandle(uint32_t device,
                                                  const void* blob,
                                                  uint32_t count) {
  std::unique_lock<std::mutex> lock(resource_mutex);
  const uint32_t size = count * uint32_t(sizeof(XnaVertexElement));
  const uint32_t handle =
      CreateBuffer(xe::kernel::xna::XnaGuestResource::kDeclaration, 0, size, 0);
  if (handle == UINT32_MAX) {
    return handle;
  }
  uint32_t written_address = 0;
  if (blob && size) {
    auto* storage = xe::kernel::xna::XnaGuestResourceData(handle);
    auto* resource = xe::kernel::xna::XnaGuestResourceLookup(handle);
    if (storage) {
      CopyGuest(storage, blob, size, uint32_t(sizeof(uint32_t)));
      // Recorded, raised after this function drops resource_mutex - the
      // invalidation callbacks take the global critical region and reach into
      // the GPU, and holding both inverts the lock order.
      written_address = resource ? resource->data : 0;
      // Read back from the STORAGE, not from the caller's buffer. Logging the
      // source only ever proved the source was right, which is not the
      // question - a declaration that reads back as zeroes at bind time gives
      // every fetch usage 0, format 0, and a shader that fetches one float.
      const auto* written = reinterpret_cast<const xe::be<uint32_t>*>(storage);
      XELOGD(
          "[xna] declaration {:08X}: {} element(s), {} bytes at guest {:08X}",
          handle, count, size, resource ? resource->data : 0);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = i * 5;
        XELOGD(
            "[xna]    element {}: stream {} offset {} format {} usage {} "
            "index {}",
            i, uint32_t(written[at + 0]), uint32_t(written[at + 1]),
            uint32_t(written[at + 2]), uint32_t(written[at + 3]),
            uint32_t(written[at + 4]));
      }
    } else {
      XELOGW(
          "[xna] declaration {:08X}: {} element(s) had nowhere to be stored",
          handle, count);
    }
  }
  lock.unlock();
  if (written_address) {
    xe::kernel::xna::XnaGuestRangeWritten(written_address, size);
  }
  return handle;
}

extern "C" void xna_D3D_D3D_Decl_ReleaseHandle(uint32_t device,
                                               uint32_t handle) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  xe::kernel::xna::XnaGuestResourceDestroy(handle);
}

// InteropCreateRenderTarget(device, params, flags, out a, out b) - HANDLE.
// D3D_RENDERTARGET_CREATION_PARAMS is width, height, levels, format,
// depthFormat, msType, preserve. A render target is a texture plus a depth
// surface, and the two out parameters are the surface handles XNA keeps
// alongside the returned one.
extern "C" uint32_t xna_D3D_D3D_Device_CreateRenderTarget(
    uint32_t device, const uint32_t* params, uint32_t flags, uint32_t* out_a,
    uint32_t* out_b) {
  Texture texture;
  if (params) {
    texture.width = params[0];
    texture.height = params[1];
    texture.levels = params[2] != 0 ? params[2] : 1;
    texture.format = params[3];
  } else {
    texture.levels = 1;
  }
  texture.level_data.resize(texture.levels);

  std::lock_guard<std::mutex> lock(resource_mutex);
  const uint32_t handle = AllocateHandle(
      xe::kernel::xna::XnaGuestResource::kTexture);
  if (handle == UINT32_MAX) {
    return handle;
  }
  textures[handle] = std::move(texture);
  // A render target IS a resolve destination, so it gets the padded size.
  AllocateTextureStorage(handle, textures[handle], 1, /*resolve_target=*/true);
  if (out_a) {
    *out_a = handle;
  }
  if (out_b) {
    *out_b = AllocateHandle();
  }
  return handle;
}

extern "C" void xna_D3D_D3D_Device_ReleaseRenderTarget(uint32_t device,
                                                       uint32_t handle) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  textures.erase(handle);
}

// The four state releases take the state handle alone - no device. The states
// carry no storage yet, so there is nothing to free.
extern "C" void xna_D3D_D3D_Device_ReleaseBlendState(uint32_t handle) {}
extern "C" void xna_D3D_D3D_Device_ReleaseDepthStencilState(uint32_t handle) {}
extern "C" void xna_D3D_D3D_Device_ReleaseRasterizerState(uint32_t handle) {}
extern "C" void xna_D3D_D3D_Device_ReleaseSamplerState(uint32_t handle) {}

// ---- occlusion queries ------------------------------------------------------
//
// Nothing is drawn yet, so no query can have a real answer. Reporting "not
// available" is honest and leaves the title polling, which is what it does on
// hardware while a query is still in flight.

extern "C" uint32_t xna_D3D_D3D_OcclusionQuery_CreateHandle(uint32_t device) {
  std::lock_guard<std::mutex> lock(resource_mutex);
  return AllocateHandle();
}

extern "C" void xna_D3D_D3D_OcclusionQuery_ReleaseHandle(uint32_t device,
                                                         uint32_t handle) {}

extern "C" uint32_t xna_D3D_D3D_OcclusionQuery_IsDataAvailable(
    uint32_t device, uint32_t handle, uint32_t* out_available) {
  if (out_available) {
    *out_available = 0;
  }
  return 0;
}

// ---- diagnostics ------------------------------------------------------------

// Asked for after a failure, to turn an error code into something readable.
// Nothing here fails in a way that carries extra detail, so it reports none.
extern "C" uint32_t xna_D3D_D3D_GetLastErrorDetails(uint32_t device,
                                                    uint32_t* out_a,
                                                    uint32_t* out_b) {
  if (out_a) {
    *out_a = 0;
  }
  if (out_b) {
    *out_b = 0;
  }
  return 0;
}

// ---- draws ------------------------------------------------------------------
//
// Not every draw goes through the packet stream. The three below hand their
// geometry over directly, because it is user data that lives in managed memory
// rather than in a buffer the stream could name by handle - SpriteBatch and
// DrawUserPrimitives both land here.
//
// All three are ERROR-convention: the caller passes the result to
// ThrowExceptionFromResult, so anything but 0 stops the title. Accepting and
// counting them is what lets a run report how much drawing a frame involves,
// which is the measurement the packet translator will be built against.

namespace {

// The scissor rectangle the title last set, reported back on request. The
// packet stream owns the real one; until it is translated, the full display is
// the honest answer.
std::mutex scissor_mutex;
Rect scissor = {0, 0, 1280, 720};

}  // namespace

static void XnaDrawSpritesDirect(int32_t count, const void* sprites,
                                 int32_t texture_width, int32_t texture_height);

extern "C" uint32_t xna_D3D_D3D_Device_DrawSprites(uint32_t device,
                                                   int32_t count,
                                                   const void* sprites,
                                                   int32_t texture_width,
                                                   int32_t texture_height) {
  xe::kernel::xna::CountSprites(count > 0 ? static_cast<uint32_t>(count) : 0);
  XnaDrawSpritesDirect(count, sprites, texture_width, texture_height);
  return 0;
}

// InteropDrawUserPrims(device, primitiveType, primitiveCount, vertexData,
// vertexCount, vertexStride) - ERROR.
extern "C" uint32_t xna_D3D_D3D_Device_DrawPrimitivesUP(
    uint32_t device, uint32_t primitive_type, uint32_t primitive_count,
    const void* vertex_data, uint32_t vertex_count, uint32_t vertex_stride) {
  xe::kernel::xna::CountUserPrimitives(primitive_count);
  return 0;
}

// InteropDrawUserPrims(device, primitiveType, DUIP_PARAMS) - ERROR. The
// parameter block is nine sequential fields carrying both the index and the
// vertex data, and at 56 bytes it arrives by reference like every other
// by-value struct on x64.
extern "C" uint32_t xna_D3D_D3D_Device_DrawIndexedPrimitivesUP(
    uint32_t device, uint32_t primitive_type, const void* params) {
  // PrimitiveCount is the third field, after MinIndex and NumVertices.
  const auto* fields = static_cast<const uint32_t*>(params);
  xe::kernel::xna::CountUserPrimitives(params ? fields[2] : 0);
  return 0;
}

// ---- render state readback --------------------------------------------------

extern "C" uint32_t xna_D3D_D3D_Device_GetScissorRect(uint32_t device,
                                                      Rect* out_rect) {
  if (!out_rect) {
    return kInvalidArg;
  }
  std::lock_guard<std::mutex> lock(scissor_mutex);
  *out_rect = scissor;
  return 0;
}

// Reads the front buffer back into system memory - a screenshot, and on the
// console the way a title captures its own output.
//
// The frame is already in guest memory: Present resolves it there, at
// copy_dest_pitch = width and 8_8_8_8, and XnaGpuFrontBuffer says where. Two
// things have to happen that a plain copy would miss. The resolve holds its
// pixels in a GPU readback buffer behind a one-shot watch, so the range has to
// be READ as the guest before the bytes exist at all; and the resolve writes
// console byte order, so each pixel is swapped on the way out.
extern "C" uint32_t xna_D3D_D3D_Device_GetBackBufferData(
    uint32_t device, void* data, const uint32_t* info, const Rect* rect) {
  if (!data || !info) {
    return kInvalidArg;
  }
  // TEXTURE_COPYDATA_INFO: level, data size, element size, element count.
  const uint32_t data_size = info[1];

  if (xe::kernel::xna::XnaDirectActive()) {
    std::memset(data, 0, data_size);
    return xe::kernel::xna::XnaDirectReadBackBuffer(
               static_cast<uint8_t*>(data), data_size)
               ? 0
               : kInvalidArg;
  }

  uint32_t front = 0, bytes = 0, width = 0, height = 0;
  if (!xe::kernel::xna::XnaGpuFrontBuffer(&front, &bytes, &width, &height)) {
    // Nothing has been presented, so a blank frame is the honest answer.
    std::memset(data, 0, data_size);
    return 0;
  }

  const uint32_t needed = width * height * 4;
  if (data_size < needed) {
    XELOGW(
        "[xna] GetBackBufferData was given {} bytes for a {}x{} frame, which "
        "needs {}",
        data_size, width, height, needed);
    std::memset(data, 0, data_size);
    return kInvalidArg;
  }

  // Springs the resolve's watch. Without it every pixel reads back as zero
  // however well the frame was drawn.
  xe::kernel::xna::XnaGuestRangeRead(front, bytes);

  // Qualified: these shims are extern "C" at global scope, so the kernel's
  // free functions are not in scope unqualified the way they are inside the
  // xna namespace.
  auto* state = xe::kernel::kernel_state();
  auto* memory = state ? state->memory() : nullptr;
  const uint8_t* source =
      memory ? memory->TranslateVirtual<const uint8_t*>(front) : nullptr;
  if (!source) {
    std::memset(data, 0, data_size);
    return kInvalidArg;
  }
  std::memset(data, 0, data_size);

  auto* out = static_cast<uint8_t*>(data);
  const uint32_t pitch = xe::align(width, 32u);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const int32_t at = xe::gpu::texture_address::Tiled2D(
          int32_t(x), int32_t(y), pitch, 2);
      if (at < 0 || uint32_t(at) + 4 > bytes) {
        continue;
      }
      const uint8_t* texel = source + at;
      uint8_t* pixel = out + (y * width + x) * 4;
      pixel[0] = texel[3];
      pixel[1] = texel[2];
      pixel[2] = texel[1];
      pixel[3] = texel[0];
    }
  }
  return 0;
}

// The generated shims issue handles from the same table, so a handle is unique
// across every resource kind and the 24-bit limit is enforced in one place.
uint32_t xe::kernel::xna::XnaAllocateHandle() {
  std::lock_guard<std::mutex> lock(resource_mutex);
  return AllocateHandle();
}

bool xe::kernel::xna::XnaLookupTexture(uint32_t handle, uint32_t level,
                                       XnaTextureView* out) {
  if (!out) {
    return false;
  }
  std::lock_guard<std::mutex> lock(resource_mutex);
  auto found = textures.find(handle);
  if (found == textures.end()) {
    return false;
  }
  const Texture& texture = found->second;
  if (level >= texture.level_data.size()) {
    return false;
  }
  const TextureLevel& stored = texture.level_data[level];
  const auto* base = xe::kernel::xna::XnaGuestResourceData(texture.handle);
  if (!base || !stored.size) {
    return false;
  }
  out->data = base + stored.offset;
  out->size = stored.size;
  out->width = LevelSize(texture.width, level);
  out->height = LevelSize(texture.height, level);
  out->format = texture.format;
  out->version = texture.version;
  return true;
}

// ---- the command stream -----------------------------------------------------
//
// This is where the drawing arrives. XNA builds high-level packets in managed
// code and hands the whole block over at once, which is why the console
// graphics interface is a few dozen entry points rather than one per draw call.

static xe::gpu::d3d12::D3D12CommandProcessor* HostedCommandProcessor();

namespace {

// The device state the stream builds up, and the record of what a frame asked
// for. Held here rather than pushed straight at the GPU: the stream sets state
// and issues draws against handles, so the state has to exist before a draw can
// be turned into anything.
class DeviceSink final : public xe::kernel::xna::HlcbSink {
 public:
  static constexpr uint32_t kMaxStreams = 4;

  void Clear(const xe::kernel::xna::HlcbClear& clear) override {
    std::lock_guard<std::mutex> lock(resource_mutex);
    clears_++;
    last_clear_ = clear;
    // Carried through to the presenter, so the window shows the colour the
    // title asked for. It is the smallest thing that proves the whole chain -
    // packet decode, a real command list, the guest output image - and every
    // draw will go the same way.
    xe::kernel::xna::XnaSetClearColor(clear.color);
    // The colour matters to the diagnosis, not just the fact of a clear: a
    // render target that resolves as opaque black is only meaningful once it is
    // known whether the title cleared it to black.
    XELOGD("[xna] clear: options {} colour {: .3f} {: .3f} {: .3f} {: .3f}",
           clear.options, clear.color[0], clear.color[1], clear.color[2],
           clear.color[3]);
    ClearBoundTargetsLocked(clear);
  }

  void ClearBoundTargetsLocked(const xe::kernel::xna::HlcbClear& clear) {
    const bool color = (clear.options & 1) != 0;
    const bool depth = (clear.options & 2) != 0;
    if (!color && !depth) {
      return;
    }
    if (!render_target_count_) {
      xe::kernel::xna::XnaGpuTarget back;
      back.width = viewport_.width > 0 ? uint32_t(viewport_.width) : 1280u;
      back.height = viewport_.height > 0 ? uint32_t(viewport_.height) : 720u;
      back.format = 0;
      back.guest_address = 0;
      const uint32_t back_depth = xe::kernel::xna::XnaGpuEdramDepthBase(
          back.width, back.height, 1);
      const bool back_depth_fits =
          back_depth != xe::kernel::xna::kXnaGpuNoDepth;
      XELOGD("[xna]    clearing the back buffer ({}x{}) at edram 0, depth {}",
             back.width, back.height,
             back_depth_fits ? int64_t(back_depth) : int64_t(-1));
      xe::kernel::xna::XnaGpuClearTarget(
          back, 0, back_depth_fits ? back_depth : 0, clear.color, color,
          depth && back_depth_fits, clear.depth);
      return;
    }
    const uint32_t count =
        std::min<uint32_t>(std::max<uint32_t>(render_target_count_, 1), 4);
    for (uint32_t slot = 0; slot < count; ++slot) {
      xe::kernel::xna::XnaGpuTarget target;
      auto found = textures.find(render_targets_[slot]);
      if (found == textures.end() || !found->second.width) {
        continue;
      }
      auto* resource =
          xe::kernel::xna::XnaGuestResourceLookup(found->second.handle);
      if (!resource || !resource->data) {
        continue;
      }
      target.width = found->second.width;
      target.height = found->second.height;
      target.format = found->second.format;
      target.guest_address = resource->data;
      const uint32_t edram = xe::kernel::xna::XnaGpuEdramBaseForSlot(
          target.width, target.height, slot, count);
      const uint32_t depth_edram = xe::kernel::xna::XnaGpuEdramDepthBase(
          target.width, target.height, count);
      const bool depth_fits = depth_edram != xe::kernel::xna::kXnaGpuNoDepth;
      XELOGD(
          "[xna]    clearing slot {} ({}x{} format {}) at edram {}, depth {}",
          slot, target.width, target.height, target.format, edram,
          depth_fits ? int64_t(depth_edram) : int64_t(-1));
      xe::kernel::xna::XnaGpuClearTarget(
          target, edram, depth_fits ? depth_edram : 0, clear.color, color,
          depth && depth_fits && slot == 0, clear.depth);
    }
  }

  void SetViewport(const xe::kernel::xna::HlcbViewport& viewport) override {
    std::lock_guard<std::mutex> lock(resource_mutex);
    viewport_ = viewport;
  }

  void SetScissorRect(const xe::kernel::xna::HlcbRect& rect) override {
    std::lock_guard<std::mutex> lock(scissor_mutex);
    scissor.x = rect.x;
    scissor.y = rect.y;
    scissor.width = rect.width;
    scissor.height = rect.height;
  }

  void SetStreamSource(
      const xe::kernel::xna::HlcbStreamSource& stream) override {
    std::lock_guard<std::mutex> lock(resource_mutex);
    if (stream.stream_index < kMaxStreams) {
      streams_[stream.stream_index] = stream;
    }
  }

  // DrawUserPrimitives carries its vertices with the call. They are staged into
  // one reusable guest buffer and bound as stream 0 for that draw.
  bool StageUserVertices(const void* data, uint32_t bytes, uint32_t stride) {
    if (!data || !bytes || !stride) {
      return false;
    }
    std::lock_guard<std::mutex> lock(resource_mutex);
    if (bytes > kUserRingBytes) {
      return false;
    }
    if (!user_stream_handle_) {
      user_stream_handle_ = xe::kernel::xna::XnaGuestResourceCreate(
          xe::kernel::xna::XnaGuestResource::kVertexBuffer);
      if (!user_stream_handle_) {
        return false;
      }
      if (!xe::kernel::xna::XnaGuestResourceResize(user_stream_handle_,
                                                   kUserRingBytes)) {
        return false;
      }
    }
    auto* resource = xe::kernel::xna::XnaGuestResourceLookup(user_stream_handle_);
    auto* base = xe::kernel::xna::XnaGuestResourceData(user_stream_handle_);
    if (!resource || !resource->data || !base) {
      return false;
    }
    uint32_t offset = xe::align(user_ring_offset_, 256u);
    if (offset + bytes > kUserRingBytes) {
      offset = 0;
    }
    user_ring_offset_ = offset + bytes;
    CopyGuest(base + offset, data, bytes, 4);
    xe::kernel::xna::XnaGuestRangeWritten(resource->data + offset, bytes);
    saved_stream_ = streams_[0];
    streams_[0].vertex_buffer = user_stream_handle_;
    streams_[0].vertex_offset = offset;
    streams_[0].stride = stride;
    user_stream_active_ = true;
    return true;
  }

  void ReleaseUserVertices() {
    std::lock_guard<std::mutex> lock(resource_mutex);
    if (!user_stream_active_) {
      return;
    }
    streams_[0] = saved_stream_;
    user_stream_active_ = false;
  }

  bool StageUserIndices(const void* data, uint32_t bytes, bool sixteen_bit,
                        uint32_t* start_index) {
    if (!data || !bytes || !start_index) {
      return false;
    }
    std::lock_guard<std::mutex> lock(resource_mutex);
    const uint32_t element = sixteen_bit ? 2u : 4u;
    if (bytes > kUserRingBytes) {
      return false;
    }
    if (!user_index_handle_) {
      user_index_handle_ = xe::kernel::xna::XnaGuestResourceCreate(
          xe::kernel::xna::XnaGuestResource::kIndexBuffer);
      if (!user_index_handle_) {
        return false;
      }
      if (!xe::kernel::xna::XnaGuestResourceResize(user_index_handle_,
                                                   kUserRingBytes)) {
        return false;
      }
    }
    auto* resource = xe::kernel::xna::XnaGuestResourceLookup(user_index_handle_);
    auto* base = xe::kernel::xna::XnaGuestResourceData(user_index_handle_);
    if (!resource || !resource->data || !base) {
      return false;
    }
    uint32_t offset = xe::align(user_index_offset_, 256u);
    if (offset + bytes > kUserRingBytes) {
      offset = 0;
    }
    user_index_offset_ = offset + bytes;
    CopyGuest(base + offset, data, bytes & ~(element - 1), element);
    xe::kernel::xna::XnaGuestRangeWritten(resource->data + offset, bytes);
    resource->stride = element;
    saved_index_buffer_ = index_buffer_;
    index_buffer_ = user_index_handle_;
    user_indices_active_ = true;
    *start_index = offset / element;
    return true;
  }

  void ReleaseUserIndices() {
    std::lock_guard<std::mutex> lock(resource_mutex);
    if (!user_indices_active_) {
      return;
    }
    index_buffer_ = saved_index_buffer_;
    user_indices_active_ = false;
  }

  uint32_t user_index_handle_ = 0;
  uint32_t user_index_offset_ = 0;
  uint32_t saved_index_buffer_ = 0;
  bool user_indices_active_ = false;

  void SetIndexBuffer(uint32_t handle) override {
    std::lock_guard<std::mutex> lock(resource_mutex);
    index_buffer_ = handle;
  }

  void SetVertexDeclaration(uint32_t handle) override {
    std::lock_guard<std::mutex> lock(resource_mutex);
    declaration_ = handle;
  }

  uint32_t VertexDeclaration() {
    std::lock_guard<std::mutex> lock(resource_mutex);
    return declaration_;
  }

  // The byte stride of one vertex in a stream, which the declaration does not
  // carry - SetStreamSource does.
  uint32_t StreamStride(uint32_t index) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    return index < kMaxStreams ? streams_[index].stride : 0;
  }

  std::vector<uint8_t> StreamHead(uint32_t index, uint32_t count) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    std::vector<uint8_t> head;
    if (index >= kMaxStreams) {
      return head;
    }
    const uint32_t handle =
        xe::kernel::xna::XnaGuestResolveHandle(streams_[index].vertex_buffer);
    auto* resource =
        handle ? xe::kernel::xna::XnaGuestResourceLookup(handle) : nullptr;
    if (!resource || !resource->data ||
        streams_[index].vertex_offset >= resource->size) {
      return head;
    }
    auto* state = xe::kernel::kernel_state();
    auto* memory = state ? state->memory() : nullptr;
    const uint8_t* source =
        memory ? memory->TranslateVirtual<const uint8_t*>(
                     resource->data + streams_[index].vertex_offset)
               : nullptr;
    if (!source) {
      return head;
    }
    const uint32_t available = resource->size - streams_[index].vertex_offset;
    head.assign(source, source + std::min(count, available));
    return head;
  }

  void SetTexture(uint32_t sampler, uint32_t texture) override {
    std::lock_guard<std::mutex> lock(resource_mutex);
    if (sampler < kMaxTextures) {
      bound_textures_[sampler] = texture;
    }
  }

  // Kept by NAME, exactly like the constant values, and for the same reason:
  // the sampler register a name occupies belongs to the shader, not to the
  // texture. SceneDepthSampler is s0 in one object of an effect and s1 in
  // another, so a slot recorded here would be right for one draw and wrong for
  // the next. Name and slot meet at the draw.
  void EffectTexture(uint32_t effect, uint32_t parameter,
                     uint32_t texture) override {
    auto* image = xe::kernel::xna::FindEffect(effect);
    if (!image || !parameter || parameter > image->parameters.size()) {
      return;
    }
    const auto& info = image->parameters[parameter - 1];
    std::lock_guard<std::mutex> lock(resource_mutex);
    texture_by_effect_[effect][info.name] = texture;
    texture_by_name_[info.name] = texture;
    auto named = textures.find(texture);
    if (named != textures.end() && named->second.name.empty()) {
      named->second.name = info.name;
    }
  }

  void EffectTechnique(uint32_t effect, uint32_t technique) override {
    xe::kernel::xna::XnaSetEffectTechnique(effect, technique);
  }

  void SetState(xe::kernel::xna::HlcbPacketType type, uint32_t handle,
                uint32_t slot) override {
    std::lock_guard<std::mutex> lock(resource_mutex);
    switch (type) {
      case xe::kernel::xna::HlcbPacketType::kSetBlendState:
        blend_state_ = handle;
        break;
      case xe::kernel::xna::HlcbPacketType::kSetDepthStencilState:
        depth_state_ = handle;
        break;
      case xe::kernel::xna::HlcbPacketType::kSetRasterizerState:
        rasterizer_state_ = handle;
        break;
      default:
        if (slot < kMaxSamplers) {
          sampler_states_[slot] = handle;
        }
        break;
    }
  }

  // A deferred renderer draws each pass into its own target and samples the
  // previous one, so the target a draw lands in has to follow the title's
  // binding - and the old target has to be resolved out to the texture that
  // names it before anything can read it back.
  void SetRenderTargets(const uint32_t* handles, uint32_t count) override {
    uint32_t previous[4];
    uint32_t previous_count;
    {
      std::lock_guard<std::mutex> lock(resource_mutex);
      previous_count = render_target_count_;
      for (uint32_t i = 0; i < previous_count; ++i) {
        previous[i] = render_targets_[i];
      }
      render_target_count_ = std::min<uint32_t>(count, 4);
      for (uint32_t i = 0; i < render_target_count_; ++i) {
        render_targets_[i] =
            xe::kernel::xna::XnaGuestResolveHandle(handles[i]);
      }
      for (uint32_t i = render_target_count_; i < 4; ++i) {
        render_targets_[i] = 0;
      }
    }
    XELOGD("[xna] set render targets: {} target(s), first handle {:08X}",
           render_target_count_,
           render_target_count_ ? render_targets_[0] : 0);
    if (!cvars::xna_geometry) {
      return;
    }
    for (uint32_t i = 0; i < previous_count; ++i) {
      ResolveRenderTarget(previous[i], i, previous_count);
    }
    xe::kernel::xna::XnaGpuEndTiledPass();
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t formats[4] = {};
    uint32_t bound = 0;
    {
      std::lock_guard<std::mutex> lock(resource_mutex);
      bound = render_target_count_;
      for (uint32_t i = 0; i < bound; ++i) {
        auto found = textures.find(render_targets_[i]);
        if (found == textures.end()) {
          continue;
        }
        if (!width) {
          width = found->second.width;
          height = found->second.height;
        }
        formats[i] = found->second.format;
      }
    }
    if (bound && width && height) {
      xe::kernel::xna::XnaGpuBeginTiledPass(width, height, bound, formats);
    }
  }

  void EffectValue(xe::kernel::xna::HlcbPacketType type, uint32_t effect,
                   uint32_t parameter, uint32_t count, const uint8_t* data,
                   uint32_t bytes) override {
    if (!data || bytes < 4) {
      return;
    }
    const bool transpose =
        type == xe::kernel::xna::HlcbPacketType::kEffectSetValueMatrixTranspose ||
        type ==
            xe::kernel::xna::HlcbPacketType::kEffectSetValueMatrixTransposeArray;
    // The packet type states the stride of the title's own data and the header
    // states how many elements followed it. Without both, an array of matrices
    // is indistinguishable from one very long matrix, and every element after
    // the first is gathered from the wrong floats.
    const uint32_t element_bytes = xe::kernel::xna::EffectValueElementSize(
        static_cast<uint32_t>(type));
    const bool integral =
        type == xe::kernel::xna::HlcbPacketType::kEffectSetValueBool ||
        type == xe::kernel::xna::HlcbPacketType::kEffectSetValueInt ||
        type == xe::kernel::xna::HlcbPacketType::kEffectSetValueBoolArray ||
        type == xe::kernel::xna::HlcbPacketType::kEffectSetValueIntArray;
    std::vector<float> converted;
    if (integral) {
      converted.resize(bytes / 4);
      for (uint32_t i = 0; i < bytes / 4; ++i) {
        int32_t value;
        std::memcpy(&value, data + i * 4, sizeof(value));
        converted[i] = static_cast<float>(value);
      }
      data = reinterpret_cast<const uint8_t*>(converted.data());
      bytes = static_cast<uint32_t>(converted.size() * 4);
    }
    RecordConstant(effect, parameter, transpose, data, bytes, count,
                   element_bytes);
    if (type != xe::kernel::xna::HlcbPacketType::kEffectSetValueMatrix &&
        type !=
            xe::kernel::xna::HlcbPacketType::kEffectSetValueMatrixTranspose) {
      return;
    }
    if (bytes < 64) {
      return;
    }
    std::lock_guard<std::mutex> lock(resource_mutex);
    Matrix matrix;
    for (uint32_t i = 0; i < 16; ++i) {
      std::memcpy(&matrix.m[i], data + i * 4, sizeof(float));
    }
    matrix.transposed =
        type == xe::kernel::xna::HlcbPacketType::kEffectSetValueMatrixTranspose;
    matrices_[parameter] = matrix;
    last_matrix_ = parameter;
  }

  // WHICH FLOAT OF THE TITLE'S DATA GOES IN ONE COMPONENT OF ONE REGISTER.
  //
  // The one rule both halves of this obey - the write when the title sets a
  // value, and the write at the draw when the mapping is finally known. Element
  // e of an array starts stride floats in, whatever the shader reserved for it;
  // inside an element, a column-major parameter puts column k in register k and
  // a row-major one puts row k there.
  static uint32_t SourceFloat(uint32_t element, uint32_t register_in_element,
                              uint32_t component, uint32_t stride,
                              bool column_major) {
    const uint32_t within = column_major
                                ? (component * 4 + register_in_element)
                                : (register_in_element * 4 + component);
    return element * stride + within;
  }

  // How many registers one element of an array occupies in this shader. The
  // shader's own RegisterCount is authoritative and is not what the declared
  // type implies - a 4x4 matrix is given three registers when the compiler
  // never reads the fourth - so it is divided by the DECLARED element count,
  // not by however many the title happened to send this call.
  static uint32_t RegistersPerElement(uint32_t reserved, uint32_t declared) {
    if (!declared || reserved < declared) {
      return 1;
    }
    return reserved % declared == 0 ? reserved / declared : reserved;
  }

  // Every value the title sets, placed at the constant register the shader
  // reads it from, taken from that shader's own constant table.
  void RecordConstant(uint32_t effect, uint32_t parameter, bool transpose,
                      const uint8_t* data, uint32_t bytes, uint32_t count,
                      uint32_t element_bytes) {
    auto* image = xe::kernel::xna::FindEffect(effect);
    if (!image || !parameter || parameter > image->parameters.size()) {
      return;
    }
    const uint32_t index = parameter - 1;
    const auto& info = image->parameters[index];
    const uint32_t registers =
        std::min<uint32_t>(RegistersFor(info), (bytes + 15) / 16);
    if (!registers) {
      return;
    }
    const uint32_t total_floats = bytes / 4;
    const uint32_t declared = info.elements ? info.elements : 1;
    // Four floats at the very least, or a single register would be gathered
    // from the element after it.
    const uint32_t stride =
        element_bytes && element_bytes != UINT32_MAX
            ? std::max<uint32_t>(element_bytes / 4, 4)
            : std::max<uint32_t>(total_floats / (count ? count : 1), 4);
    // However many the title actually sent, never more than the data holds or
    // the parameter declares.
    const uint32_t elements = std::min(
        {count ? count : 1u, declared, std::max<uint32_t>(total_floats / stride, 1u)});

    // KEPT, NOT PLACED.
    //
    // A title sets its parameters before it applies a pass, and the register a
    // parameter occupies is only known once that pass's shader is analyzed - so
    // at the moment this runs there is often no mapping at all, and the value
    // was being dropped and never asked for again. The value is state; the
    // mapping is per shader; they meet at the draw.
    {
      std::lock_guard<std::mutex> lock(resource_mutex);
      auto& stored = parameter_values_[info.name];
      stored.assign(reinterpret_cast<const float*>(data),
                    reinterpret_cast<const float*>(data) + (bytes / 4));
      stored.resize((stored.size() + 3) & ~size_t(3), 0.0f);
      parameter_transposed_[info.name] = transpose;
      parameter_class_[info.name] = info.parameter_class;
      // The shape travels with the value, because the draw has to lay it out
      // again once it knows the mapping and by then the packet is long gone.
      parameter_elements_[info.name] = elements;
      parameter_declared_elements_[info.name] = declared;
      parameter_stride_[info.name] = stride;
      auto& owned = parameters_by_effect_[effect][info.name];
      owned.value = stored;
      owned.transposed = transpose;
      owned.parameter_class = info.parameter_class;
      owned.elements = elements;
      owned.declared = declared;
      owned.stride = stride;

      // WHAT THE TITLE ACTUALLY SET. A view or projection matrix has a shape
      // you can recognise on sight - an orthonormal upper 3x3, a translation in
      // the last row or column, a lone -1 for the w. Printed once per parameter
      // so the values in the registers can be checked against the values that
      // were meant to go there, rather than assuming the placement is right.
      // EVERY SET. This was once per name, then once per change between finite
      // and not, and both filters showed only the press start screen's
      // legitimately degenerate camera and hid the valid one the title sets a
      // moment later.
    }

    // NOTHING IS PLACED HERE. THE VALUE IS ALL THAT IS KEPT.
    //
    // Placing it meant writing every register that ANY shader in the effect
    // gives the name, and the shaders of one effect disagree: the light shader
    // of effect 168 puts _FarClippingDistance at c0 while another object of the
    // same effect puts _ProjectionToView at c0..c3. Setting one stamped over
    // the other, and the shader that read c0 got the wrong parameter entirely.
    //
    // A register file belongs to one shader, so it is built at the draw from
    // the table of the shader about to run - see ApplyStoredConstants.
  }

  void Draw(const xe::kernel::xna::HlcbDraw& draw) override {
    std::lock_guard<std::mutex> lock(resource_mutex);
    draws_++;
    primitives_ += draw.primitive_count;
    if (!draw.user_data && streams_[0].vertex_buffer != 0) {
      auto* resource = xe::kernel::xna::XnaGuestResourceLookup(
          xe::kernel::xna::XnaGuestResolveHandle(streams_[0].vertex_buffer));
      if (!resource || !resource->data || !resource->size) {
        empty_stream_draws_++;
      }
    }
  }

  static uint32_t SpriteDeclaration() {
    static uint32_t handle = 0;
    if (handle) {
      return handle;
    }
    const XnaVertexElement elements[3] = {
        {0, 0, 2, 0, 0},
        {0, 12, 4, 1, 0},
        {0, 16, 1, 2, 0},
    };
    const uint32_t created = xna_D3D_D3D_Decl_CreateHandle(0, elements, 3);
    if (created == UINT32_MAX) {
      return 0;
    }
    handle = created;
    return handle;
  }

  void DrawSprites(const xe::kernel::xna::HlcbSprites& sprites) override {
    uint32_t texture = 0;
    uint32_t previous_declaration = 0;
    {
      std::lock_guard<std::mutex> lock(resource_mutex);
      // Each sprite is a fixed-size record; the count follows from the block.
      sprite_draws_++;
      sprite_bytes_ += sprites.data_size;
      // THE PACKET DOES NOT NAME THE TEXTURE. A DrawSprites header reads
      // 3B000000 in a real buffer - handle zero - because the texture was bound
      // by the SetTexture packet immediately before it. Taking the handle from
      // the header is why every batch resolved to no texture and drew nothing,
      // while the counts said twenty-one batches had arrived.
      texture = sprites.texture ? sprites.texture : bound_textures_[0];
      previous_declaration = declaration_;
    }
    XELOGD("[xna] DrawSprites: {} sprite(s), {} byte(s), texture {:08X}",
           sprites.count, sprites.data_size, texture);
    if (!sprites.data || !sprites.count ||
        sprites.data_size < sprites.count * 56u) {
      XELOGW("[xna] DrawSprites dropped: {} sprite(s) in {} byte(s), data {}",
             sprites.count, sprites.data_size, sprites.data ? "present" : "null");
      return;
    }
    if (!cvars::xna_geometry) {
      xe::kernel::xna::XnaQueueSprites(
          texture, static_cast<uint32_t>(sprites.texture_width),
          static_cast<uint32_t>(sprites.texture_height), sprites.count,
          sprites.data, sprites.data_size);
      return;
    }
    const uint32_t declaration = SpriteDeclaration();
    if (!declaration) {
      XELOGW("[xna] DrawSprites dropped: the sprite declaration was not created");
      return;
    }
    struct SpriteVertex {
      float x, y, z;
      uint32_t colour;
      float u, v;
    };
    static_assert(sizeof(SpriteVertex) == 24, "sprite vertex is 24 bytes");
    const float texture_width =
        float(sprites.texture_width > 0 ? sprites.texture_width : 1);
    const float texture_height =
        float(sprites.texture_height > 0 ? sprites.texture_height : 1);
    std::vector<SpriteVertex> vertices;
    vertices.reserve(size_t(sprites.count) * 6);
    for (uint32_t s = 0; s < sprites.count; ++s) {
      const uint8_t* record = sprites.data + size_t(s) * 56;
      float f[12];
      std::memcpy(f, record, sizeof(f));
      uint32_t effects = 0;
      uint32_t colour = 0;
      std::memcpy(&effects, record + 48, sizeof(effects));
      std::memcpy(&colour, record + 52, sizeof(colour));
      float src_x = f[0], src_y = f[1], src_w = f[2], src_h = f[3];
      if (src_w == 0.0f) {
        src_w = texture_width;
      }
      if (src_h == 0.0f) {
        src_h = texture_height;
      }
      const float dst_x = f[4], dst_y = f[5], dst_w = f[6], dst_h = f[7];
      const float origin_x = f[8] / src_w;
      const float origin_y = f[9] / src_h;
      const float rotation = f[10];
      const float depth = f[11];
      const float c = rotation != 0.0f ? std::cos(rotation) : 1.0f;
      const float sn = rotation != 0.0f ? std::sin(rotation) : 0.0f;
      SpriteVertex corners[4];
      for (uint32_t k = 0; k < 4; ++k) {
        const float cx = float(k & 1);
        const float cy = float(k >> 1);
        const float dx = (cx - origin_x) * dst_w;
        const float dy = (cy - origin_y) * dst_h;
        const float ux = (effects & 1) ? 1.0f - cx : cx;
        const float vy = (effects & 2) ? 1.0f - cy : cy;
        corners[k].x = dst_x + dx * c - dy * sn;
        corners[k].y = dst_y + dx * sn + dy * c;
        corners[k].z = depth;
        corners[k].colour = colour;
        corners[k].u = (src_x + src_w * ux) / texture_width;
        corners[k].v = (src_y + src_h * vy) / texture_height;
      }
      vertices.push_back(corners[0]);
      vertices.push_back(corners[1]);
      vertices.push_back(corners[2]);
      vertices.push_back(corners[1]);
      vertices.push_back(corners[3]);
      vertices.push_back(corners[2]);
    }
    {
      std::lock_guard<std::mutex> lock(resource_mutex);
      declaration_ = declaration;
    }
    Nexia_XnaDrawUserPrimitives(
        0, int32_t(sprites.count * 2), vertices.data(),
        uint32_t(vertices.size() * sizeof(SpriteVertex)),
        uint32_t(sizeof(SpriteVertex)));
    {
      std::lock_guard<std::mutex> lock(resource_mutex);
      declaration_ = previous_declaration;
    }
  }

  void Other(xe::kernel::xna::HlcbPacketType type, uint32_t handle) override {
    std::lock_guard<std::mutex> lock(resource_mutex);
    other_[static_cast<uint32_t>(type)]++;
  }

  std::string Describe() {
    std::lock_guard<std::mutex> lock(resource_mutex);
    std::string text =
        "  " + std::to_string(clears_) + " clear(s), last to rgba(" +
        std::to_string(last_clear_.color[0]) + ", " +
        std::to_string(last_clear_.color[1]) + ", " +
        std::to_string(last_clear_.color[2]) + ")\n" + "  " +
        std::to_string(draws_) + " draw(s) totalling " +
        std::to_string(primitives_) + " primitive(s)\n" + "  " +
        std::to_string(sprite_draws_) + " sprite packet(s), " +
        std::to_string(sprite_bytes_) + " bytes of sprite data\n" +
        "  viewport " + std::to_string(viewport_.width) + "x" +
        std::to_string(viewport_.height) + "\n";
    if (empty_stream_draws_ != 0) {
      text += "  " + std::to_string(empty_stream_draws_) +
              " draw(s) from a vertex buffer that was never written\n";
    }
    for (const auto& entry : other_) {
      text += "  " + std::to_string(entry.second) + " packet(s) of type " +
              std::to_string(entry.first) + " not acted on\n";
    }
    return text;
  }

  // The bound streams and index buffer as guest addresses, for the draw path.
  uint32_t CollectStreams(xe::kernel::xna::XnaGpuStream* out, uint32_t max) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    uint32_t count = 0;
    for (uint32_t i = 0; i < kMaxStreams && count < max; ++i) {
      const uint32_t handle =
          xe::kernel::xna::XnaGuestResolveHandle(streams_[i].vertex_buffer);
      auto* resource =
          handle ? xe::kernel::xna::XnaGuestResourceLookup(handle) : nullptr;
      if (!resource || !resource->data || !resource->size) {
        continue;
      }
      if (streams_[i].vertex_offset >= resource->size) {
        continue;
      }
      out[count].guest_address = resource->data + streams_[i].vertex_offset;
      out[count].size_bytes = resource->size - streams_[i].vertex_offset;
      out[count].stride = streams_[i].stride;
      ++count;
    }
    return count;
  }

  void IndexBuffer(uint32_t* address, uint32_t* size, bool* is_32bit) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    *address = 0;
    *size = 0;
    *is_32bit = false;
    const uint32_t handle =
        xe::kernel::xna::XnaGuestResolveHandle(index_buffer_);
    auto* resource =
        handle ? xe::kernel::xna::XnaGuestResourceLookup(handle) : nullptr;
    if (!resource || !resource->data || !resource->size) {
      return;
    }
    *address = resource->data;
    *size = resource->size;
    *is_32bit = resource->stride == 4;
  }

  // Where a draw should land, and how big it is. Each bound target gets its
  // own EDRAM base so the passes of a deferred frame do not write over one
  // another.
  // Places every parameter the title has ever set into the registers THIS
  // shader assigns them. Called for the shaders a draw is about to use, which
  // is the first moment both halves are known.
  // Returns how many registers were actually filled. Having a mapping is not
  // the same as having values: a shader can inherit a mapping naming parameters
  // this title never sets, in which case nothing is written and the caller has
  // to know that rather than assume the mapping did its job.
  uint32_t ApplyStoredConstants(
      const std::map<std::string, uint32_t>& registers_for_shader,
      const std::map<std::string, uint32_t>& counts_for_shader,
      const std::map<std::string, std::vector<float>>& defaults_for_shader,
      uint32_t bank, uint32_t effect) {
    uint32_t written = 0;
    if (registers_for_shader.empty() || bank >= kConstantBanks) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(resource_mutex);
    auto owned = parameters_by_effect_.find(effect);
    for (const auto& entry : registers_for_shader) {
      const ParameterValue* mine = nullptr;
      if (owned != parameters_by_effect_.end()) {
        auto found = owned->second.find(entry.first);
        if (found != owned->second.end() && found->second.value.size() >= 4) {
          mine = &found->second;
        }
      }
      auto value = parameter_values_.find(entry.first);
      if (!mine && (value == parameter_values_.end() ||
                    value->second.size() < 4)) {
        auto fallback = defaults_for_shader.find(entry.first);
        if (fallback == defaults_for_shader.end()) {
          continue;
        }
        const uint32_t registers =
            static_cast<uint32_t>(fallback->second.size() / 4);
        for (uint32_t r = 0; r < registers; ++r) {
          const uint32_t at = entry.second + r;
          if (at >= kMaxConstantRegisters) {
            break;
          }
          std::memcpy(constant_registers_[bank][at],
                      fallback->second.data() + r * 4, sizeof(float) * 4);
          constant_register_set_[bank][at] = true;
          ++written;
        }
        continue;
      }
      const std::vector<float>& floats =
          mine ? mine->value : value->second;
      const uint32_t parameter_class =
          mine ? mine->parameter_class : parameter_class_[entry.first];
      const bool transpose =
          mine ? mine->transposed : parameter_transposed_[entry.first];
      const bool column_major = (parameter_class == 3) != transpose;
      auto count = counts_for_shader.find(entry.first);
      const uint32_t reserved = count == counts_for_shader.end()
                                    ? 4u
                                    : count->second;
      // The same split the write at set time made, from the shape kept with
      // the value. A matrix array laid out as one long matrix gathers every
      // element after the first from the wrong floats.
      const uint32_t total_floats = static_cast<uint32_t>(floats.size());
      const uint32_t declared = std::max<uint32_t>(
          mine ? mine->declared : parameter_declared_elements_[entry.first], 1);
      const uint32_t stride = std::max<uint32_t>(
          mine ? mine->stride : parameter_stride_[entry.first], 4);
      const uint32_t elements = std::min<uint32_t>(
          std::max<uint32_t>(
              mine ? mine->elements : parameter_elements_[entry.first], 1),
          declared);
      const uint32_t per_element = RegistersPerElement(reserved, declared);
      for (uint32_t e = 0; e < elements; ++e) {
        for (uint32_t k = 0; k < per_element; ++k) {
          const uint32_t offset = e * per_element + k;
          const uint32_t at = entry.second + offset;
          if (at >= kMaxConstantRegisters || offset >= reserved) {
            break;
          }
          for (uint32_t c = 0; c < 4; ++c) {
            const uint32_t source =
                SourceFloat(e, k, c, stride, column_major);
            constant_registers_[bank][at][c] =
                source < total_floats ? floats[source] : 0.0f;
          }
          constant_register_set_[bank][at] = true;
          ++written;
        }
      }
    }
    return written;
  }

  // Empties one stage's constant file. Called before the shader about to run
  // fills it, because whatever the last shader left in a register is not this
  // shader's parameter - the two disagree about what c0 is, and the leftover
  // reads as a perfectly plausible value.
  void ResetBank(uint32_t bank) {
    if (bank >= kConstantBanks) {
      return;
    }
    std::lock_guard<std::mutex> lock(resource_mutex);
    std::memset(constant_registers_[bank], 0,
                sizeof(constant_registers_[bank]));
    std::memset(constant_register_set_[bank], 0,
                sizeof(constant_register_set_[bank]));
  }

  bool IsRegisterSet(uint32_t index, uint32_t bank) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    return index < kMaxConstantRegisters && bank < kConstantBanks &&
           constant_register_set_[bank][index];
  }

  // Which of the two constant files a stage reads from.
  static uint32_t BankFor(xe::gpu::xenos::ShaderType type) {
    return type == xe::gpu::xenos::ShaderType::kVertex ? 0u : 1u;
  }

  // LAST RESORT FOR A SHADER WHOSE NAMES COULD NOT BE READ.
  //
  // A vertex shader that reads a run of four registers and writes oPos from
  // four dp4s is doing one thing: transforming by a matrix. When the name walk
  // returns nothing for such a shader those registers stay empty and every
  // vertex lands on the origin. The title sets combined transforms by name -
  // MatrixTransform and WorldViewProj among them - so one of those goes into
  // the run rather than leaving it blank. A wrong matrix draws in the wrong
  // place; an empty one draws nothing at all.
  bool ApplyFallbackTransform(uint32_t first_register, uint32_t bank) {
    static const char* kCombined[] = {"MatrixTransform", "WorldViewProj",
                                      "_WorldViewProjection",
                                      "_WorldViewProj"};
    if (bank >= kConstantBanks) {
      return false;
    }
    std::lock_guard<std::mutex> lock(resource_mutex);
    for (const char* name : kCombined) {
      auto found = parameter_values_.find(name);
      if (found == parameter_values_.end() || found->second.size() < 16) {
        continue;
      }
      const bool column_major = (parameter_class_[name] == 3) !=
                                parameter_transposed_[name];
      for (uint32_t r = 0; r < 4; ++r) {
        const uint32_t at = first_register + r;
        if (at >= kMaxConstantRegisters) {
          break;
        }
        for (uint32_t c = 0; c < 4; ++c) {
          const uint32_t source = column_major ? (c * 4 + r) : (r * 4 + c);
          constant_registers_[bank][at][c] = found->second[source];
        }
        constant_register_set_[bank][at] = true;
      }
      XELOGD("[xna] placed \"{}\" into {} c{}..c{} as the fallback transform",
             name, bank ? "pixel" : "vertex", first_register,
             first_register + 3);
      return true;
    }
    return false;
  }

  // The literals a shader was compiled with, written to the top of the constant
  // file where its code addresses them. Nothing the title sets can fill these -
  // they are the compiler's own values, not parameters.
  void SetShaderLiterals(const std::vector<float>& literals, uint32_t bank) {
    if (literals.empty() || bank >= kConstantBanks) {
      return;
    }
    const uint32_t count =
        std::min<uint32_t>(static_cast<uint32_t>(literals.size() / 4),
                           kMaxConstantRegisters);
    std::lock_guard<std::mutex> lock(resource_mutex);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t at = kMaxConstantRegisters - count + i;
      std::memcpy(constant_registers_[bank][at], literals.data() + i * 4,
                  sizeof(float) * 4);
      constant_register_set_[bank][at] = true;
    }
  }

  // THE TEXTURES THE PIXEL SHADERS ARE ABOUT TO SAMPLE.
  //
  // Nothing in the draw path bound any, so every tfetch in every pixel shader
  // read whatever happened to be in the fetch constant file - which was the
  // swap texture at constant 0 and zeroes everywhere else. The geometry passes
  // wrote a G-buffer nothing then sampled, and every later pass composited
  // black, while the sprite path drew the HUD through its own descriptor and
  // looked perfectly healthy.
  // `samplers` is the running shader's own name-to-register map, read from its
  // constant table. Slot-bound textures go in first as the base, then the
  // effect's named bindings are placed at the registers THIS shader gives them.
  void ReportTextureCensusLocked() {
    uint32_t total = 0;
    uint32_t never_uploaded = 0;
    uint32_t never_bound = 0;
    uint32_t discarded = 0;
    for (const auto& entry : textures) {
      ++total;
      const Texture& texture = entry.second;
      never_uploaded += texture.uploads ? 0 : 1;
      never_bound += texture.binds ? 0 : 1;
      discarded += texture.discards ? 1 : 0;
    }
    XELOGD(
        "[xna] texture census: {} live, {} never uploaded, {} never bound, {} "
        "had a copy discarded",
        total, never_uploaded, never_bound, discarded);
    // Only the ones with something wrong with them - the whole table every
    // census was tens of thousands of lines saying nothing.
    for (const auto& entry : textures) {
      const Texture& texture = entry.second;
      if (texture.uploads && texture.binds && !texture.discards) {
        continue;
      }
      XELOGD(
          "[xna]    texture {:08X}: {}x{} format {} {} level(s), {} upload(s) "
          "{} discard(s) {} bind(s)",
          entry.first, texture.width, texture.height, texture.format,
          texture.levels, texture.uploads, texture.discards, texture.binds);
    }
  }

  void CollectTextures(xe::kernel::xna::XnaGpuTextureBinding* out,
                       uint32_t count,
                       const std::map<std::string, uint32_t>& samplers,
                       uint32_t effect) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    static const std::map<std::string, uint32_t> kNoBindings;
    auto for_effect = texture_by_effect_.find(effect);
    const std::map<std::string, uint32_t>* bindings =
        for_effect != texture_by_effect_.end() ? &for_effect->second
                                               : &kNoBindings;
    if (++collect_calls_ % 256 == 0) {
      ReportTextureCensusLocked();
    }
    const auto describe = [&](uint32_t texture, uint32_t slot) {
      if (slot >= count || slot >= kMaxTextures) {
        return;
      }
      if (!texture) {
        return;
      }
      auto found = textures.find(texture);
      if (found == textures.end() || !found->second.width ||
          !found->second.handle) {
        XELOGD(
            "[xna] effect {} slot {}: texture {:08X} not placed - {}", effect,
            slot, texture,
            found == textures.end()
                ? "no texture record"
                : (!found->second.width ? "zero width" : "no guest handle"));
        return;
      }
      auto* resource =
          xe::kernel::xna::XnaGuestResourceLookup(found->second.handle);
      if (!resource || !resource->data) {
        XELOGD(
            "[xna] effect {} slot {}: texture {:08X} not placed - guest "
            "resource {:08X} has no memory",
            effect, slot, texture, found->second.handle);
        return;
      }
      out[slot].guest_address = resource->data;
      out[slot].width = found->second.width;
      out[slot].height = found->second.height;
      out[slot].format = found->second.format;
      out[slot].levels = found->second.levels ? found->second.levels : 1;
      out[slot].tiled = found->second.resolves != 0;
      out[slot].handle = texture;
      out[slot].type = found->second.type;
      // The sampler state the title bound to this same slot decides wrap vs
      // clamp and point vs linear. Without it every world texture was clamped,
      // so a wrapped facade showed one stretched edge instead of tiling.
      if (slot < kMaxSamplers && sampler_states_[slot]) {
        std::lock_guard<std::mutex> plock(sampler_params_mutex);
        auto sp = sampler_params.find(sampler_states_[slot]);
        if (sp != sampler_params.end()) {
          out[slot].address_u = sp->second.address_u;
          out[slot].address_v = sp->second.address_v;
          out[slot].mag_filter = sp->second.mag_filter;
          out[slot].min_filter = sp->second.min_filter;
          out[slot].mip_filter = sp->second.mip_filter;
          out[slot].aniso = sp->second.aniso;
          out[slot].sampler_set = true;
        }
      }
      ++found->second.binds;
    };
    for (uint32_t slot = 0; slot < count && slot < kMaxTextures; ++slot) {
      out[slot] = xe::kernel::xna::XnaGpuTextureBinding();
      describe(bound_textures_[slot], slot);
    }
    // THE SAMPLER AND THE TEXTURE DO NOT SHARE A NAME.
    //
    // A shader's constant table names DiffuseMapSampler; the title binds
    // _DiffuseMapTexture. In HLSL those are joined by
    //   sampler DiffuseMapSampler = sampler_state { Texture = <_DiffuseMapTexture>; };
    // and that link lives in the effect's sampler state, which is not parsed
    // yet. Until it is, they are paired on their stem - leading underscore
    // gone, one trailing Sampler/Texture/Map removed - which joins every pair
    // this title uses. Each pairing is logged, and so is every sampler left
    // without a texture, so the log says outright where the convention fails
    // instead of the screen going quietly black.
    for (const auto& entry : samplers) {
      auto bound = bindings->find(entry.first);
      if (bound == bindings->end()) {
        const std::string want = TextureNameStem(entry.first);
        for (auto it = bindings->begin(); it != bindings->end(); ++it) {
          if (TextureNameStem(it->first) == want) {
            bound = it;
            break;
          }
        }
      }
      // A sampler carries its filter in its name - SourcePointSampler,
      // SourceLinearSampler - so its stem keeps that infix (SourcePoint) while
      // the texture it reads is just _SourceTexture (stem Source). The exact
      // stem match above misses those; here a texture whose stem is a PREFIX of
      // the sampler's stem is accepted, longest first so _SourceDepthTexture
      // wins over _SourceTexture for a SourceDepth sampler. This is what left
      // the post-process source - the whole rendered scene - unbound.
      if (bound == bindings->end()) {
        const std::string want = TextureNameStem(entry.first);
        size_t best = 0;
        for (auto it = bindings->begin(); it != bindings->end(); ++it) {
          const std::string stem = TextureNameStem(it->first);
          if (!stem.empty() && stem.size() > best && stem.size() <= want.size() &&
              want.compare(0, stem.size(), stem) == 0) {
            bound = it;
            best = stem.size();
          }
        }
      }
      if (bound == bindings->end() || !bound->second) {
        continue;
      }
      describe(bound->second, entry.second);
    }
  }

  bool LookupParameter(uint32_t effect, const std::string& name, uint32_t reg,
                       float* out) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    const std::vector<float>* value = nullptr;
    auto for_effect = parameters_by_effect_.find(effect);
    if (for_effect != parameters_by_effect_.end()) {
      auto found = for_effect->second.find(name);
      if (found != for_effect->second.end()) {
        value = &found->second.value;
      }
    }
    if (!value) {
      auto shared = parameter_values_.find(name);
      if (shared != parameter_values_.end()) {
        value = &shared->second;
      }
    }
    const size_t first = size_t(reg) * 4;
    if (!value || value->size() <= first) {
      return false;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      out[i] = first + i < value->size() ? (*value)[first + i] : 0.0f;
    }
    return true;
  }

  uint32_t BoundEffectTexture(uint32_t effect, const std::string& name) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    auto for_effect = texture_by_effect_.find(effect);
    if (for_effect == texture_by_effect_.end()) {
      return 0;
    }
    auto found = for_effect->second.find(name);
    if (found == for_effect->second.end() || found->second == UINT32_MAX) {
      return 0;
    }
    return found->second;
  }

  bool StoredParameterValue(uint32_t effect, const std::string& name,
                            std::vector<float>* out) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    auto for_effect = parameters_by_effect_.find(effect);
    if (for_effect != parameters_by_effect_.end()) {
      auto found = for_effect->second.find(name);
      if (found != for_effect->second.end()) {
        *out = found->second.value;
        return true;
      }
    }
    auto shared = parameter_values_.find(name);
    if (shared != parameter_values_.end()) {
      *out = shared->second;
      return true;
    }
    return false;
  }

  void CloneEffectState(uint32_t from, uint32_t to) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    auto textures_from = texture_by_effect_.find(from);
    if (textures_from != texture_by_effect_.end()) {
      texture_by_effect_[to] = textures_from->second;
    }
    auto parameters_from = parameters_by_effect_.find(from);
    if (parameters_from != parameters_by_effect_.end()) {
      parameters_by_effect_[to] = parameters_from->second;
    }
    XELOGD("[xna] effect {} cloned from {}: {} texture(s), {} parameter(s)", to,
           from, texture_by_effect_[to].size(), parameters_by_effect_[to].size());
  }

  uint32_t RenderTargetCount() {
    std::lock_guard<std::mutex> lock(resource_mutex);
    return render_target_count_;
  }

  void CurrentDepthState(bool* enable_out, bool* write_out,
                         uint32_t* function_out) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    auto found = depth_states.find(depth_state_);
    if (found == depth_states.end()) {
      *enable_out = false;
      *write_out = false;
      *function_out = 0;
      return;
    }
    *enable_out = found->second.depth_enable != 0;
    *write_out = found->second.depth_write_enable != 0;
    *function_out = static_cast<uint32_t>(found->second.depth_function);
  }

  // The factors and functions of the currently bound BlendState, in XNA's own
  // enums. Defaults to Opaque (source One, destination Zero, Add) when no state
  // is bound, which is what the draw path used for everything before.
  void CurrentBlendState(uint32_t* color_src, uint32_t* color_dst,
                         uint32_t* color_op, uint32_t* alpha_src,
                         uint32_t* alpha_dst, uint32_t* alpha_op) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    BlendSettings state;
    auto found = blend_states.find(blend_state_);
    if (found != blend_states.end()) {
      state = found->second;
    }
    *color_src = state.color_src;
    *color_dst = state.color_dst;
    *color_op = state.color_op;
    *alpha_src = state.alpha_src;
    *alpha_dst = state.alpha_dst;
    *alpha_op = state.alpha_op;
  }

  void CurrentColorWriteMasks(uint32_t* masks_out) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    BlendSettings state;
    auto found = blend_states.find(blend_state_);
    if (found != blend_states.end()) {
      state = found->second;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      masks_out[i] = state.color_write[i];
    }
  }

  // The XNA SurfaceFormat of each bound slot. A deferred G-buffer binds targets
  // of different formats at once, so one format for all of them is wrong.
  void RenderTargetFormats(uint32_t* formats_out, uint32_t count) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    for (uint32_t i = 0; i < count; ++i) {
      formats_out[i] = 0;
      if (i >= render_target_count_) {
        continue;
      }
      auto found = textures.find(render_targets_[i]);
      if (found != textures.end()) {
        formats_out[i] = found->second.format;
      }
    }
  }

  bool CurrentRenderTarget(xe::kernel::xna::XnaGpuTarget* out) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    if (!render_target_count_) {
      return false;
    }
    auto found = textures.find(render_targets_[0]);
    if (found == textures.end() || !found->second.width) {
      return false;
    }
    out->width = found->second.width;
    out->height = found->second.height;
    out->format = found->second.format;
    auto* resource =
        found->second.handle
            ? xe::kernel::xna::XnaGuestResourceLookup(found->second.handle)
            : nullptr;
    out->guest_address = resource ? resource->data : 0;
    return true;
  }

  void RenderTargetAddresses(uint32_t* addresses_out, uint32_t count) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    for (uint32_t i = 0; i < count; ++i) {
      addresses_out[i] = 0;
      if (i >= render_target_count_) {
        continue;
      }
      auto found = textures.find(render_targets_[i]);
      if (found == textures.end() || !found->second.handle) {
        continue;
      }
      auto* resource =
          xe::kernel::xna::XnaGuestResourceLookup(found->second.handle);
      addresses_out[i] = resource ? resource->data : 0;
    }
  }

  xe::kernel::xna::HlcbViewport CurrentViewport() {
    std::lock_guard<std::mutex> lock(resource_mutex);
    return viewport_;
  }

  // Every register the title has set so far, for every draw. Nothing is
  // consumed here: the next draw needs the same transform this one used.
  std::vector<xe::kernel::xna::XnaGpuConstant> CurrentConstants() {
    std::lock_guard<std::mutex> lock(resource_mutex);
    std::vector<xe::kernel::xna::XnaGpuConstant> current;
    // Flattened the way the register file is laid out: the vertex bank is
    // c0..c255 and the pixel bank c256..c511, which is exactly where
    // SHADER_CONSTANT_000 and SHADER_CONSTANT_256 sit.
    for (uint32_t bank = 0; bank < kConstantBanks; ++bank) {
      for (uint32_t i = 0; i < kMaxConstantRegisters; ++i) {
        if (!constant_register_set_[bank][i]) {
          continue;
        }
        xe::kernel::xna::XnaGpuConstant constant;
        constant.register_index = bank * kMaxConstantRegisters + i;
        std::memcpy(constant.value, constant_registers_[bank][i],
                    sizeof(constant.value));
        current.push_back(constant);
      }
    }
    return current;
  }

  uint32_t ViewportWidth() const {
    return static_cast<uint32_t>(viewport_.width);
  }
  uint32_t ViewportHeight() const {
    return static_cast<uint32_t>(viewport_.height);
  }

  // What the title bound to a sampler slot, for the readback path
  // (GraphicsDevice.Textures[i]). Zero means nothing is bound there.
  uint32_t BoundTexture(uint32_t slot) {
    std::lock_guard<std::mutex> lock(resource_mutex);
    if (slot >= kMaxTextures) {
      return 0;
    }
    return bound_textures_[slot];
  }

 private:
  struct Matrix {
    float m[16] = {};
    bool transposed = false;
  };

  std::map<uint32_t, Matrix> matrices_;
  uint32_t last_matrix_ = 0;

  // How many float constant registers a parameter occupies.
  //
  // D3DXPARAMETER_CLASS is 0 SCALAR, 1 VECTOR, 2 MATRIX_ROWS,
  // 3 MATRIX_COLUMNS, 4 OBJECT, 5 STRUCT. Only OBJECT - a texture or a sampler
  // - occupies no registers.
  //
  // This returned zero for class 3, on the belief that 3 meant the object
  // class. Class 3 is a column-major matrix, which is what every transform in
  // this title is, so World, View, Projection and WorldViewProj were each given
  // no registers and dropped before they were ever written. Every parameter
  // declared after one was then off by the registers the matrix should have
  // taken. The draws were correct and every vertex was being transformed by an
  // all-zero matrix.
  static uint32_t RegistersFor(
      const xe::kernel::xna::EffectParameterInfo& info) {
    const uint32_t elements = info.elements ? info.elements : 1;
    switch (info.parameter_class) {
      case 4:  // OBJECT - texture or sampler, no constant registers.
        return 0;
      case 2:  // MATRIX_ROWS - one register per row.
        return elements * (info.rows ? info.rows : 1);
      case 3:  // MATRIX_COLUMNS - one register per column.
        return elements * (info.columns ? info.columns : 1);
      default:  // SCALAR and VECTOR both fit in a single register.
        return elements;
    }
  }

  // The ALU constant file, not a queue of pending writes. A title sets a
  // parameter once and then draws with it many times, so a constant has to
  // survive the draw that first used it - draining these left every draw after
  // the first transforming its vertices by zero.

  // Every parameter value the title has set, by name, kept until a shader that
  // uses it says which register it belongs in.
  std::map<std::string, std::vector<float>> parameter_values_;
  std::map<std::string, bool> parameter_transposed_;
  std::map<std::string, uint32_t> parameter_class_;
  // The shape of the value, kept because the draw has to lay it out again once
  // it knows the mapping: how many elements arrived, how many the parameter
  // declares, and how many floats apart consecutive elements are.
  std::map<std::string, uint32_t> parameter_elements_;
  std::map<std::string, uint32_t> parameter_declared_elements_;
  std::map<std::string, uint32_t> parameter_stride_;
  struct ParameterValue {
    std::vector<float> value;
    bool transposed = false;
    uint32_t parameter_class = 0;
    uint32_t elements = 1;
    uint32_t declared = 1;
    uint32_t stride = 4;
  };
  std::map<uint32_t, std::map<std::string, ParameterValue>> parameters_by_effect_;

  // TWO CONSTANT FILES, NOT ONE.
  //
  // Xenos gives each stage its own bank of 256 float registers - the vertex
  // shader reads SHADER_CONSTANT_000 upward, the pixel shader SHADER_CONSTANT_256
  // upward - and D3DX allocates each shader's parameters from c0 of its own bank.
  // Sharing one array let a pixel shader's _FogStartDist_And_EndDistInv, which
  // genuinely sits at c0 of the pixel bank, land on top of the vertex shader's
  // _View, and sent every pixel constant to a bank the pixel shader cannot see.
  static constexpr uint32_t kMaxConstantRegisters = 256;
  static constexpr uint32_t kConstantBanks = 2;
  float constant_registers_[kConstantBanks][kMaxConstantRegisters][4] = {};
  bool constant_register_set_[kConstantBanks][kMaxConstantRegisters] = {};

  void ResolveRenderTarget(uint32_t handle, uint32_t index,
                           uint32_t target_count) {
    xe::kernel::xna::XnaGpuTarget target;
    {
      std::lock_guard<std::mutex> lock(resource_mutex);
      auto found = textures.find(handle);
      if (found == textures.end() || !found->second.width ||
          !found->second.handle) {
        // Silent until now. If a G-buffer target never resolves, every pass
        // that samples it reads whatever its guest memory last held, and the
        // frame composites to nothing with no other trace.
        XELOGD(
            "[xna] render target {:08X} slot {} not resolved: {}", handle,
            index,
            found == textures.end()
                ? "no texture record"
                : (!found->second.width ? "zero width" : "no guest handle"));
        return;
      }
      auto* resource =
          xe::kernel::xna::XnaGuestResourceLookup(found->second.handle);
      if (!resource || !resource->data) {
        XELOGD(
            "[xna] render target {:08X} slot {} not resolved: guest resource "
            "{:08X} has no memory",
            handle, index, found->second.handle);
        return;
      }
      target.width = found->second.width;
      target.height = found->second.height;
      target.format = found->second.format;
      target.guest_address = resource->data;
      ++found->second.resolves;
    }
    // The same tile the draw wrote this slot to - see XnaGpuEdramBaseForSlot.
    const uint32_t edram =
        xe::kernel::xna::XnaGpuEdramBaseForSlot(target.width, target.height,
                                                index, target_count);
    XELOGD(
        "[xna] resolving render target {:08X} slot {}: {}x{} XNA format {} "
        "from edram {} to guest {:08X}",
        handle, index, target.width, target.height, target.format, edram,
        target.guest_address);
    xe::kernel::xna::XnaGpuResolveTarget(target, edram);

    if (xe::logging::ShouldLog(xe::LogLevel::Debug)) {
      CountSettledResolves();
      auto* processor = HostedCommandProcessor();
      if (pending_resolves_.size() < 64) {
        pending_resolves_.push_back(
            {target.guest_address, target.width * target.height * 4,
             processor ? processor->GetCurrentSubmission() : 0});
      }
    }
  }

  void CountSettledResolves() {
    auto* processor = HostedCommandProcessor();
    auto* state = xe::kernel::kernel_state();
    auto* memory = state ? state->memory() : nullptr;
    if (!processor || !memory) {
      return;
    }
    while (!pending_resolves_.empty()) {
      const SettledResolve entry = pending_resolves_.front();
      uint32_t coloured = 0;
      uint32_t non_zero = 0;
      uint32_t first = 0;
      const auto counted = processor->CountResolvedPixels(
          memory->GetPhysicalAddress(entry.address), entry.bytes, &coloured,
          &non_zero, &first);
      if (counted ==
          xe::gpu::d3d12::D3D12CommandProcessor::ResolveCount::kNotReady) {
        return;
      }
      pending_resolves_.pop_front();
      if (counted ==
          xe::gpu::d3d12::D3D12CommandProcessor::ResolveCount::kNoContract) {
        XELOGD("[xna]    settled {:08X}: no resolve contract covers it",
               entry.address);
        continue;
      }
      XELOGD(
          "[xna]    settled {:08X}: {} of {} pixels coloured, {} non-zero, "
          "first {:08X}",
          entry.address, coloured, entry.bytes / 4, non_zero, first);
    }
  }

  struct SettledResolve {
    uint32_t address = 0;
    uint32_t bytes = 0;
    uint64_t submission = 0;
  };
  std::deque<SettledResolve> pending_resolves_;
  uint32_t collect_calls_ = 0;

  uint32_t render_targets_[4] = {};
  uint32_t render_target_count_ = 0;

  static constexpr uint32_t kMaxTextures = 32;

  xe::kernel::xna::HlcbClear last_clear_;
  xe::kernel::xna::HlcbViewport viewport_;
  xe::kernel::xna::HlcbStreamSource streams_[kMaxStreams];
  uint32_t bound_textures_[kMaxTextures] = {};
  // Textures bound through an effect parameter, kept by name because the slot
  // is the running shader's business - see EffectTexture.
  std::map<std::string, uint32_t> texture_by_name_;
  std::map<uint32_t, std::map<std::string, uint32_t>> texture_by_effect_;
  uint32_t index_buffer_ = 0;
  uint32_t declaration_ = 0;
  uint32_t blend_state_ = 0;
  uint32_t depth_state_ = 0;
  static constexpr uint32_t kUserRingBytes = 8u * 1024u * 1024u;
  uint32_t user_stream_handle_ = 0;
  uint32_t user_ring_offset_ = 0;
  bool user_stream_active_ = false;
  xe::kernel::xna::HlcbStreamSource saved_stream_{};
  uint32_t rasterizer_state_ = 0;
  static constexpr uint32_t kMaxSamplers = 32;
  uint32_t sampler_states_[kMaxSamplers] = {};

  uint64_t clears_ = 0;
  uint64_t draws_ = 0;
  uint64_t primitives_ = 0;
  uint64_t sprite_draws_ = 0;
  uint64_t sprite_bytes_ = 0;
  uint64_t empty_stream_draws_ = 0;
  std::map<uint32_t, uint64_t> other_;
};

DeviceSink device_sink;

}  // namespace

static xe::gpu::d3d12::D3D12CommandProcessor* HostedCommandProcessor() {
  auto* state = xe::kernel::kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  auto* graphics = emulator ? emulator->graphics_system() : nullptr;
  if (!graphics) {
    return nullptr;
  }
  return dynamic_cast<xe::gpu::d3d12::D3D12CommandProcessor*>(
      graphics->command_processor());
}

static xe::gpu::Shader* LoadHostedShader(
    xe::gpu::d3d12::D3D12CommandProcessor* processor,
    xe::gpu::xenos::ShaderType type, const uint32_t* ucode,
    uint32_t dword_count) {
  if (processor) {
    return processor->HostedLoadShader(type, ucode, dword_count);
  }
  return xe::kernel::xna::XnaDirectLoadShader(type, ucode, dword_count);
}

static void SetActiveHostedShaders(
    xe::gpu::d3d12::D3D12CommandProcessor* processor,
    xe::gpu::Shader* vertex_shader, xe::gpu::Shader* pixel_shader) {
  if (processor) {
    processor->HostedSetActiveShaders(vertex_shader, pixel_shader);
    return;
  }
  xe::kernel::xna::XnaDirectSetActiveShaders(vertex_shader, pixel_shader);
}

static xe::gpu::Shader* ActiveHostedShader(
    xe::gpu::d3d12::D3D12CommandProcessor* processor, bool vertex) {
  if (processor) {
    return vertex ? processor->active_vertex_shader()
                  : processor->active_pixel_shader();
  }
  return xe::kernel::xna::XnaDirectActiveShader(vertex);
}

// EVERYTHING KNOWN ABOUT A SHADER, FOUND BY THE ONLY IDENTITY A DRAW CARRIES.
//
// A draw holds a xe::gpu::Shader*, and its ucode hash is what the pipeline
// cache and the draw breadcrumbs key on. The rest of what an effect knows about
// that shader - the literals compiled into it, the registers its parameters
// occupy - lived in the effect image, reachable only by an index into the
// effect that produced it. So the literals had to be pushed at apply time and
// hoped to still be right at the draw, and the constants of every shader in an
// effect shared one register file. Registering the data under the hash makes it
// reachable from the draw itself.
struct HostedShaderData {
  std::vector<float> literals;
  std::map<std::string, uint32_t> constant_registers;
  std::map<std::string, uint32_t> constant_counts;
  std::map<std::string, std::vector<float>> constant_defaults;
  // Sampler name to sampler register, for THIS shader. A texture bound by name
  // has no slot until a shader says so.
  std::map<std::string, uint32_t> sampler_registers;
  // Where this microcode was first seen, so a hash in a draw breadcrumb or a
  // register dump can be read as something other than sixteen hex digits.
  std::string origin;
};
static std::mutex shader_data_mutex;
static std::map<uint64_t, HostedShaderData> shader_data_by_hash;

static void RegisterShaderData(uint64_t hash,
                               const xe::kernel::xna::EffectShaderInfo& info,
                               const std::string& origin) {
  std::lock_guard<std::mutex> lock(shader_data_mutex);
  auto& entry = shader_data_by_hash[hash];
  if (entry.origin.empty()) {
    entry.origin = origin;
  }
  if (entry.literals.empty() && !info.literals.empty()) {
    entry.literals = info.literals;
  }
  // THE SAME MICROCODE APPEARS IN MANY OBJECTS.
  //
  // An effect declares a hundred shader objects and titles share them across
  // techniques and effects, so one hash is reached through many objects - and
  // the name walk succeeds on some of them and returns nothing on others. The
  // hash is the identity, so a mapping learned anywhere serves everywhere, and
  // an object that could not read its own names inherits them from one that
  // could instead of drawing with an empty constant file.
  // Samplers travel on their own. A shader can name textures and no float
  // constants at all - a blit or a composite pass is exactly that - and hanging
  // this off the constant map would leave those shaders sampling nothing.
  if (entry.sampler_registers.empty() && !info.sampler_registers.empty()) {
    entry.sampler_registers = info.sampler_registers;
    XELOGD("[xna] shader {:016X} ({}): took {} sampler mapping(s) from {}",
           hash, entry.origin, entry.sampler_registers.size(), origin);
  }
  if (entry.constant_registers.empty() && !info.constant_registers.empty()) {
    entry.constant_registers = info.constant_registers;
    entry.constant_counts = info.constant_counts;
    entry.constant_defaults = info.constant_defaults;
    XELOGI(
        "[xna] shader {:016X} ({}): took {} constant mapping(s), {} with a "
        "compiled default, from {}",
        hash, entry.origin, entry.constant_registers.size(),
        entry.constant_defaults.size(), origin);
  } else if (!entry.constant_registers.empty() &&
             info.constant_registers.empty()) {
    XELOGI("[xna] shader {:016X} ({}): {} reused the mapping already known",
           hash, entry.origin, origin);
  }
}

std::string xe::kernel::xna::HostedShaderOriginFor(uint64_t hash) {
  std::lock_guard<std::mutex> lock(shader_data_mutex);
  auto found = shader_data_by_hash.find(hash);
  return found == shader_data_by_hash.end() ? std::string("unknown")
                                            : found->second.origin;
}

const HostedShaderData* HostedShaderDataFor(uint64_t hash) {
  std::lock_guard<std::mutex> lock(shader_data_mutex);
  auto found = shader_data_by_hash.find(hash);
  return found == shader_data_by_hash.end() ? nullptr : &found->second;
}

// XNA numbers its usages differently from D3D9, and the shader signature speaks
// D3DDECLUSAGE. Position is 0 in both, which is exactly why this is easy to
// miss: everything else is wrong while the position fetch looks right.
uint32_t D3dUsageForXnaUsage(uint32_t usage) {
  switch (usage) {
    case 0: return 0;    // Position          -> POSITION
    case 1: return 10;   // Color             -> COLOR
    case 2: return 5;    // TextureCoordinate -> TEXCOORD
    case 3: return 3;    // Normal            -> NORMAL
    case 4: return 7;    // Binormal          -> BINORMAL
    case 5: return 6;    // Tangent           -> TANGENT
    case 6: return 2;    // BlendIndices      -> BLENDINDICES
    case 7: return 1;    // BlendWeight       -> BLENDWEIGHT
    case 8: return 12;   // Depth             -> DEPTH
    case 9: return 11;   // Fog               -> FOG
    case 10: return 4;   // PointSize         -> PSIZE
    case 11: return 13;  // Sample            -> SAMPLE
    case 12: return 8;   // TessellateFactor  -> TESSFACTOR
    default: return UINT32_MAX;
  }
}

// XNA VertexElementFormat to what the fetch instruction has to say: the Xenos
// format, whether the components are signed, and whether they are normalized
// into [0,1]/[-1,1] or kept as integers.
//
// TAKEN FROM THE CONSOLE, NOT DEDUCED. RuntimeHost's own converter at
// 0x8A1CC230 turns each XNA format into a packed token whose low six bits are
// the Xenos format, bit 8 is signed and bit 9 is "integer" (num_format_all,
// where zero means normalized). Every value below is that table read out of the
// binary - note that a switch jump table permutes the code blocks, so reading
// them in address order gives every format the wrong token.
//
// Color and Byte4 differ by bit 9 and NOTHING else: D3DCOLOR gets no component
// swizzle here, so no BGRA fixup belongs in this path.
bool XenosFormatForXnaFormat(uint32_t format, xe::gpu::xenos::VertexFormat* out,
                             bool* is_signed, bool* is_integer) {
  using xe::gpu::xenos::VertexFormat;
  // The console marks every float format signed AND integer, and that pairing
  // is not decoration - it is the "take these bits as they are" combination.
  // For the 32 bit INTEGER formats the translator emits IToF and then, if the
  // integer bit is clear, multiplies by 1/(2^31-1) and treats the same bits as
  // a repeating fraction. The _FLOAT variants take a different arm and read
  // neither flag, so today these values change nothing - but k_32_32_32_32 is
  // 35 and k_32_32_32_32_FLOAT is 38, so a three-off error in the table below
  // lands in the integer arm, and then the flags decide whether every position
  // arrives intact or divided by two billion. Matching the console costs
  // nothing and removes that trap.
  *is_signed = true;
  *is_integer = true;
  switch (format) {
    case 0: *out = VertexFormat::k_32_FLOAT; return true;           // Single
    case 1: *out = VertexFormat::k_32_32_FLOAT; return true;        // Vector2
    case 2: *out = VertexFormat::k_32_32_32_FLOAT; return true;     // Vector3
    case 3: *out = VertexFormat::k_32_32_32_32_FLOAT; return true;  // Vector4
    case 4:                                                         // Color
      *out = VertexFormat::k_8_8_8_8;
      *is_signed = false;
      *is_integer = false;
      return true;
    case 5:                                                         // Byte4
      *out = VertexFormat::k_8_8_8_8;
      *is_signed = false;
      return true;
    case 6: *out = VertexFormat::k_16_16; return true;              // Short2
    case 7: *out = VertexFormat::k_16_16_16_16; return true;        // Short4
    case 8:                                             // NormalizedShort2
      *out = VertexFormat::k_16_16;
      *is_integer = false;
      return true;
    case 9:                                             // NormalizedShort4
      *out = VertexFormat::k_16_16_16_16;
      *is_integer = false;
      return true;
    case 10: *out = VertexFormat::k_16_16_FLOAT; return true;        // Half2
    case 11: *out = VertexFormat::k_16_16_16_16_FLOAT; return true;  // Half4
    default: return false;
  }
}

// How many bytes one element of each XNA vertex format occupies.
uint32_t XnaVertexElementBytes(uint32_t format) {
  switch (format) {
    case 0: return 4;    // Single
    case 1: return 8;    // Vector2
    case 2: return 12;   // Vector3
    case 3: return 16;   // Vector4
    case 4: return 4;    // Color
    case 5: return 4;    // Byte4
    case 6: return 4;    // Short2
    case 7: return 8;    // Short4
    case 8: return 4;    // NormalizedShort2
    case 9: return 8;    // NormalizedShort4
    case 10: return 4;   // HalfVector2
    case 11: return 8;   // HalfVector4
    default: return 0;
  }
}

// Fills a copy of the microcode's blank vfetch instructions in from the
// declaration, the way the console runtime would have.
//
// A fetch with no matching element is left blank and reported rather than
// failing the whole shader: D3D9 leaves an input the declaration does not
// supply undefined, it does not refuse the draw. Only POSITION is worth
// refusing over, because without it there is no geometry to place. Bailing on
// any mismatch left four effects - the sprite and fullscreen-quad ones - with
// every fetch blank when only one input was actually missing.
constexpr uint32_t kNullVertexFetchConstant = 80;

bool PatchVertexFetches(
    std::vector<uint32_t>* ucode,
    const std::vector<xe::kernel::xna::EffectFetchSemantic>& semantics,
    const std::vector<XnaVertexElement>& elements,
    const uint32_t* stream_strides, uint32_t stream_count,
    const std::string& origin) {
  if (semantics.empty() || elements.empty()) {
    return false;
  }

  // THE STREAM STRIDE IS NOT ALWAYS KNOWN YET.
  //
  // DrawUserPrimitives hands its vertices over directly and never sets a stream
  // source, so at this point stride is zero and every fetch was being refused -
  // which is why the sprite and fullscreen-quad effects stayed blank while the
  // mesh ones patched. Where the stream says nothing, the declaration implies
  // it: the vertex is as long as its furthest element reaches.
  uint32_t strides[8] = {};
  const uint32_t usable_streams =
      std::min<uint32_t>(stream_count, uint32_t(std::size(strides)));
  for (uint32_t i = 0; i < usable_streams; ++i) {
    strides[i] = stream_strides[i];
  }
  for (const auto& element : elements) {
    if (element.stream >= usable_streams || strides[element.stream]) {
      continue;
    }
    uint32_t implied = 0;
    for (const auto& other : elements) {
      if (other.stream != element.stream) {
        continue;
      }
      implied = std::max(implied,
                         other.offset + XnaVertexElementBytes(other.format));
    }
    strides[element.stream] = (implied + 3) & ~3u;
  }

  bool placed_any = false;
  for (const auto& semantic : semantics) {
    const XnaVertexElement* match = nullptr;
    for (const auto& element : elements) {
      if (element.usage_index != semantic.usage_index) {
        continue;
      }
      if (D3dUsageForXnaUsage(element.usage) == semantic.usage) {
        match = &element;
        break;
      }
    }
    if (!match || match->stream >= usable_streams) {
      std::string have;
      for (const auto& element : elements) {
        have += fmt::format("{}({}) ", element.usage,
                            D3dUsageForXnaUsage(element.usage));
      }
      XELOGW(
          "[xna] {}: no vertex element for d3d usage {} index {} - the "
          "shader's fetch at instruction {} reads zero; declaration has "
          "xna(d3d) {}",
          origin, semantic.usage, semantic.usage_index, semantic.instruction,
          have);
      const uint32_t blank_at = semantic.instruction * 3;
      if (blank_at + 3 <= ucode->size()) {
        uint32_t* blank_words = ucode->data() + blank_at;
        blank_words[0] &= ~(uint32_t(0x7F) << 20);
        blank_words[0] |= (kNullVertexFetchConstant / 3) << 20;
        blank_words[0] |= (kNullVertexFetchConstant % 3) << 25;
        blank_words[1] &= ~((uint32_t(0x3F) << 16) | (uint32_t(1) << 12) |
                            (uint32_t(1) << 13));
        blank_words[1] |= uint32_t(xe::gpu::xenos::VertexFormat::k_32_32_32_32_FLOAT)
                          << 16;
        blank_words[2] &= ~uint32_t(0x7FFFFFFF);
      }
      continue;
    }
    xe::gpu::xenos::VertexFormat format;
    bool is_signed = false;
    bool is_integer = false;
    if (!XenosFormatForXnaFormat(match->format, &format, &is_signed,
                                 &is_integer)) {
      XELOGW("[xna] vertex element format {} has no Xenos equivalent",
             match->format);
      continue;
    }
    const uint32_t stride_bytes = strides[match->stream];
    // Both are DWORD counts in the instruction, and both fields are too narrow
    // to hold a byte count - stride is eight bits, so a 64 byte vertex only
    // fits at all because it is stored as 16.
    if (!stride_bytes || (stride_bytes & 3) || (match->offset & 3) ||
        stride_bytes / 4 > 255) {
      XELOGW("[xna] stream {} stride {} offset {} cannot be expressed as dwords",
             match->stream, stride_bytes, match->offset);
      continue;
    }
    const uint32_t at = semantic.instruction * 3;
    if (at + 3 > ucode->size()) {
      continue;
    }
    uint32_t* words = ucode->data() + at;
    const uint32_t original_swizzle = words[1] & 0xFFF;
    // WHICH FETCH CONSTANT - AND IT MUST NOT BE A LOW ONE.
    //
    // Vertex and texture fetch constants share one 192 dword file: vertex
    // constant N is dwords N*2..N*2+1, texture constant M is dwords M*6..M*6+5.
    // Putting a vertex stream at constant 0 lands it inside TEXTURE constant 0,
    // which every pixel shader here samples. The console's own blank fetches
    // are constant 95 for exactly this reason - vertex constants are handed out
    // from the top of the file, textures from the bottom - so streams take 95
    // downward and stay clear of the sixteen texture constants in use.
    const uint32_t fetch_constant = 95 - match->stream;
    words[0] &= ~(uint32_t(0x7F) << 20);
    words[0] |= (fetch_constant / 3) << 20;
    words[0] |= (fetch_constant % 3) << 25;
    // dst_swiz takes the low twelve bits, then fomat_comp_all at 12,
    // num_format_all at 13, signed_rf_mode_all at 14, is_index_rounded at 15,
    // and format is the six bits from 16. Signedness is bit 12; bit 13 set
    // means integer, because num_format_all reads 0 as normalized.
    words[1] &= ~((uint32_t(0x3F) << 16) | (uint32_t(1) << 12) |
                  (uint32_t(1) << 13));
    words[1] |= uint32_t(format) << 16;
    words[1] |= (is_signed ? 1u : 0u) << 12;
    words[1] |= (is_integer ? 1u : 0u) << 13;
    uint32_t decl_type = 0;
    if (xe::kernel::xna::XnaVertexElementFormatToDeclType(match->format,
                                                         &decl_type)) {
      const uint32_t element_swizzle = (decl_type >> 10) & 0xFFF;
      const uint32_t compiled_swizzle = words[1] & 0xFFF;
      uint32_t swizzle = 0;
      for (uint32_t c = 0; c < 4; ++c) {
        uint32_t select = (compiled_swizzle >> (3 * c)) & 7;
        if (select < 4) {
          select = (element_swizzle >> (3 * select)) & 7;
        }
        swizzle |= select << (3 * c);
      }
      words[1] = (words[1] & ~uint32_t(0xFFF)) | swizzle;
    }
    words[2] &= ~uint32_t(0x7FFFFFFF);
    words[2] |= stride_bytes / 4;
    words[2] |= (match->offset / 4) << 8;
    placed_any = true;
    XELOGI(
        "[xna] {}: fetch at instruction {} d3d usage {} index {} <- stream {} "
        "offset {} xna format {} usage {}, stride {}, xenos format {} signed {} "
        "integer {}, swizzle {:03X} -> {:03X}",
        origin, semantic.instruction, semantic.usage, semantic.usage_index,
        match->stream, match->offset, match->format, match->usage,
        stride_bytes, uint32_t(format), is_signed, is_integer,
        original_swizzle, words[1] & 0xFFF);
  }
  return placed_any;
}

// The declaration currently bound, unpacked. Empty if nothing is bound or the
// blob is not a whole number of elements.
std::vector<XnaVertexElement> CurrentVertexElements(uint32_t* handle_out) {
  std::vector<XnaVertexElement> elements;
  *handle_out = device_sink.VertexDeclaration();
  const uint32_t handle =
      xe::kernel::xna::XnaGuestResolveHandle(*handle_out);
  if (!handle) {
    return elements;
  }
  auto* resource = xe::kernel::xna::XnaGuestResourceLookup(handle);
  if (!resource || !resource->size ||
      resource->size % sizeof(XnaVertexElement)) {
    XELOGD(
        "[xna] declaration {:08X} (resolved {:08X}): no usable elements - "
        "resource {}, size {}",
        *handle_out, handle, resource ? "found" : "MISSING",
        resource ? resource->size : 0);
    return elements;
  }
  const auto* data = reinterpret_cast<const XnaVertexElement*>(
      xe::kernel::xna::XnaGuestResourceData(handle));
  if (!data) {
    return elements;
  }
  elements.resize(resource->size / sizeof(XnaVertexElement));
  CopyGuest(elements.data(), data, resource->size, uint32_t(sizeof(uint32_t)));

  // A declaration cannot name the same usage twice - D3D rejects it outright -
  // so two identical elements mean this memory is not a declaration any more.
  // Refusing here turns silent corruption into a stated one: patching from a
  // zeroed declaration gives every fetch usage POSITION and format Single, and
  // a shader that quietly fetches a single float per vertex looks exactly like
  // a shader that was never patched at all.
  bool distinct = true;
  for (size_t i = 1; i < elements.size() && distinct; ++i) {
    for (size_t j = 0; j < i && distinct; ++j) {
      distinct = elements[i].usage != elements[j].usage ||
                 elements[i].usage_index != elements[j].usage_index;
    }
  }
  if (!distinct) {
    XELOGE(
        "[xna] declaration {:08X} at guest {:08X} has repeated usages - it has "
        "been overwritten since it was created, refusing to patch from it",
        *handle_out, resource->data);
    elements.clear();
  }
  return elements;
}

// The pass names its vertex and pixel shader; the microcode is the title's own,
// and goes to the same pipeline cache a guest draw would use.
// APPLY AND DRAW ARE ON DIFFERENT CLOCKS.
//
// The console applies a pass by putting an EffectApply packet (type 22) in the
// command buffer, so on real hardware the apply, the declaration and the draw
// are all in one stream and arrive in order. Here the apply is a direct call
// that fires immediately while SetVertexDeclaration and SetStreamSource travel
// in the packet buffer and land whenever it is flushed - so at apply time the
// declaration belongs to some earlier draw, or to nothing at all.
//
// That is why binding the shaders at apply produced two variants of the same
// shader, one patched and one blank, and why forty-seven draws ran the blank
// one. The pass is only remembered here; the shaders are bound at the draw,
// which is the first moment the declaration is the one this draw will use.
static std::mutex applied_pass_mutex;
static uint32_t applied_effect_ = 0;
static uint32_t applied_pass_ = 0;

// Patching and loading is not free - the pipeline cache hashes the whole
// microcode, and at two shaders a draw that is tens of megabytes of hashing a
// frame. The result only changes when the pass or the vertex layout does, so
// the last one is kept and simply made active again.
struct BoundPass {
  uint32_t effect = 0;
  uint32_t pass = UINT32_MAX;
  uint32_t declaration = 0;
  uint32_t strides[4] = {};
  uint32_t vertex_index = UINT32_MAX;
  uint32_t pixel_index = UINT32_MAX;
  xe::gpu::Shader* vertex = nullptr;
  xe::gpu::Shader* pixel = nullptr;
};
static BoundPass last_bound_;

void BindPassShaders(uint32_t effect_handle, uint32_t pass);

extern "C" void Nexia_XnaEffectApply(uint32_t effect_handle, uint32_t pass) {
  auto* processor = HostedCommandProcessor();
  auto* image = xe::kernel::xna::FindEffect(effect_handle);
  if ((!processor && !xe::kernel::xna::XnaDirectActive()) || !image) {
    return;
  }

  xe::kernel::xna::EffectPassInfo* selected = nullptr;
  if (image->current_technique < image->techniques.size()) {
    auto& technique = image->techniques[image->current_technique];
    if (pass < technique.passes.size()) {
      selected = &technique.passes[pass];
    }
  }
  if (!selected) {
    uint32_t seen = 0;
    for (auto& technique : image->techniques) {
      for (auto& entry : technique.passes) {
        if (seen++ == pass) {
          selected = &entry;
          break;
        }
      }
      if (selected) {
        break;
      }
    }
  }
  if (!selected) {
    return;
  }

  // The microcode of the two objects this pass binds is analyzed here, the
  // first time the title actually applies it - not when the effect loaded.
  xe::kernel::xna::ResolvePassShadersOnUse(
      image, selected,
      [effect_handle](const std::string& name, uint32_t reg, float* out) {
        return device_sink.LookupParameter(effect_handle, name, reg, out);
      });
  {
    std::lock_guard<std::mutex> lock(applied_pass_mutex);
    applied_effect_ = effect_handle;
    applied_pass_ = pass;
  }
}

// Patches this pass's shaders against the declaration in force and hands them
// to the pipeline cache, which also makes them the active pair. Called from the
// draw, after the packet buffer has been flushed.
void BindPassShaders(uint32_t effect_handle, uint32_t pass) {
  auto* processor = HostedCommandProcessor();
  auto* image = xe::kernel::xna::FindEffect(effect_handle);
  if ((!processor && !xe::kernel::xna::XnaDirectActive()) || !image) {
    return;
  }
  xe::kernel::xna::EffectPassInfo* selected = nullptr;
  if (image->current_technique < image->techniques.size()) {
    auto& technique = image->techniques[image->current_technique];
    if (pass < technique.passes.size()) {
      selected = &technique.passes[pass];
    }
  }
  if (!selected) {
    uint32_t seen = 0;
    for (auto& technique : image->techniques) {
      for (auto& entry : technique.passes) {
        if (seen++ == pass) {
          selected = &entry;
          break;
        }
      }
      if (selected) {
        break;
      }
    }
  }
  if (!selected) {
    return;
  }

  // A shader is only complete once its fetches have been filled in from this,
  // so the patched program - not the one the effect stores - is what goes to
  // the pipeline cache. Its hash differs, which is exactly right: the same
  // shader against a different declaration is a different program.
  uint32_t declaration_handle = 0;
  const auto elements = CurrentVertexElements(&declaration_handle);
  uint32_t strides[DeviceSink::kMaxStreams];
  for (uint32_t i = 0; i < DeviceSink::kMaxStreams; ++i) {
    strides[i] = device_sink.StreamStride(i);
  }

  if (last_bound_.effect == effect_handle && last_bound_.pass == pass &&
      last_bound_.declaration == declaration_handle &&
      last_bound_.vertex_index == selected->vertex_shader_index &&
      last_bound_.pixel_index == selected->pixel_shader_index &&
      std::equal(std::begin(strides), std::end(strides),
                 std::begin(last_bound_.strides)) &&
      (last_bound_.vertex || last_bound_.pixel)) {
    SetActiveHostedShaders(processor, last_bound_.vertex, last_bound_.pixel);
    return;
  }
  last_bound_ = BoundPass();
  last_bound_.effect = effect_handle;
  last_bound_.pass = pass;
  last_bound_.vertex_index = selected->vertex_shader_index;
  last_bound_.pixel_index = selected->pixel_shader_index;
  last_bound_.declaration = declaration_handle;
  std::copy(std::begin(strides), std::end(strides),
            std::begin(last_bound_.strides));

  static std::map<std::array<uint32_t, 9>,
                  std::pair<xe::gpu::Shader*, xe::gpu::Shader*>>
      bound_passes;
  const std::array<uint32_t, 9> bound_key = {
      effect_handle, pass, declaration_handle,
      selected->vertex_shader_index, selected->pixel_shader_index,
      strides[0], strides[1], strides[2], strides[3]};
  auto bound = bound_passes.find(bound_key);
  if (bound != bound_passes.end()) {
    last_bound_.vertex = bound->second.first;
    last_bound_.pixel = bound->second.second;
    SetActiveHostedShaders(processor, last_bound_.vertex, last_bound_.pixel);
    return;
  }

  for (uint32_t stream = 0; stream < DeviceSink::kMaxStreams; ++stream) {
    bool used = false;
    for (const auto& element : elements) {
      used = used || element.stream == stream;
    }
    if (!used) {
      continue;
    }
    const auto head = device_sink.StreamHead(
        stream, std::max<uint32_t>(strides[stream], 16) * 2);
    std::string text;
    for (const uint8_t byte : head) {
      text += fmt::format("{:02X} ", byte);
    }
    XELOGI(
        "[xna] effect {} pass {}: declaration {:08X} stream {} stride {} "
        "first bytes {}",
        effect_handle, pass, declaration_handle, stream, strides[stream], text);
  }

  auto load = [&](uint32_t index, xe::gpu::xenos::ShaderType type) {
    if (index >= image->shaders.size()) {
      return;
    }
    const auto& info = image->shaders[index];
    if (!info.shader || info.shader->ucode_dword_count() == 0) {
      return;
    }
    const uint32_t* ucode = info.shader->ucode_dwords();
    uint32_t ucode_count =
        static_cast<uint32_t>(info.shader->ucode_dword_count());
    std::vector<uint32_t> patched;
    if (!info.fetch_semantics.empty() && !info.ucode.empty()) {
      patched = info.ucode;
      const bool patched_ok = PatchVertexFetches(
          &patched, info.fetch_semantics, elements, strides,
          DeviceSink::kMaxStreams,
          fmt::format("effect {} technique {} \"{}\" pass {}", effect_handle,
                      image->current_technique,
                      image->current_technique < image->techniques.size()
                          ? image->techniques[image->current_technique].name
                          : std::string("?"),
                      pass));
      if (!patched_ok) {
        // A vfetch left blank reads nothing, so the shader transforms a vertex
        // that was never loaded - the one outcome here worth saying out loud.
        XELOGW(
            "[xna] effect {} pass {}: {} vertex fetch(es) LEFT BLANK - none "
            "could be placed from declaration {:08X} with {} element(s)",
            effect_handle, pass, info.fetch_semantics.size(),
            declaration_handle, elements.size());
      }
      if (patched_ok) {
        ucode = patched.data();
        ucode_count = static_cast<uint32_t>(patched.size());
      }
    }
    xe::gpu::Shader* shader =
        LoadHostedShader(processor, type, ucode, ucode_count);
    if (type == xe::gpu::xenos::ShaderType::kVertex) {
      last_bound_.vertex = shader;
    } else {
      last_bound_.pixel = shader;
    }
    if (shader) {
      RegisterShaderData(
          shader->ucode_data_hash(), info,
          fmt::format("effect {} pass {} {} at byte {}", effect_handle, pass,
                      type == xe::gpu::xenos::ShaderType::kVertex ? "vertex"
                                                                  : "pixel",
                      info.dword_offset * 4));
    }
  };
  load(selected->vertex_shader_index, xe::gpu::xenos::ShaderType::kVertex);
  load(selected->pixel_shader_index, xe::gpu::xenos::ShaderType::kPixel);
  bound_passes[bound_key] = {last_bound_.vertex, last_bound_.pixel};
}

// The draw arrives with its arguments intact rather than packed into a packet
// and read back out. The state that goes with it was flushed by the caller, so
// the sink already holds the streams, declaration, textures and constants this
// draw is meant to use.
extern "C" void Nexia_XnaDraw(int32_t primitive_type, int32_t base_vertex,
                              int32_t min_vertex_index, int32_t num_vertices,
                              int32_t start_index, int32_t primitive_count,
                              int32_t indexed);

// SunBurn's post-process chain draws every full-frame quad through
// DrawUserPrimitives, which carries its vertices with the call instead of in a
// bound stream.
extern "C" void Nexia_XnaDrawUserPrimitives(int32_t primitive_type,
                                            int32_t primitive_count,
                                            const void* vertex_data,
                                            uint32_t vertex_data_size,
                                            uint32_t vertex_stride) {
  if (!device_sink.StageUserVertices(vertex_data, vertex_data_size,
                                     vertex_stride)) {
    XELOGW("[xna] user primitives dropped: {} byte(s) stride {}",
           vertex_data_size, vertex_stride);
    return;
  }
  const uint32_t vertices =
      vertex_stride ? vertex_data_size / vertex_stride : 0;
  Nexia_XnaDraw(primitive_type, 0, 0, static_cast<int32_t>(vertices), 0,
                primitive_count, 0);
  device_sink.ReleaseUserVertices();
}

extern "C" void Nexia_XnaDrawUserIndexedPrimitives(
    int32_t primitive_type, int32_t num_vertices, int32_t primitive_count,
    const void* vertex_data, uint32_t vertex_data_size, const void* index_data,
    uint32_t index_data_size, uint32_t vertex_stride, uint32_t sixteen_bit) {
  if (!device_sink.StageUserVertices(vertex_data, vertex_data_size,
                                     vertex_stride)) {
    XELOGW("[xna] user indexed primitives dropped: {} vertex byte(s) stride {}",
           vertex_data_size, vertex_stride);
    return;
  }
  uint32_t start_index = 0;
  if (!device_sink.StageUserIndices(index_data, index_data_size,
                                    sixteen_bit != 0, &start_index)) {
    XELOGW("[xna] user indexed primitives dropped: {} index byte(s)",
           index_data_size);
    device_sink.ReleaseUserVertices();
    return;
  }
  Nexia_XnaDraw(primitive_type, 0, 0, num_vertices,
                static_cast<int32_t>(start_index), primitive_count, 1);
  device_sink.ReleaseUserIndices();
  device_sink.ReleaseUserVertices();
}

extern "C" void Nexia_XnaDraw(int32_t primitive_type, int32_t base_vertex,
                              int32_t min_vertex_index, int32_t num_vertices,
                              int32_t start_index, int32_t primitive_count,
                              int32_t indexed) {
  xe::kernel::xna::HlcbDraw draw;
  draw.indexed = indexed != 0;
  draw.primitive_type = static_cast<uint32_t>(primitive_type);
  draw.primitive_count = static_cast<uint32_t>(primitive_count);
  draw.base_vertex = static_cast<uint32_t>(base_vertex);
  draw.min_vertex_index = static_cast<uint32_t>(min_vertex_index);
  draw.vertex_count = static_cast<uint32_t>(num_vertices);
  draw.start_index = static_cast<uint32_t>(start_index);
  draw.start_vertex = draw.indexed ? 0u : static_cast<uint32_t>(start_index);

  device_sink.Draw(draw);

  if (!cvars::xna_geometry) {
    return;
  }
  xe::kernel::xna::XnaGpuStream streams[4];
  const uint32_t stream_count = device_sink.CollectStreams(streams, 4);
  if (!stream_count) {
    XELOGD("[xna] draw dropped: no vertex streams bound");
    return;
  }

  xe::kernel::xna::XnaGpuDraw gpu;
  gpu.primitive_type = draw.primitive_type;
  gpu.primitive_count = draw.primitive_count;
  gpu.start_vertex = draw.start_vertex;
  gpu.base_vertex = draw.base_vertex;
  gpu.start_index = draw.start_index;
  gpu.indexed = draw.indexed;

  // The shaders this draw will run, and the literals they were compiled with,
  // written before the constants are taken - a snapshot made first would not
  // contain them.
  auto* processor = HostedCommandProcessor();
  if (processor || xe::kernel::xna::XnaDirectActive()) {
    // Bound HERE, not at the apply. The declaration and the streams reached the
    // sink through the packet buffer and are only guaranteed current now.
    uint32_t effect_handle = 0;
    uint32_t pass = 0;
    {
      std::lock_guard<std::mutex> lock(applied_pass_mutex);
      effect_handle = applied_effect_;
      pass = applied_pass_;
    }
    if (effect_handle) {
      BindPassShaders(effect_handle, pass);
    }
    gpu.vertex_shader = ActiveHostedShader(processor, true);
    gpu.pixel_shader = ActiveHostedShader(processor, false);
    for (auto* shader : {gpu.vertex_shader, gpu.pixel_shader}) {
      if (!shader) {
        continue;
      }
      const uint32_t bank =
          DeviceSink::BankFor(static_cast<xe::gpu::Shader*>(shader)->type());
      // The file is this shader's alone, so it starts empty. Inheriting the
      // last shader's registers is how a light shader came to read a projection
      // matrix out of the register its own table calls _FarClippingDistance.
      device_sink.ResetBank(bank);
      const uint64_t shader_hash =
          static_cast<xe::gpu::Shader*>(shader)->ucode_data_hash();
      const auto* data = HostedShaderDataFor(shader_hash);
      // Which branch this takes - whether the shader has data at all, whether
      // its mapping placed anything, whether the fallback fired - said outright
      // on every draw rather than guessed at from the first one.
      if (!data) {
        XELOGD("[xna] setup {:016X}: no registered data at draw time",
               shader_hash);
        continue;
      }
      if (!data->literals.empty()) {
        device_sink.SetShaderLiterals(data->literals, bank);
      }
      // The parameters the title set, into the registers THIS shader's own
      // constant table gives them - and no others.
      device_sink.ApplyStoredConstants(data->constant_registers,
                                       data->constant_counts,
                                       data->constant_defaults, bank,
                                       effect_handle);

      // THE TEST IS WHETHER THE REGISTERS THIS SHADER READS GOT FILLED.
      //
      // Counting what was written anywhere is not enough: a shader that
      // inherited a mapping writes registers it never reads, so the count is
      // non-zero while the four it actually uses stay blank - which is how
      // c0..c3 stayed NOT WRITTEN through two attempts at this. The lowest
      // register the shader reads is checked directly, and a transform goes
      // there if it is still empty.
      auto* as_shader = static_cast<xe::gpu::Shader*>(shader);
      if (!as_shader->is_ucode_analyzed()) {
        XELOGD("[xna] setup {:016X}: microcode not analyzed at draw time",
               shader_hash);
        continue;
      }
      const auto& map = as_shader->constant_register_map();
      uint32_t lowest = UINT32_MAX;
      for (uint32_t i = 0; i < 252; ++i) {
        if (map.float_bitmap[i / 64] & (uint64_t(1) << (i % 64))) {
          lowest = i;
          break;
        }
      }
      // Only a vertex shader with no table of its own. A pixel shader does not
      // transform anything, and a shader that stated where its transform goes
      // does not need one guessed on top of it.
      bool fell_back = false;
      if (bank == 0 && data->constant_registers.empty() &&
          lowest != UINT32_MAX && !device_sink.IsRegisterSet(lowest, bank)) {
        fell_back = device_sink.ApplyFallbackTransform(lowest, bank);
      }
      if (fell_back) {
        XELOGW(
            "[xna] setup {:016X} ({}): {} shader had no table of its own - a "
            "transform was placed at c{}",
            shader_hash, xe::kernel::xna::HostedShaderOriginFor(shader_hash),
            bank ? "pixel" : "vertex",
            lowest == UINT32_MAX ? -1 : int32_t(lowest));
      }
    }
  }

  gpu.constants = device_sink.CurrentConstants();
  xe::kernel::xna::XnaGpuTarget target;
  if (device_sink.CurrentRenderTarget(&target)) {
    gpu.target_width = target.width;
    gpu.target_height = target.height;
    gpu.target_count = device_sink.RenderTargetCount();
    device_sink.RenderTargetFormats(gpu.target_formats, 4);
    device_sink.RenderTargetAddresses(gpu.target_addresses, 4);
  }
  device_sink.CurrentDepthState(&gpu.depth_enable, &gpu.depth_write_enable,
                                &gpu.depth_function);
  device_sink.CurrentBlendState(&gpu.color_src, &gpu.color_dst, &gpu.color_op,
                                &gpu.alpha_src, &gpu.alpha_dst, &gpu.alpha_op);
  device_sink.CurrentColorWriteMasks(gpu.color_write);
  // Resolved against the PIXEL shader, which is what samples them. Its own
  // constant table says which register each sampler name occupies, and that
  // differs between the shaders of one effect.
  {
    // BOTH stages. A vertex shader samples textures too - this title's fetches
    // one at instruction 23 - and collecting only the pixel shader's map left
    // that slot with no descriptor at all.
    uint32_t drawing_effect = 0;
    {
      std::lock_guard<std::mutex> lock(applied_pass_mutex);
      drawing_effect = applied_effect_;
    }
    std::map<std::string, uint32_t> samplers;
    for (auto* shader : {gpu.pixel_shader, gpu.vertex_shader}) {
      if (!shader) {
        continue;
      }
      const auto* data = HostedShaderDataFor(
          static_cast<xe::gpu::Shader*>(shader)->ucode_data_hash());
      if (!data) {
        continue;
      }
      const uint32_t stage_base = shader == gpu.vertex_shader ? 16 : 0;
      for (const auto& entry : data->sampler_registers) {
        samplers.emplace(entry.first, entry.second + stage_base);
      }
    }
    device_sink.CollectTextures(gpu.textures,
                                xe::kernel::xna::XnaGpuDraw::kMaxTextureSlots,
                                samplers, drawing_effect);
  }
  gpu.viewport_width = device_sink.ViewportWidth();
  gpu.viewport_height = device_sink.ViewportHeight();
  {
    const xe::kernel::xna::HlcbViewport viewport =
        device_sink.CurrentViewport();
    gpu.viewport_x = viewport.x;
    gpu.viewport_y = viewport.y;
    gpu.viewport_min_depth = viewport.min_depth;
    gpu.viewport_max_depth = viewport.max_depth;
  }
  if (draw.indexed) {
    device_sink.IndexBuffer(&gpu.index_guest_address, &gpu.index_size_bytes,
                            &gpu.index_32bit);
    if (!gpu.index_guest_address) {
      XELOGD("[xna] draw dropped: indexed with no index buffer bound");
      return;
    }
  }
  xe::kernel::xna::XnaGpuIssueDraw(gpu, streams, stream_count);
}

std::string xe::kernel::xna::DescribeXnaDeviceState() {
  return device_sink.Describe();
}

static void XnaDrawSpritesDirect(int32_t count, const void* sprites,
                                 int32_t texture_width, int32_t texture_height) {
  if (count <= 0 || !sprites) {
    return;
  }
  xe::kernel::xna::HlcbSprites batch;
  batch.texture = 0;
  batch.count = static_cast<uint32_t>(count);
  batch.texture_width = texture_width;
  batch.texture_height = texture_height;
  batch.data = static_cast<const uint8_t*>(sprites);
  batch.data_size = batch.count * 56u;
  device_sink.DrawSprites(batch);
}

// InteropGetTexture(device, index, out TextureType, out TEXTURE_CREATION_PARAMS)
//
// THIS RETURNS THE HANDLE, NOT AN ERROR CODE.
//
// TextureCollection::get_Item compares the return against -1 and yields null
// for it, so 0xFFFFFFFF is how an empty slot is reported. Anything else is
// taken as a live handle: it is looked up with GetKnownResourceFromHandle and,
// when that finds nothing, a fresh Texture2D/3D/Cube is constructed around the
// handle and the creation params. The generated stub returned 0 - a perfectly
// valid handle as far as that caller is concerned - so every read of
// GraphicsDevice.Textures[i] handed the title a brand new 0x0 Texture2D built
// from zeroed creation params. SunBurn reads that collection to save and
// restore device state around its post-process passes.
//
// TEXTURE_CREATION_PARAMS is six sequential uint32s: width, height, depth,
// levels, format, isVideo.
extern "C" uint32_t xna_D3D_D3D_Texture_GetTexture(uint32_t device,
                                                   uint32_t index,
                                                   uint32_t* type_out,
                                                   uint32_t* params_out) {
  const uint32_t handle = device_sink.BoundTexture(index);
  if (!handle) {
    return UINT32_MAX;
  }

  std::lock_guard<std::mutex> lock(resource_mutex);
  auto found = textures.find(handle);
  if (found == textures.end()) {
    return UINT32_MAX;
  }
  const Texture& texture = found->second;

  if (type_out) {
    *type_out = texture.type;
  }
  if (params_out) {
    params_out[0] = texture.width;
    params_out[1] = texture.height;
    params_out[2] = texture.depth;
    params_out[3] = texture.levels;
    params_out[4] = texture.format;
    params_out[5] = texture.is_video ? 1 : 0;
  }
  return handle;
}

extern "C" uint32_t xna_D3D_D3D_Device_ReceivePackets(uint32_t device,
                                                      const uint8_t* packets,
                                                      uint32_t size) {
  xe::kernel::xna::CountPackets(size);
  xe::kernel::xna::HlcbDecoder::Walk(packets, size, &device_sink);
  return 0;
}

// ---- effects ----------------------------------------------------------------

// InteropCreateEffect(blob, size, flags, out EFFECT_DESC) - HANDLE, compared
// against 0xFFFFFFFF by the caller.
//
// The blob has already been through Nexia_XnaPrepareEffect at the top of
// Effect..ctor, which put it into host byte order and pulled its shaders out,
// so this looks the finished image up by content instead of parsing a 200KB
// container a second time. A blob that was never prepared is parsed here, which
// covers any path that reaches the console runtime without going through the
// managed hook.
extern "C" uint32_t xna_D3D_D3D_Effect_CreateHandle(const void* blob,
                                                    uint32_t size,
                                                    uint32_t flags,
                                                    uint32_t* out_desc) {
  const auto* data = static_cast<const uint8_t*>(blob);
  uint32_t handle = xe::kernel::xna::FindPreparedEffect(data, size);
  if (handle == 0) {
    // Nothing was prepared for this constructor, so the blob is taken as it
    // comes. It is NOT required to carry the console effect magic: what arrives
    // here is the inner D3DX effect the container wraps, signature 0xFEFF0901,
    // and demanding the outer magic of it rejected every effect that had
    // already been read correctly. The shaders are found the same way either
    // way - by decoding control flow - so no signature is needed to look.
    auto image = std::make_unique<xe::kernel::xna::EffectImage>();
    image->valid = true;
    image->size = size;
    handle = xe::kernel::xna::RegisterEffect(std::move(image));
  }

  // The blob handed to this call is the effect BODY - the container has already
  // been stepped over by Effect..ctor - so its D3DX header is read here, where
  // the bytes are, rather than back at the container.
  auto* image = xe::kernel::xna::FindEffect(handle);
  if (image && image->technique_count == 0 && data && size) {
    image->owned_body =
        std::make_shared<std::vector<uint8_t>>(data, data + size);
    const uint8_t* owned = image->owned_body->data();
    if (xe::kernel::xna::ParseD3dxEffect(owned, size, image)) {
      // The tables are what make the located microcode trustworthy: until a
      // pass names a shader, all the scan produced was bytes that decoded.
      if (xe::kernel::xna::WalkEffectTables(owned, size, image)) {
        // The tables say which object each pass binds; the object section says
        // where that microcode is. Only shaders named by a pass are analyzed.
        xe::kernel::xna::ParseEffectObjects(owned, size, image);
        xe::kernel::xna::BuildEffectShaderTables(owned, size, image);
        xe::kernel::xna::ResolvePassShaders(owned, size, image);
      }
    }
  }

  // EFFECT_DESC is two counts, Parameters and Techniques, and the title walks
  // them. Reporting the shader count as techniques would be a lie the title
  // would act on, so until the container's technique table is read these are
  // reported as they are actually known: none.
  // EFFECT_DESC is { Parameters, Techniques }. The constructor builds a
  // collection of each and then indexes technique 0, so these have to be the
  // real counts - which is why they are read rather than reported as none.
  if (out_desc) {
    out_desc[0] = image ? image->parameter_count : 0;
    out_desc[1] = image ? image->technique_count : 0;
  }
  return handle;
}

// InteropCloneEffect(device, source, out EFFECT_DESC) - HANDLE.
//
// A clone is a SEPARATE effect with the same contents. Effect..ctor(Effect) has
// no bytes to parse - it never sees the blob - so everything it needs must come
// back from here: a live handle, and a desc whose technique count is right. It
// immediately does CurrentTechnique = _techniques[0], and a defaulted stub made
// that index a collection built from zero techniques, so the property setter
// threw ArgumentNullException before the title had drawn anything.
//
// BuiltInEffectReader clones rather than constructs, so every BasicEffect a
// title loads from content arrives through here rather than through
// CreateHandle.
extern "C" uint32_t xna_D3D_D3D_Effect_CloneEffect(uint32_t device,
                                                   uint32_t source,
                                                   uint32_t* out_desc) {
  uint32_t source_handle = source;
  auto* original = xe::kernel::xna::FindEffect(source);
  if (!original) {
    source_handle = device;
    original = xe::kernel::xna::FindEffect(device);
  }
  if (!original) {
    return UINT32_MAX;
  }
  // Copied, not shared: the clone gets its own parameter values, and the two
  // are expected to drift apart the moment either is used.
  auto copy = std::make_unique<xe::kernel::xna::EffectImage>(*original);
  const uint32_t handle = xe::kernel::xna::RegisterEffect(std::move(copy));
  if (!handle) {
    return UINT32_MAX;
  }
  device_sink.CloneEffectState(source_handle, handle);
  auto* image = xe::kernel::xna::FindEffect(handle);
  if (out_desc) {
    out_desc[0] = image ? image->parameter_count : 0;
    out_desc[1] = image ? image->technique_count : 0;
  }
  return handle;
}

extern "C" void xna_D3D_D3D_Effect_ReleaseHandle(uint32_t device,
                                                 uint32_t handle) {}

// ---- effect reflection -------------------------------------------------------
//
// THE FIRST ARGUMENT IS THE DEVICE, NOT THE EFFECT. Read out of the callers:
//
//   GetParameter(device.pComPtr, D3DXPARAM_INPUT{effect, parent, index, type},
//                out desc, name, semantic)
//   GetTechnique(device.pComPtr, effect.pComPtr, index, out desc, name)
//   GetPass(device.pComPtr, effect.pComPtr, techniqueHandle, index,
//           out desc, name)
//
// The effect is the SECOND argument, and for GetParameter it is not an argument
// at all - it is the first word of the input struct. Looking the device handle
// up as an effect made every parameter fail, the collection come out empty, and
// BasicEffect throw a NullReferenceException at its first property set.
//
// All three return HANDLES, compared against 0xFFFFFFFF: EffectParameter and
// EffectTechnique both store the result straight into their _handle field. A
// technique handle is what GetPass is then given, so it has to round-trip.

namespace {

// A StringBuilder, marshalled as UTF-16. The caller allocates 260 characters.
//
// NOT ANSI, despite the P/Invoke declaring no CharSet at all - which is the
// trap, because unspecified reads as "Ansi" in the metadata and that is not
// what this runtime does. Writing bytes produced names that were correct in
// the native log and garbage in managed: "Texture" plus its terminator came
// back as the four-character string U+6554 U+7478 U+7275 U+0065, which is
// exactly those eight bytes re-read as UTF-16LE. Every EffectParameter
// therefore carried a name no lookup could match, Parameters["SpecularColor"]
// returned null, and BasicEffect threw on the first property it set.
//
// The names are ASCII, so widening is a zero extension, low byte first.
constexpr size_t kMaxNameChars = 255;

void WriteName(uint16_t* out, const std::string& name) {
  if (!out) {
    return;
  }
  const size_t length = std::min(name.size(), kMaxNameChars);
  for (size_t i = 0; i < length; ++i) {
    out[i] = static_cast<uint8_t>(name[i]);
  }
  out[length] = 0;
}

// The container stores D3DX class and type values; EFFECT_PARAMETER_DESC is
// declared with the XNA EffectParameterClass and EffectParameterType enums,
// which are NOT the same numbering. D3DX distinguishes row-major from
// column-major matrices and XNA does not, so everything above Matrix is shifted
// by one - passing the raw D3DX value through reports "Texture" as a Struct and
// "MatrixTransform" as an Object.
uint32_t ToXnaParameterClass(uint32_t d3dx_class) {
  switch (d3dx_class) {
    case 0: return 0;  // scalar
    case 1: return 1;  // vector
    case 2: return 2;  // matrix_rows    -> Matrix
    case 3: return 2;  // matrix_columns -> Matrix
    case 4: return 3;  // object
    case 5: return 4;  // struct
    default: return 3;
  }
}

// D3DX and XNA agree from Void through TextureCube, and then XNA simply stops:
// it has no name for a sampler or a shader. Those are reported as Void rather
// than as some other type that happens to be in range, because a wrong type
// here is a silent mis-set later.
uint32_t ToXnaParameterType(uint32_t d3dx_type) {
  return d3dx_type <= 9 ? d3dx_type : 0;
}

// Handles are one-based so that zero is never a live object and 0xFFFFFFFF
// stays "no such thing".
constexpr uint32_t kNoHandle = UINT32_MAX;

}  // namespace

extern "C" uint32_t xna_D3D_D3D_Effect_GetParameter(uint32_t device,
                                                    const uint32_t* input,
                                                    uint32_t* out_desc,
                                                    uint16_t* out_name,
                                                    uint16_t* out_semantic) {
  if (!input) {
    return kNoHandle;
  }
  // D3DXPARAM_INPUT is { handle, parentHandle, index, type } - the effect is
  // the first word, and only the index is used here, which holds ONLY while no
  // parameter has children. The deferred lighting effect breaks that:
  // _LightPos_Radius, _LightColor, _LightSpotDir and
  // _LightModel_Fill_SpotAng_InvSpotAng are each declared with ten elements, so
  // XNA builds a child collection for them and asks for the elements with the
  // parent set. Answering those with the top level parameter at the same index
  // hands back the wrong parameter entirely.
  // The effect is the first word of the input struct, which the x64 ABI passes
  // by reference at 16 bytes. If that does not resolve, the device argument is
  // tried instead - one of the two IS the effect, and which cannot be settled
  // by reading the caller alone.
  auto* image = xe::kernel::xna::FindEffect(input[0]);
  if (!image) {
    image = xe::kernel::xna::FindEffect(device);
  }
  const uint32_t index = input[2];

  if (input[1]) {
    // Silent unless it happens. If it does, every array element of the
    // lighting parameters resolved to the wrong parameter, which is why
    // SunBurn's Class47 skips its whole light upload and _LightCount is never
    // set - the selector then reads the effect's compiled-in 0 and picks light
    // shader 0 of 80.
    XELOGW(
        "[xna] GetParameter asked for a CHILD: parent {:08X} index {} on "
        "effect {:08X} - answered with the top level parameter",
        input[1], index, input[0]);
  }

  if (!image || index >= image->parameters.size()) {
    return kNoHandle;
  }
  const auto& parameter = image->parameters[index];

  if (out_desc) {
    // { Class, Type, Rows, Columns, Elements, Annotations, StructMembers },
    // passed through as the D3DX values the container stores.
    out_desc[0] = ToXnaParameterClass(parameter.parameter_class);
    out_desc[1] = ToXnaParameterType(parameter.type);
    out_desc[2] = parameter.rows;
    out_desc[3] = parameter.columns;
    out_desc[4] = parameter.elements;
    out_desc[5] = 0;
    out_desc[6] = 0;
  }
  WriteName(out_name, parameter.name);
  WriteName(out_semantic, std::string());

  // PER EFFECT, NOT GLOBAL. The previous trace was capped at four calls across
  // the whole run, which SpriteBatch exhausted before BasicEffect - the effect
  // that actually fails - logged anything at all. Counting per effect says how
  // many of its parameters the collection actually asked for, which is the
  // question: 26 calls means the collection is built and the name lookup is
  // what fails, one call means the loop stops after the first.
  {
    static std::mutex counts_mutex;
    static std::map<uint32_t, uint32_t> counts;
    std::lock_guard<std::mutex> lock(counts_mutex);
    const uint32_t seen = ++counts[input[0]];
    const bool last = index + 1 == image->parameters.size();
    if (seen <= 2 || last) {
      XELOGI("[xna] GetParameter effect {} index {} -> \"{}\" (call {} of {})",
             input[0], index, parameter.name, seen,
             image->parameters.size());
    }
  }
  return index + 1;
}

extern "C" uint32_t xna_D3D_D3D_Effect_GetTexture(
    uint32_t device, uint32_t effect, uint32_t parameter,
    uint32_t* texture_out, uint32_t* type_out,
    TextureCreationParams* params_out) {
  if (texture_out) {
    *texture_out = UINT32_MAX;
  }
  if (type_out) {
    *type_out = 0;
  }
  if (params_out) {
    *params_out = TextureCreationParams{};
  }
  auto* image = xe::kernel::xna::FindEffect(effect);
  if (!image || !parameter || parameter > image->parameters.size()) {
    return 0;
  }
  const uint32_t handle =
      device_sink.BoundEffectTexture(effect,
                                     image->parameters[parameter - 1].name);
  if (!handle) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(resource_mutex);
  auto found = textures.find(handle);
  if (found == textures.end()) {
    return 0;
  }
  const Texture& texture = found->second;
  if (texture_out) {
    *texture_out = handle;
  }
  if (type_out) {
    *type_out = texture.type;
  }
  if (params_out) {
    params_out->width = texture.width;
    params_out->height = texture.height;
    params_out->depth = texture.depth;
    params_out->levels = texture.levels;
    params_out->format = texture.format;
    params_out->is_video = texture.is_video ? 1 : 0;
  }
  return 0;
}

extern "C" uint32_t xna_D3D_D3D_Effect_GetValue(uint32_t device,
                                                uint32_t effect,
                                                uint32_t parameter,
                                                uint32_t type, void* data,
                                                uint32_t size,
                                                uint32_t count) {
  if (!data || !size) {
    return 0;
  }
  std::memset(data, 0, size);
  auto* image = xe::kernel::xna::FindEffect(effect);
  if (!image || !parameter || parameter > image->parameters.size()) {
    return 0;
  }
  std::vector<float> value;
  if (!device_sink.StoredParameterValue(
          effect, image->parameters[parameter - 1].name, &value)) {
    return 0;
  }
  const uint32_t floats = std::min<uint32_t>(uint32_t(value.size()), size / 4);
  constexpr uint32_t kBoolean = 0;
  constexpr uint32_t kInt32 = 1;
  constexpr uint32_t kBooleanArray = 8;
  constexpr uint32_t kInt32Array = 9;
  if (type == kBoolean || type == kInt32 || type == kBooleanArray ||
      type == kInt32Array) {
    auto* out = static_cast<int32_t*>(data);
    const bool boolean = type == kBoolean || type == kBooleanArray;
    for (uint32_t i = 0; i < floats; ++i) {
      out[i] = boolean ? (value[i] != 0.0f ? 1 : 0) : int32_t(value[i]);
    }
  } else {
    std::memcpy(data, value.data(), size_t(floats) * sizeof(float));
  }
  return 0;
}

extern "C" uint32_t xna_D3D_D3D_Effect_GetTechnique(uint32_t device,
                                                    uint32_t effect,
                                                    uint32_t index,
                                                    uint32_t* out_desc,
                                                    uint16_t* out_name) {
  auto* image = xe::kernel::xna::FindEffect(effect);
  if (!image) {
    image = xe::kernel::xna::FindEffect(device);
  }
  if (!image || index >= image->techniques.size()) {
    return kNoHandle;
  }
  const auto& technique = image->techniques[index];
  if (out_desc) {
    // { Passes, Annotations } - the pass count builds EffectPassCollection.
    out_desc[0] = static_cast<uint32_t>(technique.passes.size());
    out_desc[1] = 0;
  }
  WriteName(out_name, technique.name);
  // GetPass is handed this value back, so it has to identify the technique.
  return index + 1;
}

extern "C" uint32_t xna_D3D_D3D_Effect_GetPass(uint32_t device, uint32_t effect,
                                               uint32_t technique_handle,
                                               uint32_t index,
                                               uint32_t* out_desc,
                                               uint16_t* out_name) {
  auto* image = xe::kernel::xna::FindEffect(effect);
  if (!image) {
    image = xe::kernel::xna::FindEffect(device);
  }
  if (!image || technique_handle == 0) {
    return kNoHandle;
  }
  const uint32_t technique_index = technique_handle - 1;
  if (technique_index >= image->techniques.size()) {
    return kNoHandle;
  }
  auto& technique = image->techniques[technique_index];
  if (index >= technique.passes.size()) {
    return kNoHandle;
  }
  auto& pass = technique.passes[index];
  // A selector pass has no object until a draw evaluates its preshader, and
  // the preshader's inputs are not set at construction - running it here reads
  // zero for every parameter and lands on element -1. The descriptor only has
  // to be non-zero so EffectPass records the stage as drawable; element 0
  // stands in, and the real per-draw selection happens at Apply.
  const auto placeholder =
      [](const xe::kernel::xna::EffectShaderSelector& sel,
         uint32_t direct) -> uint32_t {
    if (direct != UINT32_MAX) {
      return direct + 1;
    }
    if (sel.valid && !sel.element_object_indices.empty() &&
        sel.element_object_indices[0] != UINT32_MAX) {
      return sel.element_object_indices[0] + 1;
    }
    return 0;
  };
  if (out_desc) {
    // { Annotations, vs, ps }. Zero means the pass leaves that stage alone,
    // which is what the console reports for a pass binding no shader there.
    //
    // Answered from the OBJECT each stage will use, not from the analyzed
    // shader. GetPass runs while the effect is being constructed, and the
    // microcode is not analyzed until the pass is first applied - so reporting
    // the shader index here said "no shader" for every pass. EffectPass then
    // records _canDrawVertexShader as false, never sets _vertexShaderActive on
    // apply, and VerifyCanDraw refuses the first draw.
    out_desc[0] = 0;
    out_desc[1] = placeholder(pass.vertex_selector, pass.vertex_object_index);
    out_desc[2] = placeholder(pass.pixel_selector, pass.pixel_object_index);
  }
  WriteName(out_name, pass.name);
  return index + 1;
}
