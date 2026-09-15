/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_dependencies.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/xna/xna_host_payload.h"
#include "xenia/kernel/xna/xna_runtime_install.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif  // XE_PLATFORM_WIN32

namespace xe {
namespace kernel {
namespace xna {

namespace {

struct WantedFile {
  const char* name;
  // A second accepted spelling, or nullptr. MonoGame renamed its OpenAL native
  // between versions, and either one satisfies the dependency.
  const char* alternate;
  bool required;
  const char* note;
};

// MonoGame's DesktopGL build needs its native companions beside it; without
// them the managed assembly loads and then fails at the first window.
const WantedFile kWanted[] = {
    {"MonoGame.Framework.dll", nullptr, true,
     "MonoGame, from a NuGet package or an existing MonoGame game folder"},
#if XE_PLATFORM_WIN32
    {"SDL2.dll", nullptr, true,
     "shipped with MonoGame.Framework.DesktopGL, or the "
     "MonoGame.Library.SDL package"},
    {"openal.dll", "soft_oal.dll", false,
     "OpenAL; audio is silent without it. From MonoGame.Library.OpenAL "
     "(openal.dll) or older MonoGame builds (soft_oal.dll)"},
#else
    {"libSDL2-2.0.so.0", nullptr, true,
     "shipped with MonoGame.Framework.DesktopGL"},
    {"libopenal.so.1", nullptr, false, "OpenAL; audio is silent without it"},
#endif  // XE_PLATFORM_WIN32
};

// Native packages carry the same file for several architectures, under
// runtimes/<rid>/native/. Copying the first one found installs whichever the
// directory walk happened to reach - on a machine with win-arm64 and win-x64
// side by side that is the wrong one, and MonoGame fails to load it.
#if XE_PLATFORM_WIN32
#if defined(_M_ARM64) || defined(__aarch64__)
constexpr const char* kHostRid = "win-arm64";
#else
constexpr const char* kHostRid = "win-x64";
#endif
#else
#if defined(__aarch64__)
constexpr const char* kHostRid = "linux-arm64";
#else
constexpr const char* kHostRid = "linux-x64";
#endif
#endif  // XE_PLATFORM_WIN32

// Accepts a file that is not architecture-specific at all, or one whose path
// names this machine's runtime identifier.
bool IsForThisMachine(const std::filesystem::path& path) {
  const std::string text =
      xe::utf8::lower_ascii(xe::path_to_utf8(path));
  if (text.find("runtimes") == std::string::npos) {
    return true;
  }
  return text.find(kHostRid) != std::string::npos;
}

// Published by the build (see src/xenia/app/CMakeLists.txt), not by the user -
// listed so a broken deployment is reported rather than guessed at.
const WantedFile kProvidedByBuild[] = {
    {"Nexia.Xna.Host.dll", nullptr, true,
     "embedded in Nexia and written here when an XNA title launches"},
    {"Nexia.Xna.Host.runtimeconfig.json", nullptr, true,
     "embedded with the host; hostfxr will not start without it"},
    {"Mono.Cecil.dll", nullptr, true,
     "embedded with the host; used to fabricate the XNA facades in memory"},
};

bool DotnetRuntimePresent(std::string* out_detail) {
  std::vector<std::filesystem::path> roots;
  if (const char* env = std::getenv("DOTNET_ROOT")) {
    roots.emplace_back(xe::to_path(env));
  }
#if XE_PLATFORM_WIN32
  if (const char* program_files = std::getenv("ProgramFiles")) {
    roots.emplace_back(xe::to_path(program_files) / "dotnet");
  }
#else
  roots.emplace_back("/usr/share/dotnet");
  roots.emplace_back("/usr/local/share/dotnet");
#endif  // XE_PLATFORM_WIN32

  std::error_code ec;
  for (const auto& root : roots) {
    const auto fxr = root / "host" / "fxr";
    if (std::filesystem::is_directory(fxr, ec)) {
      *out_detail = xe::path_to_utf8(root);
      return true;
    }
  }
  *out_detail = "install the .NET desktop runtime, or set DOTNET_ROOT";
  return false;
}

void AppendCheck(std::vector<XnaDependency>* out, const WantedFile& wanted,
                 const std::filesystem::path& overlay) {
  XnaDependency dependency;
  dependency.name = wanted.name;
  dependency.required = wanted.required;

  std::error_code ec;
  dependency.present =
      std::filesystem::exists(overlay / xe::to_path(wanted.name), ec) ||
      (wanted.alternate &&
       std::filesystem::exists(overlay / xe::to_path(wanted.alternate), ec));
  dependency.detail =
      dependency.present ? xe::path_to_utf8(overlay) : wanted.note;
  out->push_back(std::move(dependency));
}

// Runs a command and waits, with no console window. The updater already
// extracts its downloads with tar and falls back to PowerShell; the same pair
// is used here rather than linking a zip decoder for three files.
bool RunHidden(const std::wstring& command_line) {
#if XE_PLATFORM_WIN32
  std::vector<wchar_t> mutable_command(command_line.begin(),
                                       command_line.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process)) {
    return false;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return exit_code == 0;
#else
  return false;
#endif  // XE_PLATFORM_WIN32
}

bool ExtractArchive(const std::filesystem::path& archive,
                    const std::filesystem::path& destination) {
  // bsdtar ships with Windows 10 1803 and later and reads zip files.
  // Quoted: a user's folder can contain spaces, and tar would see two
  // arguments instead of one.
  std::wstring tar = L"tar -xf \"" + archive.wstring() +
                     L"\" -C \"" + destination.wstring() + L"\"";
  if (RunHidden(tar)) {
    return true;
  }
  XELOGW("XnaDependencies: tar could not read {}, trying PowerShell",
         xe::path_to_utf8(archive));
  std::wstring powershell =
      L"powershell -NoProfile -NonInteractive -Command "
      L"\"Expand-Archive -LiteralPath '" +
      archive.wstring() + L"' -DestinationPath '" + destination.wstring() +
      L"' -Force\"";
  return RunHidden(powershell);
}

// The console's own XNA assemblies, from the XNA Indie Player title update
// (Runtime/v4.0). With these installed a title's rendering goes through Nexia
// instead of MonoGame, which is the only way its 360 content can be read.
// Optional: without them a title still runs, it just cannot draw correctly.
const char* const kConsoleRuntime[] = {
    "MXF.dlx",
    "MXF.Avatar.dlx",
    "MXF.Game.dlx",
    "MXF.GamerServices.dlx",
    "MXF.Graphics.dlx",
    "MXF.Input.Touch.dlx",
    "MXF.Net.dlx",
    "MXF.Storage.dlx",
    "MXF.Video.dlx",
    "MXF.Xact.dlx",
    // The console's Compact Framework BCL, from the same update. Installing it
    // is safe: the managed loader only consults these after the desktop
    // runtime has already failed to supply the name, which for mscorlib and
    // System never happens. What it actually covers is the CF libraries the
    // desktop does not have.
    "mscorlib.dlx",
    "System.dlx",
    "System.Core.dlx",
    "System.xml.dlx",
    "System.xml.linq.dlx",
    "System.sr.dlx",
};

std::filesystem::path ConsoleRuntimePath() {
  return XnaOverlayPath() / "console";
}

}  // namespace

std::filesystem::path XnaOverlayPath() {
  return xe::filesystem::GetExecutableFolder() / "xna";
}

std::vector<XnaDependency> CheckXnaDependencies() {
  std::vector<XnaDependency> results;

  XnaDependency runtime;
  runtime.name = ".NET runtime";
  runtime.required = true;
  runtime.present = DotnetRuntimePresent(&runtime.detail);
  results.push_back(std::move(runtime));

  XnaDependency xna_runtime;
  xna_runtime.name = "Microsoft XNA runtime";
  xna_runtime.required = true;
  std::string missing_runtime;
  xna_runtime.present = XnaRuntimeInstalled(&missing_runtime);
  xna_runtime.detail =
      xna_runtime.present
          ? "XNA Framework 3.1 and 4.0 Refresh, .NET Framework 3.5, .NET 9 "
            "Runtime, DirectX June 2010"
          : "missing " + missing_runtime +
                " - launching an XNA title offers to install it";
  results.push_back(std::move(xna_runtime));

  const auto overlay = XnaOverlayPath();
  for (const auto& wanted : kProvidedByBuild) {
    AppendCheck(&results, wanted, overlay);
  }

  std::error_code ec;
  const bool have_console = std::filesystem::exists(
      ConsoleRuntimePath() / xe::to_path("MXF.Graphics.dlx"), ec);

  XnaDependency console;
  console.name = "Console XNA runtime";
  // The preferred path, and the only one that can read a title's own content.
  console.required = true;
  console.present = have_console;
  console.detail =
      have_console
          ? xe::path_to_utf8(ConsoleRuntimePath())
          : "not embedded in this build - build Nexia with "
            "XE_XNA_CONSOLE_RUNTIME_DIR pointing at the MXF*.dlx and "
            "Compact Framework .dlx files";
  results.push_back(std::move(console));

  // MonoGame is the fallback for when the console runtime is absent: it can
  // start a title but cannot read its content, so it stops being worth
  // reporting as missing once the real thing is installed.
  for (const auto& wanted : kWanted) {
    XnaDependency fallback;
    fallback.name = wanted.name;
    fallback.required = wanted.required && !have_console;
    const auto native_dir = overlay / "runtimes" / kHostRid / "native";
    auto installed = [&](const char* name) {
      return name &&
             (std::filesystem::exists(overlay / xe::to_path(name), ec) ||
              std::filesystem::exists(native_dir / xe::to_path(name), ec));
    };
    fallback.present = installed(wanted.name) || installed(wanted.alternate);
    fallback.detail = fallback.present ? xe::path_to_utf8(overlay)
                      : have_console   ? "not needed - the console runtime is "
                                         "installed and takes precedence"
                                       : wanted.note;
    results.push_back(std::move(fallback));
  }
  return results;
}

bool XnaDependenciesSatisfied() {
  const auto results = CheckXnaDependencies();
  return std::none_of(results.cbegin(), results.cend(),
                      [](const XnaDependency& dependency) {
                        return dependency.required && !dependency.present;
                      });
}

std::string DescribeXnaDependencies() {
  std::string text;
  for (const auto& dependency : CheckXnaDependencies()) {
    text += dependency.present ? "[ok]      " : "[MISSING] ";
    text += dependency.name;
    if (!dependency.present && !dependency.required) {
      text += " (optional)";
    }
    text += "\n          ";
    text += dependency.detail;
    text += "\n";
  }
  text += "\nOverlay: ";
  text += xe::path_to_utf8(XnaOverlayPath());
  text += "\nEmbedded host: ";
  text += kXnaHostPayloadCount ? kXnaHostPayloadVersion
                               : "none (built without dotnet)";
  return text;
}

void LogXnaDependencies() {
  // XELOGE for now so it is visible whatever the log level is set to.
  XELOGE("XNA dependencies:\n{}", DescribeXnaDependencies());
}

namespace {

bool FileMatches(const std::filesystem::path& path, const uint8_t* data,
                 size_t size) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) ||
      std::filesystem::file_size(path, ec) != size || ec) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::vector<uint8_t> existing(size);
  if (size && !in.read(reinterpret_cast<char*>(existing.data()),
                       static_cast<std::streamsize>(size))) {
    return false;
  }
  return !size || std::memcmp(existing.data(), data, size) == 0;
}

bool PayloadBytes(const XnaHostPayloadFile& file, const uint8_t** out_data,
                  size_t* out_size) {
  if (file.data) {
    *out_data = file.data;
    *out_size = file.size;
    return true;
  }
#if XE_PLATFORM_WIN32
  if (file.resource_id) {
    HRSRC resource = FindResourceW(
        nullptr, MAKEINTRESOURCEW(file.resource_id), MAKEINTRESOURCEW(10));
    if (resource) {
      HGLOBAL loaded = LoadResource(nullptr, resource);
      const void* bytes = loaded ? LockResource(loaded) : nullptr;
      const DWORD size = SizeofResource(nullptr, resource);
      if (bytes && size == file.size) {
        *out_data = static_cast<const uint8_t*>(bytes);
        *out_size = size;
        return true;
      }
    }
  }
#endif  // XE_PLATFORM_WIN32
  return false;
}

std::string ReadInstalledHostVersion(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::string version;
  if (!in || !std::getline(in, version)) {
    return "none";
  }
  return version;
}

}  // namespace

