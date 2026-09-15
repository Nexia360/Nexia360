/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_avatar_assets.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xna/xna_avatar.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

namespace avatar = xe::kernel::xna::avatar;

constexpr uint32_t kAssetsBytes = 0x14;
constexpr uint32_t kSkeletonBytes = 0x8;
constexpr uint32_t kJointBytes = 0x60;
constexpr uint32_t kInfoBytes = 0x20;
constexpr uint32_t kModelBytes = 0x34;
constexpr uint32_t kBatchBytes = 0x200;
constexpr uint32_t kTextureBytes = 0x2C;
constexpr uint32_t kParamBytes = 0x18;
constexpr uint32_t kMaxParams = 20;
constexpr uint32_t kParamTexture = 1;
constexpr uint32_t kParamPixelConstant = 3;
constexpr uint32_t kUsageColorSkin = 13;
constexpr uint32_t kUsageColorCustom0 = 22;
constexpr uint32_t kUsageCount = 28;
constexpr uint32_t kComponentMaskBody = 0x2u;
constexpr uint32_t kComponentMaskHair = 0x4u;
constexpr uint32_t kNoJoint = 0xFFFFFFFFu;
constexpr uint32_t kComponentMaskAll = 0x1FFFu;
constexpr uint32_t kComponentMaskCarryable = 0x1000u;
constexpr uint32_t kCarryableBytes = 0x5C;
constexpr uint32_t kComponentMaskHead = 0x1u;
constexpr uint32_t kVertexFixedBytes = 0x1C;
constexpr uint32_t kTextureAlignment = 0x1000;

std::atomic<uint32_t> coordinate_system{1};

struct Arena {
  uint8_t* base = nullptr;
  uint32_t guest = 0;
  uint32_t size = 0;
  uint32_t used = 0;

  uint32_t Take(uint32_t bytes, uint32_t alignment) {
    const uint32_t at = xe::align(guest + used, alignment) - guest;
    if (uint64_t(at) + bytes > size) {
      return UINT32_MAX;
    }
    used = at + bytes;
    return at;
  }
};

void Put16(uint8_t* p, uint16_t value) {
  xe::store_and_swap<uint16_t>(p, value);
}

void Put32(uint8_t* p, uint32_t value) {
  xe::store_and_swap<uint32_t>(p, value);
}

void PutFloat(uint8_t* p, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Put32(p, bits);
}

void PutVector(uint8_t* p, float x, float y, float z, float w) {
  PutFloat(p, x);
  PutFloat(p + 4, y);
  PutFloat(p + 8, z);
  PutFloat(p + 12, w);
}

uint32_t MirrorNormal(uint32_t normal) {
  const uint32_t z = (0u - (normal >> 22)) & 0x3FFu;
  return (normal & 0x3FFFFFu) | (z << 22);
}

bool BlockLayout(uint32_t format, uint32_t* block_pixels,
                 uint32_t* bytes_log2) {
  switch (format & 0x3F) {
    case 0x12:
      *block_pixels = 4;
      *bytes_log2 = 3;
      return true;
    case 0x13:
    case 0x14:
      *block_pixels = 4;
      *bytes_log2 = 4;
      return true;
    case 0x06:
      *block_pixels = 1;
      *bytes_log2 = 2;
      return true;
    default:
      return false;
  }
}

