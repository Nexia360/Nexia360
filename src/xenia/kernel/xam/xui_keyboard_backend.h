/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_KEYBOARD_BACKEND_H_
#define XENIA_KERNEL_XAM_XUI_KEYBOARD_BACKEND_H_

#include <filesystem>

#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/presenter.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

// Makes the console's own keyboard the one xe::ui::KeyboardDialog presents,
// if the dashboard assets have been imported. Returns false and leaves the
// ImGui keyboard in place when they have not. Safe to call again after an
// Install Content run, which is how a fresh import takes effect.
bool InstallKeyboardBackend(xe::ui::Presenter* presenter,
                            xe::ui::ImmediateDrawer* immediate_drawer,
                            const std::filesystem::path& asset_directory);

void UninstallKeyboardBackend();

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
