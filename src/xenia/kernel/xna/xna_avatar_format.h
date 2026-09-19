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
constexpr size_t kManifestBytes = 0x3E8;
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
int32_t BlendShapeSlot(uint32_t kind);
const char* SlotName(uint32_t slot);
const char* ColorName(uint32_t color);

struct Description {
  uint8_t body = 1;
  uint8_t height = 128;
  uint8_t weight = 128;
  std::array<uint16_t, kSlotCount> items;
  std::array<uint32_t, kColorCount> colors;
  std::array<std::array<uint32_t, 3>, kClothingSlotCount> custom;
  std::array<std::array<uint8_t, 16>, 3> blend_shapes = {};
  std::vector<std::array<uint8_t, 32>> components;
  std::array<std::array<uint8_t, 32>, 4> required = {};
  uint32_t height_bits = 0;
  uint32_t weight_bits = 0;
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
  // NOT this asset's identity. For a pack entry these sixteen bytes name the
  // asset that REPLACES this one: a hat-compatible hair for a hairstyle, a
  // hiding template for clothing, an animation for a carryable, an eye shadow
  // for an eye texture. Only installed content (external != 0) stores its own
  // id here. Two thirds of the pack leaves it zero. Use ManifestAssetId() to
  // get an id that names one entry and survives a round trip.
  std::array<uint8_t, 16> asset_id = {};
  uint32_t external = 0;
  // The colours this item can be worn in, as the pack states them: nine slots
  // of three RGB triples, at the very front of the entry's record block. Only
  // shirts carry any in the shipping pack - 36 with one slot and three with
  // nine - and the rest of the catalogue leaves the whole region zero, which is
  // what `colour_count` being zero means.
  //
  // `colour_count` is how many slots are populated and `colours_per_slot` how
  // many triples of the first slot are, which is exactly the pair the editor's
  // asset record packs into one byte as `(count << 4) | per`. The second number
  // picks the tile's widget class, so it is not decoration.
  static constexpr uint32_t kColourSlots = 9;
  static constexpr uint32_t kColoursPerSlot = 3;
  std::array<uint8_t, kColourSlots * kColoursPerSlot * 3> colours = {};
  uint8_t colour_count = 0;
  uint8_t colours_per_slot = 0;
  // The pack's own bytes from the entry's +0x07, kept verbatim because XAM
  // hands them to a title verbatim: its record filler (0x8197E300) is
  // `memcpy(record + 0x1C, entry + 0x07, 0x91)`. The first of them is the
  // colour layout the Avatar Editor reads as `groups = byte >> 4`, and the
  // rest are the colour table. Nothing here may be synthesised - see
  // `flags_byte()`.
  static constexpr size_t kRecordBlockBytes = 0x91;
  std::array<uint8_t, kRecordBlockBytes> record_block = {};
  // The pack's flags BYTE, at entry +0x06 - the third byte of `flags`, which
  // reads the dword at +0x04. XAM writes exactly this byte, zero extended,
  // into the record at +0x18. Bit 0 marks a listable asset (the editor drops a
  // record without it) and bit 3 marks one whose colour comes from the Colour
  // menu's palette rather than from its own colourways.
  uint8_t flags_byte() const { return uint8_t((flags >> 8) & 0xFF); }
  // The two ends of a same-kind substitution, resolved at load. A hairstyle's
  // `substitute` is the version worn under a hat; that version's
  // `substitute_for` points back, and it is never offered as a choice.
  uint32_t substitute = UINT32_MAX;
  uint32_t substitute_for = UINT32_MAX;
  bool is_substitute() const { return substitute_for != UINT32_MAX; }
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
  // The model states one index buffer and each batch its byte offset into it,
  // so a triangle has a number that spans the whole model. A hiding template
  // addresses triangles by that number, which is the only reason this is kept.
  uint32_t first_triangle = 0;
  std::vector<Param> params;
  std::vector<Vertex> vertices;
  std::vector<uint16_t> indices;
};

struct Model {
  std::vector<Batch> batches;
  std::vector<std::shared_ptr<const Texture>> textures;
};

