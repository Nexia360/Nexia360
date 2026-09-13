/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_gpu.h"
#include "xenia/kernel/xna/xna_direct.h"
#include "xenia/kernel/xna/xna_exports.h"
#include "xenia/kernel/xna/xna_guest_heap.h"
#include "xenia/kernel/xna/xna_runtimehost.h"
#include "xenia/kernel/xna/xna_present.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/memory.h"
#include "xenia/emulator.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/base/cvar.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"

namespace xe {
namespace kernel {
namespace xna {

namespace {

using namespace xe::gpu;

RegisterFile* Registers() {
  auto* state = kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  auto* graphics = emulator ? emulator->graphics_system() : nullptr;
  return graphics ? graphics->register_file() : nullptr;
}

xe::gpu::d3d12::D3D12CommandProcessor* Processor() {
  auto* state = kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  auto* graphics = emulator ? emulator->graphics_system() : nullptr;
  if (!graphics) {
    return nullptr;
  }
  return dynamic_cast<xe::gpu::d3d12::D3D12CommandProcessor*>(
      graphics->command_processor());
}

void Set(RegisterFile& regs, uint32_t index, uint32_t value) {
  auto* processor = Processor();
  if (processor) {
    processor->HostedWriteRegister(index, value);
    return;
  }
  regs.values[index] = value;
}

void SetFloat(RegisterFile& regs, uint32_t index, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Set(regs, index, bits);
}

constexpr uint32_t kNullVertexFetchConstant = 80;
constexpr uint32_t kNullVertexBytes = 64;

uint32_t NullVertexFetchAddress() {
  static uint32_t handle = 0;
  static uint32_t address = 0;
  if (address) {
    return address;
  }
  if (!handle) {
    handle = XnaGuestResourceCreate(XnaGuestResource::kVertexBuffer);
    if (!handle) {
      return 0;
    }
  }
  if (!XnaGuestResourceResize(handle, kNullVertexBytes)) {
    return 0;
  }
  auto* data = XnaGuestResourceData(handle);
  auto* resource = XnaGuestResourceLookup(handle);
  if (!data || !resource || !resource->data) {
    return 0;
  }
  std::memset(data, 0, kNullVertexBytes);
  XnaGuestRangeWritten(resource->data, kNullVertexBytes);
  address = resource->data;
  return address;
}

// The register file stores raw dwords; a kFloat register is only a float once
// its bits are read back as one.
float GetFloat(const RegisterFile& regs, uint32_t index) {
  float value;
  const uint32_t bits = regs.values[index];
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// The argument is an XNA PrimitiveType, so it must be compared against XNA
// values. Casting it to the Xenos enum sent every type to the default arm -
// a strip of N primitives came out as N * 3 indices instead of N + 2.
// XNA PrimitiveType to the Xenos one. They are not the same numbering, and
// the conversion is RuntimeHost's - see xna_runtimehost.h.
xenos::PrimitiveType ToXenosPrimitive(uint32_t xna_primitive) {
  xenos::PrimitiveType value = xenos::PrimitiveType::kTriangleList;
  if (!xe::kernel::xna::XnaPrimitiveToXenos(xna_primitive, &value)) {
    XELOGW("[xna] no conversion for XNA PrimitiveType {} - the console "
           "refuses this", xna_primitive);
  }
  return value;
}

// CONVERTED FIRST, COUNTED SECOND. The console's table is indexed by the
// XENOS primitive, not the XNA one, so going straight from the XNA value to a
// vertex count only agrees by coincidence - and stops agreeing the moment a
// fan is involved, since Xenos numbers fan 5 and strip 6 where Direct3D 9 has
// them the other way round.
uint32_t VertexCountFor(uint32_t xna_primitive, uint32_t primitive_count) {
  uint32_t indices = primitive_count * 3;
  xenos::PrimitiveType primitive = ToXenosPrimitive(xna_primitive);
  xe::kernel::xna::XenosPrimitiveIndexCount(primitive, primitive_count,
                                            &indices);
  return indices;
}

void IssueOnGpuThread(const XnaGpuDraw& draw,
                      const std::vector<XnaGpuStream>& streams,
                      uint32_t band_y0, uint32_t band_rows);

std::atomic<bool> drew_this_frame{false};
std::atomic<bool> drew_ever{false};

// Where a frame's draws actually end up. IssueDraw reports at least four
// "this draw has no effect" paths as success, so its return value cannot tell a
// rasterized draw from a discarded one, and a per-draw line cannot show which
// stage is losing them. These are counted per stage and reported once a frame,
// so one run says where the pixels die instead of whether one guess worked.
// How the last draw of the frame described its colour surface. The render target
// cache keys its host textures by base, pitch and format, so a resolve that
// describes any of the three differently finds a different render target - an
// untouched one - and copies black out of it no matter how well the frame drew.
struct LastSurface {
  std::atomic<uint32_t> pitch{0};
  std::atomic<uint32_t> height{0};
  std::atomic<uint32_t> base{0};
  std::atomic<uint32_t> format{0};
  std::atomic<uint32_t> targets{0};
};
LastSurface last_surface;

struct FrameCounters {
  std::atomic<uint32_t> submitted{0};
  std::atomic<uint32_t> no_rasterize{0};
  std::atomic<uint32_t> refused{0};
  std::atomic<uint32_t> reached_pipeline{0};
  std::atomic<uint32_t> drawn{0};
  std::atomic<uint32_t> into_lighting{0};
  std::atomic<uint32_t> textured{0};
};
FrameCounters frame_counters;
uint32_t front_buffer_guest = 0;
uint32_t front_buffer_bytes = 0;
uint32_t front_buffers[2] = {0, 0};
uint32_t front_buffers_bytes = 0;
uint32_t front_buffer_index = 0;
// The size the last Present resolved at, which is what a readback has to be
// laid out as - the allocation is rounded up to tiles and is bigger.
uint32_t front_buffer_width = 0;
uint32_t front_buffer_height = 0;
uint32_t resolve_rect_guest = 0;

void EnsureResolveRect() {
  if (resolve_rect_guest) {
    return;
  }
  auto* state = kernel_state();
  auto* memory = state ? state->memory() : nullptr;
  if (memory) {
    resolve_rect_guest =
        memory->SystemHeapAlloc(6 * sizeof(float), 32, kSystemHeapPhysical);
  }
}

// ONE BUFFER PER SIZE, WRITTEN ONCE.
//
// The resolve rect is guest memory the GPU reads through a vertex fetch, and
// a store from here raises no invalidation - so the shared memory uploads
// whatever was there the first time and serves that copy forever. With a
// single shared buffer every resolve and every clear after the first one
// therefore used the FIRST extent ever written: once a 200x120 render target
// had been cleared, the 1280x720 back buffer clear behind it covered only a
// 200x120 corner and the rest of the frame kept the previous case's picture.
//
// Announcing the write would fix it, but it would also mean raising the
// physical memory callbacks from the GPU thread on every resolve. Giving each
// distinct size its own buffer removes the problem instead: the bytes at an
// address never change, so the one upload is always right.
std::map<uint64_t, uint32_t> resolve_rects;

uint32_t ResolveRectFor(uint32_t width, uint32_t height) {
  const uint64_t key = (uint64_t(width) << 32) | height;
  auto found = resolve_rects.find(key);
  if (found != resolve_rects.end()) {
    return found->second;
  }
  auto* state = kernel_state();
  auto* memory = state ? state->memory() : nullptr;
  if (!memory) {
    return 0;
  }
  const uint32_t address =
      memory->SystemHeapAlloc(6 * sizeof(float), 32, kSystemHeapPhysical);
  if (!address) {
    return 0;
  }
  auto* rect = memory->TranslateVirtual<float*>(address);
  rect[0] = 0.0f;                rect[1] = 0.0f;
  rect[2] = float(width);        rect[3] = 0.0f;
  rect[4] = float(width);        rect[5] = float(height);
  resolve_rects[key] = address;
  return address;
}

uint32_t EnsureFrontBuffer(uint32_t width, uint32_t height) {
  // BIG ENOUGH FOR WHAT THE RESOLVE WRITES, NOT FOR WHAT THE IMAGE MEASURES.
  //
  // A resolve destination is written with its pitch and height rounded up to
  // the GPU's tile size, so a 1280x720 target is filled as though it were
  // 1280x768 - about 0x14000 bytes more than width * height * 4. Allocating the
  // exact image size let the copy run off the end into whatever the guest heap
  // handed out next, and what it handed out next was a vertex declaration.
  //
  // That declaration then read back as all zeroes, every element became usage
  // POSITION with format Single, and the shader that should fetch a 56 byte
  // vertex was patched to fetch one float at stride 20. Nothing rendered, and
  // nothing about it looked like memory corruption.
  const uint32_t needed =
      xe::round_up(width, 128) * xe::round_up(height, 128) * 4;
  if (front_buffers_bytes < needed || !front_buffers[0] || !front_buffers[1]) {
    auto* state = kernel_state();
    auto* memory = state ? state->memory() : nullptr;
    if (!memory) {
      return 0;
    }
    for (auto& buffer : front_buffers) {
      if (buffer) {
        XnaGuestPayloadFree(buffer);
        buffer = 0;
      }
    }
    front_buffers_bytes = 0;
    front_buffer_guest = 0;
    front_buffer_bytes = 0;
    for (auto& buffer : front_buffers) {
      buffer = XnaGuestPayloadAlloc(needed);
      if (!buffer) {
        return 0;
      }
    }
    front_buffers_bytes = needed;
  }
  front_buffer_index ^= 1;
  front_buffer_guest = front_buffers[front_buffer_index];
  front_buffer_bytes = front_buffers_bytes;
  EnsureResolveRect();
  return front_buffer_guest;
}

// GetResolveInfo refuses CopyCommand::kNull, so a clear has to be issued as a
// kRaw copy with the clear bits set - which means it also COPIES. Sending that
// copy to the target's own guest memory stamps stale EDRAM over the surface the
// title is about to sample. It goes here instead and is never read.
uint32_t clear_scratch_guest = 0;
uint32_t clear_scratch_bytes = 0;

uint32_t EnsureClearScratch(uint32_t width, uint32_t height) {
  const uint32_t needed =
      xe::round_up(width, 128) * xe::round_up(height, 128) * 4;
  if (clear_scratch_guest && clear_scratch_bytes >= needed) {
    return clear_scratch_guest;
  }
  auto* state = kernel_state();
  auto* memory = state ? state->memory() : nullptr;
  if (!memory) {
    return 0;
  }
  if (clear_scratch_guest) {
    XnaGuestPayloadFree(clear_scratch_guest);
    clear_scratch_guest = 0;
    clear_scratch_bytes = 0;
  }
  const uint32_t address = XnaGuestPayloadAlloc(needed);
  if (!address) {
    return 0;
  }
  clear_scratch_guest = address;
  clear_scratch_bytes = needed;
  return address;
}

struct TiledPass {
  bool active = false;
  uint32_t height = 0;
  uint32_t band_rows = 0;
  std::vector<std::function<void(uint32_t, uint32_t)>> ops;
};

std::mutex tiled_pass_mutex;
TiledPass tiled_pass;

bool RecordTiledOp(std::function<void(uint32_t, uint32_t)> op) {
  std::lock_guard<std::mutex> lock(tiled_pass_mutex);
  if (!tiled_pass.active) {
    return false;
  }
  tiled_pass.ops.push_back(std::move(op));
  return true;
}

}  // namespace

static xenos::CompareFunction XenosCompareForXnaCompare(uint32_t xna);
static xenos::BlendFactor XenosBlendFactorForXna(uint32_t xna);
static xenos::BlendOp XenosBlendOpForXna(uint32_t xna);

bool XnaGpuIssueDraw(const XnaGpuDraw& draw, const XnaGpuStream* streams,
                     uint32_t stream_count) {
  if (XnaDirectActive()) {
    drew_this_frame.store(true);
    drew_ever.store(true);
    return XnaDirectDraw(draw, streams, stream_count);
  }
  auto* processor = Processor();
  if (!processor || !Registers()) {
    return false;
  }

  // ISSUED ON THE COMMAND PROCESSOR'S OWN THREAD, NOT THE TITLE'S.
  //
  // A hosted title calls this from its managed thread, and IssueDraw expects
  // to be the only thing touching the command list, the register file and the
  // caches. Handing the work over is what the ring buffer does for a guest
  // title; CallInThread is the same hand-off without the packets. The state is
  // written inside the lambda so a draw queued later cannot overwrite the
  // registers of one that has not run yet.
  std::vector<XnaGpuStream> owned(streams, streams + stream_count);
  XnaGpuDraw copy = draw;
  if (RecordTiledOp([copy, owned](uint32_t band_y0, uint32_t band_rows) {
        IssueOnGpuThread(copy, owned, band_y0, band_rows);
      })) {
    return true;
  }
  processor->CallInThread(
      [copy, owned]() { IssueOnGpuThread(copy, owned, 0, 0); });
  return true;
}

namespace {

void IssueOnGpuThread(const XnaGpuDraw& draw,
                      const std::vector<XnaGpuStream>& streams,
                      uint32_t band_y0, uint32_t band_rows) {
  auto* processor = Processor();
  auto* registers = Registers();
  if (!processor || !registers) {
    return;
  }
  auto* memory = kernel_state() ? kernel_state()->memory() : nullptr;
  if (!memory) {
    return;
  }
  const uint32_t stream_count = static_cast<uint32_t>(streams.size());
  RegisterFile& regs = *registers;

  const uint32_t width = draw.target_width    ? draw.target_width
                         : draw.viewport_width ? draw.viewport_width
                                               : 1280;
  const uint32_t height = draw.target_height    ? draw.target_height
                          : draw.viewport_height ? draw.viewport_height
                                                 : 720;

  // Colour and depth into EDRAM, which is what makes IssueDraw draw at all -
  // kCopy sends it to IssueCopy instead, and a zero surface pitch makes it
  // return without doing anything.
  reg::RB_MODECONTROL mode_control;
  mode_control.value = 0;
  mode_control.edram_mode = xenos::EdramMode::kColorDepth;
  Set(regs, XE_GPU_REG_RB_MODECONTROL, mode_control.value);

  reg::RB_SURFACE_INFO surface_info;
  surface_info.value = 0;
  surface_info.surface_pitch = width;
  surface_info.msaa_samples = xenos::MsaaSamples::k1X;
  Set(regs, XE_GPU_REG_RB_SURFACE_INFO, surface_info.value);

  const uint32_t target_count =
      std::min<uint32_t>(draw.target_count ? draw.target_count : 1, 4);
  uint32_t color_mask = 0;
  for (uint32_t slot = 0; slot < 4; ++slot) {
    reg::RB_COLOR_INFO color_info;
    color_info.value = 0;
    color_info.color_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
    if (slot < target_count) {
      color_info.color_format = static_cast<xenos::ColorRenderTargetFormat>(
          XnaGpuColorFormatFor(draw.target_formats[slot]));
      color_info.color_base =
          XnaGpuEdramBaseForSlot(width, height, slot, target_count);
      color_mask |= (draw.color_write[slot] & 0xFu) << (slot * 4);
    }
    Set(regs, reg::RB_COLOR_INFO::rt_register_indices[slot], color_info.value);
  }
  Set(regs, XE_GPU_REG_RB_COLOR_MASK, color_mask);

  // Depth was never configured here, so both of these kept whatever the last
  // pass left. A stale test discards every fragment, and a stale base of zero
  // puts depth on the same tile as colour, where its writes overwrite the
  // picture. The base is placed after the colour slots, and the clear has to be
  // told the same one - a clear that lands anywhere else leaves the buffer the
  // test reads uncleared. When the slots do not fit in EDRAM that placement
  // falls back to tile zero, which collides with colour.
  const uint32_t depth_base =
      XnaGpuEdramDepthBase(width, height, target_count);
  const bool depth_fits = depth_base != kXnaGpuNoDepth;

  reg::RB_DEPTHCONTROL depth_control;
  depth_control.value = 0;
  depth_control.z_enable = (draw.depth_enable && depth_fits) ? 1 : 0;
  depth_control.z_write_enable =
      (draw.depth_write_enable && depth_fits) ? 1 : 0;
  depth_control.zfunc = XenosCompareForXnaCompare(draw.depth_function);
  Set(regs, XE_GPU_REG_RB_DEPTHCONTROL, depth_control.value);

  reg::RB_DEPTH_INFO depth_info;
  depth_info.value = 0;
  depth_info.depth_base = depth_fits ? depth_base : 0;
  depth_info.depth_format = xenos::DepthRenderTargetFormat::kD24S8;
  Set(regs, XE_GPU_REG_RB_DEPTH_INFO, depth_info.value);

  // Everything below is read by the draw path and was never written by it, so
  // each one held whatever the previous pass happened to leave. A guest title
  // sets all of them in its PM4 stream; a hosted one has no stream, so they
  // have to be stated here. Any single one of them can throw the whole frame
  // away silently - a stale alpha test kills every fragment, a stale index
  // clamp collapses every triangle, a stale window offset moves the picture off
  // the screen.
  reg::RB_COLORCONTROL color_control;
  color_control.value = 0;
  color_control.alpha_func = xenos::CompareFunction::kAlways;
  color_control.alpha_test_enable = 0;
  color_control.alpha_to_mask_enable = 0;
  Set(regs, XE_GPU_REG_RB_COLORCONTROL, color_control.value);

  // The window of the constant file each shader stage may address. Left stale
  // these can exclude the registers the constants were just written to.
  reg::SQ_VS_CONST vs_const;
  vs_const.value = 0;
  vs_const.base = 0;
  vs_const.size = 255;
  Set(regs, XE_GPU_REG_SQ_VS_CONST, vs_const.value);
  reg::SQ_PS_CONST ps_const;
  ps_const.value = 0;
  ps_const.base = 0;
  ps_const.size = 255;
  Set(regs, XE_GPU_REG_SQ_PS_CONST, ps_const.value);

  reg::PA_SC_WINDOW_OFFSET window_offset;
  window_offset.value = 0;
  window_offset.window_y_offset = -int32_t(band_y0);
  Set(regs, XE_GPU_REG_PA_SC_WINDOW_OFFSET, window_offset.value);
  Set(regs, XE_GPU_REG_SQ_CONTEXT_MISC, 0);
  Set(regs, XE_GPU_REG_SQ_INTERPOLATOR_CNTL, 0);
  Set(regs, XE_GPU_REG_RB_STENCILREFMASK, 0);

  // The title's BlendState, translated from XNA's enums. Opaque resolves to
  // source One, destination Zero, Add - identical to what this hardcoded
  // before, so opaque geometry is unchanged - while an alpha-blended or
  // additive object now blends instead of overwriting the target with its full
  // colour. Rendered opaque, a transparent mesh paints a solid patch, which is
  // what left the title's transparent surfaces reading as white blocks. A
  // source factor of zero from an unset state cannot happen: the defaults on
  // the draw describe Opaque, not a zeroed struct.
  reg::RB_BLENDCONTROL blend;
  blend.value = 0;
  blend.color_srcblend = XenosBlendFactorForXna(draw.color_src);
  blend.color_comb_fcn = XenosBlendOpForXna(draw.color_op);
  blend.color_destblend = XenosBlendFactorForXna(draw.color_dst);
  blend.alpha_srcblend = XenosBlendFactorForXna(draw.alpha_src);
  blend.alpha_comb_fcn = XenosBlendOpForXna(draw.alpha_op);
  blend.alpha_destblend = XenosBlendFactorForXna(draw.alpha_dst);
  for (uint32_t slot = 0; slot < 4; ++slot) {
    Set(regs, reg::RB_BLENDCONTROL::rt_register_indices[slot], blend.value);
  }

  // Clipping on, no user clip planes.
  reg::PA_CL_CLIP_CNTL clip_cntl;
  clip_cntl.value = 0;
  clip_cntl.clip_disable = 0;
  Set(regs, XE_GPU_REG_PA_CL_CLIP_CNTL, clip_cntl.value);

  reg::PA_SU_VTX_CNTL vtx_cntl;
  vtx_cntl.value = 0;
  vtx_cntl.pix_center = xenos::PixelCenter::kD3DZero;
  vtx_cntl.round_mode = xenos::VertexRounding::kRoundToEven;
  vtx_cntl.quant_mode = xenos::VertexQuantization::k_1_16th;
  Set(regs, XE_GPU_REG_PA_SU_VTX_CNTL, vtx_cntl.value);

  // base_vertex is applied to the fetch address above, the way start_index is.
  // Direct3D 9 puts it here instead, so leaving a stale value would apply it a
  // second time on top of the offset already taken.
  Set(regs, XE_GPU_REG_VGT_INDX_OFFSET, 0);
  reg::VGT_MAX_VTX_INDX max_vtx;
  max_vtx.value = 0;
  max_vtx.max_indx = 0xFFFFFF;
  Set(regs, XE_GPU_REG_VGT_MAX_VTX_INDX, max_vtx.value);
  Set(regs, XE_GPU_REG_VGT_MIN_VTX_INDX, 0);

  // No tessellation, no primitive restart. Left stale, path_select can say
  // tessellation is enabled, which sends the draw down a path that produces no
  // host vertices - and a draw with no host vertices is another of the cases
  // IssueDraw reports as success.
  reg::VGT_OUTPUT_PATH_CNTL output_path;
  output_path.value = 0;
  output_path.path_select = xenos::VGTOutputPath::kVertexReuse;
  Set(regs, XE_GPU_REG_VGT_OUTPUT_PATH_CNTL, output_path.value);
  Set(regs, XE_GPU_REG_VGT_HOS_CNTL, 0);
  Set(regs, XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX, 0);

  // The scissor and the screen have to admit the whole target or every
  // fragment is thrown away before it reaches the render target cache.
  const uint32_t scissor_top = band_rows ? band_y0 : 0;
  const uint32_t scissor_bottom = band_rows ? band_y0 + band_rows : height;
  Set(regs, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, scissor_top << 16);
  Set(regs, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR,
      (scissor_bottom << 16) | width);
  Set(regs, XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL, 0);
  Set(regs, XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR, (height << 16) | width);

  // A half-width, half-height scale about the centre is the standard viewport
  // transform: clip space is -1..1 and the target is 0..width.
  reg::PA_CL_VTE_CNTL vte;
  vte.value = 0;
  vte.vport_x_scale_ena = 1;
  vte.vport_x_offset_ena = 1;
  vte.vport_y_scale_ena = 1;
  vte.vport_y_offset_ena = 1;
  vte.vport_z_scale_ena = 1;
  vte.vport_z_offset_ena = 1;
  // The vertex shader emits a real clip-space W, not its reciprocal, so the
  // hardware has to take 1/W itself - that is what VTX_W0_FMT selects. Left 0,
  // W is taken to ALREADY be 1/W, so the perspective divide multiplies by W
  // instead of dividing: near geometry (small W) collapses to screen centre and
  // far geometry flies off, every 3D triangle becoming a wedge through the
  // middle. A 2D sprite emits W=1, where 1/W == W, so it is unharmed - which is
  // why the HUD was correct while the whole 3D scene folded into the centre.
  vte.vtx_w0_fmt = 1;
  Set(regs, XE_GPU_REG_PA_CL_VTE_CNTL, vte.value);
  SetFloat(regs, XE_GPU_REG_PA_CL_VPORT_XSCALE, float(width) * 0.5f);
  SetFloat(regs, XE_GPU_REG_PA_CL_VPORT_XOFFSET, float(width) * 0.5f);
  SetFloat(regs, XE_GPU_REG_PA_CL_VPORT_YSCALE, float(height) * -0.5f);
  SetFloat(regs, XE_GPU_REG_PA_CL_VPORT_YOFFSET, float(height) * 0.5f);
  SetFloat(regs, XE_GPU_REG_PA_CL_VPORT_ZSCALE, 1.0f);
  SetFloat(regs, XE_GPU_REG_PA_CL_VPORT_ZOFFSET, 0.0f);

  reg::PA_SU_SC_MODE_CNTL su_sc;
  su_sc.value = 0;
  su_sc.face = 0;
  su_sc.poly_mode = xenos::PolygonModeEnable::kDisabled;
  su_sc.vtx_window_offset_enable = band_rows ? 1 : 0;
  Set(regs, XE_GPU_REG_PA_SU_SC_MODE_CNTL, su_sc.value);

  // The streams the title bound, as fetch constants: the shader's own vfetch
  // instructions read through these, so no input layout is involved.
  // An indexed draw's indices are relative to base_vertex, and a non-indexed
  // one begins at start_vertex. The Xenos has no register for either, so like
  // the index range they have to move the fetch address. These were carried on
  // the draw and never applied, so every draw read from the front of the
  // buffer no matter which part of the mesh it asked for.
  const uint32_t first_vertex =
      draw.indexed ? draw.base_vertex : draw.start_vertex;

  // WHICH fetch slots to write. A shader's vfetch instructions name the slots
  // they read, and those indices are baked into the microcode - they are not
  // simply 0, 1, 2. Writing stream i into slot i left every slot the shader
  // actually reads holding zeros, which is an address of zero and a size of
  // zero, and the GPU faults on it. That is a lost device, not a black frame.
  uint32_t fetch_slots[32];
  uint32_t fetch_slot_count = 0;
  bool wants_null_fetch = false;
  auto* draw_vertex_shader = static_cast<xe::gpu::Shader*>(draw.vertex_shader);
  if (draw_vertex_shader && draw_vertex_shader->is_ucode_analyzed()) {
    const auto& map = draw_vertex_shader->constant_register_map();
    // HIGHEST FIRST, because the patcher hands stream S the constant 95 - S to
    // stay clear of the texture constants at the bottom of the file. Collected
    // ascending, the streams would be paired with the constants in reverse and
    // a two stream draw would read each stream through the other's descriptor.
    for (uint32_t i = 0; i < 96 && fetch_slot_count < 32; ++i) {
      const uint32_t slot = 95 - i;
      if (!(map.vertex_fetch_bitmap[slot / 32] & (uint32_t(1) << (slot % 32)))) {
        continue;
      }
      if (slot == kNullVertexFetchConstant) {
        wants_null_fetch = true;
        continue;
      }
      fetch_slots[fetch_slot_count++] = slot;
    }
  }

  if (wants_null_fetch) {
    const uint32_t null_vertex_address = NullVertexFetchAddress();
    if (null_vertex_address) {
      xenos::xe_gpu_vertex_fetch_t null_fetch;
      null_fetch.dword_0 = 0;
      null_fetch.dword_1 = 0;
      null_fetch.type = xenos::FetchConstantType::kVertex;
      null_fetch.address = memory->GetPhysicalAddress(null_vertex_address) >> 2;
      null_fetch.endian = xenos::Endian::k8in32;
      null_fetch.size = kNullVertexBytes >> 2;
      const uint32_t null_base =
          XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + kNullVertexFetchConstant * 2;
      Set(regs, null_base + 0, null_fetch.dword_0);
      Set(regs, null_base + 1, null_fetch.dword_1);
    }
  }
  if (!fetch_slot_count) {
    for (uint32_t slot = 0; slot < stream_count && slot < 32; ++slot) {
      fetch_slots[fetch_slot_count++] = slot;
    }
  }
  // Every slot the shader reads gets a real buffer. Where there is no stream
  // for one, the last stream is repeated - a slot left at zero is an address of
  // zero, and the shader will fetch through it regardless.
  for (uint32_t slot_index = 0; slot_index < fetch_slot_count; ++slot_index) {
    const uint32_t i =
        slot_index < stream_count ? slot_index : stream_count - 1;
    if (i >= stream_count) {
      break;
    }
    const XnaGpuStream& stream = streams[i];
    if (!stream.guest_address || !stream.size_bytes) {
      continue;
    }
    uint32_t address = stream.guest_address;
    uint32_t size_bytes = stream.size_bytes;
    if (first_vertex && stream.stride) {
      const uint32_t skip = first_vertex * stream.stride;
      if (skip >= size_bytes) {
        XELOGD(
            "[xna] gpu draw skipped: first vertex {} at stride {} is past the "
            "{} byte stream at {:08X}",
            first_vertex, stream.stride, size_bytes, stream.guest_address);
        return;
      }
      address += skip;
      size_bytes -= skip;
    }
    xenos::xe_gpu_vertex_fetch_t fetch;
    fetch.dword_0 = 0;
    fetch.dword_1 = 0;
    fetch.type = xenos::FetchConstantType::kVertex;
    fetch.address = memory->GetPhysicalAddress(address) >> 2;
    fetch.endian = xenos::Endian::k8in32;
    fetch.size = size_bytes >> 2;
    const uint32_t base =
        XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_slots[slot_index] * 2;
    Set(regs, base + 0, fetch.dword_0);
    Set(regs, base + 1, fetch.dword_1);

    {
      const uint8_t* bytes = memory->TranslateVirtual<const uint8_t*>(address);
      const uint32_t stride = stream.stride ? stream.stride : 16;
      std::string text;
      for (uint32_t v = 0; bytes && v < 3 && (v + 1) * stride <= size_bytes;
           ++v) {
        text += fmt::format(" | v{}:", v);
        for (uint32_t w = 0; w < stride / 4 && w < 8; ++w) {
          uint32_t word;
          std::memcpy(&word, bytes + v * stride + w * 4, sizeof(word));
          word = xe::byte_swap(word);
          float as_float;
          std::memcpy(&as_float, &word, sizeof(as_float));
          text += fmt::format(" {:08X}({:g})", word, as_float);
        }
      }
      XELOGD(
          "[xna] trace fetch slot {}: guest {:08X} phys {:08X} {} bytes "
          "stride {}{}",
          fetch_slots[slot_index], address,
          memory->GetPhysicalAddress(address), size_bytes, stream.stride,
          text);
    }
  }

  // EVERY BOUND TEXTURE GETS A DESCRIPTOR. Texture fetch constant N is dwords
  // N*6 of the same file the vertex fetches live at the top of, and a tfetch
  // through a slot nobody wrote samples zeroes - which for a deferred renderer
  // means the lighting pass reads an empty G-buffer and every later pass
  // composites black.
  uint32_t texture_slots_written = 0;
  const uint32_t lowest_stream_constant =
      fetch_slot_count ? fetch_slots[fetch_slot_count - 1] : 96;
  for (uint32_t slot = 0; slot < XnaGpuDraw::kMaxTextureSlots; ++slot) {
    const XnaGpuTextureBinding& binding = draw.textures[slot];
    if (!binding.guest_address || !binding.width || !binding.height) {
      const bool overlaps_vertex_constants =
          slot * 6 + 5 >= lowest_stream_constant * 2 ||
          (wants_null_fetch && slot == kNullVertexFetchConstant / 3);
      if (overlaps_vertex_constants) {
        continue;
      }
      for (uint32_t i = 0; i < 6; ++i) {
        Set(regs, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + i, 0);
      }
      processor->HostedTextureFetchWritten(slot);
      continue;
    }
    xenos::xe_gpu_texture_fetch_t texture = {};
    texture.type = xenos::FetchConstantType::kTexture;
    texture.format = XnaGpuTextureFormatFor(binding.format);
    texture.tiled = binding.tiled ? 1 : 0;
    texture.endianness =
        XnaGpuTextureEndianFor(XnaGpuTextureFormatFor(binding.format));
    // PHYSICAL, not the guest virtual address. The texture cache turns this
    // straight back into a shared memory range - `RequestRange(base_page << 12,
    // ...)` - and shared memory only covers the 512 MB of physical space. A
    // virtual address like FEBE8000 shifts to a page far outside it, the
    // request fails, and LoadTextureDataFromResidentMemory just `continue`s:
    // no texture, no error, and no "Loaded" line to notice the absence by.
    texture.base_address =
        memory->GetPhysicalAddress(binding.guest_address) >> 12;
    texture.pitch = (binding.width + 31) >> 5;
    texture.dimension = xenos::DataDimension::k2DOrStacked;
    texture.size_2d.width = binding.width - 1;
    texture.size_2d.height = binding.height - 1;
    texture.size_2d.stack_depth = 0;
    // The title's own SamplerState, when it bound one to this slot: wrap vs
    // clamp and point vs linear. A world texture that tiles was being clamped
    // to one stretched edge before this. Falls back to clamp + point, which is
    // what a 0..1 sprite wants and what this used unconditionally before.
    if (binding.sampler_set) {
      texture.clamp_x = static_cast<xenos::ClampMode>(binding.address_u);
      texture.clamp_y = static_cast<xenos::ClampMode>(binding.address_v);
      texture.clamp_z = static_cast<xenos::ClampMode>(binding.address_v);
      texture.mag_filter = static_cast<xenos::TextureFilter>(binding.mag_filter);
      texture.min_filter = static_cast<xenos::TextureFilter>(binding.min_filter);
      texture.mip_filter = static_cast<xenos::TextureFilter>(binding.mip_filter);
      texture.aniso_filter = static_cast<xenos::AnisoFilter>(binding.aniso);
    } else {
      texture.clamp_x = xenos::ClampMode::kClampToEdge;
      texture.clamp_y = xenos::ClampMode::kClampToEdge;
      texture.clamp_z = xenos::ClampMode::kClampToEdge;
    }
    texture.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    for (uint32_t i = 0; i < 6; ++i) {
      Set(regs, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + i,
          reinterpret_cast<const uint32_t*>(&texture)[i]);
    }
    processor->HostedTextureFetchWritten(slot);
    texture_slots_written |= uint32_t(1) << slot;
  }

  // The values the title set, at the registers its shaders read. Applied here
  // rather than when the packet arrived so a draw queued later cannot
  // overwrite the constants of one that has not run yet.
  // Indices 0-255 are the vertex bank and 256-511 the pixel bank, and the two
  // are contiguous in the register file - SHADER_CONSTANT_256_X is exactly
  // SHADER_CONSTANT_000_X plus 256 registers. Stopping at 256 here meant no
  // pixel shader ever received a constant.
  for (const XnaGpuConstant& constant : draw.constants) {
    if (constant.register_index >= 512) {
      continue;
    }
    const uint32_t at =
        XE_GPU_REG_SHADER_CONSTANT_000_X + constant.register_index * 4;
    for (uint32_t c = 0; c < 4; ++c) {
      SetFloat(regs, at + c, constant.value[c]);
    }
  }

  uint32_t index_count =
      VertexCountFor(draw.primitive_type, draw.primitive_count);

  // The title draws a range out of one shared index buffer, and start_index is
  // where that range begins. The hardware has nowhere to be told this - there
  // is no first-index register - so it has to move the base address instead.
  // Passing the buffer start unchanged made every indexed draw read the same
  // indices from the front of the buffer.
  const uint32_t index_stride = draw.index_32bit ? 4u : 2u;
  uint32_t index_base = draw.index_guest_address;
  uint32_t index_bytes_left = draw.index_size_bytes;
  if (draw.indexed) {
    const uint32_t start_bytes = draw.start_index * index_stride;
    if (start_bytes >= draw.index_size_bytes) {
      XELOGD(
          "[xna] gpu draw skipped: start index {} is past the {} byte index "
          "buffer at {:08X}",
          draw.start_index, draw.index_size_bytes, draw.index_guest_address);
      return;
    }
    index_base += start_bytes;
    index_bytes_left = draw.index_size_bytes - start_bytes;
    const uint32_t available = index_bytes_left / index_stride;
    if (index_count > available) {
      XELOGD(
          "[xna] gpu draw truncated: wanted {} indices from index {}, buffer "
          "at {:08X} holds {} more",
          index_count, draw.start_index, draw.index_guest_address, available);
      index_count = available;
    }
  }
  if (!index_count) {
    XELOGD("[xna] gpu draw skipped: no indices to draw");
    return;
  }

  // The primitive processor reads the type from VGT_DRAW_INITIATOR, not from
  // the argument handed to IssueDraw, so leaving this unset makes every draw
  // arrive as kNone.
  // The index buffer is described by VGT_DMA_BASE/VGT_DMA_SIZE, not by the
  // IndexBufferInfo handed to IssueDraw - leaving them unset makes every
  // indexed draw look like it has an empty index buffer.
  if (draw.indexed && index_base) {
    Set(regs, XE_GPU_REG_VGT_DMA_BASE, memory->GetPhysicalAddress(index_base));
    reg::VGT_DMA_SIZE dma_size;
    dma_size.value = 0;
    dma_size.num_words = index_bytes_left / index_stride;
    dma_size.swap_mode =
        draw.index_32bit ? xenos::Endian::k8in32 : xenos::Endian::k8in16;
    Set(regs, XE_GPU_REG_VGT_DMA_SIZE, dma_size.value);

  }

  auto* vertex_shader = static_cast<xe::gpu::Shader*>(draw.vertex_shader);
  auto* pixel_shader = static_cast<xe::gpu::Shader*>(draw.pixel_shader);

  // WHICH SLOTS THE SHADERS ACTUALLY FETCH FROM.
  //
  // Writing a descriptor is not the same as the cache being asked for it: the
  // texture cache loads a texture because the SHADER declares a binding on that
  // fetch constant. A log full of correct descriptors and no "Loaded ...
  // texture" line means the two sides never met, and only the shader can say
  // which side is wrong. Both stages - vertex shaders fetch textures too, and
  // this title's do.
  for (auto* shader : {vertex_shader, pixel_shader}) {
    if (!shader || !shader->is_ucode_analyzed()) {
      continue;
    }
    uint32_t wanted = 0;
    for (const auto& binding : shader->texture_bindings()) {
      wanted |= uint32_t(1) << (binding.fetch_constant & 31);
    }
    const uint32_t missing = wanted & ~texture_slots_written;
    if (missing) {
      XELOGW(
          "[xna] {} shader {:016X} fetches texture constants {:08X} but only "
          "{:08X} were written - {:08X} read an unwritten descriptor",
          shader == vertex_shader ? "vertex" : "pixel",
          shader->ucode_data_hash(), wanted, texture_slots_written, missing);
    }
  }
  processor->HostedSetActiveShaders(vertex_shader, pixel_shader);

  // THIS REGISTER DECIDES WHETHER THE DRAW RASTERIZES AT ALL.
  //
  // IsRasterizationPotentiallyDone rejects the draw outright when vs_export_mode
  // is kMultipass, which is 7 - all three bits set. Never having written this
  // register left it holding whatever the last pass put there, and IssueDraw
  // reports a rejected draw as success, so every draw logged as issued while
  // nothing rasterized.
  //
  // The register counts are left at zero deliberately: the interpolator count
  // is max(the shader's own static bound, what this field allows), so zero lets
  // the shader speak for itself rather than inflating the count past what the
  // vertex shader actually writes.
  reg::SQ_PROGRAM_CNTL program_cntl;
  program_cntl.value = 0;
  program_cntl.vs_export_mode = xenos::VertexShaderExportMode::kPosition1Vector;
  program_cntl.vs_num_reg = 0;
  program_cntl.ps_num_reg = 0;
  program_cntl.param_gen = 0;
  program_cntl.gen_index_vtx = 0;
  uint32_t interpolators = 0;
  if (vertex_shader && vertex_shader->is_ucode_analyzed()) {
    interpolators = xe::bit_count(vertex_shader->writes_interpolators());
  }
  program_cntl.vs_export_count = interpolators ? interpolators - 1 : 0;
  Set(regs, XE_GPU_REG_SQ_PROGRAM_CNTL, program_cntl.value);

  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = 0;
  initiator.prim_type = ToXenosPrimitive(draw.primitive_type);
  initiator.source_select =
      draw.indexed ? xenos::SourceSelect::kDMA : xenos::SourceSelect::kAutoIndex;
  initiator.major_mode = xenos::MajorMode::kImplicit;
  initiator.index_size =
      draw.index_32bit ? xenos::IndexFormat::kInt32 : xenos::IndexFormat::kInt16;
  initiator.num_indices = index_count;
  Set(regs, XE_GPU_REG_VGT_DRAW_INITIATOR, initiator.value);
  // IssueDraw reports several "this draw has no effect" paths as success, so
  // its return value cannot tell a rasterized draw from a discarded one. The
  // predicates it gates on are public, so ask them directly instead.
  frame_counters.submitted.fetch_add(1);
  const bool polygonal = xe::gpu::draw_util::IsPrimitivePolygonal(regs);
  const bool rasterizes =
      xe::gpu::draw_util::IsRasterizationPotentiallyDone(regs, polygonal);

  if (!rasterizes) {
    frame_counters.no_rasterize.fetch_add(1);
    XELOGD(
        "[xna] gpu draw will not rasterize: edram_mode {}, surface_pitch {}, "
        "vs_export_mode {}, polygonal {}",
        uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode),
        uint32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch),
        uint32_t(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode), polygonal);
  }