bool DeployXnaHostPayload(std::string* out_message) {
  if (!kXnaHostPayloadCount) {
    *out_message =
        "this build embeds no Nexia.Xna.Host; using whatever is installed";
    return true;
  }

  std::error_code ec;
  const auto overlay = XnaOverlayPath();
  std::filesystem::create_directories(overlay, ec);
  if (ec) {
    *out_message =
        "could not create " + xe::path_to_utf8(overlay) + ": " + ec.message();
    return false;
  }

  const auto version_path = overlay / "Nexia.Xna.Host.version";
  const std::string installed = ReadInstalledHostVersion(version_path);

  size_t replaced = 0;
  std::vector<std::string> failed;
  for (size_t i = 0; i < kXnaHostPayloadCount; ++i) {
    const auto& file = kXnaHostPayload[i];
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!PayloadBytes(file, &data, &size)) {
      failed.push_back(std::string(file.name) +
                       " (not embedded in this executable)");
      continue;
    }
    const auto target = overlay / xe::to_path(file.name);
    if (FileMatches(target, data, size)) {
      continue;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    auto staging = target;
    staging += ".new";
    {
      std::ofstream out(staging, std::ios::binary | std::ios::trunc);
      if (out) {
        out.write(reinterpret_cast<const char*>(data),
                  static_cast<std::streamsize>(size));
      }
      if (!out) {
        failed.push_back(std::string(file.name) + " (could not write " +
                         xe::path_to_utf8(staging) + ")");
        continue;
      }
    }
    std::filesystem::rename(staging, target, ec);
    if (ec) {
      failed.push_back(std::string(file.name) + " (" + ec.message() + ")");
      std::filesystem::remove(staging, ec);
      continue;
    }
    XELOGI("XnaHost payload: wrote {} ({} bytes)", file.name, size);
    ++replaced;
  }

  if (!failed.empty()) {
    *out_message = fmt::format(
        "could not replace Nexia.Xna.Host {} with {} in {}:", installed,
        kXnaHostPayloadVersion, xe::path_to_utf8(overlay));
    for (const auto& entry : failed) {
      *out_message += "\n  " + entry;
    }
    return false;
  }

  if (replaced || installed != kXnaHostPayloadVersion) {
    std::ofstream out(version_path, std::ios::trunc);
    out << kXnaHostPayloadVersion << "\n";
  }
  *out_message =
      replaced ? fmt::format("Nexia.Xna.Host {} -> {} ({} file(s) replaced)",
                             installed, kXnaHostPayloadVersion, replaced)
               : fmt::format("Nexia.Xna.Host {} is current",
                             kXnaHostPayloadVersion);
  return true;
}

