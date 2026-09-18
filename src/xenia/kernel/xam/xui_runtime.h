/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_RUNTIME_H_
#define XENIA_KERNEL_XAM_XUI_RUNTIME_H_

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xam/xam_ui_new.h"
#include "xenia/kernel/xam/xui_font.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

constexpr uint32_t kAnchorLeft = 0x1;
constexpr uint32_t kAnchorTop = 0x2;
constexpr uint32_t kAnchorRight = 0x4;
constexpr uint32_t kAnchorBottom = 0x8;
constexpr uint32_t kAnchorHorizontal = kAnchorLeft | kAnchorRight;
constexpr uint32_t kAnchorVertical = kAnchorTop | kAnchorBottom;

constexpr float kDesignWidth = 852.0f;
constexpr float kDesignHeight = 480.0f;

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

struct Element {
  const Node* source = nullptr;
  const Node* visual = nullptr;
  std::string class_name;
  std::string id;
  Rect rect;
  float opacity = 1.0f;
  bool visible = true;
  // Where a scale or rotation is applied from, in the element's own space.
  // An element grows from its top-left corner unless it says otherwise, so
  // this is not the centre by default.
  float pivot_x = 0.0f;
  float pivot_y = 0.0f;
  // What a script has scaled and turned this element by since it was laid
  // out. Scale is already in the rect; these are what it can read back, and
  // the rotation is what the drawer turns the element by.
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float rotation = 0.0f;
  std::vector<Element> children;
};

// One loaded XZP package plus the scenes parsed out of it.
class Package {
 public:
  bool LoadFromFile(const std::filesystem::path& path);
  bool LoadFromMemory(std::vector<uint8_t> data);

  const PackageEntry* Find(const std::string_view name) const;
  Scene* Open(const std::string_view name);

  const std::vector<PackageEntry>& entries() const { return entries_; }

 private:
  std::vector<uint8_t> data_;
  std::vector<PackageEntry> entries_;
  std::map<std::string, std::unique_ptr<Scene>> scenes_;
};

// skin.xur: a canvas whose children are XuiVisual entries keyed by Id.
class Skin {
 public:
  bool Load(const uint8_t* data, size_t size);
  const Node* Find(const std::string_view name) const;
  const Scene& scene() const { return scene_; }
  bool loaded() const { return loaded_; }

 private:
  Scene scene_;
  std::map<std::string, const Node*> visuals_;
  bool loaded_ = false;
};

// Assets installed alongside the dashboard files: the module packages plus the
// shared skin and fonts. Knows nothing about the presenter, so an offline tool
// can open an installed asset directory too.
class AssetStore {
 public:
  bool Open(const std::filesystem::path& directory);
  bool loaded() const { return loaded_; }

  Package* GetPackage(const std::string& name);
  Skin* skin() { return &skin_; }
  Font* font() { return &font_; }

 private:
  std::filesystem::path directory_;
  std::map<std::string, std::unique_ptr<Package>> packages_;
  std::vector<uint8_t> skin_data_;
  std::vector<uint8_t> font_data_;
  Skin skin_;
  Font font_;
  bool loaded_ = false;
};

const Value* Property(const Node& node, const std::string_view name);
bool BoolProperty(const Node& node, const std::string_view name, bool fallback);
float FloatProperty(const Node& node, const std::string_view name,
                    float fallback);
int64_t IntProperty(const Node& node, const std::string_view name,
                    int64_t fallback);
std::string StringProperty(const Node& node, const std::string_view name);

// Builds the runtime tree and computes absolute rectangles in design space.
class Layout {
 public:
  explicit Layout(const Skin* skin) : skin_(skin) {}

  Element Build(const Node& root, float offset_x = 0.0f, float offset_y = 0.0f,
                float zoom = 1.0f) const;

  // Scales a scene authored smaller than the frame so it fills it, centred.
  Element BuildFitted(const Node& root, float frame_width = kDesignWidth,
                      float frame_height = kDesignHeight) const;

  const Element* FindById(const Element& root, const std::string_view id) const;

 private:
  Element Instantiate(const Node& node) const;
  void Arrange(Element& element, float origin_x, float origin_y, float scale_x,
               float scale_y, float opacity, float stretch_x,
               float stretch_y) const;

  const Skin* skin_ = nullptr;
};

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
