/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_runtimehost.h"

namespace xe {
namespace kernel {
namespace xna {

using gpu::xenos::BlendFactor;
using gpu::xenos::BlendOp;
using gpu::xenos::CompareFunction;
using gpu::xenos::PrimitiveType;
using gpu::xenos::StencilOp;

// sub_8A1CC458, ids 0x48/0x4C/0x54/0x58 - ColorSourceBlend,
// ColorDestinationBlend, AlphaSourceBlend, AlphaDestinationBlend. The console
// validates 0..12 and then indexes the byte table at 0x8A01B868, whose entries
// select a case block relative to 0x8A1CC4C4. Resolved, that is this:
bool XnaBlendFactorToXenos(uint32_t xna, BlendFactor* out) {
  switch (xna) {
    case 0:
      *out = BlendFactor::kOne;
      return true;  // One
    case 1:
      *out = BlendFactor::kZero;
      return true;  // Zero
    case 2:
      *out = BlendFactor::kSrcColor;
      return true;
    case 3:
      *out = BlendFactor::kOneMinusSrcColor;
      return true;
    case 4:
      *out = BlendFactor::kSrcAlpha;
      return true;
    case 5:
      *out = BlendFactor::kOneMinusSrcAlpha;
      return true;
    case 6:
      *out = BlendFactor::kDstColor;
      return true;
    case 7:
      *out = BlendFactor::kOneMinusDstColor;
      return true;
    case 8:
      *out = BlendFactor::kDstAlpha;
      return true;
    case 9:
      *out = BlendFactor::kOneMinusDstAlpha;
      return true;
    case 10:
      *out = BlendFactor::kConstantColor;
      return true;  // BlendFactor
    case 11:
      *out = BlendFactor::kOneMinusConstantColor;
      return true;
    case 12:
      *out = BlendFactor::kSrcAlphaSaturate;
      return true;  // 16, not 14
    default:
      return false;
  }
}

// sub_8A1CC458, ids 0x50/0x5C - ColorBlendFunction, AlphaBlendFunction.
// Straight-line in the binary rather than table driven, and the only surprise
// is ReverseSubtract landing on 4.
bool XnaBlendOpToXenos(uint32_t xna, BlendOp* out) {
  switch (xna) {
    case 0:
      *out = BlendOp::kAdd;
      return true;  // Add             -> 0
    case 1:
      *out = BlendOp::kSubtract;
      return true;  // Subtract        -> 1
    case 2:
      *out = BlendOp::kRevSubtract;
      return true;  // ReverseSubtract -> 4
    case 3:
      *out = BlendOp::kMin;
      return true;  // Min             -> 2
    case 4:
      *out = BlendOp::kMax;
      return true;  // Max             -> 3
    default:
      return false;
  }
}

// sub_8A1CC458, ids 0x2C/0x68/0x80 - DepthBufferFunction, StencilFunction,
// CounterClockwiseStencilFunction. XNA begins its enum at Always and Xenos
// begins at Never, so every entry moves.
bool XnaCompareToXenos(uint32_t xna, CompareFunction* out) {
  switch (xna) {
    case 0:
      *out = CompareFunction::kAlways;
      return true;  // -> 7
    case 1:
      *out = CompareFunction::kNever;
      return true;  // -> 0
    case 2:
      *out = CompareFunction::kLess;
      return true;  // -> 1
    case 3:
      *out = CompareFunction::kLessEqual;
      return true;  // -> 3
    case 4:
      *out = CompareFunction::kEqual;
      return true;  // -> 2
    case 5:
      *out = CompareFunction::kGreaterEqual;
      return true;  // -> 6
    case 6:
      *out = CompareFunction::kGreater;
      return true;  // -> 4
    case 7:
      *out = CompareFunction::kNotEqual;
      return true;  // -> 5
    default:
      return false;
  }
}

// sub_8A1CC458, ids 0x74/0x78/0x7C/0x90/0x94/0x98/0x9C - the stencil pass,
// fail and depth-fail operations for both faces.
//
// The two middle pairs are crossed over, and it is not a mistake in the
// reading: XNA's Increment and Decrement are the WRAPPING forms and its
// IncrementSaturation and DecrementSaturation are the CLAMPING ones, while
// Xenos lists clamp first. Mapping these by position gives four of the eight
// the wrong behaviour, silently.
bool XnaStencilOpToXenos(uint32_t xna, StencilOp* out) {
  switch (xna) {
    case 0:
      *out = StencilOp::kKeep;
      return true;  // Keep      -> 0
    case 1:
      *out = StencilOp::kZero;
      return true;  // Zero      -> 1
    case 2:
      *out = StencilOp::kReplace;
      return true;  // Replace   -> 2
    case 3:
      *out = StencilOp::kIncrementWrap;
      return true;  // Increment -> 6
    case 4:
      *out = StencilOp::kDecrementWrap;
      return true;  // Decrement -> 7
    case 5:
      *out = StencilOp::kIncrementClamp;
      return true;  // IncrSat   -> 3
    case 6:
      *out = StencilOp::kDecrementClamp;
      return true;  // DecrSat   -> 4
    case 7:
      *out = StencilOp::kInvert;
      return true;  // Invert    -> 5
    default:
      return false;
  }
}

// sub_8A1CC1E0. XNA has only these four, and anything else is E_INVALIDARG on
// the console rather than a silent fallback to triangles.
bool XnaPrimitiveToXenos(uint32_t xna, PrimitiveType* out) {
  switch (xna) {
    case 0:
      *out = PrimitiveType::kTriangleList;
      return true;  // -> 4
    case 1:
      *out = PrimitiveType::kTriangleStrip;
      return true;  // -> 6, not 5
    case 2:
      *out = PrimitiveType::kLineList;
      return true;  // -> 2
    case 3:
      *out = PrimitiveType::kLineStrip;
      return true;  // -> 3
    default:
      return false;
  }
}

// The table at 0x8A0012F8, eight entries of { multiplier, addend }, read as
// count * mult + add and indexed by the XENOS primitive value. Reproduced as a
// switch so an index that never appears cannot quietly return zero indices.
bool XenosPrimitiveIndexCount(PrimitiveType primitive, uint32_t primitive_count,
                              uint32_t* out) {
  switch (primitive) {
    case PrimitiveType::kPointList:
      *out = primitive_count;
      return true;  // 1x + 0
    case PrimitiveType::kLineList:
      *out = primitive_count * 2;
      return true;  // 2x + 0
    case PrimitiveType::kLineStrip:
      *out = primitive_count + 1;
      return true;  // 1x + 1
    case PrimitiveType::kTriangleList:
      *out = primitive_count * 3;
      return true;  // 3x + 0
    case PrimitiveType::kTriangleFan:
    case PrimitiveType::kTriangleStrip:
      *out = primitive_count + 2;
      return true;  // 1x + 2
    default:
      return false;
  }
}

bool XnaVertexElementFormatToDeclType(uint32_t xna, uint32_t* out) {
  switch (xna) {
    case 0:
      *out = 0x2C83A4;
      return true;
    case 1:
      *out = 0x2C23A5;
      return true;
    case 2:
      *out = 0x2A23B9;
      return true;
    case 3:
      *out = 0x1A23A6;
      return true;
    case 4:
      *out = 0x1A2086;
      return true;
    case 5:
      *out = 0x1A2286;
      return true;
    case 6:
      *out = 0x2C0759;
      return true;
    case 7:
      *out = 0x014F5A;
      return true;
    case 8:
      *out = 0x2C0559;
      return true;
    case 9:
      *out = 0x014D5A;
      return true;
    case 10:
      *out = 0x2C075F;
      return true;
    case 11:
      *out = 0x014F60;
      return true;
    default:
      return false;
  }
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