  {
    XELOGD(
        "[xna] trace draw: prim {} indices {} indexed {} first_vertex {} | "
        "surface {:08X} color0 {:08X} depth {:08X} zctl {:08X} mask {:08X} "
        "blend0 {:08X} | vport x {:g}+{:g} y {:g}+{:g} z {:g}+{:g} vte {:08X} "
        "| scissor_br {:08X} clip {:08X} su_sc {:08X} program {:08X} | vs "
        "{:016X} ps {:016X}",
        draw.primitive_type, index_count, draw.indexed, first_vertex,
        regs.values[XE_GPU_REG_RB_SURFACE_INFO],
        regs.values[reg::RB_COLOR_INFO::rt_register_indices[0]],
        regs.values[XE_GPU_REG_RB_DEPTH_INFO],
        regs.values[XE_GPU_REG_RB_DEPTHCONTROL],
        regs.values[XE_GPU_REG_RB_COLOR_MASK],
        regs.values[reg::RB_BLENDCONTROL::rt_register_indices[0]],
        GetFloat(regs, XE_GPU_REG_PA_CL_VPORT_XSCALE),
        GetFloat(regs, XE_GPU_REG_PA_CL_VPORT_XOFFSET),
        GetFloat(regs, XE_GPU_REG_PA_CL_VPORT_YSCALE),
        GetFloat(regs, XE_GPU_REG_PA_CL_VPORT_YOFFSET),
        GetFloat(regs, XE_GPU_REG_PA_CL_VPORT_ZSCALE),
        GetFloat(regs, XE_GPU_REG_PA_CL_VPORT_ZOFFSET),
        regs.values[XE_GPU_REG_PA_CL_VTE_CNTL],
        regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
        regs.values[XE_GPU_REG_PA_CL_CLIP_CNTL],
        regs.values[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
        regs.values[XE_GPU_REG_SQ_PROGRAM_CNTL],
        vertex_shader ? vertex_shader->ucode_data_hash() : uint64_t(0),
        pixel_shader ? pixel_shader->ucode_data_hash() : uint64_t(0));
    for (auto* shader : {vertex_shader, pixel_shader}) {
      if (!shader) {
        continue;
      }
      const bool is_vertex = shader == vertex_shader;
      const uint32_t bank = is_vertex ? XE_GPU_REG_SHADER_CONSTANT_000_X
                                      : XE_GPU_REG_SHADER_CONSTANT_256_X;
      const bool analyzed = shader->is_ucode_analyzed();
      std::string text;
      for (uint32_t r = 0; r < (analyzed ? 256u : 32u); ++r) {
        if (analyzed &&
            !(shader->constant_register_map().float_bitmap[r / 64] &
              (uint64_t(1) << (r % 64)))) {
          continue;
        }
        const uint32_t at = bank + r * 4;
        text += fmt::format(" c{}=({:g} {:g} {:g} {:g})", r,
                            GetFloat(regs, at), GetFloat(regs, at + 1),
                            GetFloat(regs, at + 2), GetFloat(regs, at + 3));
      }
      XELOGD("[xna] trace {} constants{}:{}", is_vertex ? "vs" : "ps",
             analyzed ? "" : " (not analyzed yet, c0-c31)", text);
    }
  }

  const bool issued = processor->HostedIssueDraw(
      ToXenosPrimitive(draw.primitive_type), index_count, draw.indexed,
      // The same physical address VGT_DMA_BASE gets. IndexBufferInfo::guest_base
      // is a physical address - the primitive processor reads the indices out of
      // shared memory with it - and handing it the virtual one had it fetching
      // from somewhere else entirely.
      memory->GetPhysicalAddress(index_base), draw.index_32bit,
      static_cast<uint32_t>(xenos::Endian::kNone), index_bytes_left);

  // A shader is only analyzed once the draw reaches pipeline configuration, so
  // an analyzed shader is the proof that this draw got past every early exit.
  const bool reached_pipeline =
      vertex_shader && vertex_shader->is_ucode_analyzed();
  if (reached_pipeline) {
    frame_counters.reached_pipeline.fetch_add(1);
  }
  if (!issued) {
    frame_counters.refused.fetch_add(1);
  } else if (reached_pipeline && rasterizes) {
    frame_counters.drawn.fetch_add(1);
    if (draw.target_count >= 2 && draw.target_formats[0] == 9) {
      frame_counters.into_lighting.fetch_add(1);
    }
    for (uint32_t slot = 0; slot < XnaGpuDraw::kMaxTextureSlots; ++slot) {
      if (draw.textures[slot].guest_address) {
        frame_counters.textured.fetch_add(1);
        break;
      }
    }
  }

  // The registers come from each shader's own D3DXSHADER_CONSTANTTABLE, and the
  // microcode says which it reads, so the two can be compared. Checked after
  // the draw, not before: LoadShader does not analyze the microcode, the
  // pipeline path does, so before the draw this map is empty for any shader
  // that has not been drawn with yet.
  // Reported for BOTH stages against their OWN bank. Checking every constant
  // against the vertex shader's map counted pixel constants as misplaced vertex
  // ones and never said a word about whether the pixel shader got anything.
  // Checked on EVERY draw, not the first one per shader - reporting once hides
  // the draw where a register went wrong and shows only the one where it
  // happened to be right.
  const auto report_stage = [&](xe::gpu::Shader* shader, const char* label,
                                uint32_t bank_base) {
    if (!shader || !shader->is_ucode_analyzed() || draw.constants.empty()) {
      return;
    }
    const uint64_t hash = shader->ucode_data_hash();
    const auto& map = shader->constant_register_map();
    uint32_t written_and_read = 0;
    uint32_t written_in_bank = 0;
    for (const XnaGpuConstant& constant : draw.constants) {
      if (constant.register_index < bank_base ||
          constant.register_index >= bank_base + 256) {
        continue;
      }
      const uint32_t index = constant.register_index - bank_base;
      ++written_in_bank;
      if (map.float_bitmap[index / 64] & (uint64_t(1) << (index % 64))) {
        ++written_and_read;
      }
    }
    uint32_t read_total = 0;
    for (uint32_t word = 0; word < 4; ++word) {
      read_total += xe::bit_count(map.float_bitmap[word]);
    }
    // Silent while every register the shader reads was written, loud when one
    // was not - a shader transforming by an unwritten register is the whole
    // failure, and saying so on every healthy draw only buries it.
    if (written_and_read < read_total) {
      XELOGW(
          "[xna] constants for {} {:016X} ({}): {} written to its bank, shader "
          "reads {}, only {} land where it reads",
          label, hash, xe::kernel::xna::HostedShaderOriginFor(hash),
          written_in_bank, read_total, written_and_read);
    }
  };
  report_stage(vertex_shader, "VS", 0);
  report_stage(pixel_shader, "PS", 256);

  if (issued && rasterizes) {
    last_surface.pitch.store(width);
    last_surface.height.store(height);
    last_surface.base.store(XnaGpuEdramBaseForSlot(width, height, 0, 1));
    last_surface.format.store(XnaGpuColorFormatFor(draw.target_formats[0]));
    last_surface.targets.store(target_count);
  }
  if (issued) {
    drew_this_frame.store(true);
    drew_ever.store(true);
  }
}

}  // namespace

// XNA SurfaceFormat to the texture format a fetch constant names. Only the
// formats this title's content actually arrives in are mapped; anything else
// falls back to 8_8_8_8 rather than fetching through a format that means
// something different.
xenos::TextureFormat XnaGpuTextureFormatFor(uint32_t xna_surface_format) {
  switch (xna_surface_format) {
    case 0:  return xenos::TextureFormat::k_8_8_8_8;       // Color
    case 1:  return xenos::TextureFormat::k_5_6_5;         // Bgr565
    case 2:  return xenos::TextureFormat::k_1_5_5_5;       // Bgra5551
    case 3:  return xenos::TextureFormat::k_4_4_4_4;       // Bgra4444
    case 4:  return xenos::TextureFormat::k_DXT1;          // Dxt1
    case 5:  return xenos::TextureFormat::k_DXT2_3;        // Dxt3
    case 6:  return xenos::TextureFormat::k_DXT4_5;        // Dxt5
    case 7:  return xenos::TextureFormat::k_8_8;           // NormalizedByte2
    case 8:  return xenos::TextureFormat::k_8_8_8_8;       // NormalizedByte4
    case 9:  return xenos::TextureFormat::k_2_10_10_10;    // Rgba1010102
    case 10: return xenos::TextureFormat::k_16_16;         // Rg32
    case 11: return xenos::TextureFormat::k_16_16_16_16;   // Rgba64
    case 12: return xenos::TextureFormat::k_8;             // Alpha8
    case 13: return xenos::TextureFormat::k_32_FLOAT;      // Single
    case 14: return xenos::TextureFormat::k_32_32_FLOAT;   // Vector2
    case 15: return xenos::TextureFormat::k_32_32_32_32_FLOAT;  // Vector4
    case 16: return xenos::TextureFormat::k_16_FLOAT;      // HalfSingle
    case 17: return xenos::TextureFormat::k_16_16_FLOAT;   // HalfVector2
    case 18: return xenos::TextureFormat::k_16_16_16_16_FLOAT;  // HalfVector4
    default: return xenos::TextureFormat::k_8_8_8_8;
  }
}

// How wide the byte swap has to be for console-authored pixels of a given
// format. The unit is the format's smallest addressable field, not the texel:
// DXT blocks hold 16 bit colour endpoints, so they swap in halfwords even
// though a block is eight or sixteen bytes.
xenos::Endian XnaGpuTextureEndianFor(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_8_8_8_8:
      // Four 8 bit channels in one dword, written big-endian by the console.
      return format == xenos::TextureFormat::k_8 ? xenos::Endian::kNone
                                                 : xenos::Endian::k8in32;
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_1_5_5_5:
    case xenos::TextureFormat::k_4_4_4_4:
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT4_5:
      return xenos::Endian::k8in16;
    default:
      return xenos::Endian::k8in32;
  }
}

uint32_t XnaGpuColorFormatFor(uint32_t xna_surface_format) {
  using xenos::ColorRenderTargetFormat;
  switch (xna_surface_format) {
    case 0:   // Color
      return uint32_t(ColorRenderTargetFormat::k_8_8_8_8);
    case 9:   // Rgba1010102 - the light accumulation targets.
      return uint32_t(ColorRenderTargetFormat::k_2_10_10_10);
    case 10:  // Rg32
      return uint32_t(ColorRenderTargetFormat::k_16_16);
    case 11:  // Rgba64
      return uint32_t(ColorRenderTargetFormat::k_16_16_16_16);
    case 13:  // Single
      return uint32_t(ColorRenderTargetFormat::k_32_FLOAT);
    case 14:  // Vector2
      return uint32_t(ColorRenderTargetFormat::k_32_32_FLOAT);
    case 16:  // HalfSingle - no one-channel half target, the pair holds it.
    case 17:  // HalfVector2 - linear view depth and specular power.
      return uint32_t(ColorRenderTargetFormat::k_16_16_FLOAT);
    case 18:  // HalfVector4
      return uint32_t(ColorRenderTargetFormat::k_16_16_16_16_FLOAT);
    case 19:  // HdrBlendable is 7e3 on the Xbox 360, not a half format.
      return uint32_t(ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
    default:
      return uint32_t(ColorRenderTargetFormat::k_8_8_8_8);
  }
}

// The texture-side format a resolve must write so the copy stays bitwise
// equivalent to the render target it reads - the pairs Xenia checks in
// IsColorResolveFormatBitwiseEquivalent.
// THE CONSOLE'S OWN CONVERSIONS, not ours - see xna_runtimehost.h, where each
// of these is the table read out of RuntimeHost.xex with the address it came
// from. These three wrappers keep the call sites unchanged and fall back to
// the value the console would have refused the call over, so an enum nobody
// converted still reaches the log rather than becoming a plausible default.
static xenos::BlendFactor XenosBlendFactorForXna(uint32_t xna) {
  xenos::BlendFactor value = xenos::BlendFactor::kOne;
  if (!xe::kernel::xna::XnaBlendFactorToXenos(xna, &value)) {
    XELOGW("[xna] no conversion for XNA Blend {} - the console refuses this", xna);
  }
  return value;
}

static xenos::BlendOp XenosBlendOpForXna(uint32_t xna) {
  xenos::BlendOp value = xenos::BlendOp::kAdd;
  if (!xe::kernel::xna::XnaBlendOpToXenos(xna, &value)) {
    XELOGW("[xna] no conversion for XNA BlendFunction {} - the console refuses this", xna);
  }
  return value;
}

static xenos::CompareFunction XenosCompareForXnaCompare(uint32_t xna) {
  xenos::CompareFunction value = xenos::CompareFunction::kAlways;
  if (!xe::kernel::xna::XnaCompareToXenos(xna, &value)) {
    XELOGW("[xna] no conversion for XNA CompareFunction {} - the console refuses this", xna);
  }
  return value;
}

static xenos::Endian128 ResolveDestEndianFor(uint32_t xna_surface_format) {
  switch (XnaGpuTextureEndianFor(XnaGpuTextureFormatFor(xna_surface_format))) {
    case xenos::Endian::kNone:
      return xenos::Endian128::kNone;
    case xenos::Endian::k8in16:
      return xenos::Endian128::k8in16;
    case xenos::Endian::k16in32:
      return xenos::Endian128::k16in32;
    default:
      return xenos::Endian128::k8in32;
  }
}

static xenos::ColorFormat ResolveDestFormatFor(uint32_t xna_surface_format) {
  switch (static_cast<xenos::ColorRenderTargetFormat>(
      XnaGpuColorFormatFor(xna_surface_format))) {
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      return xenos::ColorFormat::k_2_10_10_10;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return xenos::ColorFormat::k_16_16;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return xenos::ColorFormat::k_16_16_16_16;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return xenos::ColorFormat::k_16_16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return xenos::ColorFormat::k_16_16_16_16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return xenos::ColorFormat::k_32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return xenos::ColorFormat::k_32_32_FLOAT;
    default:
      return xenos::ColorFormat::k_8_8_8_8;
  }
}

static uint32_t XnaGpuEdramTilesFor(uint32_t width, uint32_t height) {
  const uint32_t pitch_tiles = xenos::GetSurfacePitchTiles(
      width ? width : 1, xenos::MsaaSamples::k1X, false);
  const uint32_t height_tiles =
      (std::max<uint32_t>(height, 1) + xenos::kEdramTileHeightSamples - 1) /
      xenos::kEdramTileHeightSamples;
  return pitch_tiles * height_tiles;
}

static uint32_t XnaGpuEdramSurfaceCount(uint32_t target_count) {
  return std::min<uint32_t>(std::max<uint32_t>(target_count, 1), 4) + 1;
}

static uint32_t XnaGpuEdramBudgetTiles(uint32_t target_count) {
  return xenos::kEdramTileCount / XnaGpuEdramSurfaceCount(target_count);
}

static uint32_t XnaGpuBandRows(uint32_t width, uint32_t target_count,
                               const uint32_t* formats) {
  const uint32_t pitch_tiles = xenos::GetSurfacePitchTiles(
      width ? width : 1, xenos::MsaaSamples::k1X, false);
  uint32_t row_tiles = pitch_tiles;
  const uint32_t count =
      std::min<uint32_t>(std::max<uint32_t>(target_count, 1), 4);
  for (uint32_t slot = 0; formats && slot < count; ++slot) {
    if (xenos::IsColorRenderTargetFormat64bpp(
            static_cast<xenos::ColorRenderTargetFormat>(
                XnaGpuColorFormatFor(formats[slot])))) {
      row_tiles = pitch_tiles * 2;
    }
  }
  if (!row_tiles) {
    return 0;
  }
  const uint32_t tile_rows = XnaGpuEdramBudgetTiles(target_count) / row_tiles;
  return (tile_rows * xenos::kEdramTileHeightSamples) & ~uint32_t(31);
}

uint32_t XnaGpuEdramDepthBase(uint32_t width, uint32_t height,
                              uint32_t target_count) {
  const uint32_t tiles = XnaGpuEdramTilesFor(width, height);
  if (!tiles) {
    return kXnaGpuNoDepth;
  }
  const uint32_t budget = XnaGpuEdramBudgetTiles(target_count);
  if (tiles <= budget || XnaGpuBandRows(width, target_count, nullptr)) {
    return (XnaGpuEdramSurfaceCount(target_count) - 1) * budget;
  }
  const uint64_t base = uint64_t(target_count) * tiles;
  if (base + tiles > xenos::kEdramTileCount) {
    return kXnaGpuNoDepth;
  }
  return static_cast<uint32_t>(base);
}

uint32_t XnaGpuEdramBaseForSlot(uint32_t width, uint32_t height,
                                uint32_t slot, uint32_t target_count) {
  if (!slot) {
    return 0;
  }
  const uint32_t tiles_per_slot = XnaGpuEdramTilesFor(width, height);
  const uint32_t budget = XnaGpuEdramBudgetTiles(target_count);
  if (tiles_per_slot && (tiles_per_slot <= budget ||
                         XnaGpuBandRows(width, target_count, nullptr))) {
    return slot * budget;
  }
  // EDRAM addressing is 11 bits and periodic, so a base past the end wraps onto
  // another slot's tiles instead of failing. Slots that cannot fit share slot
  // 0's region rather than silently corrupting a neighbour.
  const uint64_t base = uint64_t(slot) * tiles_per_slot;
  if (!tiles_per_slot || base >= xenos::kEdramTileCount) {
    return 0;
  }
  return static_cast<uint32_t>(base);
}

bool XnaGpuBeginTiledPass(uint32_t width, uint32_t height,
                          uint32_t target_count, const uint32_t* formats) {
  if (XnaDirectActive()) {
    return false;
  }
  const uint32_t rows = XnaGpuBandRows(width, target_count, formats);
  if (!rows || rows >= height) {
    return false;
  }
  std::lock_guard<std::mutex> lock(tiled_pass_mutex);
  tiled_pass.active = true;
  tiled_pass.height = height;
  tiled_pass.band_rows = rows;
  tiled_pass.ops.clear();
  XELOGD("[xna] tiled pass: {}x{}, {} target(s), bands of {} rows", width,
         height, target_count, rows);
  return true;
}

void XnaGpuEndTiledPass() {
  TiledPass pass;
  {
    std::lock_guard<std::mutex> lock(tiled_pass_mutex);
    if (!tiled_pass.active) {
      return;
    }
    pass = std::move(tiled_pass);
    tiled_pass = TiledPass();
  }
  auto* processor = Processor();
  if (!processor || pass.ops.empty()) {
    return;
  }
  auto ops =
      std::make_shared<std::vector<std::function<void(uint32_t, uint32_t)>>>(
          std::move(pass.ops));
  const uint32_t height = pass.height;
  const uint32_t rows = pass.band_rows;
  processor->CallInThread([ops, height, rows]() {
    for (uint32_t y0 = 0; y0 < height; y0 += rows) {
      const uint32_t band = std::min(rows, height - y0);
      for (auto& op : *ops) {
        op(y0, band);
      }
    }
  });
}

bool XnaGpuFrontBuffer(uint32_t* out_address, uint32_t* out_bytes,
                       uint32_t* out_width, uint32_t* out_height) {
  if (!front_buffer_guest || !front_buffer_width || !front_buffer_height) {
    return false;
  }
  if (out_address) {
    *out_address = front_buffer_guest;
  }
  if (out_bytes) {
    *out_bytes = front_buffer_bytes;
  }
  if (out_width) {
    *out_width = front_buffer_width;
  }
  if (out_height) {
    *out_height = front_buffer_height;
  }
  return true;
}

static void SetResolveWindow(RegisterFile& regs, uint32_t width,
                             uint32_t height) {
  Set(regs, XE_GPU_REG_PA_SC_WINDOW_OFFSET, 0);
  Set(regs, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, 0);
  Set(regs, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, (height << 16) | width);
  Set(regs, XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL, 0);
  Set(regs, XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR, (height << 16) | width);
  reg::PA_SU_SC_MODE_CNTL su_sc;
  su_sc.value = regs.values[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  su_sc.vtx_window_offset_enable = 0;
  Set(regs, XE_GPU_REG_PA_SU_SC_MODE_CNTL, su_sc.value);
}

void XnaGpuResolveTarget(const XnaGpuTarget& target, uint32_t edram_base) {
  if (XnaDirectActive()) {
    return;
  }
  auto* processor = Processor();
  if (!processor || !Registers() || !target.guest_address || !target.width) {
    return;
  }
  EnsureResolveRect();
  XnaGpuTarget copy = target;
  auto resolve = [copy, edram_base](uint32_t band_y0, uint32_t band_rows) {
    auto* processor_in_thread = Processor();
    auto* registers = Registers();
    auto* memory = kernel_state() ? kernel_state()->memory() : nullptr;
    if (!processor_in_thread || !registers || !memory || !resolve_rect_guest) {
      return;
    }
    RegisterFile& regs = *registers;

    reg::RB_SURFACE_INFO surface_info;
    surface_info.value = 0;
    surface_info.surface_pitch = copy.width;
    surface_info.msaa_samples = xenos::MsaaSamples::k1X;
    Set(regs, XE_GPU_REG_RB_SURFACE_INFO, surface_info.value);
    const uint32_t rows = band_rows ? band_rows : copy.height;
    SetResolveWindow(regs, copy.width, rows);

    reg::RB_COLOR_INFO color_info;
    color_info.value = 0;
    color_info.color_base = edram_base;
    // The same format the draw wrote this slot with, or the resolve reads the
    // tiles back as something they are not.
    color_info.color_format = static_cast<xenos::ColorRenderTargetFormat>(
        XnaGpuColorFormatFor(copy.format));
    Set(regs, XE_GPU_REG_RB_COLOR_INFO, color_info.value);

    reg::RB_COPY_CONTROL copy_control;
    copy_control.value = 0;
    copy_control.copy_src_select = 0;
    copy_control.copy_sample_select = xenos::CopySampleSelect::k0;
    copy_control.copy_command = xenos::CopyCommand::kRaw;
    Set(regs, XE_GPU_REG_RB_COPY_CONTROL, copy_control.value);
    const uint32_t bpp_log2 =
        xenos::IsColorRenderTargetFormat64bpp(
            static_cast<xenos::ColorRenderTargetFormat>(
                XnaGpuColorFormatFor(copy.format)))
            ? 3u
            : 2u;
    const int32_t band_offset =
        band_rows ? xe::gpu::texture_address::Tiled2D(
                        0, int32_t(band_y0), xe::align(copy.width, 32u),
                        bpp_log2)
                  : 0;
    Set(regs, XE_GPU_REG_RB_COPY_DEST_BASE,
        memory->GetPhysicalAddress(copy.guest_address) +
            uint32_t(band_offset));

    reg::RB_COPY_DEST_PITCH copy_pitch;
    copy_pitch.value = 0;
    copy_pitch.copy_dest_pitch = copy.width;
    copy_pitch.copy_dest_height = copy.height;
    Set(regs, XE_GPU_REG_RB_COPY_DEST_PITCH, copy_pitch.value);

    reg::RB_COPY_DEST_INFO copy_info;
    copy_info.value = 0;
    copy_info.copy_dest_endian = ResolveDestEndianFor(copy.format);
    copy_info.copy_dest_format = ResolveDestFormatFor(copy.format);
    copy_info.copy_dest_number =
        xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction;
    Set(regs, XE_GPU_REG_RB_COPY_DEST_INFO, copy_info.value);

    const uint32_t rect_guest = ResolveRectFor(copy.width, rows);
    if (!rect_guest) {
      return;
    }
    xenos::xe_gpu_vertex_fetch_t rect_fetch = {};
    rect_fetch.type = xenos::FetchConstantType::kVertex;
    rect_fetch.address = memory->GetPhysicalAddress(rect_guest) >> 2;
    rect_fetch.endian = xenos::Endian::kNone;
    rect_fetch.size = 6;
    Set(regs, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 0, rect_fetch.dword_0);
    Set(regs, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1, rect_fetch.dword_1);

    XELOGD("[xna]    resolve copy issued: {}",
           processor_in_thread->HostedIssueCopy());
  };
  if (RecordTiledOp(resolve)) {
    return;
  }
  processor->CallInThread([resolve]() { resolve(0, 0); });
}

void XnaGpuClearTarget(const XnaGpuTarget& target, uint32_t edram_base,
                       uint32_t depth_edram_base, const float* color,
                       bool clear_color, bool clear_depth, float depth) {
  if (XnaDirectActive()) {
    if (clear_color || clear_depth) {
      drew_ever.store(true);
      XnaDirectClear(target, color, clear_color, clear_depth, depth);
    }
    return;
  }
  auto* processor = Processor();
  if (!processor || !Registers() || !target.width ||
      (!clear_color && !clear_depth)) {
    return;
  }
  if (clear_color && clear_depth) {
    XnaGpuClearTarget(target, edram_base, depth_edram_base, color, true, false,
                      depth);
    XnaGpuClearTarget(target, edram_base, depth_edram_base, color, false, true,
                      depth);
    return;
  }
  EnsureResolveRect();
  const uint32_t scratch = EnsureClearScratch(target.width, target.height);
  if (!scratch) {
    return;
  }
  XnaGpuTarget copy = target;
  copy.guest_address = scratch;
  const float rgba[4] = {color ? color[0] : 0.0f, color ? color[1] : 0.0f,
                         color ? color[2] : 0.0f, color ? color[3] : 0.0f};
  auto clear = [copy, edram_base, depth_edram_base, rgba, clear_color,
                clear_depth, depth](uint32_t, uint32_t band_rows) {
    auto* processor_in_thread = Processor();
    auto* registers = Registers();
    auto* memory = kernel_state() ? kernel_state()->memory() : nullptr;
    if (!processor_in_thread || !registers || !memory || !resolve_rect_guest) {
      return;
    }
    RegisterFile& regs = *registers;

    reg::RB_SURFACE_INFO surface_info;
    surface_info.value = 0;
    surface_info.surface_pitch = copy.width;
    surface_info.msaa_samples = xenos::MsaaSamples::k1X;
    Set(regs, XE_GPU_REG_RB_SURFACE_INFO, surface_info.value);
    const uint32_t rows = band_rows ? band_rows : copy.height;
    SetResolveWindow(regs, copy.width, rows);

    reg::RB_COLOR_INFO color_info;
    color_info.value = 0;
    color_info.color_base = edram_base;
    color_info.color_format = static_cast<xenos::ColorRenderTargetFormat>(
        XnaGpuColorFormatFor(copy.format));
    Set(regs, XE_GPU_REG_RB_COLOR_INFO, color_info.value);

    reg::RB_DEPTH_INFO clear_depth_info;
    clear_depth_info.value = 0;
    clear_depth_info.depth_base = depth_edram_base;
    clear_depth_info.depth_format = xenos::DepthRenderTargetFormat::kD24S8;
    Set(regs, XE_GPU_REG_RB_DEPTH_INFO, clear_depth_info.value);

    const auto quantize = [](float value) -> uint32_t {
      const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
      return static_cast<uint32_t>(clamped * 255.0f + 0.5f) & 0xFF;
    };
    Set(regs, XE_GPU_REG_RB_COLOR_CLEAR,
        (quantize(rgba[3]) << 24) | (quantize(rgba[2]) << 16) |
            (quantize(rgba[1]) << 8) | quantize(rgba[0]));
    Set(regs, XE_GPU_REG_RB_COLOR_CLEAR_LO, 0);
    const float held = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
    Set(regs, XE_GPU_REG_RB_DEPTH_CLEAR,
        (static_cast<uint32_t>(held * 16777215.0f + 0.5f) << 8) | 0xFF);

    reg::RB_COPY_CONTROL copy_control;
    copy_control.value = 0;
    copy_control.copy_src_select = 0;
    copy_control.copy_sample_select = xenos::CopySampleSelect::k0;
    copy_control.copy_command = xenos::CopyCommand::kRaw;
    copy_control.color_clear_enable = clear_color ? 1 : 0;
    copy_control.depth_clear_enable = clear_depth ? 1 : 0;
    Set(regs, XE_GPU_REG_RB_COPY_CONTROL, copy_control.value);
    Set(regs, XE_GPU_REG_RB_COPY_DEST_BASE,
        memory->GetPhysicalAddress(copy.guest_address));

    reg::RB_COPY_DEST_PITCH copy_pitch;
    copy_pitch.value = 0;
    copy_pitch.copy_dest_pitch = copy.width;
    copy_pitch.copy_dest_height = copy.height;
    Set(regs, XE_GPU_REG_RB_COPY_DEST_PITCH, copy_pitch.value);

    reg::RB_COPY_DEST_INFO copy_info;
    copy_info.value = 0;
    copy_info.copy_dest_endian = ResolveDestEndianFor(copy.format);
    copy_info.copy_dest_format = ResolveDestFormatFor(copy.format);
    copy_info.copy_dest_number =
        xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction;
    Set(regs, XE_GPU_REG_RB_COPY_DEST_INFO, copy_info.value);

    const uint32_t rect_guest = ResolveRectFor(copy.width, rows);
    if (!rect_guest) {
      return;
    }
    xenos::xe_gpu_vertex_fetch_t rect_fetch = {};
    rect_fetch.type = xenos::FetchConstantType::kVertex;
    rect_fetch.address = memory->GetPhysicalAddress(rect_guest) >> 2;
    rect_fetch.endian = xenos::Endian::kNone;
    rect_fetch.size = 6;
    Set(regs, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 0, rect_fetch.dword_0);
    Set(regs, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1, rect_fetch.dword_1);

    XELOGD("[xna]    clear copy issued: {}",
           processor_in_thread->HostedIssueCopy());
  };
  if (RecordTiledOp(clear)) {
    return;
  }
  processor->CallInThread([clear]() { clear(0, 0); });
}

bool XnaGpuHasDrawn() { return drew_this_frame.load(); }

bool XnaGpuHasEverDrawn() { return drew_ever.load(); }

void XnaGpuPresent(uint32_t width, uint32_t height) {
  if (XnaDirectActive()) {
    drew_this_frame.store(false);
    XnaDirectPresent();
    return;
  }
  auto* processor = Processor();
  if (!processor || !Registers()) {
    return;
  }
  XnaGpuEndTiledPass();
  drew_this_frame.store(false);
  const uint32_t front = EnsureFrontBuffer(width, height);
  front_buffer_width = width;
  front_buffer_height = height;
  if (!front) {
    return;
  }
  processor->CallInThread([width, height, front]() {
    auto* processor_in_thread = Processor();
    auto* registers = Registers();
    if (!processor_in_thread || !registers) {
      return;
    }
    RegisterFile& regs = *registers;

    // The surface the resolve reads FROM has to be described here too. Left
    // alone it keeps whatever the last draw set, and the HDR passes render at
    // quarter size - so a full-screen resolve was being measured against a
    // 320-wide surface and refused.
    reg::RB_SURFACE_INFO surface_info;
    surface_info.value = 0;
    surface_info.surface_pitch = width;
    surface_info.msaa_samples = xenos::MsaaSamples::k1X;
    Set(regs, XE_GPU_REG_RB_SURFACE_INFO, surface_info.value);
    SetResolveWindow(regs, width, height);

    reg::RB_COLOR_INFO color_info;
    color_info.value = 0;
    color_info.color_base = 0;
    color_info.color_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
    Set(regs, XE_GPU_REG_RB_COLOR_INFO, color_info.value);

    reg::RB_COPY_CONTROL copy_control;
    copy_control.value = 0;
    copy_control.copy_src_select = 0;
    copy_control.copy_sample_select = xenos::CopySampleSelect::k0;
    copy_control.copy_command = xenos::CopyCommand::kRaw;
    Set(regs, XE_GPU_REG_RB_COPY_CONTROL, copy_control.value);
    Set(regs, XE_GPU_REG_RB_COPY_DEST_BASE,
        kernel_state()->memory()->GetPhysicalAddress(front));

    reg::RB_COPY_DEST_PITCH copy_pitch;
    copy_pitch.value = 0;
    copy_pitch.copy_dest_pitch = width;
    copy_pitch.copy_dest_height = height;
    Set(regs, XE_GPU_REG_RB_COPY_DEST_PITCH, copy_pitch.value);

    reg::RB_COPY_DEST_INFO copy_info;
    copy_info.value = 0;
    copy_info.copy_dest_endian = xenos::Endian128::k8in32;
    copy_info.copy_dest_format = xenos::ColorFormat::k_8_8_8_8;
    copy_info.copy_dest_number = xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction;
    Set(regs, XE_GPU_REG_RB_COPY_DEST_INFO, copy_info.value);

    // THE RESOLVE RECTANGLE COMES FROM VERTEX FETCH CONSTANT 0.
    //
    // draw_util reads three vertices of two floats through vf0 to learn the
    // area to copy, and rejects anything whose size is not six dwords - which
    // is what the title's own vertex stream sitting there produced. The values
    // are written host-order and the fetch says kNone, so GpuSwap leaves them
    // alone.
    const uint32_t rect_guest = ResolveRectFor(width, height);
    if (rect_guest) {
      xenos::xe_gpu_vertex_fetch_t rect_fetch = {};
      rect_fetch.type = xenos::FetchConstantType::kVertex;
      rect_fetch.address =
          kernel_state()->memory()->GetPhysicalAddress(rect_guest) >> 2;
      rect_fetch.endian = xenos::Endian::kNone;
      rect_fetch.size = 6;
      Set(regs, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 0, rect_fetch.dword_0);
      Set(regs, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1, rect_fetch.dword_1);
    }

    XELOGD("[xna]    present copy issued: {}",
           processor_in_thread->HostedIssueCopy());

    // The swap texture is described by texture fetch constant 0, not by the
    // pointer alone - RequestSwapTexture reads it to learn the format and size.
    xenos::xe_gpu_texture_fetch_t fetch = {};
    fetch.type = xenos::FetchConstantType::kTexture;
    fetch.format = xenos::TextureFormat::k_8_8_8_8;
    fetch.endianness = xenos::Endian::k8in32;
    fetch.base_address =
        kernel_state()->memory()->GetPhysicalAddress(front) >> 12;
    fetch.pitch = (width + 31) >> 5;
    fetch.tiled = 1;
    // Without this the size fields are read as size_1d, and a 720-high buffer
    // arrives as a 1D texture 5891328 texels wide.
    fetch.dimension = xenos::DataDimension::k2DOrStacked;
    fetch.size_2d.width = width - 1;
    fetch.size_2d.height = height - 1;
    fetch.size_2d.stack_depth = 0;
    fetch.clamp_x = xenos::ClampMode::kClampToEdge;
    fetch.clamp_y = xenos::ClampMode::kClampToEdge;
    fetch.clamp_z = xenos::ClampMode::kClampToEdge;
    fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    for (uint32_t i = 0; i < 6; ++i) {
      Set(regs, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i,
          reinterpret_cast<const uint32_t*>(&fetch)[i]);
    }

    // THE SWAP, UNGATED.
    //
    // Normally there is no swap here: the sprite pass owns the output so the
    // HUD can be drawn over the scene, and it takes the resolved texture as a
    // background. That hand-off is conditional on three separate things -
    // RequestSwapTexture accepting the descriptor, the background being cached,
    // and the composite wanting to run - and every one of them fails silently.
    //
    // With xna_direct_swap the resolved image goes straight to the screen
    // instead, HUD and all. It answers the only question left: whether the
    // draws put anything in the render target. A picture without a HUD means
    // the scene is fine and the compositor is losing it; a black screen means
    // the draws produced nothing and everything downstream is innocent.
    processor_in_thread->HostedIssueSwap(
        kernel_state()->memory()->GetPhysicalAddress(front), width, height);

    // The whole frame in one line: how many draws the title asked for, and how
    // many survived each stage that can silently discard one.
    XELOGI(
        "[xna] frame: {} submitted, {} never rasterized, {} refused, {} "
        "reached pipeline, {} drawn ({} with a texture, {} into the lighting "
        "buffers) -> resolved {}x{} to {:08X}",
        frame_counters.submitted.exchange(0),
        frame_counters.no_rasterize.exchange(0),
        frame_counters.refused.exchange(0),
        frame_counters.reached_pipeline.exchange(0),
        frame_counters.drawn.exchange(0),
        frame_counters.textured.exchange(0),
        frame_counters.into_lighting.exchange(0), width, height,
        kernel_state()->memory()->GetPhysicalAddress(front));

    // The two surfaces side by side. The resolve can only find what the last
    // draw wrote if all three of base, pitch and format agree.
    XELOGI(
        "[xna] resolve source: last draw wrote pitch {} height {} base {} "
        "format {} ({} target(s)); present reads pitch {} height {} base 0 "
        "format {}",
        last_surface.pitch.load(), last_surface.height.load(),
        last_surface.base.load(), last_surface.format.load(),
        last_surface.targets.load(), width, height,
        uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8));
  });
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
