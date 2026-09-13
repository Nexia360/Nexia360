/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_launcher.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <algorithm>
#include <mutex>
#include <string>
#include <span>
#include <system_error>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xna/xna_dependencies.h"
#include "xenia/kernel/xna/xna_host.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/xcontent_devices/stfs_container_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace kernel {
namespace xna {

namespace {

// Enough of a PE to answer one question. A managed image is an ordinary PE
// whose 15th data directory (the COM descriptor / CLI header) is present; a
// 360 xex or a native exe has it empty.
constexpr size_t kComDescriptorIndex = 14;

bool ReadEntry(vfs::Entry* entry, size_t offset, void* buffer, size_t length) {
  vfs::File* file = nullptr;
  if (entry->Open(vfs::FileAccess::kGenericRead, &file) != X_STATUS_SUCCESS ||
      !file) {
    return false;
  }
  size_t read = 0;
  const X_STATUS status = file->ReadSync(
      std::span<uint8_t>(static_cast<uint8_t*>(buffer), length), offset, &read);
  file->Destroy();
  return status == X_STATUS_SUCCESS && read == length;
}

template <typename T>
bool ReadValue(vfs::Entry* entry, size_t offset, T* out) {
  return ReadEntry(entry, offset, out, sizeof(T));
}

// PE headers are little-endian even on the 360, because these images were
// built for the desktop CLR's format and never byte-swapped.
bool IsManagedAssembly(vfs::Entry* entry) {
  uint16_t mz = 0;
  if (!ReadValue(entry, 0, &mz) || mz != 0x5A4D) {  // 'MZ'
    return false;
  }
  uint32_t pe_offset = 0;
  if (!ReadValue(entry, 0x3C, &pe_offset)) {
    return false;
  }
  // A believable e_lfanew: inside the file and past the DOS header.
  if (pe_offset < 0x40 || pe_offset + 0x100 > entry->size()) {
    return false;
  }
  uint32_t pe_signature = 0;
  if (!ReadValue(entry, pe_offset, &pe_signature) ||
      pe_signature != 0x00004550) {  // 'PE\0\0'
    return false;
  }

  const size_t optional_header = pe_offset + 0x18;
  uint16_t magic = 0;
  if (!ReadValue(entry, optional_header, &magic)) {
    return false;
  }
  // PE32 puts the directories at 0x60, PE32+ at 0x70 - the extra 16 bytes are
  // the widened stack/heap fields.
  size_t directories;
  if (magic == 0x010B) {
    directories = optional_header + 0x60;
  } else if (magic == 0x020B) {
    directories = optional_header + 0x70;
  } else {
    return false;
  }

  struct {
    uint32_t rva;
    uint32_t size;
  } com_descriptor = {};
  if (!ReadValue(entry, directories + kComDescriptorIndex * 8,
                 &com_descriptor)) {
    return false;
  }
  return com_descriptor.rva != 0 && com_descriptor.size != 0;
}

// Local rather than borrowed: these names are ASCII filenames out of a package
// table, and a full Unicode-aware fold would be answering a bigger question
// than the one asked.
bool HasExtension(const std::string& name, const char* extension) {
  const size_t length = std::strlen(extension);
  if (name.size() <= length) {
    return false;
  }
  const char* tail = name.c_str() + (name.size() - length);
  for (size_t i = 0; i < length; ++i) {
    char a = tail[i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    if (a != extension[i]) {
      return false;
    }
  }
  return true;
}

// Depth-first, but shallow in practice: the managed exe sits one directory down
// from the package root.
//
// The path is accumulated on the way down rather than read back from
// Entry::absolute_path(), which carries the device's mount prefix. ResolvePath
// wants a path RELATIVE to the device - the file system normally strips that
// prefix before calling it - so the path built here is the one that can be
// handed straight back.
vfs::Entry* FindManagedEntry(vfs::Entry* directory, int depth,
                             const std::string& prefix,
                             std::string* out_relative) {
  if (depth > 4) {
    return nullptr;
  }
  for (const auto& child : directory->children()) {
    if (child->attributes() & vfs::kFileAttributeDirectory) {
      continue;
    }
    if (HasExtension(child->name(), ".exe") && IsManagedAssembly(child.get())) {
      *out_relative = prefix + child->name();
      return child.get();
    }
  }
  for (const auto& child : directory->children()) {
    if (!(child->attributes() & vfs::kFileAttributeDirectory)) {
      continue;
    }
    if (auto* found = FindManagedEntry(child.get(), depth + 1,
                                       prefix + child->name() + "\\",
                                       out_relative)) {
      return found;
    }
  }
  return nullptr;
}

bool ExtractTree(vfs::Entry* directory, const std::filesystem::path& out_dir) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    XELOGE("XnaLauncher: cannot create {}: {}", xe::path_to_utf8(out_dir),
           ec.message());
    return false;
  }

