/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_AVATAR_ASSETS_H_
#define XENIA_KERNEL_XAM_XAM_AVATAR_ASSETS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

constexpr uint32_t kAvatarResultBufferSize = 0x10000;
constexpr uint32_t kAvatarGpuBufferSize = 0x200000;

void SetAvatarCoordinateSystem(uint32_t coordinate_system);
uint32_t AvatarCoordinateSystem();

X_RESULT BuildAvatarAssets(const uint8_t* metadata, size_t metadata_size,
                           uint32_t component_mask, uint32_t result_guest,
                           uint32_t gpu_guest);

// Fills in the mip chain of every texture in an asset buffer BuildAvatarAssets
// produced, writing the levels into the caller's guest buffer and storing the
// address and level count back into each texture record.
X_RESULT GenerateAvatarMipMaps(uint32_t assets_guest, uint32_t buffer_guest,
                               uint32_t buffer_size);

X_RESULT LoadAvatarAnimation(const std::array<uint8_t, 16>& asset_id,
                             uint32_t object_guest, const std::string& label);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_AVATAR_ASSETS_H_
