/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <map>
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"

namespace xe {

void LaunchWebBrowser(const std::string_view url) {
  auto wide_url = xe::to_utf16(url);
  ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(wide_url.c_str()),
                nullptr, nullptr, SW_SHOWNORMAL);
}

void LaunchSelf() {
  const std::filesystem::path executable = xe::filesystem::GetExecutablePath();
  const std::filesystem::path folder = xe::filesystem::GetExecutableFolder();

  XELOGI("Relaunching: {}", xe::path_to_utf8(executable));

  // SEE_MASK_NOASYNC because the caller is on its way out: without it the
  // shell call can be abandoned when this process dies, and nothing starts.
  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOASYNC;
  info.lpVerb = L"open";
  info.lpFile = reinterpret_cast<LPCWSTR>(executable.c_str());
  info.lpDirectory = reinterpret_cast<LPCWSTR>(folder.c_str());
  info.nShow = SW_SHOWNORMAL;

  if (!ShellExecuteExW(&info)) {
    XELOGE("Relaunch failed: {:08X}", GetLastError());
  }
}

void LaunchFileExplorer(const std::filesystem::path& url) {
  ShellExecuteW(nullptr, L"explore", url.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

void ShowSimpleMessageBox(SimpleMessageBoxType type,
                          const std::string_view message) {
  const wchar_t* title;
  std::u16string wide_message = xe::to_utf16(message);
  DWORD type_flags = MB_OK | MB_APPLMODAL | MB_SETFOREGROUND;
  switch (type) {
    default:
    case SimpleMessageBoxType::Help:
      title = L"Xenia Help";
      type_flags |= MB_ICONINFORMATION;
      break;
    case SimpleMessageBoxType::Warning:
      title = L"Xenia Warning";
      type_flags |= MB_ICONWARNING;
      break;
    case SimpleMessageBoxType::Error:
      title = L"Xenia Error";
      type_flags |= MB_ICONERROR;
      break;
  }
  MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(wide_message.c_str()), title,
              type_flags);
}

static std::map<const uint32_t, DWORD> xeniaToWindowsPriorityClassMapping = {
    {0, NORMAL_PRIORITY_CLASS},
    {1, ABOVE_NORMAL_PRIORITY_CLASS},
    {2, HIGH_PRIORITY_CLASS},
    {3, REALTIME_PRIORITY_CLASS}};

bool SetProcessPriorityClass(const uint32_t priority_class) {
  if (!xeniaToWindowsPriorityClassMapping.count(priority_class)) {
    return false;
  }

  return SetPriorityClass(GetCurrentProcess(),
                          xeniaToWindowsPriorityClassMapping[priority_class]);
}

bool IsUseNexusForGameBarEnabled() {
  constexpr LPCWSTR reg_path = L"SOFTWARE\\Microsoft\\GameBar";
  constexpr LPCWSTR key = L"UseNexusForGameBarEnabled";

  DWORD value = 0;
  DWORD dataSize = sizeof(value);

  RegGetValue(HKEY_CURRENT_USER, reg_path, key, RRF_RT_DWORD, nullptr, &value,
              &dataSize);

  return static_cast<bool>(value);
}

}  // namespace xe
