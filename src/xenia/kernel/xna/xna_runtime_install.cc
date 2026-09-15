/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_runtime_install.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/utf8.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"

#include <shellapi.h>
#endif  // XE_PLATFORM_WIN32

namespace xe {
namespace kernel {
namespace xna {

namespace {

std::atomic<bool> install_pending{false};

#if XE_PLATFORM_WIN32
bool RegistryFlag(const wchar_t* key, const wchar_t* value) {
  for (DWORD view :
       {DWORD(RRF_SUBKEY_WOW6432KEY), DWORD(RRF_SUBKEY_WOW6464KEY)}) {
    DWORD data = 0;
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_DWORD | view,
                     nullptr, &data, &size) == ERROR_SUCCESS) {
      return data != 0;
    }
  }
  return false;
}

std::filesystem::path WindowsDirectory() {
  wchar_t buffer[MAX_PATH];
  const UINT length = GetWindowsDirectoryW(buffer, MAX_PATH);
  if (!length || length >= MAX_PATH) {
    return std::filesystem::path(L"C:\\Windows");
  }
  return std::filesystem::path(buffer);
}

std::filesystem::path Windows32BitSystemDirectory() {
  wchar_t buffer[MAX_PATH];
  UINT length = GetSystemWow64DirectoryW(buffer, MAX_PATH);
  if (length && length < MAX_PATH) {
    return std::filesystem::path(buffer);
  }
  length = GetSystemDirectoryW(buffer, MAX_PATH);
  if (length && length < MAX_PATH) {
    return std::filesystem::path(buffer);
  }
  return WindowsDirectory() / "System32";
}

bool Exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

bool DotNet9Installed() {
  std::vector<std::filesystem::path> roots;
  if (const wchar_t* dotnet_root = _wgetenv(L"DOTNET_ROOT")) {
    roots.emplace_back(dotnet_root);
  }
  if (const wchar_t* program_files = _wgetenv(L"ProgramFiles")) {
    roots.emplace_back(std::filesystem::path(program_files) / "dotnet");
  }
  roots.emplace_back(L"C:\\Program Files\\dotnet");

  for (const auto& root : roots) {
    std::error_code ec;
    const auto shared = root / "shared" / "Microsoft.NETCore.App";
    for (const auto& entry : std::filesystem::directory_iterator(shared, ec)) {
      if (entry.is_directory(ec) &&
          entry.path().filename().wstring().rfind(L"9.", 0) == 0) {
        return true;
      }
    }
  }
  return false;
}

bool RunInstaller(std::string* out_error) {
  std::error_code ec;
  const auto directory =
      std::filesystem::temp_directory_path(ec) / "Nexia" / "XNA_Install";
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    *out_error =
        "could not create " + xe::path_to_utf8(directory) + ": " + ec.message();
    return false;
  }

  const auto script = directory / "install.ps1";
  {
    std::ofstream out(script, std::ios::binary | std::ios::trunc);
    if (out) {
      out.write(reinterpret_cast<const char*>(kXnaInstallScript),
                static_cast<std::streamsize>(kXnaInstallScriptSize));
    }
    if (!out) {
      *out_error = "could not write " + xe::path_to_utf8(script);
      return false;
    }
  }

  const std::wstring parameters =
      L"-NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\"";

  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  info.lpVerb = L"runas";
  info.lpFile = L"powershell.exe";
  info.lpParameters = parameters.c_str();
  info.nShow = SW_SHOWNORMAL;

  if (!ShellExecuteExW(&info)) {
    const DWORD err = GetLastError();
    *out_error = err == ERROR_CANCELLED
                     ? "Windows administrator permission was declined."
                     : fmt::format("could not start the installer ({})", err);
    return false;
  }

  DWORD exit_code = 0;
  if (info.hProcess) {
    WaitForSingleObject(info.hProcess, INFINITE);
    GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);
  }
  if (exit_code != 0) {
    *out_error =
        fmt::format("the installer stopped with exit code {}", exit_code);
    return false;
  }
  return true;
}
#endif  // XE_PLATFORM_WIN32

void Notify(std::string title, std::string text) {
  auto* emulator = kernel_state() ? kernel_state()->emulator() : nullptr;
  auto* display_window = emulator ? emulator->display_window() : nullptr;
  auto* imgui_drawer = emulator ? emulator->imgui_drawer() : nullptr;
  if (!display_window || !imgui_drawer) {
    return;
  }
  display_window->app_context().CallInUIThread([imgui_drawer, title, text]() {
    new xe::ui::HostNotificationWindow(imgui_drawer, title, text, 0);
  });
}

void InstallInBackground() {
  std::thread([]() {
#if XE_PLATFORM_WIN32
    std::string error;
    const bool ran = RunInstaller(&error);
    std::string missing;
    const bool installed = XnaRuntimeInstalled(&missing);
    if (installed) {
      XELOGI("XnaRuntime: the Microsoft XNA runtime is installed");
      Notify("XNA runtime installed",
             "Launch the game again. If the installer asked for a restart, "
             "restart Windows first.");
    } else if (!ran) {
      XELOGE("XnaRuntime: install did not complete: {}", error);
      Notify("XNA runtime not installed", error);
    } else {
      XELOGE("XnaRuntime: installer finished but still missing: {}", missing);
      Notify("XNA runtime still missing",
             "Still missing: " + missing +
                 ". A restart may be needed - see the log for details.");
    }
#endif  // XE_PLATFORM_WIN32
    install_pending.store(false);
  }).detach();
}

}  // namespace