// One vertex a blend shape moves. `offset` is a BYTE offset into the model's
// vertex buffer, exactly as the pack states it - the batch it lands in decides
// the stride, so nothing here assumes one.
struct ShapeVertex {
  uint32_t offset = 0;
  float position[3] = {};
  uint32_t packed_normal = 0;
};

// A chin, a nose or a pair of ears. The pack ships the deformation twice, once
// for each head, so the vertices split into two runs a fixed number of
// vertices apart and only the run that lands inside the model is used.
struct Shape {
  std::vector<ShapeVertex> vertices;
  // A hiding template fills the tag 4 record's OTHER half: the triangles of
  // the body a garment covers, by whole-model triangle number. `body` is the
  // body the template was authored against, 1 male and 2 female.
  std::vector<uint32_t> hidden;
  uint32_t body = 0;
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

// One slice of a raw avatar texture, to and from straight RGBA8. `format` is
// the Xenos code the pack stores: 0x06 k_8_8_8_8, 0x12 DXT1, 0x13 DXT3,
// 0x14 DXT5. Both ends keep the console's halfword byte swap.
bool DecodeTextureSlice(const uint8_t* data, size_t size, uint32_t format,
                        uint32_t width, uint32_t height, uint32_t pitch,
                        uint8_t* rgba);
bool EncodeTextureSlice(const uint8_t* rgba, uint32_t format, uint32_t width,
                        uint32_t height, uint32_t pitch, uint8_t* out,
                        size_t size);

// Bytes one slice of `format` occupies at this size, and the row pitch that
// goes with it. Zero when the format is not one of the four above.
uint32_t TextureSliceBytes(uint32_t format, uint32_t width, uint32_t height,
                           uint32_t* out_pitch);

struct Skeleton {
  uint32_t count = 0;
  uint8_t parents[kMaxJoints] = {};
  float bind[kMaxJoints][3] = {};
  float scale[kMaxJoints][3];
  Skeleton();
};

constexpr size_t kKeyFloats = 10;

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
bool DecodeCarryableClip(const std::vector<uint8_t>& data, Clip* out);
bool DecodeCarryableSkeleton(const std::vector<uint8_t>& data, Skeleton* out);

struct Carryable {
  Skeleton skeleton;
  std::shared_ptr<const Clip> body;
  std::shared_ptr<const Clip> joints;
};
const Skeleton& MainSkeleton();
Skeleton ScaledSkeleton(const Description& description);

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
  const Entry* FindAsset(const uint8_t* asset_id) const;
  bool AddAsset(const std::array<uint8_t, 16>& asset_id, std::string name,
                std::vector<uint8_t> blob);
  bool Records(uint32_t index, std::vector<Record>* out) const;
  std::shared_ptr<const Model> LoadModel(uint32_t index);
  std::shared_ptr<const Shape> LoadShape(uint32_t index);
  std::shared_ptr<const Texture> LoadFeature(uint32_t index);
  std::shared_ptr<const Clip> LoadClip(uint32_t index);
  std::shared_ptr<const Carryable> LoadCarryable(uint32_t index);
  bool AnimationStream(uint32_t index, std::vector<uint8_t>* out) const;
  std::shared_ptr<const RawModel> LoadRawModel(uint32_t index);
  std::shared_ptr<const RawTexture> LoadRawTexture(uint32_t index);

