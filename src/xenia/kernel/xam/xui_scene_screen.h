/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_SCENE_SCREEN_H_
#define XENIA_KERNEL_XAM_XUI_SCENE_SCREEN_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "xenia/kernel/xam/xui_lua.h"
#include "xenia/kernel/xam/xui_overlay.h"
#include "xenia/kernel/xam/xui_scene_draw.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

// A dashboard scene drawn as the console draws it: the XUR supplies the
// geometry and the art, the skin supplies each control's appearance, the
// scene's own timelines supply every state it can be in, and its Lua module
// supplies the behaviour.
//
// This is what a scene needs to be on screen and usable. What it is wired to
// underneath - which profile, which game list - is the script's business, and
// the host functions it reaches for beyond the XUI ones answer with nothing.
class SceneScreen : public Screen {
 public:
  // `scene_file` is the .xur inside `package_name`; `script_module` is the
  // Lua that drives it, if it has one.
  SceneScreen(std::string package_name, std::string scene_file,
              std::string script_module = std::string());

  bool Prepare(AssetStore& assets) override;
  void Draw(class Draw& draw, const Element& root) override;

  // A press from whoever owns this screen. Returns true when the scene used
  // it, so an unused Back can close the overlay instead.
  bool HandleInput(LuaHost::Button button);

  const std::string& scene_file() const { return scene_file_; }

 private:
  // Loads one scene out of the package and lays it out. Also used for the
  // scenes this one can navigate to.
  bool LoadScene(AssetStore& assets, const std::string& file, Scene** out_scene,
                 Element* out_root);

  std::string package_name_;
  std::string scene_file_;
  std::string script_module_;

  Package* package_ = nullptr;
  Package* shared_ = nullptr;
  // Art the scenes share between them lives beside neither.
  Package* common_ = nullptr;
  Scene* scene_ = nullptr;
  // The dashboard carries its own skin, which knows controls the flash skin
  // has never heard of.
  std::vector<uint8_t> skin_data_;
  Skin own_skin_;
  const Skin* skin_ = nullptr;
  LuaHost* lua_ = nullptr;

  // CUST paths for the scene and for the skin, which are separate tables
  // because a figure's Points index is an offset into its own section.
  std::map<uint32_t, FigurePath> figures_;
  std::map<uint32_t, FigurePath> skin_figures_;
};

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
