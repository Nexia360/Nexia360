/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_TITLE_UPDATE_MANAGER_H_
#define XENIA_KERNEL_UTIL_TITLE_UPDATE_MANAGER_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace util {

struct TitleUpdateEntry {
  std::string id;       // library folder name
  std::string name;     // editable display name (defaults to version)
  std::string version;  // patch version string read from the .xexp
  uint32_t version_value = 0;
  uint64_t size_bytes = 0;
  std::string source_file;  // original installed package filename
};

// Manages installed title updates as a per-title library under
// <Device>/Library/<title_id>/, with a JSON manifest, and links the active one
// into the content tree (<content>/0/<title_id>/000B0000/) where the loader
// finds it. Host-side; owned by the Emulator.
class TitleUpdateManager {
 public:
  explicit TitleUpdateManager(const std::filesystem::path& content_root);

  std::vector<TitleUpdateEntry> List(uint32_t title_id) const;
  std::string GetActive(uint32_t title_id) const;

  // Host path of the active update's folder in the library, or empty if none.
  // The loader reads the patch directly from here - no content-tree link/copy.
  std::filesystem::path GetActiveLibraryPath(uint32_t title_id) const;

  bool SetActive(uint32_t title_id, const std::string& id);
  bool Rename(uint32_t title_id, const std::string& id,
              const std::string& new_name);
  bool Remove(uint32_t title_id, const std::string& id);

  // Moves a freshly installed title update (currently extracted under the
  // content tree as source_dirname) into the library, names it from the .xexp
  // version, records it in the manifest, and (when auto_activate) links it
  // active. Returns the library id, or empty on failure.
  std::string ImportFromContent(uint32_t title_id,
                                const std::string& source_dirname,
                                bool auto_activate);

  // One-time migration: pull any real (non-linked) update folders already in
  // the content tree into the library and activate one if none is active.
  void MigrateLegacy(uint32_t title_id);

  // Scans the whole content tree and migrates every title's legacy updates.
  // Idempotent; safe to run at startup.
  void MigrateAllLegacy();

 private:
  std::filesystem::path device_root() const;
  std::filesystem::path library_root(uint32_t title_id) const;
  std::filesystem::path manifest_path(uint32_t title_id) const;
  std::filesystem::path content_update_dir(uint32_t title_id) const;
  std::filesystem::path content_header_dir(uint32_t title_id) const;

  bool LoadManifest(uint32_t title_id, std::string& active,
                    std::vector<TitleUpdateEntry>& entries) const;
  bool SaveManifest(uint32_t title_id, const std::string& active,
                    const std::vector<TitleUpdateEntry>& entries) const;

  void DeactivateLink(uint32_t title_id, const std::string& id);
  std::string MakeUniqueId(uint32_t title_id, const std::string& preferred,
                           const std::vector<TitleUpdateEntry>& entries) const;

  static bool ReadXexpVersion(const std::filesystem::path& xexp,
                              std::string& out_version, uint32_t& out_value);
  static std::string SanitizeId(const std::string& name);

  std::filesystem::path content_root_;
};

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_TITLE_UPDATE_MANAGER_H_
