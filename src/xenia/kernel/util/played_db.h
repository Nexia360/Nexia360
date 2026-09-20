/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_PLAYED_DB_H_
#define XENIA_KERNEL_UTIL_PLAYED_DB_H_

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct sqlite3;

namespace xe {
namespace kernel {

// One mount a title was running with, so a relaunch can put its filesystem
// back the way it was without re-deriving it: "GAME:" and the container it
// came from, "UPDATE:" and the title update that was applied, and so on.
struct PlayedMount {
  std::string mount;
  std::string path;
};

// A title this machine has run, with everything needed to list it, launch it
// again and say something about it.
struct PlayedTitle {
  int64_t row = 0;
  uint32_t title_id = 0;
  // Which release this copy is, as eight hex digits. Title updates are
  // published per media id, so two copies of one title id are not the same
  // thing and do not share an update.
  std::string media_id;
  std::string title_name;
  std::filesystem::path path;
  uint32_t content_type = 0;
  // XContentMetadata::category, straight off the container. The dashboard
  // splits My Games from My Apps on it: an Xbox 360 title with a non-zero
  // category is an app, everything else is a game. Zero for a bare .xex,
  // which carries no content metadata to state one.
  uint32_t category = 0;
  bool is_xna = false;
  int64_t first_run_utc = 0;
  int64_t last_run_utc = 0;
  int64_t run_count = 0;
  int64_t play_seconds = 0;
  // The title's own icon, exactly as its XDBF carries it (a PNG). Kept here
  // so a list can draw the game without opening its container.
  std::vector<uint8_t> icon;
};

// How a listing is ordered. The values are the dashboard's own: its library
// sort list (DataSet.LibrarySort) builds "SortOrder=1" for its default entry
// and "SortOrder=5" for the one with the calendar icon, and DataSet.Library
// falls back to 1 when a filter states none.
enum class PlayedSort : uint32_t {
  // Most recently played first. The default everywhere - it is what the
  // recent list means, and what the hub opens on.
  kMostRecentlyPlayed = 1,
  // By title name, A to Z. A title with no name sorts last rather than first,
  // since an empty string would otherwise win every comparison.
  kTitleName = 2,
  // Most played first, by total time.
  kMostPlayed = 3,
  // First played first - the order they were added.
  kFirstPlayed = 4,
  // By date, which is the calendar entry in the dashboard's own sort list and
  // is the same ordering as kMostRecentlyPlayed.
  kByDate = 5,
};

// <storage_root>/played.db - what has been played, how much, and where it
// lives. This replaces recent.toml, which held a name, a path and a timestamp
// and nothing else; the dashboard's My Games tile wants an icon, a content
// type, a play history and the title's achievements, and none of that fits in
// a list of five strings.
//
// recent.toml is still read once, on an empty database, so an existing list
// survives the change. The file is left alone afterwards.
class PlayedDB {
 public:
  PlayedDB() = default;
  ~PlayedDB();

  PlayedDB(const PlayedDB&) = delete;
  PlayedDB& operator=(const PlayedDB&) = delete;

  // Opens (creating if needed) the database and applies the schema. Safe to
  // call more than once; later calls are no-ops.
  bool Open(const std::filesystem::path& path);
  bool is_open() const { return db_ != nullptr; }

  // Records a launch. Inserts the title if it is new, bumps its run count and
  // last-run time either way, and returns the row to hand back to EndSession.
  // Zero means the write failed.
  int64_t BeginSession(const PlayedTitle& title);

  // Adds this session's length to the title's total.
  bool EndSession(int64_t row, int64_t seconds);

  bool SetIcon(int64_t row, std::span<const uint8_t> png);
  bool SetMounts(int64_t row, const std::vector<PlayedMount>& mounts);
  std::vector<PlayedMount> GetMounts(int64_t row) const;

  // A title that has been run from several paths appears once, as its most
  // recent copy - whichever way the list is ordered, the dedupe is always by
  // most recent, so changing the order never changes which copy is offered.
  // `limit` of zero means all of them.
  std::vector<PlayedTitle> GetRecent(
      size_t limit, PlayedSort sort = PlayedSort::kMostRecentlyPlayed) const;
  std::optional<PlayedTitle> Get(int64_t row) const;
  bool Remove(int64_t row);

  // One-time migration off recent.toml. Does nothing when the database
  // already holds anything.
  bool ImportRecentToml(const std::filesystem::path& toml_path);

 private:
  bool ApplySchema();

  sqlite3* db_ = nullptr;
  mutable std::mutex mutex_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_PLAYED_DB_H_
