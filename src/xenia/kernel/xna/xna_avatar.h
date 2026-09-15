/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_AVATAR_H_
#define XENIA_KERNEL_XNA_XNA_AVATAR_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "xenia/kernel/xna/xna_avatar_format.h"
#include "xenia/kernel/xna/xna_gpu.h"

namespace xe {
namespace kernel {
namespace xna {

struct XnaAvatarDrawBatch {
  const avatar::GpuVertex* vertices = nullptr;
  uint32_t vertex_count = 0;
  const uint16_t* indices = nullptr;
  uint32_t index_count = 0;
  avatar::GpuConstants constants = {};
  const avatar::Texture* textures[avatar::kLayerCount] = {};
  uint64_t texture_ids[avatar::kLayerCount] = {};
};

bool XnaAvatarCaptureTarget(XnaGpuDraw* out);
bool XnaDirectDrawAvatar(const XnaGpuDraw& target,
                         const XnaAvatarDrawBatch* batches, uint32_t count);

avatar::Catalog* XnaAvatarCatalog();
std::filesystem::path XnaAvatarProfilePath(uint64_t xuid);
bool XnaAvatarLoadProfile(uint64_t xuid, avatar::Description* out);
bool XnaAvatarSaveProfile(uint64_t xuid,
                          const avatar::Description& description);
bool XnaAvatarWriteProfileFile(uint64_t xuid,
                               const avatar::Description& description);
void XnaAvatarWriteProfileSetting(uint64_t xuid,
                                  const avatar::Description& description);
void XnaAvatarSyncProfileSetting(uint64_t xuid);

using XnaAvatarDescriptionBytes =
    std::array<uint8_t, avatar::kDescriptionBytes>;
using XnaAvatarManifestBytes = std::array<uint8_t, avatar::kManifestBytes>;

XnaAvatarManifestBytes XnaAvatarManifestForXuid(uint64_t xuid);
XnaAvatarManifestBytes XnaAvatarRandomManifest(int32_t wire_body);

uint32_t XnaAvatarCreateRenderer(const uint8_t* description, size_t size);
bool XnaAvatarRendererReady(uint32_t handle);
bool XnaAvatarDestroyRenderer(uint32_t handle);
void XnaAvatarDraw(uint32_t handle, const float* world, const float* view,
                   const float* projection, const float* light_color,
                   const float* light_direction, const float* ambient,
                   const uint32_t* expression, const float* bones);
void XnaAvatarBindPose(float* out);
XnaAvatarDescriptionBytes XnaAvatarDescriptionForGamer(uint32_t gamer);
XnaAvatarDescriptionBytes XnaAvatarDescriptionForXuid(uint64_t xuid);
XnaAvatarDescriptionBytes XnaAvatarRandomDescription(int32_t wire_body);
float XnaAvatarHeight(const uint8_t* description, size_t size);
uint32_t XnaAvatarBodyType(const uint8_t* description, size_t size);
uint32_t XnaAvatarCreateAnimation(uint32_t preset, float* length);
bool XnaAvatarDestroyAnimation(uint32_t handle);
void XnaAvatarUpdateAnimation(uint32_t handle, float seconds,
                              uint32_t* expression, float* bones);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_AVATAR_H_
