/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_avatar.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xna {

namespace {

constexpr uint32_t kAvatarTitleId = 0xFFFE07DFu;
constexpr float kFallbackAnimationSeconds = 3.0f;
constexpr uint32_t kWireMale = 1;
constexpr uint32_t kWireFemale = 0;

struct Renderer {
  std::atomic<bool> ready{false};
  std::mutex mutex;
  avatar::Description description;
  std::shared_ptr<const avatar::Scene> scene;
};

struct Animation {
  uint32_t preset = 0;
  std::shared_ptr<const avatar::Clip> clip;
};

std::mutex registry_mutex;
std::map<uint32_t, std::shared_ptr<Renderer>> renderers;
std::map<uint32_t, Animation> animations;
std::atomic<uint32_t> next_handle{0x100};
std::mutex random_mutex;
std::mt19937 random_generator{std::random_device{}()};

std::shared_ptr<Renderer> FindRenderer(uint32_t handle) {
  std::lock_guard<std::mutex> lock(registry_mutex);
  auto found = renderers.find(handle);
  return found == renderers.end() ? nullptr : found->second;
}

XnaAvatarDescriptionBytes Serialize(const avatar::Description& description) {
  return avatar::SerializeDescription(description);
}

uint64_t SlotXuid(uint32_t slot) {
  auto* state = kernel_state();
  if (!state || !state->xam_state() || slot >= 4) {
    return 0;
  }
  auto* profiles = state->xam_state()->profile_manager();
  auto* profile =
      profiles ? profiles->GetProfile(static_cast<uint8_t>(slot)) : nullptr;
  return profile ? profile->xuid() : 0;
}

void CopyMatrices(const avatar::Matrix* local, float* out) {
  for (uint32_t j = 0; j < avatar::kBoneCount; ++j) {
    std::memcpy(out + j * 16, local[j].data(), 16 * sizeof(float));
  }
}

}  // namespace

avatar::Catalog* XnaAvatarCatalog() {
  auto* state = kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  if (!emulator) {
    return nullptr;
  }
  return avatar::SharedCatalog(emulator->content_root());
}

std::filesystem::path XnaAvatarProfilePath(uint64_t xuid) {
  auto* state = kernel_state();
  if (!state || !state->xam_state() || !xuid) {
    return {};
  }
  auto* profiles = state->xam_state()->profile_manager();
  if (!profiles) {
    return {};
  }
  const xam::UserProfile* profile = profiles->GetProfileAny(xuid);
  const uint64_t offline_xuid = profile ? profile->xuid() : xuid;
  return profiles->GetProfileContentPath(offline_xuid, kAvatarTitleId) /
         "NexiaAvatar.nxav";
}

bool XnaAvatarLoadProfile(uint64_t xuid, avatar::Description* out) {
  const std::filesystem::path path = XnaAvatarProfilePath(xuid);
  if (path.empty()) {
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::vector<uint8_t> bytes(avatar::kDescriptionBytes);
  file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (size_t(file.gcount()) != bytes.size()) {
    return false;
  }
  return avatar::ParseDescription(bytes.data(), bytes.size(), out);
}

bool XnaAvatarSaveProfile(uint64_t xuid,
                          const avatar::Description& description) {
  if (!XnaAvatarWriteProfileFile(xuid, description)) {
    return false;
  }
  XnaAvatarWriteProfileSetting(xuid, description);
  return true;
}

void XnaAvatarWriteProfileSetting(uint64_t xuid,
                                  const avatar::Description& description) {
  auto* state = kernel_state();
  if (!state || !state->xam_state()) {
    return;
  }
  auto* tracker = state->xam_state()->user_tracker();
  auto* profiles = state->xam_state()->profile_manager();
  if (!tracker || !profiles) {
    return;
  }
  const xam::UserProfile* profile = profiles->GetProfileAny(xuid);
  if (!profile) {
    return;
  }
  const auto bytes = Serialize(description);
  const std::vector<uint8_t> data(
      bytes.begin(),
      bytes.begin() + std::min<size_t>(bytes.size(), xam::kMaxUserDataSize));
  const xam::UserSetting setting(
      xam::UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1, data);
  tracker->UpsertSetting(profile->xuid(), kDashboardID, &setting);
  XELOGI("[xna] avatar: {:016X} written to the dashboard profile",
         profile->xuid());
}

void XnaAvatarSyncProfileSetting(uint64_t xuid) {
  auto* state = kernel_state();
  if (!state || !state->xam_state()) {
    return;
  }
  auto* tracker = state->xam_state()->user_tracker();
  xam::UserProfile* user = state->xam_state()->GetUserProfile(xuid);
  if (!tracker || !user) {
    return;
  }
  avatar::Description saved;
  if (!XnaAvatarLoadProfile(xuid, &saved)) {
    return;
  }
  const auto setting = tracker->GetSetting(
      user, kDashboardID,
      static_cast<uint32_t>(
          xam::UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1));
  if (setting && setting->get_type() == xam::X_USER_DATA_TYPE::BINARY) {
    const std::span<const uint8_t> bytes = setting->get_extended_data();
    avatar::Description stored;
    if (avatar::ParseDescription(bytes.data(), bytes.size(), &stored)) {
      return;
    }
  }
  XnaAvatarWriteProfileSetting(xuid, saved);
}

bool XnaAvatarWriteProfileFile(uint64_t xuid,
                               const avatar::Description& description) {
  const std::filesystem::path path = XnaAvatarProfilePath(xuid);
  if (path.empty()) {
    return false;
  }
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    XELOGW("[xna] avatar: could not write {}", xe::path_to_utf8(path));
    return false;
  }
  const auto bytes = avatar::SerializeDescription(description);
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  XELOGI("[xna] avatar: saved {:016X} to {}", xuid, xe::path_to_utf8(path));
  return bool(file);
}

uint32_t XnaAvatarCreateRenderer(const uint8_t* description, size_t size) {
  const uint32_t handle = next_handle.fetch_add(1);
  auto renderer = std::make_shared<Renderer>();
  avatar::Catalog* catalog = XnaAvatarCatalog();
  renderer->description = avatar::DescriptionFromBytes(catalog, description, size);
  {
    std::lock_guard<std::mutex> lock(registry_mutex);
    renderers[handle] = renderer;
  }
  std::thread([renderer, catalog, handle] {
    std::shared_ptr<const avatar::Scene> scene;
    if (catalog) {
      scene = std::make_shared<const avatar::Scene>(
          avatar::BuildScene(*catalog, renderer->description));
      XELOGI("[xna] avatar: renderer {} ready with {} parts", handle,
             scene->parts.size());
    } else {
      XELOGW("[xna] avatar: renderer {} has no avatar assets - install the "
             "Avatar update (AvatarAssetPack.toc) to see avatars",
             handle);
    }
    {
      std::lock_guard<std::mutex> lock(renderer->mutex);
      renderer->scene = scene;
    }
    renderer->ready.store(true);
  }).detach();
  return handle;
}

bool XnaAvatarRendererReady(uint32_t handle) {
  auto renderer = FindRenderer(handle);
  return !renderer || renderer->ready.load();
}

bool XnaAvatarDestroyRenderer(uint32_t handle) {
  std::lock_guard<std::mutex> lock(registry_mutex);
  return renderers.erase(handle) != 0;
}

void XnaAvatarDraw(uint32_t handle, const float* world, const float* view,
                   const float* projection, const float* light_color,
                   const float* light_direction, const float* ambient,
                   const uint32_t* expression, const float* bones) {
  auto renderer = FindRenderer(handle);
  if (!renderer || !renderer->ready.load()) {
    return;
  }
  std::shared_ptr<const avatar::Scene> scene;
  {
    std::lock_guard<std::mutex> lock(renderer->mutex);
    scene = renderer->scene;
  }
  if (!scene || scene->parts.empty()) {
    return;
  }
  XnaGpuDraw target;
  if (!XnaAvatarCaptureTarget(&target)) {
    return;
  }
  avatar::Matrix local[avatar::kMaxJoints];
  for (uint32_t j = 0; j < avatar::kMaxJoints; ++j) {
    if (j < avatar::kBoneCount) {
      std::memcpy(local[j].data(), bones + j * 16, 16 * sizeof(float));
    } else {
      local[j] = avatar::Identity();
    }
  }
  avatar::Matrix skin[avatar::kMaxJoints];
  avatar::SkinMatrices(avatar::MainSkeleton(), local, skin);
  avatar::Matrix world_matrix;
  avatar::Matrix view_matrix;
  avatar::Matrix projection_matrix;
  std::memcpy(world_matrix.data(), world, 16 * sizeof(float));
  std::memcpy(view_matrix.data(), view, 16 * sizeof(float));
  std::memcpy(projection_matrix.data(), projection, 16 * sizeof(float));
  const avatar::Matrix world_view_projection = avatar::Multiply(
      avatar::Multiply(world_matrix, view_matrix), projection_matrix);
  avatar::Expression face;
  face.mouth = expression[0];
  face.left_eyebrow = expression[1];
  face.right_eyebrow = expression[2];
  face.left_eye = expression[3];
  face.right_eye = expression[4];

  size_t batch_total = 0;
  for (const avatar::Part& part : scene->parts) {
    batch_total += part.model->batches.size();
  }
  std::vector<std::vector<avatar::GpuVertex>> vertices(batch_total);
  std::vector<XnaAvatarDrawBatch> batches;
  batches.reserve(batch_total);
  size_t slot = 0;
  for (const avatar::Part& part : scene->parts) {
    for (const avatar::Batch& batch : part.model->batches) {
      std::vector<avatar::GpuVertex>& skinned = vertices[slot++];
      avatar::SkinBatch(batch, skin, &skinned,
                        part.kind == avatar::kKindBody ? avatar::kBodyInset
                                                       : 0.0f);
      if (skinned.empty() || batch.indices.empty()) {
        continue;
      }
      const avatar::Material material =
          avatar::BuildMaterial(*scene, part, batch, face);
      XnaAvatarDrawBatch draw;
      draw.vertices = skinned.data();
      draw.vertex_count = uint32_t(skinned.size());
      draw.indices = batch.indices.data();
      draw.index_count = uint32_t(batch.indices.size());
      std::memcpy(draw.constants.world_view_projection,
                  world_view_projection.data(), 16 * sizeof(float));
      std::memcpy(draw.constants.world, world_matrix.data(),
                  16 * sizeof(float));
      for (int k = 0; k < 3; ++k) {
        draw.constants.light_direction[k] = light_direction[k];
        draw.constants.light_color[k] = light_color[k];
        draw.constants.ambient[k] = ambient[k];
      }
      avatar::FillMaterialConstants(material, &draw.constants);
      for (uint32_t i = 0; i < avatar::kLayerCount; ++i) {
        draw.textures[i] = material.textures[i];
        draw.texture_ids[i] = material.texture_ids[i];
      }
      batches.push_back(draw);
    }
  }
  if (!batches.empty()) {
    XnaDirectDrawAvatar(target, batches.data(), uint32_t(batches.size()));
  }
}

void XnaAvatarBindPose(float* out) {
  const avatar::Skeleton& skeleton = avatar::MainSkeleton();
  for (uint32_t j = 0; j < avatar::kBoneCount; ++j) {
    const uint32_t parent = skeleton.parents[j];
    const bool root = parent >= j;
    avatar::Matrix bind = avatar::Identity();
    for (int k = 0; k < 3; ++k) {
      bind[12 + k] =
          skeleton.bind[j][k] - (root ? 0.0f : skeleton.bind[parent][k]);
    }
    if (root) {
      bind[0] = -1.0f;
      bind[10] = -1.0f;
      bind[12] = -bind[12];
      bind[14] = -bind[14];
    }
    std::memcpy(out + j * 16, bind.data(), 16 * sizeof(float));
  }
}

XnaAvatarDescriptionBytes XnaAvatarDescriptionForXuid(uint64_t xuid) {
  avatar::Description description;
  if (xuid && XnaAvatarLoadProfile(xuid, &description)) {
    XELOGI("[xna] avatar: {:016X} uses {}", xuid,
           xe::path_to_utf8(XnaAvatarProfilePath(xuid)));
    return Serialize(description);
  }
  avatar::Catalog* catalog = XnaAvatarCatalog();
  if (!catalog) {
    XELOGW("[xna] avatar: {:016X} has no catalog, empty description", xuid);
    return Serialize(description);
  }
  XELOGI("[xna] avatar: {:016X} has no saved avatar at '{}', random", xuid,
         xe::path_to_utf8(XnaAvatarProfilePath(xuid)));
  std::mt19937 generator(uint32_t(xuid ^ (xuid >> 32)));
  return Serialize(avatar::RandomDescription(*catalog, generator, -1));
}

XnaAvatarDescriptionBytes XnaAvatarDescriptionForGamer(uint32_t gamer) {
  const uint64_t xuid = SlotXuid(gamer);
  if (xuid) {
    return XnaAvatarDescriptionForXuid(xuid);
  }
  avatar::Catalog* catalog = XnaAvatarCatalog();
  if (!catalog) {
    return Serialize(avatar::Description());
  }
  std::mt19937 generator(gamer + 1);
  return Serialize(avatar::RandomDescription(*catalog, generator, -1));
}

XnaAvatarDescriptionBytes XnaAvatarRandomDescription(int32_t wire_body) {
  const int32_t body = wire_body == int32_t(kWireMale)     ? 1
                       : wire_body == int32_t(kWireFemale) ? 0
                                                           : -1;
  avatar::Catalog* catalog = XnaAvatarCatalog();
  std::lock_guard<std::mutex> lock(random_mutex);
  if (!catalog) {
    avatar::Description description;
    description.body = uint8_t(body >= 0 ? body : int32_t(random_generator() & 1));
    description.height = uint8_t(random_generator() & 0xFF);
    return Serialize(description);
  }
  return Serialize(avatar::RandomDescription(*catalog, random_generator, body));
}

float XnaAvatarHeight(const uint8_t* description, size_t size) {
  return avatar::DescriptionHeight(
      avatar::DescriptionFromBytes(nullptr, description, size));
}

uint32_t XnaAvatarBodyType(const uint8_t* description, size_t size) {
  const avatar::Description parsed =
      avatar::DescriptionFromBytes(nullptr, description, size);
  return parsed.body ? kWireMale : kWireFemale;
}

uint32_t XnaAvatarCreateAnimation(uint32_t preset, float* length) {
  const uint32_t handle = next_handle.fetch_add(1);
  Animation animation;
  animation.preset = preset;
  const int32_t clip = avatar::PresetClip(preset);
  avatar::Catalog* catalog = XnaAvatarCatalog();
  if (catalog && clip >= 0) {
    animation.clip = catalog->LoadClip(uint32_t(clip));
  }
  *length = animation.clip ? animation.clip->Length()
                           : kFallbackAnimationSeconds;
  if (!animation.clip) {
    XELOGW("[xna] avatar: animation preset {} (clip {}) is unavailable", preset,
           clip);
  }
  std::lock_guard<std::mutex> lock(registry_mutex);
  animations[handle] = animation;
  return handle;
}

bool XnaAvatarDestroyAnimation(uint32_t handle) {
  std::lock_guard<std::mutex> lock(registry_mutex);
  return animations.erase(handle) != 0;
}

void XnaAvatarUpdateAnimation(uint32_t handle, float seconds,
                              uint32_t* expression, float* bones) {
  std::shared_ptr<const avatar::Clip> clip;
  {
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto found = animations.find(handle);
    if (found != animations.end()) {
      clip = found->second.clip;
    }
  }
  for (uint32_t i = 0; i < 5; ++i) {
    expression[i] = 0;
  }
  avatar::Matrix local[avatar::kMaxJoints];
  if (clip) {
    avatar::SamplePose(*clip, avatar::MainSkeleton(), seconds, local);
  } else {
    avatar::BindPose(avatar::MainSkeleton(), local);
  }
  CopyMatrices(local, bones);
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
