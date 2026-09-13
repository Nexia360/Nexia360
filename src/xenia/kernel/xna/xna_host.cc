/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_host.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/kernel/xna/xna_exports.h"
#include "xenia/kernel/xna/xna_os.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#else
#include <dlfcn.h>
#endif  // XE_PLATFORM_WIN32

namespace xe {
namespace kernel {
namespace xna {

namespace {

// hostfxr's own header is part of the .NET SDK, which Nexia does not build
// against - these are its published signatures, declared here so the library
// can be resolved at run time instead.
#if XE_PLATFORM_WIN32
using host_char_t = wchar_t;
#else
using host_char_t = char;
#endif  // XE_PLATFORM_WIN32

using hostfxr_handle = void*;

using hostfxr_initialize_for_runtime_config_fn =
    int32_t(*)(const host_char_t* runtime_config_path,
                       const void* parameters, hostfxr_handle* out_context);
using hostfxr_get_runtime_delegate_fn = int32_t(*)(
    hostfxr_handle context, int32_t delegate_type, void** out_delegate);
using hostfxr_close_fn = int32_t(*)(hostfxr_handle context);

// hdt_load_assembly_and_get_function_pointer.
constexpr int32_t kLoadAssemblyAndGetFunctionPointer = 5;

using load_assembly_and_get_function_pointer_fn =
    int32_t(*)(const host_char_t* assembly_path,
                       const host_char_t* type_name,
                       const host_char_t* method_name,
                       const host_char_t* delegate_type_name, void* reserved,
                       void** out_delegate);

// Passing this as the delegate type name means "the method is
// [UnmanagedCallersOnly]; use its own signature", which is what lets the
// bootstrap take a raw table pointer with no marshalling at all.
const host_char_t* const kUnmanagedCallersOnly =
    reinterpret_cast<const host_char_t*>(-1);

// The managed bootstrap's entry point, as hostfxr wants it named.
using xna_bootstrap_start_fn = int32_t(*)(const XnaOsTable* os_table,
                                                  const char* game_path_utf8);

std::basic_string<host_char_t> ToHostString(
    const std::filesystem::path& path) {
#if XE_PLATFORM_WIN32
  return path.wstring();
#else
  return path.string();
#endif  // XE_PLATFORM_WIN32
}

// For the ASCII identifiers hostfxr wants (type and method names), where going
// through std::filesystem::path would drag in a locale conversion for no
// reason.
std::basic_string<host_char_t> ToHostString(const char* ascii) {
  std::basic_string<host_char_t> out;
  for (const char* p = ascii; *p; ++p) {
    out.push_back(static_cast<host_char_t>(*p));
  }
  return out;
}

void* OpenLibrary(const std::filesystem::path& path) {
#if XE_PLATFORM_WIN32
  return reinterpret_cast<void*>(LoadLibraryW(path.wstring().c_str()));
#else
  return dlopen(path.string().c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif  // XE_PLATFORM_WIN32
}

template <typename T>
T GetExport(void* module, const char* name) {
#if XE_PLATFORM_WIN32
  return reinterpret_cast<T>(
      GetProcAddress(reinterpret_cast<HMODULE>(module), name));
#else
  return reinterpret_cast<T>(dlsym(module, name));
#endif  // XE_PLATFORM_WIN32
}

#if XE_PLATFORM_WIN32
constexpr const char* kHostFxrName = "hostfxr.dll";
#elif XE_PLATFORM_MAC
constexpr const char* kHostFxrName = "libhostfxr.dylib";
#else
constexpr const char* kHostFxrName = "libhostfxr.so";
#endif  // XE_PLATFORM_WIN32

std::vector<std::filesystem::path> DotnetRoots() {
  std::vector<std::filesystem::path> roots;
  // An explicit DOTNET_ROOT wins - it is how a user points at a private or
  // side-by-side install.
  if (const char* env = std::getenv("DOTNET_ROOT")) {
    roots.emplace_back(xe::to_path(env));
  }
#if XE_PLATFORM_WIN32
  if (const char* program_files = std::getenv("ProgramFiles")) {
    roots.emplace_back(xe::to_path(program_files) / "dotnet");
  }
  roots.emplace_back("C:/Program Files/dotnet");
#else
  roots.emplace_back("/usr/share/dotnet");
  roots.emplace_back("/usr/local/share/dotnet");
#endif  // XE_PLATFORM_WIN32
  return roots;
}

// host/fxr holds one directory per installed runtime version. Any of them can
// start a newer app, so the highest wins - and versions sort by string here
// only because they are all "major.minor.patch" with the same shape, which is
// why this compares component-wise instead.
std::filesystem::path FindHostFxr() {
  std::filesystem::path best;
  std::vector<int> best_parts;

  for (const auto& root : DotnetRoots()) {
    std::error_code ec;
    const auto fxr_root = root / "host" / "fxr";
    if (!std::filesystem::is_directory(fxr_root, ec)) {
      continue;
    }
    for (const auto& entry : std::filesystem::directory_iterator(fxr_root, ec)) {
      if (!entry.is_directory()) {
        continue;
      }
      const auto candidate = entry.path() / kHostFxrName;
      if (!std::filesystem::exists(candidate, ec)) {
        continue;
      }
      std::vector<int> parts;
      int value = 0;
      bool have_digit = false;
      for (char c : xe::path_to_utf8(entry.path().filename())) {
        if (c >= '0' && c <= '9') {
          value = value * 10 + (c - '0');
          have_digit = true;
        } else {
          if (have_digit) {
            parts.push_back(value);
          }
          value = 0;
          have_digit = false;
        }
      }
      if (have_digit) {
        parts.push_back(value);
      }
      if (best.empty() || parts > best_parts) {
        best = candidate;
        best_parts = parts;
      }
    }
  }
  return best;
}

}  // namespace

XnaHost& XnaHost::Instance() {
  static XnaHost instance;
  return instance;
}

XnaHost::~XnaHost() { Stop(); }

void XnaHost::Fail(std::string message) {
  XELOGE("XnaHost: {}", message);
  last_error_ = std::move(message);
}

bool XnaHost::LoadHostFxr() {
  if (hostfxr_module_) {
    return true;
  }
  const auto path = FindHostFxr();
  if (path.empty()) {
    Fail("no .NET runtime found - install the .NET desktop runtime, or set "
         "DOTNET_ROOT");
    return false;
  }
  hostfxr_module_ = OpenLibrary(path);
  if (!hostfxr_module_) {
    Fail("found " + xe::path_to_utf8(path) + " but could not load it");
    return false;
  }
  XELOGI("XnaHost: using {}", xe::path_to_utf8(path));
  return true;
}

bool XnaHost::Start(const std::filesystem::path& bootstrap_assembly,
                    const std::filesystem::path& game_assembly) {
  if (running_) {
    Fail("a title is already hosted");
    return false;
  }
  last_error_.clear();

  std::error_code ec;
  if (!std::filesystem::exists(bootstrap_assembly, ec)) {
    Fail("missing bootstrap assembly " + xe::path_to_utf8(bootstrap_assembly));
    return false;
  }
  auto runtime_config = bootstrap_assembly;
  runtime_config.replace_extension();
  runtime_config += ".runtimeconfig.json";
  if (!std::filesystem::exists(runtime_config, ec)) {
    Fail("missing " + xe::path_to_utf8(runtime_config));
    return false;
  }
  if (!LoadHostFxr()) {
    return false;
  }

  auto initialize = GetExport<hostfxr_initialize_for_runtime_config_fn>(
      hostfxr_module_, "hostfxr_initialize_for_runtime_config");
  auto get_delegate = GetExport<hostfxr_get_runtime_delegate_fn>(
      hostfxr_module_, "hostfxr_get_runtime_delegate");
  if (!initialize || !get_delegate) {
    Fail("hostfxr is missing its own exports");
    return false;
  }

  const auto config_string = ToHostString(runtime_config);
  int32_t status = initialize(config_string.c_str(), nullptr, &host_context_);
  // Positive statuses are successes that carry information (Success_*), and
  // treating them as failures would reject a perfectly good runtime.
  if (status < 0 || !host_context_) {
    Fail(fmt::format("hostfxr_initialize_for_runtime_config failed ({:#010X})",
                     static_cast<uint32_t>(status)));
    Stop();
    return false;
  }

  void* raw_loader = nullptr;
  status = get_delegate(host_context_, kLoadAssemblyAndGetFunctionPointer,
                        &raw_loader);
  if (status < 0 || !raw_loader) {
    Fail(fmt::format("hostfxr_get_runtime_delegate failed ({:#010X})",
                     static_cast<uint32_t>(status)));
    Stop();
    return false;
  }

  auto load_assembly =
      reinterpret_cast<load_assembly_and_get_function_pointer_fn>(raw_loader);

  const auto assembly_string = ToHostString(bootstrap_assembly);
  const auto type_string = ToHostString("Nexia.Xna.Bootstrap, Nexia.Xna.Host");
  const auto method_string = ToHostString("Start");

  void* raw_start = nullptr;
  status = load_assembly(assembly_string.c_str(), type_string.c_str(),
                         method_string.c_str(), kUnmanagedCallersOnly, nullptr,
                         &raw_start);
  if (status < 0 || !raw_start) {
    Fail(fmt::format("could not bind Nexia.Xna.Bootstrap.Start ({:#010X})",
                     static_cast<uint32_t>(status)));
    Stop();
    return false;
  }

  const std::string game_path = xe::path_to_utf8(game_assembly);
  // Recorded before the title runs: STORAGE_GetStorageLocation reports this,
  // and XNA takes the directory of it to resolve every relative content path.
  SetXnaTitlePath(game_path);
  auto start = reinterpret_cast<xna_bootstrap_start_fn>(raw_start);
  const int32_t result = start(GetXnaOsTable(), game_path.c_str());
  if (result != 0) {
    Fail(fmt::format("the managed bootstrap refused to start ({})", result));
    Stop();
    return false;
  }

  running_ = true;
  XELOGI("XnaHost: hosting {}", game_path);
  return true;
}

void XnaHost::Stop() {
  if (host_context_) {
    if (auto close = GetExport<hostfxr_close_fn>(hostfxr_module_,
                                                 "hostfxr_close")) {
      close(host_context_);
    }
    host_context_ = nullptr;
  }
  // The runtime itself is deliberately left loaded: CoreCLR cannot be unloaded
  // from a process, and closing the library out from under it would crash on
  // the next managed callback.
  running_ = false;
}

bool XnaHostWritable(const void* begin, const void* end) {
#if XE_PLATFORM_WIN32
  constexpr DWORD kWritable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE;
  auto* at = static_cast<const uint8_t*>(begin);
  auto* stop = static_cast<const uint8_t*>(end);
  while (at < stop) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(at, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & PAGE_GUARD) || !(info.Protect & kWritable)) {
      return false;
    }
    at = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return true;
#else
  return false;
#endif  // XE_PLATFORM_WIN32
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
