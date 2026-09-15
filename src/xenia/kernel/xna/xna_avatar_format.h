/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_AVATAR_FORMAT_H_
#define XENIA_KERNEL_XNA_XNA_AVATAR_FORMAT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace xna {
namespace avatar {

constexpr uint32_t kBoneCount = 71;
constexpr uint32_t kMaxJoints = 72;
constexpr size_t kDescriptionBytes = 1020;
constexpr uint32_t kLayerCount = 6;
constexpr uint16_t kNoItem = 0xFFFF;

enum Slot : uint32_t {
  kSlotHair,
  kSlotShirt,
  kSlotTrousers,
  kSlotShoes,
  kSlotHat,
  kSlotGloves,
  kSlotGlasses,
  kSlotWristwear,
  kSlotEarrings,
  kSlotRing,
  kSlotCarryable,
  kSlotEyes,
  kSlotEyebrows,
  kSlotMouth,
  kSlotFacialHair,
  kSlotFacePaint,
  kSlotEyeShadow,
  kSlotCount,
};

constexpr uint32_t kClothingSlotCount = kSlotCarryable + 1;

enum ColorIndex : uint32_t {
  kColorSkin,
  kColorHair,
  kColorLips,
  kColorIris,
  kColorEyebrow,
  kColorEyeShadow,
  kColorFacialHair,
  kColorFeature1,
  kColorFeature2,
  kColorCount,
};

enum LayerKind : uint32_t {
  kLayerNone,
  kLayerColor,
  kLayerMask,
  kLayerDecal,
  kLayerFeature,
};

enum Feature : uint32_t {
  kFeatureFacePaint,
  kFeatureFacialHair,
  kFeatureEyebrows,
  kFeatureEyes,
  kFeatureMouth,
  kFeatureEyeShadow,
  kFeatureCount,
};

constexpr uint32_t kKindHead = 0x1;
constexpr uint32_t kKindBody = 0x2;
constexpr uint32_t kKindAnimation = 0x400000;
constexpr uint32_t kKindHidingTemplate = 0x01000000;

uint32_t SlotBit(uint32_t slot);
int32_t PrimarySlot(uint32_t kind);
uint32_t SlotCoverage(uint32_t kind);
const char* SlotName(uint32_t slot);
const char* ColorName(uint32_t color);

struct Description {
  uint8_t body = 1;
  uint8_t height = 128;
  uint8_t weight = 128;
  std::array<uint16_t, kSlotCount> items;
  std::array<uint32_t, kColorCount> colors;
  std::array<std::array<uint32_t, 3>, kClothingSlotCount> custom;
  Description();
};

std::array<uint8_t, kDescriptionBytes> SerializeDescription(
    const Description& description);
bool ParseDescription(const uint8_t* bytes, size_t size, Description* out);
bool HasDescriptionMagic(const uint8_t* bytes, size_t size);
float DescriptionHeight(const Description& description);

struct Entry {
  uint32_t index = 0;
  uint32_t kind = 0;
  uint32_t flags = 0;
  uint32_t blob = 0;
  uint32_t size = 0;
  std::string name;
  std::array<uint8_t, 16> asset_id = {};
  uint32_t BodyMask() const { return flags >> 24; }
};

struct Record {
  uint8_t type = 0;
  std::vector<uint8_t> data;
};

struct Texture {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t slices = 0;
  std::vector<uint8_t> rgba;
};

struct Param {
  uint32_t type = 0;
  uint32_t usage = 0;
  uint32_t data[4] = {};
};

struct Vertex {
  float position[3];
  float normal[3];
  float weights[4];
  uint8_t bones[4];
  float uv[kLayerCount][2];
};

struct Batch {
  uint32_t shader = 0;
  uint32_t uv_sets = 0;
  std::vector<Param> params;
  std::vector<Vertex> vertices;
  std::vector<uint16_t> indices;
};

struct Model {
  std::vector<Batch> batches;
  std::vector<std::shared_ptr<const Texture>> textures;
};

struct RawVertex {
  float position[3] = {};
  uint32_t normal = 0;
  uint32_t weights = 0;
  uint32_t bindings = 0;
  uint32_t color = 0;
  uint16_t uv[kLayerCount * 2] = {};
};

struct RawBatch {
  uint32_t shader = 0;
  uint32_t triangles = 0;
  uint32_t uv_sets = 0;
  uint32_t stride = 0;
  uint32_t index_stride = 0;
  uint32_t vb_offset = 0;
  uint32_t ib_offset = 0;
  std::vector<Param> params;
  std::vector<RawVertex> vertices;
  std::vector<uint16_t> indices;
};

struct RawTexture {
  uint32_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t total_size = 0;
  uint32_t slice_size = 0;
  uint32_t slices = 0;
  uint32_t pitch = 0;
  uint32_t rows = 0;
  bool zero_fill = false;
  bool tiled = false;
  uint32_t gpu_offset = 0;
  uint32_t gpu_size = 0;
  std::vector<uint8_t> data;
};

struct RawModel {
  uint32_t cpu_size = 0;
  uint32_t gpu_size = 0;
  uint32_t texture_size = 0;
  uint32_t vb_size = 0;
  uint32_t ib_size = 0;
  uint32_t vb_offset = 0;
  uint32_t ib_offset = 0;
  uint32_t batches_offset = 0;
  uint32_t textures_offset = 0;
  std::vector<RawBatch> batches;
  std::vector<RawTexture> textures;
};

bool DecodeRawModel(const std::vector<uint8_t>& data, RawModel* out);
bool DecodeRawTexture(const std::vector<uint8_t>& data, RawTexture* out);

struct Skeleton {
  uint32_t count = 0;
  uint8_t parents[kMaxJoints] = {};
  float bind[kMaxJoints][3] = {};
};

struct Clip {
  uint32_t frames = 0;
  uint32_t joints = 0;
  float rate = 0.0f;
  std::vector<float> keys;
  float Length() const;
};

using Matrix = std::array<float, 16>;

struct Expression {
  uint32_t mouth = 0;
  uint32_t left_eyebrow = 0;
  uint32_t right_eyebrow = 0;
  uint32_t left_eye = 0;
  uint32_t right_eye = 0;
};

bool DecodeModel(const std::vector<uint8_t>& data, Model* out);
bool DecodeTexture(const std::vector<uint8_t>& data, Texture* out);
bool DecodeSkeleton(const uint8_t* data, size_t size, Skeleton* out);
bool DecodeClip(const std::vector<uint8_t>& data, Clip* out);
const Skeleton& MainSkeleton();

constexpr uint32_t kAnimationObjectBytes = 0x5990;
constexpr uint32_t kAnimationHeaderBytes = 0x28;
constexpr uint32_t kAnimationSizeOffset = 0x5988;
constexpr uint32_t kAnimationBufferOffset = 0x598C;

bool WriteAnimationObject(const std::vector<uint8_t>& stream, bool mirror,
                          uint8_t* object);

class Catalog {
 public:
  bool Load(const std::filesystem::path& path);
  bool loaded() const { return !entries_.empty(); }
  const std::vector<Entry>& entries() const { return entries_; }
  const Entry* Find(uint32_t index) const;
  bool Records(uint32_t index, std::vector<Record>* out) const;
  std::shared_ptr<const Model> LoadModel(uint32_t index);
  std::shared_ptr<const Texture> LoadFeature(uint32_t index);
  std::shared_ptr<const Clip> LoadClip(uint32_t index);
  bool AnimationStream(uint32_t index, std::vector<uint8_t>* out) const;
  std::shared_ptr<const RawModel> LoadRawModel(uint32_t index);
  std::shared_ptr<const RawTexture> LoadRawTexture(uint32_t index);