 private:
  std::vector<uint8_t> data_;
  std::vector<std::vector<uint8_t>> externals_;
  std::vector<Entry> entries_;
  std::mutex mutex_;
  std::map<uint32_t, std::shared_ptr<const Model>> models_;
  std::map<uint32_t, std::shared_ptr<const Shape>> shapes_;
  std::map<uint32_t, std::shared_ptr<const Texture>> features_;
  std::map<uint32_t, std::shared_ptr<const Clip>> clips_;
  std::map<uint32_t, std::shared_ptr<const RawModel>> raw_models_;
  std::map<uint32_t, std::shared_ptr<const RawTexture>> raw_textures_;
  std::map<uint32_t, std::shared_ptr<const Carryable>> carryables_;
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
void PlaceItem(const Catalog& catalog, Description* description, uint32_t slot,
               uint16_t entry);
Description RandomDescription(const Catalog& catalog, std::mt19937& rng,
                              int32_t body);
Description DescriptionFromBytes(const Catalog* catalog, const uint8_t* bytes,
                                 size_t size);
std::array<uint8_t, 16> ManifestAssetId(const Entry& entry);
// The chin, nose and ears a description names, written into a head's vertices.
// The console does this inside XamAvatarGetAssets, so it has to happen on the
// raw model a title is handed as well as on the one we draw ourselves.
bool ReshapeRawHead(Catalog& catalog, const Description& description,
                    RawModel* head);
bool HasManifestLayout(const uint8_t* bytes, size_t size);
bool ParseManifest(const Catalog* catalog, const uint8_t* bytes, size_t size,
                   Description* out);
bool ParseAnyDescription(const Catalog* catalog, const uint8_t* bytes,
                         size_t size, Description* out);
std::array<uint8_t, kManifestBytes> SerializeManifest(
    const Catalog* catalog, const Description& description, uint64_t xuid);
int32_t PresetClip(uint32_t preset);

Matrix Identity();
Matrix Multiply(const Matrix& a, const Matrix& b);
void BindPose(const Skeleton& skeleton, Matrix* local);
void SamplePose(const Clip& clip, const Skeleton& skeleton, float seconds,
                Matrix* local);
void SkinMatrices(const Skeleton& skeleton, const Matrix* local, Matrix* skin);
void SampleCarryable(const Carryable& carryable, float seconds, Matrix* local);

struct Part {
  std::shared_ptr<const Model> model;
  uint32_t entry = 0;
  uint32_t kind = 0;
  bool carried = false;
  float custom[3][4] = {};
};

struct Scene {
  uint32_t body = 1;
  Skeleton skeleton;
  std::shared_ptr<const Carryable> carryable;
  std::vector<Part> parts;
  std::shared_ptr<const Texture> features[kFeatureCount];
  uint32_t feature_entries[kFeatureCount] = {};
  float colors[kColorCount][4] = {};
};

Scene BuildScene(Catalog& catalog, const Description& description);

// The hiding template a garment names, or UINT32_MAX when it names none. An
// asset id's index field - bytes 4 and 5, record block +0x98 - is a TOC index,
// and for a garment it points at its template: 267 of them do, covering 228 of
// the pack's 231 templates, and every name lines up ("Cowboy Boots" -> "Cowboy
// Boots Hiding Template").
uint32_t HidingTemplateOf(const Catalog& catalog, uint32_t entry);

// Degenerate the named triangles, exactly as xam does - it collapses a hidden
// triangle onto its first index rather than removing it, so nothing after it
// moves. Triangle numbers span the whole model; see Batch::first_triangle.
// Returns how many landed in no batch at all, which is the one thing that
// cannot be checked against the pack offline: zero is the healthy answer.
uint32_t HideTriangles(Model* model, const std::vector<uint32_t>& triangles);
uint32_t HideTriangles(RawModel* model, const std::vector<uint32_t>& triangles);

struct Material {
  uint32_t layer[kLayerCount][4] = {};
  float tint[kLayerCount][4] = {};
  float base[4] = {};
  float custom[3][4] = {};
  const Texture* textures[kLayerCount] = {};
  uint64_t texture_ids[kLayerCount] = {};
};

Material BuildMaterial(const Scene& scene, const Part& part, const Batch& batch,
                       const Expression& expression);

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

void UnpackNormal(uint32_t packed, float out[3]);

void SkinBatch(const Batch& batch, const Matrix* skin,
               std::vector<GpuVertex>* out, float inset = 0.0f);
void FillMaterialConstants(const Material& material, GpuConstants* out);
void RenderPreview(const Scene& scene, const Matrix* local,
                   const Matrix* carried_local, const Expression& expression,
                   uint32_t width, uint32_t height, float yaw,
                   std::vector<uint8_t>* rgba);
std::vector<uint8_t> EncodePng(uint32_t width, uint32_t height,
                               const std::vector<uint8_t>& rgba);

}  // namespace avatar
}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_AVATAR_FORMAT_H_
