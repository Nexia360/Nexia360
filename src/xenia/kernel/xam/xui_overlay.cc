/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_overlay.h"

#include <algorithm>
#include <utility>

#include "xenia/base/clock.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

constexpr size_t kOverlayZOrder = 100;

// Where the dashboard keeps its Lua. luaxbox is the XUI class library every
// scene is built on and has to be searched first; the rest are the
// dashboard's own modules.
constexpr const char* kScriptPackages[] = {
    "dashluaxbox", "dashlua",    "dashsociallua",   "dashhubapp",
    "dashcommon",  "dashsocial", "dashcontrolpack",
};

}  // namespace

Overlay::Overlay(xe::ui::Presenter* presenter,
                 xe::ui::ImmediateDrawer* immediate_drawer)
    : presenter_(presenter), immediate_drawer_(immediate_drawer) {}

Overlay::~Overlay() {
  if (attached_ && presenter_) {
    presenter_->RemoveUIDrawerFromUIThread(this);
  }
}

bool Overlay::Open(const std::filesystem::path& asset_directory) {
  if (!assets_.Open(asset_directory)) {
    return false;
  }
  lua_ = std::make_unique<LuaHost>();
  if (!lua_->Open()) {
    XELOGW("xui: no Lua for this overlay: {}", lua_->last_error());
    lua_.reset();
  } else {
    // The scene scripts sit in the same packages as the scenes themselves, so
    // every package the store can open is a place to look for them.
    for (const char* name : kScriptPackages) {
      lua_->AddScriptPackage(assets_.GetPackage(name));
    }
  }
  if (presenter_ && !attached_) {
    presenter_->AddUIDrawerFromUIThread(this, kOverlayZOrder);
    attached_ = true;
  }
  return true;
}

bool Overlay::Push(Screen* screen) {
  if (!screen || !assets_.loaded() || !screen->Prepare(assets_)) {
    return false;
  }
  screens_.push_back(screen);
  if (presenter_) {
    presenter_->RequestUIPaintFromUIThread();
  }
  return true;
}

void Overlay::Remove(Screen* screen) {
  screens_.erase(std::remove(screens_.begin(), screens_.end(), screen),
                 screens_.end());
}

void Overlay::PopAll() { screens_.clear(); }

bool Overlay::GuideIsOpen() {
  KernelState* state = kernel_state();
  if (!state) {
    return false;
  }
  XamState* xam = state->xam_state();
  return xam && xam->IsUIActive();
}

void Overlay::Draw(xe::ui::UIDrawContext& ui_draw_context) {
  if (screens_.empty() || !assets_.loaded() || !immediate_drawer_) {
    return;
  }
  // A pushed screen is a modal the host put up deliberately, so it draws on
  // its own account. The guide gate is for screens the overlay raises itself
  // - only three of the keyboard's four callers are XAM dialogs, so gating
  // everything on IsUIActive() would leave the other three invisible.
  if (screens_.empty() && !GuideIsOpen()) {
    return;
  }

  // The glyph and image caches live in Draw, so it outlives the frame.
  if (!draw_) {
    draw_ = std::make_unique<xui::Draw>(immediate_drawer_, assets_.font());
  }

  // Timelines and script timers run off wall time, and a long first frame or
  // a paused emulator must not make them jump.
  const uint64_t now = xe::Clock::QueryHostSystemTime();
  const float elapsed =
      clock_ ? std::min(static_cast<float>(now - clock_) / 1.0e7f, 0.25f)
             : 0.0f;
  clock_ = now;
  if (lua_) {
    lua_->Tick(elapsed);
  }
  // Each screen states the space it was authored in, so Begin is per screen
  // rather than one hardcoded 852x480 for everything.
  for (Screen* screen : screens_) {
    immediate_drawer_->Begin(ui_draw_context, screen->design_width(),
                             screen->design_height());
    screen->Draw(*draw_, screen->root());
    immediate_drawer_->End();
  }
}

namespace {
std::unique_ptr<Overlay> shared_overlay;
}  // namespace

bool OpenSharedOverlay(xe::ui::Presenter* presenter,
                       xe::ui::ImmediateDrawer* immediate_drawer,
                       const std::filesystem::path& asset_directory) {
  CloseSharedOverlay();
  if (!presenter || !immediate_drawer) {
    return false;
  }
  auto candidate = std::make_unique<Overlay>(presenter, immediate_drawer);
  if (!candidate->Open(asset_directory)) {
    return false;
  }
  shared_overlay = std::move(candidate);
  return true;
}

void CloseSharedOverlay() { shared_overlay.reset(); }

Overlay* SharedOverlay() { return shared_overlay.get(); }

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
