/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_ASSETS_H_
#define XENIA_KERNEL_XAM_XUI_ASSETS_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

struct AssetInstallReport {
  bool ok = false;
  uint32_t build = 0;
  size_t package_count = 0;
  size_t font_count = 0;
  std::vector<std::string> missing;
  std::string text;
};

// Where an installed set of dashboard UI assets lives.
std::filesystem::path DefaultAssetDirectory();

// The name XamGetLanguageTypeface hands a system title for the face inside
// this .xtt, or empty if it is none of the three it asks for.
//
// MATCHING ON THE FILE NAME CANNOT WORK. An extracted flash font is named from
// its own sfnt table, and the two Latin faces call themselves "Xbox TC" and
// "Xbox JK" in there - nothing like xenonclatin.xtt or xenonjklatin.xtt.
std::string DashboardTypefaceFor(const std::filesystem::path& font);

// True when the file looks like an Xbox 360 NAND image - raw 528-byte pages or
// an already-stripped logical image - rather than an XContent package.
bool IsFlashImage(const std::filesystem::path& path);

AssetInstallReport InstallFromFlashImage(const std::filesystem::path& image,
                                         const std::filesystem::path& out_dir);

// The Avatar Editor's UI is not in flash - it ships in the system update the
// XNA setup already downloads and unpacks. Pulls the XUI packages out of the
// module XEXs found under `directory` (they are unencrypted, so the same
// flatten works) into out_dir.
AssetInstallReport InstallFromSystemUpdate(
    const std::filesystem::path& directory,
    const std::filesystem::path& out_dir);

// The packages InstallFromSystemUpdate writes, so a caller can tell whether
// the step still needs to run.
const std::vector<std::string>& SystemUpdatePackageNames();

// A tree produced by the packaging tool: holds nexia-ui.json plus modules/ and
// fonts/.
bool IsAssetArchiveRoot(const std::filesystem::path& directory);

AssetInstallReport InstallFromArchiveRoot(
    const std::filesystem::path& directory,
    const std::filesystem::path& out_dir);

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
