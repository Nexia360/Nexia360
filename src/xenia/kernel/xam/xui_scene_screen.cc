/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_scene_screen.h"

#include <utility>

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

SceneScreen::SceneScreen(std::string package_name, std::string scene_file,
                         std::string script_module)
    : package_name_(std::move(package_name)),
      scene_file_(std::move(scene_file)),
      script_module_(std::move(script_module)) {}

bool SceneScreen::LoadScene(AssetStore& assets, const std::string& file,
                            Scene** out_scene, Element* out_root) {
  Package* package = assets.GetPackage(package_name_);
  if (!package) {
    return false;
  }
  Scene* scene = package->Open(file);
  if (!scene) {
    return false;
  }
  if (const Scene::Section* custom = scene->FindSection("CUST")) {
    ParseFigurePaths(custom->data, custom->size, &figures_);
  }
  const Layout layout(skin_);
  *out_root = layout.BuildFitted(scene->root(), design_width_, design_height_);
  *out_scene = scene;
  return true;
}

bool SceneScreen::Prepare(AssetStore& assets) {
  package_ = assets.GetPackage(package_name_);
  if (!package_) {
    XELOGW("xui scene: no {} package installed", package_name_);
    return false;
  }
  shared_ = assets.GetPackage("sharedres");
  common_ = assets.GetPackage("dashcommon");

  // The dashboard's own skin first, wherever it is kept, then the flash one.
  skin_ = nullptr;
  for (const char* source : {"dashskin", "dashcontrolpack"}) {
    Package* package = assets.GetPackage(source);
    const PackageEntry* entry = package ? package->Find("skin.xur") : nullptr;
    if (!entry) {
      continue;
    }
    skin_data_.assign(entry->data, entry->data + entry->size);
    if (own_skin_.Load(skin_data_.data(), skin_data_.size())) {
      skin_ = &own_skin_;
      break;
    }
  }
  if (!skin_) {
    skin_ = assets.skin();
  }
  if (skin_ && skin_->loaded()) {
    if (const Scene::Section* custom = skin_->scene().FindSection("CUST")) {
      ParseFigurePaths(custom->data, custom->size, &skin_figures_);
    }
  }

  if (!LoadScene(assets, scene_file_, &scene_, &root_)) {
    XELOGW("xui scene: {} is not in {}", scene_file_, package_name_);
    return false;
  }

  Overlay* overlay = SharedOverlay();
  lua_ = overlay ? overlay->lua() : nullptr;
  if (lua_) {
    lua_->AddScriptPackage(package_);
    lua_->SetSkin(skin_);
    lua_->AddStringTable("Strings.xus");
    lua_->SetSceneRoot(scene_, &root_, scene_file_);
    // The module builds the class library on its way in, declares this
    // scene's class and hangs its handlers off it.
    if (!script_module_.empty() && !lua_->RunModule(script_module_)) {
      XELOGW("xui scene: {} did not run: {}", script_module_,
             lua_->last_error());
    }
    lua_->InitFocus();
  }
  return true;
}

void SceneScreen::Draw(class Draw& draw, const Element& root) {
  SceneDrawer drawer(&draw, package_, skin_);
  drawer.AddFallbackPackage(shared_);
  drawer.AddFallbackPackage(common_);
  drawer.SetFigures(&figures_);
  drawer.SetSkinFigures(&skin_figures_);
  // Each control holds its own playhead, so the drawer asks which one governs
  // whatever it is about to put down rather than being handed one for the
  // whole scene.
  if (lua_) {
    drawer.SetTimelineSource([this](const Element& element) {
      return lua_->PlayerFor(const_cast<Element*>(&element));
    });
  }
  drawer.Draw(root);
}

bool SceneScreen::HandleInput(LuaHost::Button button) {
  if (!lua_) {
    return false;
  }
  return lua_->HandleInput(button);
}

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
