/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_runtime.h"

#include <algorithm>
#include <functional>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

constexpr bool kDefaultShow = true;
constexpr float kDefaultOpacity = 1.0f;
constexpr float kDefaultScale = 1.0f;

}  // namespace

bool Package::LoadFromFile(const std::filesystem::path& path) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  std::vector<uint8_t> data(size > 0 ? size : 0);
  const size_t read =
      data.empty() ? 0 : fread(data.data(), 1, data.size(), file);
  fclose(file);
  if (read != data.size()) {
    return false;
  }
  return LoadFromMemory(std::move(data));
}

bool Package::LoadFromMemory(std::vector<uint8_t> data) {
  data_ = std::move(data);
  scenes_.clear();
  return OpenPackage(data_.data(), data_.size(), &entries_);
}

const PackageEntry* Package::Find(const std::string_view name) const {
  return FindPackageEntry(entries_, name);
}

Scene* Package::Open(const std::string_view name) {
  const std::string key(name);
  const auto cached = scenes_.find(key);
  if (cached != scenes_.end()) {
    return cached->second.get();
  }
  const PackageEntry* entry = Find(name);
  if (!entry) {
    return nullptr;
  }
  auto scene = std::make_unique<Scene>();
  if (!scene->Load(entry->data, entry->size)) {
    XELOGW("xui: scene {} failed to load", key);
    return nullptr;
  }
  auto result = scenes_.emplace(key, std::move(scene));
  return result.first->second.get();
}

bool Skin::Load(const uint8_t* data, size_t size) {
  if (!scene_.Load(data, size)) {
    return false;
  }
  visuals_.clear();
  // Visuals are not all direct children of the canvas - EditorSkin nests them,
  // and a name that does not resolve leaves its control with no appearance at
  // all, so index the whole tree.
  std::function<void(const Node&)> index = [&](const Node& node) {
    if (node.class_name == "XuiVisual") {
      const std::string id = StringProperty(node, "Id");
      if (!id.empty() && !visuals_.count(id)) {
        visuals_[id] = &node;
      }
    }
    for (const Node& child : node.children) {
      index(child);
    }
  };
  for (const Node& child : scene_.root().children) {
    index(child);
  }
  loaded_ = true;
  return true;
}

const Node* Skin::Find(const std::string_view name) const {
  const auto entry = visuals_.find(std::string(name));
  return entry == visuals_.end() ? nullptr : entry->second;
}

namespace {

bool ReadWholeFile(const std::filesystem::path& path,
                   std::vector<uint8_t>* out_data) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  out_data->resize(size > 0 ? size : 0);
  const size_t read = out_data->empty()
                          ? 0
                          : fread(out_data->data(), 1, out_data->size(), file);
  fclose(file);
  return read == out_data->size();
}

}  // namespace

bool AssetStore::Open(const std::filesystem::path& directory) {
  directory_ = directory;
  packages_.clear();
  loaded_ = false;

  if (!ReadWholeFile(directory / "skin.xur", &skin_data_) ||
      !skin_.Load(skin_data_.data(), skin_data_.size())) {
    XELOGW("xui: no skin at {}", xe::path_to_utf8(directory));
    return false;
  }
  // The installer names a font from its own sfnt name table, and falls back to
  // fontN.xtt when the console left none, so the UI font is found by scanning
  // rather than by a fixed file name.
  std::filesystem::path font_path;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string name =
        xe::utf8::lower_ascii(xe::path_to_utf8(entry.path().filename()));
    if (!name.ends_with(".xtt")) {
      continue;
    }
    if (name.find("segoe") != std::string::npos) {
      font_path = entry.path();
      break;
    }
    if (font_path.empty()) {
      font_path = entry.path();
    }
  }
  if (font_path.empty() || !ReadWholeFile(font_path, &font_data_) ||
      !font_.Load(font_data_.data(), font_data_.size())) {
    XELOGW("xui: no font at {}", xe::path_to_utf8(directory));
    return false;
  }
  loaded_ = true;
  return true;
}

Package* AssetStore::GetPackage(const std::string& name) {
  const auto cached = packages_.find(name);
  if (cached != packages_.end()) {
    return cached->second.get();
  }
  auto package = std::make_unique<Package>();
  if (!package->LoadFromFile(directory_ / (name + ".xzp"))) {
    return nullptr;
  }
  auto result = packages_.emplace(name, std::move(package));
  return result.first->second.get();
}

const Value* Property(const Node& node, const std::string_view name) {
  return FindProperty(node, name);
}

bool BoolProperty(const Node& node, const std::string_view name,
                  bool fallback) {
  const Value* value = Property(node, name);
  return value ? value->boolean : fallback;
}

float FloatProperty(const Node& node, const std::string_view name,
                    float fallback) {
  const Value* value = Property(node, name);
  if (!value) {
    return fallback;
  }
  if (value->type == PropertyType::kFloat) {
    return value->number;
  }
  if (value->type == PropertyType::kInteger ||
      value->type == PropertyType::kUnsigned) {
    return static_cast<float>(value->integer);
  }
  return fallback;
}

