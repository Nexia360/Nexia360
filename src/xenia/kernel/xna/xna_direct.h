/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_DIRECT_H_
#define XENIA_KERNEL_XNA_XNA_DIRECT_H_

#include <cstdint>

#include "xenia/gpu/xenos.h"
#include "xenia/kernel/xna/xna_gpu.h"

namespace xe {
namespace gpu {
class Shader;
}  // namespace gpu
}  // namespace xe

namespace xe {
namespace kernel {
namespace xna {

bool XnaDirectActive();

bool XnaDirectDraw(const XnaGpuDraw& draw, const XnaGpuStream* streams,
                   uint32_t stream_count);

void XnaDirectClear(const XnaGpuTarget& target, const float* color,
                    bool clear_color, bool clear_depth, float depth);

void XnaDirectPresent();

bool XnaDirectReadBackBuffer(uint8_t* out, uint32_t bytes);

gpu::Shader* XnaDirectLoadShader(gpu::xenos::ShaderType type,
                                 const uint32_t* ucode, uint32_t dword_count);

void XnaDirectSetActiveShaders(gpu::Shader* vertex_shader,
                               gpu::Shader* pixel_shader);

gpu::Shader* XnaDirectActiveShader(bool vertex);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_DIRECT_H_