bool InstallXnaDependenciesFrom(const std::filesystem::path& source_dir,
                                std::string* out_message) {
  std::error_code ec;
  const auto overlay = XnaOverlayPath();
  std::filesystem::create_directories(overlay, ec);
  if (ec) {
    *out_message = "Could not create " + xe::path_to_utf8(overlay) + ": " +
                   ec.message();
    return false;
  }

  // Recursive, because a NuGet package keeps the managed assembly under lib/
  // and the natives under runtimes/<rid>/native/ - a user should be able to
  // point at the package root and have it work.
  std::vector<std::string> installed;
  std::vector<std::string> failed;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(source_dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string name = xe::path_to_utf8(entry.path().filename());

    // The console runtime goes to its own subdirectory - those assemblies are
    // loaded by name, and mixing them with the host's would let the runtime
    // pick one up by accident.
    const bool is_console =
        std::any_of(std::begin(kConsoleRuntime), std::end(kConsoleRuntime),
                    [&name](const char* file) { return name == file; });
    if (is_console) {
      std::error_code console_ec;
      std::filesystem::create_directories(ConsoleRuntimePath(), console_ec);
      std::filesystem::copy_file(
          entry.path(), ConsoleRuntimePath() / entry.path().filename(),
          std::filesystem::copy_options::overwrite_existing, console_ec);
      if (console_ec) {
        failed.push_back(name);
      } else {
        XELOGI("XnaDependencies: installed console runtime {}", name);
        installed.push_back(name);
      }
      continue;
    }

    const bool wanted =
        std::any_of(std::begin(kWanted), std::end(kWanted),
                    [&name](const WantedFile& file) {
                      return name == file.name ||
                             (file.alternate && name == file.alternate);
                    });
    if (!wanted || !IsForThisMachine(entry.path())) {
      continue;
    }
    // First match wins: a package can carry the same native for several
    // architectures, and the later ones are not better.
    if (std::find(installed.cbegin(), installed.cend(), name) !=
        installed.cend()) {
      continue;
    }

    std::error_code copy_ec;
    std::filesystem::copy_file(entry.path(), overlay / entry.path().filename(),
                               std::filesystem::copy_options::overwrite_existing,
                               copy_ec);
    if (copy_ec) {
      XELOGE("XnaDependencies: could not copy {}: {}", name,
             copy_ec.message());
      failed.push_back(name);
    } else {
      XELOGI("XnaDependencies: installed {}", name);
      installed.push_back(name);
    }
  }

  if (installed.empty() && failed.empty()) {
    *out_message =
        "Nothing to install was found there.\n\nLook for a folder containing "
        "MonoGame.Framework.dll - an unpacked MonoGame.Framework.DesktopGL "
        "NuGet package, or another MonoGame game.";
    return false;
  }

  *out_message = "Installed:\n";
  for (const auto& name : installed) {
    *out_message += "  " + name + "\n";
  }
  if (!failed.empty()) {
    *out_message += "\nCould not copy:\n";
    for (const auto& name : failed) {
      *out_message += "  " + name + "\n";
    }
  }
  *out_message += "\n" + DescribeXnaDependencies();
  return failed.empty();
}

