/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_SYSTEM_H_
#define XENIA_BASE_SYSTEM_H_

#include <filesystem>
#include <string_view>

#include "xenia/base/platform.h"
#include "xenia/base/string.h"

namespace xe {

#if XE_PLATFORM_ANDROID
bool InitializeAndroidSystemForApplicationContext();
void ShutdownAndroidSystem();
#endif

// The URL must include the protocol.
void LaunchWebBrowser(const std::string_view url);
void LaunchFileExplorer(const std::filesystem::path& path);

bool SetProcessPriorityClass(const uint32_t priority_class);

// Starts a second instance of this executable, in the executable's own folder,
// with no arguments, and returns without waiting for it. Used when a title
// switch has been left pending: the launch data the guest wrote is only ever
// read at startup, so the switch needs another run of the emulator to happen.
void LaunchSelf();

// Determine if the Xbox Gamebar is enabled via the Windows registry
bool IsUseNexusForGameBarEnabled();

enum class SimpleMessageBoxType {
  Help,
  Warning,
  Error,
};

// This is expected to block the caller until the message box is closed.
void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message);

}  // namespace xe

#endif  // XENIA_BASE_SYSTEM_H_