  for (const auto& child : directory->children()) {
    const auto child_path = out_dir / xe::to_path(child->name());
    if (child->attributes() & vfs::kFileAttributeDirectory) {
      if (!ExtractTree(child.get(), child_path)) {
        return false;
      }
      continue;
    }

    std::vector<uint8_t> buffer(child->size());
    if (!buffer.empty() &&
        !ReadEntry(child.get(), 0, buffer.data(), buffer.size())) {
      XELOGE("XnaLauncher: short read on {}", child->name());
      return false;
    }
    // A zero-length entry is normal in these packages - several shader .xnb
    // are genuinely empty - so an empty file is written, not skipped.
    auto file = xe::filesystem::OpenFile(child_path, "wb");
    if (!file) {
      XELOGE("XnaLauncher: cannot write {}", xe::path_to_utf8(child_path));
      return false;
    }
    if (!buffer.empty()) {
      fwrite(buffer.data(), 1, buffer.size(), file);
    }
    fclose(file);
  }
  return true;
}

// Every XBLIG package reports the XNA Indie Player rather than a per-game id,
// so a title built here that has no content header reports the same thing and
// nothing is lost.
constexpr uint32_t kXnaIndiePlayerTitleId = 0x584E07D2;

// A PACKAGE OR A PLAIN DIRECTORY.
//
// Shipped titles arrive as an STFS container and that stays the normal case.
// A directory is accepted too, because a title built here has no container to
// arrive in: Xenia can read STFS but cannot write it, so a locally built XNA
// binary would otherwise need a packer before it could be run at all. The two
// are interchangeable from here on - both are a vfs::Device rooted at \XNA,
// and everything below asks the device rather than the file.
std::unique_ptr<vfs::Device> OpenPackage(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    auto device = std::make_unique<vfs::HostPathDevice>("\\XNA", path,
                                                        /*read_only=*/true);
    if (!device->Initialize()) {
      return nullptr;
    }
    return device;
  }

  {
    auto device = std::make_unique<vfs::StfsContainerDevice>("\\XNA", path);
    if (device->Initialize()) {
      return device;
    }
  }

  // POINTED AT THE EXE ITSELF.
  //
  // Opening the game's exe is the obvious gesture for a title that was built
  // rather than downloaded, and it is not an STFS container, so the attempt
  // above fails and there is nothing to say why. The folder it lives in is
  // mounted instead - which is the same thing a container would have provided,
  // since the exe and the assemblies beside it are the whole title.
  auto extension = path.extension().string();
  for (auto& c : extension) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (extension == ".exe") {
    auto parent = path.parent_path();
    if (!parent.empty() && std::filesystem::is_directory(parent, ec)) {
      auto device = std::make_unique<vfs::HostPathDevice>("\\XNA", parent,
                                                          /*read_only=*/true);
      if (!device->Initialize()) {
        return nullptr;
      }
      return device;
    }
  }
  return nullptr;
}