 private:
  std::vector<uint8_t> data_;
  std::vector<Entry> entries_;
  std::mutex mutex_;
  std::map<uint32_t, std::shared_ptr<const Model>> models_;
  std::map<uint32_t, std::shared_ptr<const Texture>> features_;
  std::map<uint32_t, std::shared_ptr<const Clip>> clips_;
  std::map<uint32_t, std::shared_ptr<const RawModel>> raw_models_;
  std::map<uint32_t, std::shared_ptr<const RawTexture>> raw_textures_;
};

struct Component {
  uint32_t entry = 0;
  uint32_t mask = 0;
};

std::vector<Component> Components(const Catalog& catalog,
                                  const Description& description);
int32_t FeatureEntry(const Description& description, uint32_t feature);
int32_t HeadFeatureForUsage(uint32_t usage);
int32_t FindAnimation(const Catalog& catalog, const uint8_t* asset_id);

std::filesystem::path CatalogPath(const std::filesystem::path& content_root);
Catalog* SharedCatalog(const std::filesystem::path& content_root);

bool IsHatVariant(const Entry& entry);
std::vector<uint32_t> ItemsForSlot(const Catalog& catalog, uint32_t slot,
                                   uint32_t body);
void PlaceItem(const Catalog& catalog, Description* description,
               uint32_t slot, uint16_t entry);
Description RandomDescription(const Catalog& catalog, std::mt19937& rng,
                              int32_t body);
Description DescriptionFromBytes(const Catalog* catalog, const uint8_t* bytes,
                                 size_t size);
int32_t PresetClip(uint32_t preset);

Matrix Identity();
Matrix Multiply(const Matrix& a, const Matrix& b);
void BindPose(const Skeleton& skeleton, Matrix* local);
void SamplePose(const Clip& clip, const Skeleton& skeleton, float seconds,
                Matrix* local);
void SkinMatrices(const Skeleton& skeleton, const Matrix* local, Matrix* skin);

struct Part {
  std::shared_ptr<const Model> model;
  uint32_t entry = 0;
  uint32_t kind = 0;
  float custom[3][4] = {};
};

struct Scene {
  uint32_t body = 1;
  std::vector<Part> parts;
  std::shared_ptr<const Texture> features[kFeatureCount];
  uint32_t feature_entries[kFeatureCount] = {};
  float colors[kColorCount][4] = {};
};

Scene BuildScene(Catalog& catalog, const Description& description);

struct Material {
  uint32_t layer[kLayerCount][4] = {};
  float tint[kLayerCount][4] = {};
  float base[4] = {};
  float custom[3][4] = {};
  const Texture* textures[kLayerCount] = {};
  uint64_t texture_ids[kLayerCount] = {};
};

Material BuildMaterial(const Scene& scene, const Part& part,
                       const Batch& batch, const Expression& expression);

struct GpuVertex {
  float position[3];
  float normal[3];
  float uv[kLayerCount][2];
};
static_assert(sizeof(GpuVertex) == 72, "avatar vertex layout");

struct GpuConstants {
  float world_view_projection[16];
  float world[16];
  float light_direction[4];
  float light_color[4];
  float ambient[4];
  float base[4];
  float custom[3][4];
  float tint[kLayerCount][4];
  uint32_t layer[kLayerCount][4];
};
static_assert(sizeof(GpuConstants) == 432, "avatar constant layout");

struct Lighting {
  float direction[3];
  float color[3];
  float ambient[3];
};

Lighting DefaultLighting();
constexpr float kBodyInset = 0.008f;

void UnpackNormal(uint32_t packed, float out[3]);

void SkinBatch(const Batch& batch, const Matrix* skin,
               std::vector<GpuVertex>* out, float inset = 0.0f);
void FillMaterialConstants(const Material& material, GpuConstants* out);
void RenderPreview(const Scene& scene, const Matrix* local,
                   const Expression& expression, uint32_t width,
                   uint32_t height, float yaw, std::vector<uint8_t>* rgba);
std::vector<uint8_t> EncodePng(uint32_t width, uint32_t height,
                               const std::vector<uint8_t>& rgba);

}  // namespace avatar
}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_AVATAR_FORMAT_H_