bool ExtractZipArchive(const std::filesystem::path& archive,
                       const std::filesystem::path& destination) {
  return ExtractArchive(archive, destination);
}

bool InstallXnaDependenciesFromArchive(const std::filesystem::path& archive,
                                       std::string* out_message) {
  std::error_code ec;
  // Named after the archive so a failed run leaves something identifiable
  // behind rather than an anonymous temp folder.
  const auto staging = std::filesystem::temp_directory_path(ec) /
                       ("nexia-xna-" + xe::path_to_utf8(archive.stem()));
  std::filesystem::remove_all(staging, ec);
  std::filesystem::create_directories(staging, ec);
  if (ec) {
    *out_message = "Could not create a temporary folder: " + ec.message();
    return false;
  }

  if (!ExtractArchive(archive, staging)) {
    *out_message = "Could not extract " + xe::path_to_utf8(archive) +
                   ".\n\nIt must be a .zip file.";
    std::filesystem::remove_all(staging, ec);
    return false;
  }

  const bool installed = InstallXnaDependenciesFrom(staging, out_message);
  std::filesystem::remove_all(staging, ec);
  return installed;
}

namespace {

struct PackageItem {
  std::vector<std::string> names;
  std::string destination_dir;
  bool required;
  bool native;
  std::filesystem::path found;
  std::string found_name;
};

bool InPreparedFolder(const std::filesystem::path& path) {
  for (const auto& part : path.parent_path()) {
    if (xe::utf8::lower_ascii(xe::path_to_utf8(part)) == "prepared") {
      return true;
    }
  }
  return false;
}

bool CreateZipArchive(const std::filesystem::path& staging,
                      const std::vector<std::string>& top_level,
                      const std::filesystem::path& zip_path) {
  std::wstring tar = L"tar -a -c -f \"" + zip_path.wstring() + L"\" -C \"" +
                     staging.wstring() + L"\"";
  for (const auto& item : top_level) {
    tar += L" \"" + xe::to_path(item).wstring() + L"\"";
  }
  if (RunHidden(tar)) {
    return true;
  }
  XELOGW("XnaDependencies: tar could not write {}, trying PowerShell",
         xe::path_to_utf8(zip_path));
  std::wstring powershell =
      L"powershell -NoProfile -NonInteractive -Command "
      L"\"Compress-Archive -Path '" +
      (staging / "*").wstring() + L"' -DestinationPath '" +
      zip_path.wstring() + L"' -Force\"";
  return RunHidden(powershell);
}

}  // namespace