void WriteTextureData(const avatar::RawTexture& texture, uint8_t* destination,
                      uint32_t capacity) {
  if (texture.zero_fill || texture.data.empty()) {
    return;
  }
  const size_t stored = size_t(texture.pitch) * texture.rows;
  uint32_t block_pixels = 4;
  uint32_t bytes_log2 = 4;
  const bool known = BlockLayout(texture.format, &block_pixels, &bytes_log2);
  const bool tile = texture.tiled && known;
  const uint32_t block_bytes = 1u << bytes_log2;
  const uint32_t blocks_wide =
      (texture.width + block_pixels - 1) / block_pixels;
  const uint32_t blocks_high =
      (texture.height + block_pixels - 1) / block_pixels;
  const uint32_t pitch_aligned = xe::align(blocks_wide, 32u);
  const size_t padded_pitch =
      std::max<size_t>(size_t(pitch_aligned) * block_bytes, texture.pitch);
  for (uint32_t slice = 0; slice < texture.slices; ++slice) {
    const size_t source_at = size_t(slice) * stored;
    const size_t destination_at = size_t(slice) * texture.slice_size;
    if (source_at + stored > texture.data.size() ||
        destination_at >= capacity) {
      break;
    }
    const uint8_t* source = texture.data.data() + source_at;
    uint8_t* out = destination + destination_at;
    const size_t room =
        std::min<size_t>(texture.slice_size, capacity - destination_at);
    if (!tile) {
      if (!known || padded_pitch * texture.rows > room) {
        std::memcpy(out, source, std::min(stored, room));
        continue;
      }
      for (uint32_t row = 0; row < texture.rows; ++row) {
        std::memcpy(out + row * padded_pitch,
                    source + size_t(row) * texture.pitch, texture.pitch);
      }
      continue;
    }
    for (uint32_t by = 0; by < blocks_high && by < texture.rows; ++by) {
      for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
        const size_t from =
            size_t(by) * texture.pitch + size_t(bx) * block_bytes;
        if (from + block_bytes > stored) {
          continue;
        }
        const int32_t to = xe::gpu::texture_address::Tiled2D(
            int32_t(bx), int32_t(by), pitch_aligned, bytes_log2);
        if (to < 0 || size_t(to) + block_bytes > room) {
          continue;
        }
        std::memcpy(out + to, source + from, block_bytes);
      }
    }
  }
}

void WriteSkeleton(uint8_t* joints, bool mirror,
                   const avatar::Skeleton& skeleton) {
  const float z_sign = mirror ? -1.0f : 1.0f;
  for (uint32_t j = 0; j < skeleton.count; ++j) {
    const uint32_t parent =
        skeleton.parents[j] < j ? uint32_t(skeleton.parents[j]) : kNoJoint;
    uint32_t child = kNoJoint;
    uint32_t sibling = kNoJoint;
    for (uint32_t k = j + 1; k < skeleton.count; ++k) {
      if (child == kNoJoint && skeleton.parents[k] == j) {
        child = k;
      }
      if (sibling == kNoJoint && parent != kNoJoint &&
          skeleton.parents[k] == parent) {
        sibling = k;
      }
    }
    uint8_t* out = joints + size_t(j) * kJointBytes;
    Put32(out, parent);
    Put32(out + 4, child);
    Put32(out + 8, sibling);
    const float* world = skeleton.bind[j];
    float offset[3] = {world[0], world[1], world[2]};
    if (parent != kNoJoint) {
      for (int k = 0; k < 3; ++k) {
        offset[k] -= skeleton.bind[parent][k];
      }
    } else {
      for (int k = 0; k < 3; ++k) {
        offset[k] *= skeleton.scale[j][k];
      }
    }
    PutVector(out + 0x10, world[0], world[1], world[2] * z_sign, 1.0f);
    PutVector(out + 0x20, 0.0f, 0.0f, 0.0f, 1.0f);
    PutVector(out + 0x30, offset[0], offset[1], offset[2] * z_sign, 1.0f);
    PutVector(out + 0x40, 0.0f, 0.0f, 0.0f, 1.0f);
    PutVector(out + 0x50, skeleton.scale[j][0], skeleton.scale[j][1],
              skeleton.scale[j][2], 1.0f);
  }
}

void WriteTextureRecord(uint8_t* record, const avatar::RawTexture& texture,
                        uint32_t data_guest) {
  Put32(record, texture.format);
  Put32(record + 0x04, texture.width);
  Put32(record + 0x08, texture.height);
  Put32(record + 0x0C, texture.total_size);
  Put32(record + 0x10, 0);
  Put32(record + 0x14, texture.slice_size);
  Put32(record + 0x18, 0);
  Put32(record + 0x1C, 1);
  Put32(record + 0x20, texture.slices);
  Put32(record + 0x24, data_guest);
  Put32(record + 0x28, 0);
}