// ResolvePath is public on Device but protected on XContentContainerDevice, so
// it has to be reached through the base - access is checked against the static
// type, and Device is where the interface actually lives.
vfs::Entry* ResolveInPackage(vfs::Device* device,
                             const std::string_view path) {
  return device->ResolvePath(path);
}

// THE PACKAGE STAYS OPEN FOR THE LIFE OF THE TITLE.
//
// Extracting once and running from loose files makes the extraction the source
// of truth, and it goes stale silently - the launcher's "reusing" branch had
// been serving a three day old tree. The container is the authority, so it is
// mounted and every file the title opens is answered from it.
std::mutex mounted_mutex;
std::unique_ptr<vfs::Device> mounted_package;
std::string mounted_root;
std::vector<uint8_t> mounted_icon;

bool SameCharNoCase(char a, char b) {
  const char la = (a >= 'A' && a <= 'Z') ? char(a - 'A' + 'a') : a;
  const char lb = (b >= 'A' && b <= 'Z') ? char(b - 'A' + 'a') : b;
  return la == lb || (la == '/' && lb == '\\') || (la == '\\' && lb == '/');
}

bool StripPrefix(const std::string& prefix, std::string* path) {
  if (prefix.empty() || path->size() <= prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (!SameCharNoCase(prefix[i], (*path)[i])) {
      return false;
    }
  }
  const char next = (*path)[prefix.size()];
  if (next != '\\' && next != '/') {
    return false;
  }
  path->erase(0, prefix.size() + 1);
  return true;
}

void MakePackageRelative(std::string* path) {
  if (path->size() < 2 || (*path)[1] != ':') {
    return;
  }
  std::error_code ec;
  const auto working = std::filesystem::current_path(ec);
  if (!ec && StripPrefix(xe::path_to_utf8(working), path)) {
    return;
  }
  StripPrefix(xe::path_to_utf8(xe::filesystem::GetExecutableFolder()), path);
}

vfs::Entry* ResolveTitleEntryLocked(const std::string& relative) {
  if (!mounted_package) {
    return nullptr;
  }
  // The title asks with whichever separator its own code uses and with no
  // leading slash; the package indexes on backslashes under the game folder.
  std::string tail = relative;
  MakePackageRelative(&tail);
  for (char& c : tail) {
    if (c == '/') {
      c = '\\';
    }
  }
  while (!tail.empty() && tail.front() == '\\') {
    tail.erase(0, 1);
  }
  while (!tail.empty() && tail.back() == '\\') {
    tail.pop_back();
  }
  if (tail.size() >= 2 && tail[1] == ':') {
    return nullptr;
  }
  for (size_t at = 0; at + 2 <= tail.size(); ++at) {
    if (tail[at] == '.' && tail[at + 1] == '.' &&
        (at == 0 || tail[at - 1] == '\\') &&
        (at + 2 == tail.size() || tail[at + 2] == '\\')) {
      return nullptr;
    }
  }
  std::string path = mounted_root;
  if (!path.empty() && !tail.empty()) {
    path.push_back('\\');
  }
  path += tail;
  return ResolveInPackage(mounted_package.get(), path);
}

}  // namespace

bool XnaTitleDirectoryExists(const std::string& relative) {
  std::lock_guard<std::mutex> lock(mounted_mutex);
  auto* entry = ResolveTitleEntryLocked(relative);
  return entry && (entry->attributes() & vfs::kFileAttributeDirectory) != 0;
}

bool XnaIsTitlePath(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  if (path.size() >= 2 && path[1] == ':') {
    auto* state = kernel_state();
    auto* emulator = state ? state->emulator() : nullptr;
    if (emulator) {
      std::string outside = path;
      if (StripPrefix(xe::path_to_utf8(emulator->content_root()), &outside)) {
        const size_t end = outside.find_first_of("\\/");
        const std::string owner = outside.substr(0, end);
        bool is_xuid = owner.size() == 16;
        for (char c : owner) {
          is_xuid = is_xuid && std::isxdigit(static_cast<unsigned char>(c));
        }
        if (is_xuid) {
          return false;
        }
      }
    }
  }
  std::lock_guard<std::mutex> lock(mounted_mutex);
  if (!mounted_package) {
    return false;
  }
  std::string tail = path;
  MakePackageRelative(&tail);
  if (tail.size() >= 2 && tail[1] == ':') {
    return false;
  }
  if (tail.size() >= 2 && (tail[0] == '\\' || tail[0] == '/') &&
      (tail[1] == '\\' || tail[1] == '/')) {
    return false;
  }
  return true;
}