bool BuildXnaDependencyZip(const std::filesystem::path& source_dir,
                           const std::filesystem::path& zip_path,
                           std::string* out_report) {
  std::vector<PackageItem> items;
  for (const char* name : kConsoleRuntime) {
    items.push_back({{name}, "console", true, false, {}, {}});
  }
  for (const auto& wanted : kWanted) {
    PackageItem item;
    item.names.push_back(wanted.name);
    if (wanted.alternate) {
      item.names.push_back(wanted.alternate);
    }
    item.native = std::string(wanted.name) != "MonoGame.Framework.dll";
    item.destination_dir =
        item.native ? std::string("runtimes/") + kHostRid + "/native" : "";
    item.required = item.native && wanted.required;
    items.push_back(std::move(item));
  }

  std::error_code ec;
  size_t scanned = 0;
  auto it = std::filesystem::recursive_directory_iterator(
      source_dir, std::filesystem::directory_options::skip_permission_denied,
      ec);
  for (; !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) {
      continue;
    }
    ++scanned;
    const auto& path = it->path();
    const std::string name =
        xe::utf8::lower_ascii(xe::path_to_utf8(path.filename()));
    for (auto& item : items) {
      if (!item.found.empty()) {
        continue;
      }
      bool match = false;
      for (const auto& wanted : item.names) {
        if (xe::utf8::lower_ascii(wanted) == name) {
          match = true;
          item.found_name = wanted;
          break;
        }
      }
      if (!match || InPreparedFolder(path) ||
          (item.native && !IsForThisMachine(path))) {
        continue;
      }
      item.found = path;
    }
  }

  std::string found_text;
  std::string missing_text;
  size_t found_count = 0;
  bool required_missing = false;
  for (const auto& item : items) {
    const std::string destination =
        (item.destination_dir.empty() ? "" : item.destination_dir + "/") +
        (item.found.empty() ? item.names[0] : item.found_name);
    if (item.found.empty()) {
      missing_text += "  " + destination +
                      (item.required ? " (required)" : " (optional)") + "\n";
      required_missing = required_missing || item.required;
    } else {
      found_text += "  " + destination + "  <-  " +
                    xe::path_to_utf8(item.found) + "\n";
      ++found_count;
    }
  }

  *out_report = "Scanned " + std::to_string(scanned) + " file(s) under " +
                xe::path_to_utf8(source_dir) + "\n";
  if (!found_text.empty()) {
    *out_report += "\nFound:\n" + found_text;
  }
  if (!missing_text.empty()) {
    *out_report += "\nNot found:\n" + missing_text;
  }
  if (!found_count) {
    *out_report += "\nNothing to package - no zip was written.";
    return false;
  }

  const auto staging =
      std::filesystem::temp_directory_path(ec) / "nexia-xna-dependency-zip";
  std::filesystem::remove_all(staging, ec);
  std::filesystem::create_directories(staging, ec);
  if (ec) {
    *out_report += "\nCould not create " + xe::path_to_utf8(staging) + ": " +
                   ec.message();
    return false;
  }

  std::vector<std::string> top_level;
  for (const auto& item : items) {
    if (item.found.empty()) {
      continue;
    }
    const auto directory = item.destination_dir.empty()
                               ? staging
                               : staging / xe::to_path(item.destination_dir);
    std::filesystem::create_directories(directory, ec);
    std::filesystem::copy_file(
        item.found, directory / xe::to_path(item.found_name),
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      *out_report += "\nCould not copy " + xe::path_to_utf8(item.found) +
                     ": " + ec.message();
      std::filesystem::remove_all(staging, ec);
      return false;
    }
    const std::string root =
        item.destination_dir.empty()
            ? item.found_name
            : item.destination_dir.substr(0, item.destination_dir.find('/'));
    if (std::find(top_level.begin(), top_level.end(), root) ==
        top_level.end()) {
      top_level.push_back(root);
    }
  }

  std::filesystem::remove(zip_path, ec);
  const bool written = CreateZipArchive(staging, top_level, zip_path);
  std::filesystem::remove_all(staging, ec);
  if (!written) {
    *out_report += "\nCould not write " + xe::path_to_utf8(zip_path);
    return false;
  }
  *out_report += "\nWrote " + xe::path_to_utf8(zip_path) + " with " +
                 std::to_string(found_count) + " file(s).";
  if (required_missing) {
    *out_report += " Some required files were not found.";
  }
  return true;
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