struct Palette {
  bool set[kUsageCount] = {};
  uint32_t argb[kUsageCount] = {};

  void Assign(uint32_t usage, uint32_t color) {
    set[usage] = true;
    argb[usage] = color;
  }
};

int32_t ComponentSlot(const avatar::Description& description,
                      const avatar::Component& component) {
  for (uint32_t slot = 0; slot < avatar::kClothingSlotCount; ++slot) {
    if (description.items[slot] == component.entry) {
      return int32_t(slot);
    }
  }
  for (uint32_t bit = 2; bit < 13; ++bit) {
    if (component.mask & (1u << bit)) {
      return int32_t(bit - 2);
    }
  }
  return -1;
}

Palette ComponentPalette(const avatar::Description& description,
                         const avatar::Component& component) {
  Palette palette;
  for (uint32_t color = 0; color < avatar::kColorCount; ++color) {
    palette.Assign(kUsageColorSkin + color, description.colors[color]);
  }
  const uint32_t skin = description.colors[avatar::kColorSkin];
  const uint32_t hair = description.colors[avatar::kColorHair];
  for (uint32_t k = 0; k < 3; ++k) {
    uint32_t custom = skin;
    if (component.mask & kComponentMaskHair) {
      custom = hair;
    } else if (!(component.mask & (kComponentMaskHead | kComponentMaskBody))) {
      const int32_t slot = ComponentSlot(description, component);
      if (slot >= 0) {
        custom = description.custom[slot][k];
      }
    }
    palette.Assign(kUsageColorCustom0 + k, custom);
  }
  return palette;
}

void PutColorParam(uint8_t* p, uint32_t argb) {
  PutVector(p, float((argb >> 16) & 0xFF) / 255.0f,
            float((argb >> 8) & 0xFF) / 255.0f, float(argb & 0xFF) / 255.0f,
            1.0f);
}

