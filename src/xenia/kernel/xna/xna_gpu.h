/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_GPU_H_
#define XENIA_KERNEL_XNA_XNA_GPU_H_

#include <cstdint>
#include <vector>

#include "xenia/gpu/xenos.h"

namespace xe {
namespace kernel {
namespace xna {

struct XnaGpuStream {
  uint32_t guest_address = 0;
  uint32_t size_bytes = 0;
  uint32_t stride = 0;
};

struct XnaGpuTarget {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t guest_address = 0;
};

// An XNA SurfaceFormat as the Xenos colour render target format that holds the
// same bits. A deferred renderer's targets are not all four-byte colour: the
// SunBurn G-buffer keeps linear view depth in a HalfVector2 and its light
// volumes in Rgba1010102, and describing those as 8_8_8_8 reads the depth back
// as four bytes of colour, which is what makes the whole lighting chain resolve
// to black. Returns k_8_8_8_8 for anything unrecognised.
uint32_t XnaGpuColorFormatFor(uint32_t xna_surface_format);

// XNA SurfaceFormat to the format a TEXTURE fetch constant names, which is a
// different enum from the render target one above.
gpu::xenos::TextureFormat XnaGpuTextureFormatFor(uint32_t xna_surface_format);

// The swap width console-authored pixels of that format need.
gpu::xenos::Endian XnaGpuTextureEndianFor(gpu::xenos::TextureFormat format);

// Where a bound render target slot's tiles start in EDRAM. A multiple render
// target pass needs one region per slot, and the draw that writes them and the
// resolve that reads them back have to name the same tile or the resolve reads
// a region nothing wrote. Both call this rather than repeating the arithmetic.
uint32_t XnaGpuEdramBaseForSlot(uint32_t width, uint32_t height, uint32_t slot,
                                uint32_t target_count);

// Returned by XnaGpuEdramDepthBase when the depth buffer cannot be given a
// region of its own.
constexpr uint32_t kXnaGpuNoDepth = UINT32_MAX;

// Where this pass's depth buffer starts, or kXnaGpuNoDepth when it does not
// fit. EDRAM is a 2048 tile RING: a base past the end wraps onto low tiles
// rather than failing, so a depth buffer placed after two 1280x720 colour
// targets (1440 + 720 = 2160) lands on top of colour slot 0 and the two
// scribble over each other every draw. Two 32bpp colour targets plus depth at
// 720p need 11 MB and the console had 10, which is why real hardware tiled the
// pass instead. Saying so is the honest answer: the caller turns the depth test
// and its clear off for that pass rather than aliasing depth onto a colour
// target, which is what made the whole G-buffer and lighting passes garbage
// while the single target pass that carries the HUD stayed correct.
uint32_t XnaGpuEdramDepthBase(uint32_t width, uint32_t height,
                              uint32_t target_count);

// The guest memory the last Present resolved the frame into, and the size it
// was resolved at. False before anything has been presented. This is what a
// title reads when it captures its own output - see
// D3D_Device_GetBackBufferData.
bool XnaGpuFrontBuffer(uint32_t* out_address, uint32_t* out_bytes,
                       uint32_t* out_width, uint32_t* out_height);

// Resolves one render target's EDRAM contents out to the guest memory of the
// texture that names it, so a later pass can sample what was just drawn.
void XnaGpuResolveTarget(const XnaGpuTarget& target, uint32_t edram_base);

void XnaGpuClearTarget(const XnaGpuTarget& target, uint32_t edram_base,
                       uint32_t depth_edram_base, const float* color,
                       bool clear_color, bool clear_depth, float depth);

bool XnaGpuBeginTiledPass(uint32_t width, uint32_t height,
                          uint32_t target_count, const uint32_t* formats);
void XnaGpuEndTiledPass();

struct XnaGpuConstant {
  uint32_t register_index = 0;
  float value[4] = {};
};

// One texture the title has bound, as the fetch constant needs to describe it.
// Empty guest_address means the slot is unbound.
struct XnaGpuTextureBinding {
  uint32_t guest_address = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;  // XNA SurfaceFormat
  // How many mip levels the texture was created with. A fetch that claims one
  // level for a texture the sampler will walk a chain of reads past the end of
  // what was uploaded.
  uint32_t levels = 1;
  // A resolve writes its destination TILED - draw_util addresses it with
  // texture_address::Tiled2D and has no linear path. Content the title uploads
  // is linear, because our own copy wrote it that way. The two cannot share one
  // answer, and reading a resolved target as linear is horizontal striping.
  bool tiled = false;
  // The sampler state the title bound to this slot, decoded from its
  // SamplerState.Settings. Defaults describe clamp + point, which is what the
  // fetch constant used before this travelled with the binding - correct for a
  // 0..1 sprite, wrong for a wrapped, filtered world texture.
  uint32_t address_u = 2;  // xenos::ClampMode: 0 repeat, 2 clamp-to-edge
  uint32_t address_v = 2;
  uint32_t mag_filter = 0;  // xenos::TextureFilter: 0 point, 1 linear
  uint32_t min_filter = 0;
  uint32_t mip_filter = 0;
  uint32_t aniso = 0;  // xenos::AnisoFilter
  bool sampler_set = false;
  uint32_t handle = 0;
  uint32_t type = 0;
};

struct XnaGpuDraw {
  std::vector<XnaGpuConstant> constants;
  // Indexed by SAMPLER SLOT, which is the number the shader's tfetch names.
  static constexpr uint32_t kMaxTextureSlots = 32;
  XnaGpuTextureBinding textures[kMaxTextureSlots];
  uint32_t primitive_type = 0;
  uint32_t primitive_count = 0;
  uint32_t start_vertex = 0;
  uint32_t base_vertex = 0;
  uint32_t start_index = 0;
  bool indexed = false;
  bool index_32bit = false;
  uint32_t index_guest_address = 0;
  uint32_t index_size_bytes = 0;
  uint32_t viewport_width = 0;
  uint32_t viewport_height = 0;
  // The bound render target's size, which is what the surface pitch and the
  // scissors describe - the viewport can be smaller than the target.
  uint32_t target_width = 0;
  uint32_t target_height = 0;
  // How many colour targets the title has bound. A deferred G-buffer pass binds
  // several at once, and each needs its own slot enabled and its own EDRAM
  // region - writing them all through slot 0 leaves the rest empty.
  uint32_t target_count = 0;
  // The XNA SurfaceFormat of each bound slot, in slot order.
  uint32_t target_formats[4] = {};
  // The title's DepthStencilState, as XNA reports it. depth_function is an
  // XNA CompareFunction.
  bool depth_enable = false;
  bool depth_write_enable = false;
  uint32_t depth_function = 0;
  bool stencil_enable = false;
  uint32_t stencil_function = 0;
  uint32_t stencil_pass = 0;
  uint32_t stencil_fail = 0;
  uint32_t stencil_depth_fail = 0;
  uint32_t stencil_read_mask = 0xFF;
  uint32_t stencil_write_mask = 0xFF;
  uint32_t stencil_reference = 0;
  // The title's BlendState, in XNA's own enums. The defaults describe Opaque
  // (source One, destination Zero, Add) - what the draw path hardcoded before
  // this travelled with the draw. An alpha-blended or additive object drawn
  // with those opaque factors paints its full colour as a solid patch instead
  // of blending, which is what left the transparent meshes as white blocks.
  // color_src/color_dst/alpha_src/alpha_dst are XNA Blend; color_op/alpha_op
  // are XNA BlendFunction.
  uint32_t color_src = 0;  // XNA Blend::One
  uint32_t color_dst = 1;  // XNA Blend::Zero
  uint32_t color_op = 0;   // XNA BlendFunction::Add
  uint32_t alpha_src = 0;
  uint32_t alpha_dst = 1;
  uint32_t alpha_op = 0;
  uint32_t color_write[4] = {0xF, 0xF, 0xF, 0xF};
  uint32_t target_addresses[4] = {};
  int32_t viewport_x = 0;
  int32_t viewport_y = 0;
  float viewport_min_depth = 0.0f;
  float viewport_max_depth = 1.0f;
  // The shaders that were current when this draw was recorded. They are
  // xe::gpu::Shader*, kept opaque so this header does not pull the GPU in.
  void* vertex_shader = nullptr;
  void* pixel_shader = nullptr;
};

// Puts the guest register state a draw needs where the command processor
// expects it, then issues the draw through the same path a guest title uses.
// False when the state could not be built - never a partial draw.
bool XnaGpuIssueDraw(const XnaGpuDraw& draw, const XnaGpuStream* streams,
                     uint32_t stream_count);

// Resolves what was drawn into EDRAM out to a front buffer and shows it. The
// presenter only ever displays a resolved texture, so without this a frame can
// be drawn perfectly and still never appear.
void XnaGpuPresent(uint32_t width, uint32_t height);

// True once a draw has actually been issued this frame - the sprite path is
// still what puts the HUD up, and it must not be replaced by an empty frame.
bool XnaGpuHasDrawn();

// True once geometry has been drawn at all. Which path owns the swap has to be
// stable for the life of the title, not decided per frame.
bool XnaGpuHasEverDrawn();

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_GPU_H_