bool XnaRuntimeInstalled(std::string* out_missing) {
  out_missing->clear();
#if XE_PLATFORM_WIN32
  std::vector<std::string> missing;

  if (!DotNet9Installed()) {
    missing.push_back(".NET 9 Runtime");
  }

  if (!RegistryFlag(L"SOFTWARE\\Microsoft\\NET Framework Setup\\NDP\\v3.5",
                    L"Install")) {
    missing.push_back(".NET Framework 3.5");
  }

  const auto windows = WindowsDirectory();
  if (!RegistryFlag(L"SOFTWARE\\Microsoft\\XNA\\Framework\\v3.1",
                    L"Installed") &&
      !Exists(windows / "assembly" / "GAC_32" / "Microsoft.Xna.Framework" /
              "3.1.0.0__6d5c3888ef60e27d")) {
    missing.push_back("XNA Framework 3.1");
  }

  if (!RegistryFlag(L"SOFTWARE\\Microsoft\\XNA\\Framework\\v4.0",
                    L"Installed") &&
      !Exists(windows / "Microsoft.NET" / "assembly" / "GAC_32" /
              "Microsoft.Xna.Framework" / "v4.0_4.0.0.0__842cf8be1de50553")) {
    missing.push_back("XNA Framework 4.0 Refresh");
  }

  const auto system32 = Windows32BitSystemDirectory();
  if (!Exists(system32 / "d3dx9_43.dll") ||
      !Exists(system32 / "xinput1_3.dll")) {
    missing.push_back("DirectX End-User Runtimes (June 2010)");
  }

  for (const auto& name : missing) {
    if (!out_missing->empty()) {
      *out_missing += ", ";
    }
    *out_missing += name;
  }
  return missing.empty();
#else
  return true;
#endif  // XE_PLATFORM_WIN32
}

bool XnaRuntimeInstallPending() { return install_pending.load(); }

bool InstallXnaRuntime(std::string* out_error) {
#if XE_PLATFORM_WIN32
  if (install_pending.exchange(true)) {
    *out_error = "an XNA runtime install is already running";
    return false;
  }
  const bool ran = RunInstaller(out_error);
  install_pending.store(false);
  return ran;
#else
  *out_error = "the XNA runtime installer only runs on Windows";
  return false;
#endif  // XE_PLATFORM_WIN32
}

void RequestXnaRuntimeInstall(const std::string& missing) {
#if XE_PLATFORM_WIN32
  if (install_pending.exchange(true)) {
    XELOGI("XnaRuntime: an install has already been offered");
    return;
  }

  auto* emulator = kernel_state() ? kernel_state()->emulator() : nullptr;
  auto* display_window = emulator ? emulator->display_window() : nullptr;
  auto* imgui_drawer = emulator ? emulator->imgui_drawer() : nullptr;
  if (!display_window || !imgui_drawer) {
    XELOGW("XnaRuntime: no window to ask in - nothing will be installed");
    install_pending.store(false);
    return;
  }

  auto show_dialog = [imgui_drawer, missing]() {
    std::string title = "Install the Microsoft XNA runtime?";
    std::string body =
        "This is an Xbox Live Indie Game. Nexia runs it with Microsoft's XNA\n"
        "Framework, and this PC is missing:\n"
        "  " +
        missing +
        "\n\n"
        "Choosing Accept runs Nexia's XNA installer. It downloads these from\n"
        "Microsoft's own servers, checks that each is signed by Microsoft, "
        "and\n"
        "installs them:\n"
        "  - .NET Framework 3.5 (a Windows feature)\n"
        "  - .NET 9 Runtime (runs Nexia's XNA host)\n"
        "  - XNA Framework 3.1\n"
        "  - XNA Framework 4.0 Refresh\n"
        "  - DirectX End-User Runtimes (June 2010)\n"
        "It also downloads the XNA Game Studio 4.0 Refresh setup, but does "
        "not\n"
        "run it. Nothing else on this PC is changed.\n\n"
        "Windows will ask for administrator permission, because only an\n"
        "administrator can install these. A PowerShell window shows the\n"
        "progress, and a restart may be needed afterwards.\n\n"
        "Choosing Cancel installs nothing, and the game will not start.";

    auto* dialog = new xam::MessageBoxDialog(imgui_drawer, title, body,
                                             {"Accept", "Cancel"}, 1);

    dialog->set_close_callback([dialog]() {
      if (dialog->chosen_button() != 0) {
        XELOGI("XnaRuntime: the user declined the XNA runtime install");
        install_pending.store(false);
        return;
      }
      XELOGI("XnaRuntime: the user accepted the XNA runtime install");
      InstallInBackground();
    });
  };

  auto& app_context = display_window->app_context();
  if (app_context.IsInUIThread()) {
    show_dialog();
  } else {
    app_context.CallInUIThread(show_dialog);
  }
#endif  // XE_PLATFORM_WIN32
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