bool WriteModel(const avatar::RawModel& model,
                const std::vector<const avatar::RawTexture*>& overrides,
                const Palette& palette, bool mirror, float inset, uint8_t* cpu,
                uint32_t cpu_guest, uint8_t* gpu, uint32_t gpu_guest,
                uint8_t* record) {
  const size_t batches_end =
      size_t(model.batches_offset) + model.batches.size() * kBatchBytes;
  const size_t textures_end =
      size_t(model.textures_offset) + model.textures.size() * kTextureBytes;
  if (batches_end > model.cpu_size || textures_end > model.cpu_size) {
    return false;
  }
  for (size_t index = 0; index < model.batches.size(); ++index) {
    const avatar::RawBatch& batch = model.batches[index];
    const uint32_t stride =
        batch.stride ? batch.stride : kVertexFixedBytes + 4 * batch.uv_sets;
    const uint64_t vertex_end =
        uint64_t(batch.vb_offset) + uint64_t(stride) * batch.vertices.size();
    const uint64_t index_end =
        uint64_t(batch.ib_offset) + uint64_t(batch.indices.size()) * 2;
    if (vertex_end > model.gpu_size || index_end > model.gpu_size) {
      return false;
    }
    uint8_t* out = cpu + model.batches_offset + index * kBatchBytes;
    Put32(out, batch.shader);
    const size_t params = std::min<size_t>(batch.params.size(), kMaxParams);
    for (size_t p = 0; p < params; ++p) {
      const avatar::Param& param = batch.params[p];
      uint8_t* slot = out + 4 + p * kParamBytes;
      Put32(slot, param.type);
      Put32(slot + 4, param.usage);
      if (param.type == kParamTexture) {
        Put16(slot + 8, uint16_t(param.data[0] & 0xFFFF));
        Put16(slot + 10, uint16_t(param.data[0] >> 16));
        Put32(slot + 12, param.data[1]);
      } else if (param.type == kParamPixelConstant &&
                 param.usage < kUsageCount && palette.set[param.usage]) {
        PutColorParam(slot + 8, palette.argb[param.usage]);
      } else {
        for (int k = 0; k < 4; ++k) {
          Put32(slot + 8 + 4 * k, param.data[k]);
        }
      }
    }
    Put32(out + 0x1E4, batch.triangles);
    Put32(out + 0x1E8, uint32_t(batch.vertices.size()));
    Put32(out + 0x1EC, batch.uv_sets);
    Put32(out + 0x1F0, stride);
    Put32(out + 0x1F4, batch.index_stride ? batch.index_stride : 2);
    Put32(out + 0x1F8, gpu_guest + batch.vb_offset);
    Put32(out + 0x1FC, gpu_guest + batch.ib_offset);
    for (size_t v = 0; v < batch.vertices.size(); ++v) {
      const avatar::RawVertex& vertex = batch.vertices[v];
      uint8_t* dst = gpu + batch.vb_offset + v * stride;
      float position[3] = {vertex.position[0], vertex.position[1],
                           vertex.position[2]};
      if (inset != 0.0f) {
        float normal[3];
        avatar::UnpackNormal(vertex.normal, normal);
        for (int k = 0; k < 3; ++k) {
          position[k] -= normal[k] * inset;
        }
      }
      PutFloat(dst, position[0]);
      PutFloat(dst + 4, position[1]);
      PutFloat(dst + 8, mirror ? -position[2] : position[2]);
      Put32(dst + 0x0C, mirror ? MirrorNormal(vertex.normal) : vertex.normal);
      Put32(dst + 0x10, vertex.weights);
      Put32(dst + 0x14, vertex.bindings);
      Put32(dst + 0x18, vertex.color);
      for (uint32_t k = 0; k < batch.uv_sets * 2; ++k) {
        if (kVertexFixedBytes + (k + 1) * 2 <= stride) {
          Put16(dst + kVertexFixedBytes + k * 2, vertex.uv[k]);
        }
      }
    }
    for (size_t k = 0; k < batch.indices.size(); ++k) {
      Put16(gpu + batch.ib_offset + k * 2, batch.indices[k]);
    }
  }
  for (size_t index = 0; index < model.textures.size(); ++index) {
    const avatar::RawTexture& slot = model.textures[index];
    if (uint64_t(slot.gpu_offset) + slot.gpu_size > model.gpu_size) {
      return false;
    }
    const avatar::RawTexture* chosen = &slot;
    const avatar::RawTexture* feature =
        index < overrides.size() ? overrides[index] : nullptr;
    if (feature &&
        uint64_t(feature->slice_size) * feature->slices <= slot.gpu_size) {
      chosen = feature;
    }
    uint8_t* out = cpu + model.textures_offset + index * kTextureBytes;
    WriteTextureRecord(out, *chosen, gpu_guest + slot.gpu_offset);
    WriteTextureData(*chosen, gpu + slot.gpu_offset, slot.gpu_size);
  }
  Put32(record, model.cpu_size);
  Put32(record + 0x04, model.gpu_size);
  Put32(record + 0x08, model.texture_size);
  Put32(record + 0x0C, model.vb_size);
  Put32(record + 0x10, model.ib_size);
  Put32(record + 0x14, uint32_t(model.batches.size()));
  Put32(record + 0x18, uint32_t(model.textures.size()));
  Put32(record + 0x1C, cpu_guest);
  Put32(record + 0x20, gpu_guest);
  Put32(record + 0x24, gpu_guest + model.vb_offset);
  Put32(record + 0x28, gpu_guest + model.ib_offset);
  Put32(record + 0x2C, cpu_guest + model.batches_offset);
  Put32(record + 0x30, cpu_guest + model.textures_offset);
  return true;
}

}  // namespace

void SetAvatarCoordinateSystem(uint32_t value) {
  coordinate_system.store(value);
}

uint32_t AvatarCoordinateSystem() { return coordinate_system.load(); }