int64_t IntProperty(const Node& node, const std::string_view name,
                    int64_t fallback) {
  const Value* value = Property(node, name);
  if (!value) {
    return fallback;
  }
  if (value->type == PropertyType::kInteger ||
      value->type == PropertyType::kUnsigned) {
    return value->integer;
  }
  if (value->type == PropertyType::kFloat) {
    return static_cast<int64_t>(value->number);
  }
  return fallback;
}

std::string StringProperty(const Node& node, const std::string_view name) {
  const Value* value = Property(node, name);
  return value && value->type == PropertyType::kString ? value->string
                                                       : std::string();
}

Element Layout::Instantiate(const Node& node) const {
  Element element;
  element.source = &node;
  element.class_name = node.class_name;
  element.id = StringProperty(node, "Id");
  if (skin_ && skin_->loaded()) {
    const std::string named = StringProperty(node, "Visual");
    if (!named.empty()) {
      element.visual = skin_->Find(named);
    }
    if (!element.visual) {
      element.visual = skin_->Find(node.class_name);
    }
  }
  element.children.reserve(node.children.size());
  for (const Node& child : node.children) {
    element.children.push_back(Instantiate(child));
  }
  return element;
}

void Layout::Arrange(Element& element, float origin_x, float origin_y,
                     float scale_x, float scale_y, float opacity,
                     float stretch_x, float stretch_y) const {
  const Node& node = *element.source;

  float position_x = 0.0f;
  float position_y = 0.0f;
  if (const Value* position = Property(node, "Position")) {
    if (position->type == PropertyType::kVector) {
      position_x = position->vector.value[0];
      position_y = position->vector.value[1];
    }
  }

  float own_scale_x = kDefaultScale;
  float own_scale_y = kDefaultScale;
  if (const Value* scale = Property(node, "Scale")) {
    if (scale->type == PropertyType::kVector) {
      if (scale->vector.value[0] != 0.0f) {
        own_scale_x = scale->vector.value[0];
      }
      if (scale->vector.value[1] != 0.0f) {
        own_scale_y = scale->vector.value[1];
      }
    }
  }

  float width = FloatProperty(node, "Width", 0.0f);
  float height = FloatProperty(node, "Height", 0.0f);

  const uint32_t anchor = static_cast<uint32_t>(IntProperty(node, "Anchor", 0));
  if ((anchor & kAnchorHorizontal) == kAnchorHorizontal) {
    width *= stretch_x;
  }
  if ((anchor & kAnchorVertical) == kAnchorVertical) {
    height *= stretch_y;
  }

  element.rect.x = origin_x + position_x * scale_x;
  element.rect.y = origin_y + position_y * scale_y;
  element.rect.width = width * scale_x * own_scale_x;
  element.rect.height = height * scale_y * own_scale_y;
  element.visible = BoolProperty(node, "Show", kDefaultShow);
  element.opacity = opacity * FloatProperty(node, "Opacity", kDefaultOpacity);

  // CenterPivot overrides an authored Pivot outright; otherwise the pivot is
  // the authored offset from the element's own top-left, in its own space.
  if (BoolProperty(node, "CenterPivot", false)) {
    element.pivot_x = element.rect.width * 0.5f;
    element.pivot_y = element.rect.height * 0.5f;
  } else {
    element.pivot_x = 0.0f;
    element.pivot_y = 0.0f;
    if (const Value* pivot = Property(node, "Pivot")) {
      if (pivot->type == PropertyType::kVector) {
        element.pivot_x = pivot->vector.value[0] * scale_x * own_scale_x;
        element.pivot_y = pivot->vector.value[1] * scale_y * own_scale_y;
      }
    }
  }

  for (Element& child : element.children) {
    Arrange(child, element.rect.x, element.rect.y, scale_x * own_scale_x,
            scale_y * own_scale_y, element.opacity, stretch_x, stretch_y);
  }
}

Element Layout::Build(const Node& root, float offset_x, float offset_y,
                      float zoom) const {
  Element element = Instantiate(root);
  Arrange(element, offset_x, offset_y, zoom, zoom, 1.0f, 1.0f, 1.0f);
  return element;
}

Element Layout::BuildFitted(const Node& root, float frame_width,
                            float frame_height) const {
  Element probe = Build(root);
  float zoom = 1.0f;
  if (probe.rect.width > 0.0f && probe.rect.height > 0.0f) {
    zoom = std::min(frame_width / probe.rect.width,
                    frame_height / probe.rect.height);
  }
  const float offset_x = (frame_width - probe.rect.width * zoom) * 0.5f;
  const float offset_y = (frame_height - probe.rect.height * zoom) * 0.5f;
  return Build(root, offset_x, offset_y, zoom);
}

const Element* Layout::FindById(const Element& root,
                                const std::string_view id) const {
  if (root.id == id) {
    return &root;
  }
  for (const Element& child : root.children) {
    if (const Element* found = FindById(child, id)) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