std::string XnaResolveTitlePath(const std::string& path) {
  std::lock_guard<std::mutex> lock(mounted_mutex);
  auto* entry = ResolveTitleEntryLocked(path);
  return entry ? entry->path() : std::string();
}

bool XnaStatTitlePath(const std::string& path, uint64_t* size,
                      bool* is_directory) {
  std::lock_guard<std::mutex> lock(mounted_mutex);
  auto* entry = ResolveTitleEntryLocked(path);
  if (!entry) {
    return false;
  }
  const bool directory =
      (entry->attributes() & vfs::kFileAttributeDirectory) != 0;
  if (size) {
    *size = directory ? 0 : entry->size();
  }
  if (is_directory) {
    *is_directory = directory;
  }
  return true;
}

bool XnaListTitleDirectory(const std::string& relative, bool directories,
                           std::vector<std::string>* names) {
  std::lock_guard<std::mutex> lock(mounted_mutex);
  auto* entry = ResolveTitleEntryLocked(relative);
  if (!names || !entry ||
      !(entry->attributes() & vfs::kFileAttributeDirectory)) {
    return false;
  }
  names->clear();
  for (const auto& child : entry->children()) {
    const bool is_directory =
        (child->attributes() & vfs::kFileAttributeDirectory) != 0;
    if (is_directory == directories) {
      names->push_back(child->name());
    }
  }
  return true;
}

bool XnaReadTitleFile(const std::string& relative, std::vector<uint8_t>* out) {
  std::lock_guard<std::mutex> lock(mounted_mutex);
  if (!mounted_package || !out) {
    return false;
  }
  auto* entry = ResolveTitleEntryLocked(relative);
  if (!entry || entry->attributes() & vfs::kFileAttributeDirectory) {
    return false;
  }
  vfs::File* file = nullptr;
  if (entry->Open(vfs::FileAccess::kGenericRead, &file) != X_STATUS_SUCCESS ||
      !file) {
    return false;
  }
  out->resize(static_cast<size_t>(entry->size()));
  size_t read = 0;
  const bool ok =
      out->empty() ||
      file->ReadSync(std::span<uint8_t>(out->data(), out->size()), 0, &read) ==
          X_STATUS_SUCCESS;
  file->Destroy();
  if (!ok) {
    return false;
  }
  out->resize(read);
  return true;
}

std::filesystem::path XnaPackageCachePath(const std::filesystem::path& package,
                                          const XnaPackageInfo& info) {
  auto* state = kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  const auto root = emulator ? emulator->storage_root()
                             : std::filesystem::current_path();
  // The package's own file name is the only thing that differs between two
  // XNA titles - see the header. Deliberately not "xna": that directory is
  // where the managed host is published, and unpacked titles have no business
  // sharing it with something a build step overwrites.
  auto name = package.stem();
  if (name.empty()) {
    name = xe::to_path("unknown");
  }
  return root / "xna_titles" / name;
}

