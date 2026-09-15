/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_PACKETS_H_
#define XENIA_KERNEL_XNA_XNA_PACKETS_H_

#include <cstdint>

namespace xe {
namespace kernel {
namespace xna {

// The command vocabulary, straight out of the HLCBPacketType enum in
// MXF.Graphics.dlx. The gaps are real - the console never used 3, 4, 23 and
// the rest - so the values are written out rather than renumbered.
enum class HlcbPacketType : uint32_t {
  kInvalid = 0,
  kClear = 1,
  kSetIndexBuffer = 2,
  kSetVertexDeclaration = 5,
  kSetStreamSource = 6,
  kSetTexture = 7,
  kEffectSetValueBool = 8,
  kEffectSetValueFloat = 9,
  kEffectSetValueInt = 10,
  kEffectSetValueMatrix = 11,
  kEffectSetValueString = 12,
  kEffectSetValueTexture = 13,
  kEffectSetValueVector = 14,
  kEffectSetValueMatrixTranspose = 15,
  kEffectSetValueBoolArray = 16,
  kEffectSetValueFloatArray = 17,
  kEffectSetValueIntArray = 18,
  kEffectSetValueMatrixArray = 19,
  kEffectSetValueVectorArray = 20,
  kEffectSetValueMatrixTransposeArray = 21,
  kEffectApply = 22,
  kEffectSetTexture = 24,
  kSetBlendState = 25,
  kSetDepthStencilState = 26,
  kSetRasterizerState = 27,
  kSetSamplerState = 28,
  kSetHighFrequencyState = 29,
  kSetRenderTargets = 47,
  kSetScissorRect = 48,
  kSetViewPort = 55,
  kDrawPrimitives = 56,
  kDrawIndexedPrimitives = 57,
  kDrawUserIndexedPrimitives = 58,
  kDrawSprites = 59,
  kEffectSetTechnique = 62,
  kDrawUserPrimitives = 65,
  kBeginQuery = 66,
  kEndQuery = 67,
  kDrawInstancedPrimitives = 68,
};

struct HlcbClear {
  uint32_t options = 0;  // ClearOptions: 1 target, 2 depth, 4 stencil
  float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float depth = 1.0f;
  int32_t stencil = 0;
};

struct HlcbViewport {
  int32_t x = 0;
  int32_t y = 0;
  int32_t width = 0;
  int32_t height = 0;
  float min_depth = 0.0f;
  float max_depth = 1.0f;
};

struct HlcbRect {
  int32_t x = 0;
  int32_t y = 0;
  int32_t width = 0;
  int32_t height = 0;
};

struct HlcbStreamSource {
  uint32_t vertex_buffer = 0;
  uint32_t stream_index = 0;
  uint32_t vertex_offset = 0;
  uint32_t stride = 0;
  uint32_t instance_frequency = 0;
};

struct HlcbDraw {
  bool indexed = false;
  uint32_t primitive_type = 0;
  uint32_t primitive_count = 0;
  uint32_t start_vertex = 0;
  uint32_t base_vertex = 0;
  uint32_t min_vertex_index = 0;
  uint32_t vertex_count = 0;
  uint32_t start_index = 0;
  // Set only for the DrawUser* forms, which carry their geometry inline rather
  // than naming a buffer.
  const uint8_t* user_data = nullptr;
  uint32_t user_data_size = 0;
  uint32_t vertex_stride = 0;
};

struct HlcbSprites {
  uint32_t texture = 0;
  // How many 56-byte records `data` holds.
  uint32_t count = 0;
  int32_t texture_width = 0;
  int32_t texture_height = 0;
  const uint8_t* data = nullptr;
  uint32_t data_size = 0;
};

// What a decoded stream is played into. Kept separate from the decoder so the
// same walk can drive the GPU, a trace, or a test.
class HlcbSink {
 public:
  virtual ~HlcbSink() = default;
  virtual void Clear(const HlcbClear& clear) {}
  virtual void SetViewport(const HlcbViewport& viewport) {}
  virtual void SetScissorRect(const HlcbRect& rect) {}
  virtual void SetStreamSource(const HlcbStreamSource& stream) {}
  virtual void SetIndexBuffer(uint32_t handle) {}
  virtual void SetVertexDeclaration(uint32_t handle) {}
  virtual void SetTexture(uint32_t sampler, uint32_t texture) {}
  // `slot` is the sampler index for kSetSamplerState, and 0 for the three
  // state objects that are not bound per slot.
  virtual void SetState(HlcbPacketType type, uint32_t handle,
                        uint32_t slot) {}
  virtual void SetHighFrequencyState(uint32_t state, uint32_t value) {}
  // `handles` are the render target handles, `count` how many. Zero means the
  // title went back to the back buffer.
  virtual void SetRenderTargets(const uint32_t* handles, uint32_t count) {}
  virtual void EffectValue(HlcbPacketType type, uint32_t effect,
                           uint32_t parameter, uint32_t count,
                           const uint8_t* data, uint32_t bytes) {}
  // A texture bound by EFFECT PARAMETER rather than by sampler slot. The slot
  // is not a property of the texture - the same name is s0 in one shader of an
  // effect and s1 in another - so only the name can be recorded here, and the
  // slot is resolved against whichever shader is about to run.
  virtual void EffectTexture(uint32_t effect, uint32_t parameter,
                             uint32_t texture) {}
  virtual void EffectTechnique(uint32_t effect, uint32_t technique) {}
  virtual void Draw(const HlcbDraw& draw) {}
  virtual void DrawSprites(const HlcbSprites& sprites) {}
  virtual void Other(HlcbPacketType type, uint32_t handle) {}
};

// Bytes per element for the EffectSetValue family - 64 for a matrix, 16 for a
// vector, 4 for a scalar. UINT32_MAX when the type is not one of them. This is
// the stride of the title's own data, which is the only way to find element e
// of an array: the shader may reserve three registers per matrix while the
// title still hands over all sixteen floats of each.
uint32_t EffectValueElementSize(uint32_t type);

class HlcbDecoder {
 public:
  // Walks one command buffer, calling the sink for each packet. Returns false
  // if the stream stopped making sense - a packet decoded at the wrong size
  // shifts every packet after it, so the walk stops at the first header that is
  // not a command rather than inventing a frame out of misread bytes.
  static bool Walk(const uint8_t* data, uint32_t size, HlcbSink* sink);

 private:
  static void Dispatch(HlcbPacketType type, uint32_t handle,
                       const uint8_t* body, uint32_t size, HlcbSink* sink);
};

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_PACKETS_H_
