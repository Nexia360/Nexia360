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
  std::string hash;         // XXH128 of the update's contents (dedupe key)
};

// Reserved library id for the "None" selection. Content that existed before
// any title update was chosen lives here, and it is a normal overlay - not a
// fallback to the global content tree.
inline constexpr const char* kNoTitleUpdateId = "NO_TU";

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

  // Per-profile content root for the ACTIVE selection, which is NO_TU when the
  // user picked "None". Every content type except installers resolves under
  // here, so each update keeps its own saves and DLC. Created on demand, so a
  // freshly selected update starts with an empty save area.
  std::filesystem::path GetActiveContentRoot(uint32_t title_id,
                                             uint64_t xuid) const;

  // Content root for a specific library id (a title update, or NO_TU).
  std::filesystem::path GetContentRoot(uint32_t title_id, const std::string& id,
                                       uint64_t xuid) const;

  // Moves any pre-existing global content (content/<xuid>/<title_id>/) into
  // the NO_TU overlay, once per title. Installers are left in the global tree.
  void MigrateGlobalContentToNoTu(uint32_t title_id);

  // Copies the NO_TU overlay's content for one profile into a title update.
  // NO_TU is left intact so it can be imported into several updates. Returns
  // the files that would be overwritten when dry_run is true; performs the
  // copy (overwriting those files) when it is false.
  std::vector<std::string> ImportNoTuContent(uint32_t title_id,
                                             const std::string& target_id,
                                             uint64_t xuid, bool dry_run);

  // Backfills the content hash of every library entry that predates hashing.
  void EnsureHashes(uint32_t title_id);

  // XXH128 over a title update folder's contents. Deterministic across
  // machines: files are visited in sorted relative-path order and both the
  // path and the bytes are mixed in. Empty string if the folder is unreadable.
  static std::string ComputeUpdateHash(const std::filesystem::path& dir);

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

  // One-time migration: move payloads stored at <Library>/<id>/ down into
  // <Library>/<id>/UPDATE/, leaving any per-update Content folder in place.
  void MigrateLegacyLibraryLayout(uint32_t title_id);

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
  // <Library>/<title_id>/<id>/UPDATE - the update payload, kept apart from the
  // per-update Content folder next to it.
  std::filesystem::path update_dir(uint32_t title_id,
                                   const std::string& id) const;
  std::filesystem::path content_update_dir(uint32_t title_id) const;
  std::filesystem::path content_header_dir(uint32_t title_id) const;

  bool LoadManifest(uint32_t title_id, std::string& active,
                    std::vector<TitleUpdateEntry>& entries) const;
  bool SaveManifest(uint32_t title_id, const std::string& active,
                    const std::vector<TitleUpdateEntry>& entries) const;

  // Library id of an installed update whose contents hash to `hash`, or empty.
  std::string FindByHash(const std::string& hash,
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