X_RESULT BuildAvatarAssets(const uint8_t* metadata, size_t metadata_size,
                           uint32_t component_mask, uint32_t result_guest,
                           uint32_t gpu_guest) {
  avatar::Catalog* catalog = xe::kernel::xna::XnaAvatarCatalog();
  if (!catalog) {
    XELOGW("XamAvatarGetAssets: the Avatar update is not installed");
    return X_E_FAIL;
  }
  auto* memory = kernel_memory();
  uint8_t* result =
      result_guest ? memory->TranslateVirtual<uint8_t*>(result_guest) : nullptr;
  uint8_t* gpu =
      gpu_guest ? memory->TranslateVirtual<uint8_t*>(gpu_guest) : nullptr;
  if (!result || !gpu) {
    XELOGW("XamAvatarGetAssets: result {:08X}, gpu {:08X}", result_guest,
           gpu_guest);
    return X_E_INVALIDARG;
  }
  const avatar::Description description =
      avatar::DescriptionFromBytes(catalog, metadata, metadata_size);
  const bool mirror = AvatarCoordinateSystem() == 0;
  Arena cpu{result, result_guest, kAvatarResultBufferSize};
  Arena video{gpu, gpu_guest, kAvatarGpuBufferSize};
  std::memset(result, 0, kAvatarResultBufferSize);
  std::memset(gpu, 0, kAvatarGpuBufferSize);

  const avatar::Skeleton skeleton = avatar::ScaledSkeleton(description);
  const uint32_t assets = cpu.Take(kAssetsBytes, 16);
  const uint32_t skeleton_at = cpu.Take(kSkeletonBytes, 16);
  const uint32_t joints = cpu.Take(skeleton.count * kJointBytes, 16);
  if (assets == UINT32_MAX || skeleton_at == UINT32_MAX ||
      joints == UINT32_MAX) {
    XELOGW("XamAvatarGetAssets: no room for the skeleton");
    return X_E_FAIL;
  }
  WriteSkeleton(result + joints, mirror, skeleton);
  Put32(result + skeleton_at, skeleton.count);
  Put32(result + skeleton_at + 4, result_guest + joints);

  const uint32_t mask = (component_mask & kComponentMaskAll)
                            ? (component_mask & kComponentMaskAll)
                            : kComponentMaskAll;
  std::vector<avatar::Component> wanted;
  std::shared_ptr<const avatar::Carryable> carryable;
  avatar::Component carried = {};
  for (const avatar::Component& component :
       avatar::Components(*catalog, description)) {
    if (!(component.mask & mask)) {
      continue;
    }
    if (component.mask & kComponentMaskCarryable) {
      carryable = catalog->LoadCarryable(component.entry);
      if (carryable) {
        carried = component;
        continue;
      }
    }
    wanted.push_back(component);
  }
  const uint32_t infos = cpu.Take(uint32_t(wanted.size()) * kInfoBytes, 16);
  const uint32_t models = cpu.Take(uint32_t(wanted.size()) * kModelBytes, 16);
  if (infos == UINT32_MAX || models == UINT32_MAX) {
    XELOGW("XamAvatarGetAssets: no room for {} component records",
           wanted.size());
    return X_E_FAIL;
  }

  std::vector<std::shared_ptr<const avatar::RawTexture>> features;
  uint32_t written = 0;
  for (const avatar::Component& component : wanted) {
    auto model = catalog->LoadRawModel(component.entry);
    if (!model) {
      XELOGW("XamAvatarGetAssets: asset {} did not decode", component.entry);
      continue;
    }
    const uint32_t cpu_at = cpu.Take(model->cpu_size, 16);
    const uint32_t gpu_at = video.Take(model->gpu_size, kTextureAlignment);
    if (cpu_at == UINT32_MAX || gpu_at == UINT32_MAX) {
      XELOGW("XamAvatarGetAssets: no room for asset {} ({} cpu, {} gpu bytes)",
             component.entry, model->cpu_size, model->gpu_size);
      continue;
    }
    std::vector<const avatar::RawTexture*> overrides(model->textures.size(),
                                                     nullptr);
    if (component.mask & kComponentMaskHead) {
      for (const avatar::RawBatch& batch : model->batches) {
        for (const avatar::Param& param : batch.params) {
          const uint32_t index = param.data[0] & 0xFFFF;
          if (param.type != kParamTexture || index >= overrides.size()) {
            continue;
          }
          const int32_t feature = avatar::HeadFeatureForUsage(param.usage);
          const int32_t entry =
              feature >= 0
                  ? avatar::FeatureEntry(description, uint32_t(feature))
                  : -1;
          if (entry < 0) {
            continue;
          }
          auto texture = catalog->LoadRawTexture(uint32_t(entry));
          if (texture) {
            overrides[index] = texture.get();
            features.push_back(texture);
          }
        }
      }
    }
    uint8_t* record = result + models + written * kModelBytes;
    const float inset =
        (component.mask & kComponentMaskBody) ? avatar::kBodyInset : 0.0f;
    if (!WriteModel(*model, overrides, ComponentPalette(description, component),
                    mirror, inset, result + cpu_at, result_guest + cpu_at,
                    gpu + gpu_at, gpu_guest + gpu_at, record)) {
      XELOGW("XamAvatarGetAssets: asset {} does not fit its own layout",
             component.entry);
      std::memset(record, 0, kModelBytes);
      continue;
    }
    uint8_t* info = result + infos + written * kInfoBytes;
    const avatar::Entry* entry = catalog->Find(component.entry);
    if (entry) {
      std::memcpy(info, entry->asset_id.data(), entry->asset_id.size());
    }
    Put16(info + 0x10, uint16_t(component.mask));
    ++written;
  }

  uint32_t carryable_guest = 0;
  if (carryable) {
    auto model = catalog->LoadRawModel(carried.entry);
    std::vector<uint8_t> stream;
    const uint32_t record = cpu.Take(kCarryableBytes, 16);
    const uint32_t carry_skeleton = cpu.Take(kSkeletonBytes, 16);
    const uint32_t carry_joints =
        cpu.Take(carryable->skeleton.count * kJointBytes, 16);
    const bool loaded = model &&
                        catalog->AnimationStream(carried.entry, &stream) &&
                        stream.size() > avatar::kAnimationHeaderBytes;
    const uint32_t payload =
        loaded ? uint32_t(stream.size() - avatar::kAnimationHeaderBytes) : 0;
    const uint32_t cpu_at = loaded ? cpu.Take(model->cpu_size, 16) : UINT32_MAX;
    const uint32_t gpu_at =
        loaded ? video.Take(model->gpu_size, kTextureAlignment) : UINT32_MAX;
    const uint32_t object_at =
        loaded ? video.Take(avatar::kAnimationObjectBytes, 16) : UINT32_MAX;
    const uint32_t buffer_at = loaded ? video.Take(payload, 16) : UINT32_MAX;
    if (record == UINT32_MAX || carry_skeleton == UINT32_MAX ||
        carry_joints == UINT32_MAX || cpu_at == UINT32_MAX ||
        gpu_at == UINT32_MAX || object_at == UINT32_MAX ||
        buffer_at == UINT32_MAX) {
      XELOGW(
          "XamAvatarGetAssets: carryable {} did not fit ({} animation bytes)",
          carried.entry, payload);
    } else {
      uint8_t* carry = result + record;
      WriteSkeleton(result + carry_joints, mirror, carryable->skeleton);
      Put32(result + carry_skeleton, carryable->skeleton.count);
      Put32(result + carry_skeleton + 4, result_guest + carry_joints);
      Put32(carry, result_guest + carry_skeleton);
      const avatar::Entry* entry = catalog->Find(carried.entry);
      if (entry) {
        std::memcpy(carry + 4, entry->asset_id.data(), entry->asset_id.size());
      }
      Put16(carry + 4 + 0x10, uint16_t(kComponentMaskCarryable));
      const std::vector<const avatar::RawTexture*> overrides(
          model->textures.size(), nullptr);
      const bool model_ok =
          WriteModel(*model, overrides, ComponentPalette(description, carried),
                     mirror, 0.0f, result + cpu_at, result_guest + cpu_at,
                     gpu + gpu_at, gpu_guest + gpu_at, carry + 4 + kInfoBytes);
      uint8_t* object = gpu + object_at;
      std::memset(object, 0, avatar::kAnimationObjectBytes);
      Put32(object + avatar::kAnimationSizeOffset, payload);
      Put32(object + avatar::kAnimationBufferOffset, gpu_guest + buffer_at);
      const bool animation_ok =
          avatar::WriteAnimationObject(stream, mirror, object);
      if (animation_ok) {
        const uint32_t size =
            xe::load_and_swap<uint32_t>(object + avatar::kAnimationSizeOffset);
        std::memcpy(gpu + buffer_at,
                    stream.data() + avatar::kAnimationHeaderBytes, size);
        Put32(carry + 4 + kInfoBytes + kModelBytes, gpu_guest + object_at);
      }
      if (model_ok) {
        carryable_guest = result_guest + record;
      }
      XELOGI(
          "XamAvatarGetAssets: carryable '{}' with {} joints, model {}, "
          "animation {}",
          entry ? entry->name : std::string(), carryable->skeleton.count,
          model_ok ? "written" : "rejected",
          animation_ok ? "written" : "rejected");
    }
  }

  Put32(result + assets, result_guest + skeleton_at);
  Put32(result + assets + 0x04, carryable_guest);
  Put32(result + assets + 0x08, written);
  Put32(result + assets + 0x0C, result_guest + infos);
  Put32(result + assets + 0x10, result_guest + models);
  XELOGI(
      "XamAvatarGetAssets: {} components (mask {:04X}), {} of {} result "
      "bytes, {} of {} GPU bytes, {} coordinates",
      written, mask, cpu.used, kAvatarResultBufferSize, video.used,
      kAvatarGpuBufferSize, mirror ? "left-handed" : "right-handed");
  return written ? X_ERROR_SUCCESS : X_E_FAIL;
}

