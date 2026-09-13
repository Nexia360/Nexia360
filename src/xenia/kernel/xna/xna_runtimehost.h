/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_RUNTIMEHOST_H_
#define XENIA_KERNEL_XNA_XNA_RUNTIMEHOST_H_

#include <cstdint>

#include "xenia/gpu/xenos.h"

namespace xe {
namespace kernel {
namespace xna {

// RUNTIMEHOST'S OWN CONVERSIONS, READ OUT OF THE BINARY.
//
// XNA is a PC API. Its enums are its own, they are not Direct3D's and they are
// not the Xenos ones, and the console runtime converts them before the GPU
// ever sees a value. Every function here is that conversion, taken from
// RuntimeHost.xex rather than deduced from what looks reasonable - the
// address each one came from is on it, so it can be checked again.
//
// Guessing these is how the hosted path collected its bugs. Two of the tables
// below have entries that no amount of reasoning would produce: XNA's
// Increment maps to Xenos kIncrementWrap while its IncrementSaturation maps to
// kIncrementClamp, and XNA's Equal maps to Xenos 2 while its LessEqual maps to
// 3. Read them, do not re-derive them.
//
// EVERY ONE RETURNS false FOR A VALUE IT DOES NOT KNOW, exactly where
// RuntimeHost returns E_INVALIDARG. That matters more than it looks: a
// converter with a silent default turns an unhandled enum into a plausible
// wrong answer, and the hosted path has lost days to precisely that.

// XNA Blend -> Xenos BlendFactor.
// RuntimeHost sub_8A1CC458 ids 0x48/0x4C/0x54/0x58, through the byte jump
// table at 0x8A01B868 with its case blocks based at 0x8A1CC4C4.
bool XnaBlendFactorToXenos(uint32_t xna, gpu::xenos::BlendFactor* out);

// XNA BlendFunction -> Xenos BlendOp.
// RuntimeHost sub_8A1CC458 ids 0x50/0x5C. Note ReverseSubtract is 4, not 2 -
// Xenos puts Min and Max where D3D puts the subtract modes.
bool XnaBlendOpToXenos(uint32_t xna, gpu::xenos::BlendOp* out);

// XNA CompareFunction -> Xenos CompareFunction.
// RuntimeHost sub_8A1CC458 ids 0x2C/0x68/0x80. XNA counts from Always, Xenos
// from Never, so nothing about this mapping is an identity.
bool XnaCompareToXenos(uint32_t xna, gpu::xenos::CompareFunction* out);

// XNA StencilOperation -> Xenos StencilOp.
// RuntimeHost sub_8A1CC458 ids 0x74/0x78/0x7C/0x90/0x94/0x98/0x9C. XNA's
// Increment/Decrement WRAP and its *Saturation variants CLAMP, which is the
// opposite of the order the two enums list them in.
bool XnaStencilOpToXenos(uint32_t xna, gpu::xenos::StencilOp* out);

// XNA PrimitiveType -> Xenos PrimitiveType.
// RuntimeHost sub_8A1CC1E0. TriangleStrip is 6 and TriangleFan is 5 on Xenos,
// the reverse of PC Direct3D 9.
bool XnaPrimitiveToXenos(uint32_t xna, gpu::xenos::PrimitiveType* out);

// Indices a primitive count implies, for an ALREADY CONVERTED Xenos primitive
// type. RuntimeHost's table at 0x8A0012F8, read as count * mult + add and
// indexed by the Xenos value, which is why the conversion above has to happen
// first.
bool XenosPrimitiveIndexCount(gpu::xenos::PrimitiveType primitive,
                              uint32_t primitive_count, uint32_t* out);

bool XnaVertexElementFormatToDeclType(uint32_t xna, uint32_t* out);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_RUNTIMEHOST_H_
