/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_DEPENDENCIES_H_
#define XENIA_KERNEL_XNA_XNA_DEPENDENCIES_H_

#include <filesystem>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace xna {

// What a hosted XNA title needs on this machine, and how to put it there.
//
// The list is short by design. The XNA framework assemblies are not installed
// at all - they are fabricated in memory when a title asks for them - so what
// remains is the .NET runtime, the managed host that Nexia's build publishes,
// and MonoGame, which is the only piece a user has to supply.

struct XnaDependency {
  std::string name;
  bool required = true;
  bool present = false;
  // Where it was found, or what is wrong when it was not.
  std::string detail;
};

// The directory every hosted title resolves shared assemblies from.
std::filesystem::path XnaOverlayPath();

std::vector<XnaDependency> CheckXnaDependencies();

// True when nothing required is missing.
bool XnaDependenciesSatisfied();

// Human-readable summary of the above, for a dialog.
std::string DescribeXnaDependencies();

// The same summary, to the log. The dialog can run off the bottom of the
// screen, and the log is the copy that survives being read later.
void LogXnaDependencies();

// Copies MonoGame and its native companions out of `source_dir` (searched
// recursively, so an unpacked NuGet package or an existing MonoGame game
// folder both work) into the overlay. Reports what it did in `out_message`.
bool InstallXnaDependenciesFrom(const std::filesystem::path& source_dir,
                                std::string* out_message);

// The same, from a .zip dependency package: extracted to a temporary directory
// and installed from there, then cleaned up.
bool InstallXnaDependenciesFromArchive(const std::filesystem::path& archive,
                                       std::string* out_message);

bool ExtractZipArchive(const std::filesystem::path& archive,
                       const std::filesystem::path& destination);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_DEPENDENCIES_H_