X_RESULT LoadAvatarAnimation(const std::array<uint8_t, 16>& asset_id,
                             uint32_t object_guest, const std::string& label) {
  avatar::Catalog* catalog = xe::kernel::xna::XnaAvatarCatalog();
  if (!catalog) {
    XELOGW("XamAvatarLoadAnimation {}: the Avatar update is not installed",
           label);
    return X_E_FAIL;
  }
  auto* memory = kernel_memory();
  uint8_t* object =
      object_guest ? memory->TranslateVirtual<uint8_t*>(object_guest) : nullptr;
  if (!object) {
    return X_E_INVALIDARG;
  }
  const int32_t index = avatar::FindAnimation(*catalog, asset_id.data());
  std::vector<uint8_t> stream;
  if (index < 0 || !catalog->AnimationStream(uint32_t(index), &stream)) {
    XELOGW("XamAvatarLoadAnimation {}: no such animation in the Avatar update",
           label);
    return X_E_FAIL;
  }
  const uint32_t capacity =
      xe::load_and_swap<uint32_t>(object + avatar::kAnimationSizeOffset);
  const uint32_t buffer_guest =
      xe::load_and_swap<uint32_t>(object + avatar::kAnimationBufferOffset);
  uint8_t* buffer =
      buffer_guest ? memory->TranslateVirtual<uint8_t*>(buffer_guest) : nullptr;
  const bool mirror = AvatarCoordinateSystem() == 0;
  if (!buffer || !avatar::WriteAnimationObject(stream, mirror, object)) {
    XELOGW(
        "XamAvatarLoadAnimation {}: entry {} rejected ({} stream bytes, title "
        "buffer {:08X} holds {} bytes)",
        label, index, stream.size(), buffer_guest, capacity);
    return X_E_FAIL;
  }
  const uint32_t size =
      xe::load_and_swap<uint32_t>(object + avatar::kAnimationSizeOffset);
  std::memcpy(buffer, stream.data() + avatar::kAnimationHeaderBytes, size);
  const avatar::Entry* entry = catalog->Find(uint32_t(index));
  XELOGI(
      "XamAvatarLoadAnimation {}: entry {} '{}', {} of {} bytes, {} "
      "coordinates",
      label, index, entry ? entry->name : std::string(), size, capacity,
      mirror ? "left-handed" : "right-handed");
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
