/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_UI_NEW_H_
#define XENIA_KERNEL_XAM_XAM_UI_NEW_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

enum class PropertyType : uint8_t {
  kNone = 0,
  kBool = 1,
  kInteger = 2,
  kUnsigned = 3,
  kFloat = 4,
  kString = 5,
  kColor = 6,
  kVector = 7,
  kQuaternion = 8,
  kObject = 9,
  kCustom = 10,
};

struct PropertyTable;

struct Property {
  const char* name;
  PropertyType type;
  uint32_t flags;
  const PropertyTable* sub;
};

struct PropertyTable {
  const Property* properties;
  uint32_t count;
};

struct XuiClass {
  const char* name;
  const char* base;
  PropertyTable properties;
};

const XuiClass* FindClass(const std::string_view name);
std::vector<const XuiClass*> ClassChain(const std::string_view name);

struct Vector {
  float value[3] = {0.0f, 0.0f, 0.0f};
};

struct Quaternion {
  float value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct Node;

struct Value {
  PropertyType type = PropertyType::kNone;
  bool boolean = false;
  int64_t integer = 0;
  float number = 0.0f;
  uint32_t color = 0;
  uint32_t index = 0;
  Vector vector;
  std::string string;
  std::shared_ptr<Node> object;
};

using PropertyMap = std::map<std::string, std::vector<Value>>;

// One property a timeline drives. The path walks from the target object down
// through nested object properties (XuiFigure -> Fill -> FillColor), and an
// array-valued property carries the element it animates.
struct Track {
  uint8_t levels = 0;
  uint8_t property = 0;
  std::vector<uint8_t> path;
  bool has_data = false;
  uint32_t data = 0;

  // Resolved against the target's class chain once the scene is parsed. The
  // key values are pool indices and mean nothing without it.
  std::string property_name;
  PropertyType property_type = PropertyType::kNone;
  // One key value per key in the owning timeline, as a pool index of
  // property_type. Empty when the property could not be resolved.
  std::vector<uint32_t> values;
};

// Interpolation modes carried by a key. Anything unlisted holds its value
// until the next key.
constexpr uint8_t kInterpolateHold = 0;
constexpr uint8_t kInterpolateLinear = 1;
constexpr uint8_t kInterpolateEase = 2;

struct Key {
  uint32_t frame = 0;
  uint8_t interpolation = 0;
  uint8_t flags = 0;
  // interpolation == kInterpolateEase: the three raw shaping bytes.
  uint8_t ease[3] = {0, 0, 0};
  // interpolation in {7, 10, 11, 12}: index into the scene's vectors.
  bool has_tangent = false;
  uint32_t tangent = 0;
  // Element index of this key's first track in the key value pool.
  uint32_t value_base = 0;
};

struct Timeline {
  std::string target;
  std::vector<Track> tracks;
  uint32_t key_count = 0;
  uint32_t key_block = 0;
  std::vector<Key> keys;

  uint32_t last_frame() const { return keys.empty() ? 0 : keys.back().frame; }
};

// A NAME record: the points in an object's timelines the runtime plays to, by
// name. These are the control states - Normal, Focus, Press, and the scene's
// own FadeIn/FadeOut.
struct TimelineState {
  std::string name;
  uint32_t frame = 0;
  uint8_t kind = 0;
  std::string target;
};

struct Node {
  std::string class_name;
  uint8_t flags = 0;
  uint32_t property_count = 0;
  bool shared = false;
  uint32_t shared_index = 0;
  PropertyMap properties;
  std::vector<Node> children;
  std::vector<Timeline> timelines;
  std::vector<TimelineState> states;
};

const Value* FindProperty(const Node& node, const std::string_view name);

constexpr uint8_t kObjectHasProperties = 0x01;
constexpr uint8_t kObjectHasChildren = 0x02;
constexpr uint8_t kObjectHasTimelines = 0x04;
constexpr uint8_t kObjectIsShared = 0x08;

constexpr size_t kPackageHeaderSize = 22;
constexpr size_t kSceneHeaderSize = 20;
constexpr size_t kPoolCount = 12;

struct PackageEntry {
  std::string name;
  const uint8_t* data = nullptr;
  size_t size = 0;
};

bool OpenPackage(const uint8_t* data, size_t size,
                 std::vector<PackageEntry>* out_entries);
const PackageEntry* FindPackageEntry(const std::vector<PackageEntry>& entries,
                                     const std::string_view name);

class PackedReader {
 public:
  PackedReader(const uint8_t* data, size_t size)
      : data_(data), size_(size), at_(0), failed_(false) {}

  uint8_t ReadByte();
  uint16_t ReadUint16();
  uint32_t ReadUint32();
  uint32_t ReadPacked();
  float ReadFloat();

  size_t offset() const { return at_; }
  bool eof() const { return at_ >= size_; }
  bool failed() const { return failed_; }
  void set_failed() { failed_ = true; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t at_;
  bool failed_;
};

class Scene {
 public:
  struct Section {
    std::string tag;
    const uint8_t* data = nullptr;
    size_t size = 0;
  };

  bool Load(const uint8_t* data, size_t size);

  const Node& root() const { return root_; }
  const std::vector<std::string>& strings() const { return strings_; }
  const std::vector<float>& floats() const { return floats_; }
  const std::vector<Vector>& vectors() const { return vectors_; }
  const std::vector<uint32_t>& colors() const { return colors_; }
  const std::vector<Section>& sections() const { return sections_; }
  const Section* FindSection(const std::string_view tag) const;
  std::string StringAt(uint32_t index) const;

  // Pool lookups for a resolved track value. Out of range reads give the
  // fallback rather than failing the scene, because a track can name a
  // property this build does not describe.
  float FloatAt(uint32_t index, float fallback = 0.0f) const;
  uint32_t ColorAt(uint32_t index, uint32_t fallback = 0) const;
  Vector VectorAt(uint32_t index) const;
  Quaternion QuaternionAt(uint32_t index) const;
  // A scene only ever turns things in the plane, so what a rotation means to
  // the drawer is the angle about z, in degrees.
  static float RotationDegrees(const Quaternion& value);

 private:
  void LoadStrings();
  void LoadNumbers();
  // KEYP (key values), KEYD (key records) and NAME (named states).
  void LoadKeyPools();
  // Walks the parsed tree and gives every track its property and its key
  // values. Targets name children, so this can only run once they exist.
  void ResolveTimelines(Node& node);
  void ResolveTimeline(Node& owner, Timeline& timeline);
  bool ResolveTrack(const std::string_view class_name, Track& track) const;
  Node ReadObject(PackedReader& reader);
  Node ReadNested(PackedReader& reader, const Property& property);
  PropertyMap ReadBlock(PackedReader& reader, const PropertyTable& table);
  PropertyMap ReadProperties(PackedReader& reader,
                             const std::string_view class_name);
  Value ReadValue(PackedReader& reader, const Property& property);
  Track ReadTrack(PackedReader& reader);
  std::vector<Timeline> ReadTimelines(PackedReader& reader, bool has_children,
                                      std::vector<TimelineState>* states);

  Node root_;
  std::vector<std::string> strings_;
  std::vector<float> floats_;
  std::vector<Vector> vectors_;
  std::vector<Quaternion> quaternions_;
  std::vector<uint32_t> colors_;
  std::vector<Section> sections_;
  std::vector<uint32_t> key_values_;
  std::vector<Key> key_records_;
  std::vector<TimelineState> state_records_;
  std::map<uint32_t, PropertyMap> pool_;
  uint32_t pool_hint_[kPoolCount] = {};
};

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
