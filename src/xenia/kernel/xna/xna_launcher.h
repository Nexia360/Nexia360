/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_LAUNCHER_H_
#define XENIA_KERNEL_XNA_XNA_LAUNCHER_H_

#include <filesystem>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace xna {

// Recognises an XNA title inside an STFS/LIVE package and runs it on the host
// CLR instead of the PPC path.
//
// An XBLIG package looks nothing like a normal title: there is no default.xex
// to launch, only a folder named after the GAME's title id (the package itself
// is stamped with the XNA Indie Player's) holding a managed .exe, its
// dependency assemblies, and Content\. The console booted that with its own
// launcher xex; here the host CLR runs it directly.
//
// Detection is by evidence, not by name: a PE whose COM descriptor directory is
// present is managed, and nothing else in a 360 package is.

struct XnaPackageInfo {
  // Where the managed entry assembly sits INSIDE the package, e.g.
  // "584E07D1\\BlockWorld.exe".
  std::string entry_path;
  // The folder holding it, which is also the game's title id.
  std::string root_directory;
  std::string display_name;
  uint32_t title_id = 0;
};

// True if `path` is a package holding a managed XNA title. Cheap enough to call
// on any file the user picks - it opens the container and walks its entries,
// but reads only PE headers.
bool IsXnaPackage(const std::filesystem::path& path, XnaPackageInfo* out_info);

// Unpacks the package into the emulator's storage root and starts it on the
// host CLR. Returns false and logs why on any failure; `XnaHost::last_error()`
// carries the message worth showing a user.
bool LaunchXnaPackage(const std::filesystem::path& path);

// Where a package's files are unpacked to. Keyed on the PACKAGE, not on
// anything inside it: every XBLIG package holds the same folder name
// (584E07D1, the XNA Indie Player's game folder) and carries the same title id
// (584E07D2, the Player itself), so nothing in the content identifies a game.
// Keying on the content would unpack every XNA title on top of the last one.
std::filesystem::path XnaPackageCachePath(const std::filesystem::path& package,
                                          const XnaPackageInfo& info);

// One file out of the mounted package, by its path relative to the game folder.
// False when the package is not mounted or holds no such file.
bool XnaReadTitleFile(const std::string& relative, std::vector<uint8_t>* out);

bool XnaTitleDirectoryExists(const std::string& relative);

bool XnaIsTitlePath(const std::string& path);

std::string XnaResolveTitlePath(const std::string& path);

bool XnaStatTitlePath(const std::string& path, uint64_t* size,
                      bool* is_directory);

bool XnaListTitleDirectory(const std::string& relative, bool directories,
                           std::vector<std::string>* names);

std::vector<uint8_t> XnaTitleIcon();

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_LAUNCHER_H_