bool IsXnaPackage(const std::filesystem::path& path, XnaPackageInfo* out_info) {
  auto device = OpenPackage(path);
  if (!device) {
    return false;
  }
  auto* root = ResolveInPackage(device.get(), "");
  if (!root) {
    return false;
  }
  std::string relative;
  auto* entry = FindManagedEntry(root, 0, "", &relative);
  if (!entry) {
    return false;
  }

  if (out_info) {
    out_info->entry_path = relative;
    // The managed exe's own folder is the game's title id, and is what the
    // whole title unpacks into.
    const size_t slash = relative.find_last_of("\\/");
    out_info->root_directory =
        slash == std::string::npos ? std::string() : relative.substr(0, slash);
    out_info->display_name = entry->name();

    // The XNA Indie Player's id, and every XBLIG package carries it. There is
    // no per-game title id to be had: the folder inside is the Player's game
    // folder and is identical too. Reporting the Player is at least true.
    //
    // Which is why a directory can be answered with the constant. There is no
    // content header to read, and no information lost by not reading one -
    // every XNA title reports this same id anyway.
    // The cast is not decoration: title_id is a big endian store, and letting
    // the conditional pick a common type between that and a plain uint32_t is
    // ambiguous.
    auto* container = dynamic_cast<vfs::StfsContainerDevice*>(device.get());
    out_info->title_id =
        container ? static_cast<uint32_t>(container->content_header().title_id)
                  : kXnaIndiePlayerTitleId;
  }
  return true;
}

std::vector<uint8_t> XnaTitleIcon() {
  std::lock_guard<std::mutex> lock(mounted_mutex);
  return mounted_icon;
}

bool LaunchXnaPackage(const std::filesystem::path& path) {
  XnaPackageInfo info;
  if (!IsXnaPackage(path, &info)) {
    XELOGE("XnaLauncher: {} holds no managed XNA title",
           xe::path_to_utf8(path));
    return false;
  }
  XELOGI("XnaLauncher: found {} in {} (title {:08X})", info.display_name,
         info.root_directory, info.title_id);

  // MOUNTED, NOT UNPACKED. The package is the title's filesystem: every
  // assembly and every piece of content is answered from it, so there is no
  // extraction to go stale and no second copy to disagree with it.
  {
    std::lock_guard<std::mutex> lock(mounted_mutex);
    mounted_package = OpenPackage(path);
    if (!mounted_package) {
      XELOGE("XnaLauncher: cannot mount {}", xe::path_to_utf8(path));
      return false;
    }
    mounted_root = info.root_directory;
    mounted_icon.clear();
    auto* container =
        dynamic_cast<vfs::StfsContainerDevice*>(mounted_package.get());
    const auto* header = container ? container->GetContainerHeader() : nullptr;
    if (header) {
      const auto& metadata = header->content_metadata;
      const uint32_t title_size = std::min<uint32_t>(
          metadata.title_thumbnail_size,
          uint32_t(sizeof(metadata.title_thumbnail)));
      const uint32_t package_size = std::min<uint32_t>(
          metadata.thumbnail_size, uint32_t(sizeof(metadata.thumbnail)));
      if (title_size) {
        mounted_icon.assign(metadata.title_thumbnail,
                            metadata.title_thumbnail + title_size);
      } else if (package_size) {
        mounted_icon.assign(metadata.thumbnail,
                            metadata.thumbnail + package_size);
      }
    }
    XELOGI("XnaLauncher: title icon {} byte(s)", mounted_icon.size());
  }
  const std::string game_path =
      info.entry_path.substr(info.root_directory.empty()
                                 ? 0
                                 : info.root_directory.size() + 1);
  XELOGI("XnaLauncher: mounted {}, running {} from inside it",
         xe::path_to_utf8(path), game_path);

  // Say what is actually missing here, where the whole list is known - a
  // hostfxr failure three layers down names one file and explains nothing.
  if (!XnaDependenciesSatisfied()) {
    XELOGE("XnaLauncher: dependencies are not installed:\n{}",
           DescribeXnaDependencies());
    return false;
  }

  const auto bootstrap = XnaOverlayPath() / "Nexia.Xna.Host.dll";

  auto& host = XnaHost::Instance();
  if (!host.Start(bootstrap, xe::to_path(game_path))) {
    XELOGE("XnaLauncher: {}", host.last_error());
    return false;
  }
  return true;
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
