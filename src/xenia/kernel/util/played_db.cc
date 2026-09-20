/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/played_db.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>

#include "third_party/sqlite/sqlite3.h"
#include "third_party/tomlplusplus/toml.hpp"

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"

namespace xe {
namespace kernel {

namespace {

int64_t NowUtc() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Scoped prepared statement - sqlite leaks the statement if it is not
// finalized, and every early return below would otherwise need to remember.
class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      XELOGE("PlayedDB: prepare failed: {} ({})", sqlite3_errmsg(db), sql);
      stmt_ = nullptr;
    }
  }
  ~Stmt() {
    if (stmt_) {
      sqlite3_finalize(stmt_);
    }
  }

  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  sqlite3_stmt* get() const { return stmt_; }
  operator bool() const { return stmt_ != nullptr; }

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

std::string ColumnText(sqlite3_stmt* stmt, int column) {
  const unsigned char* text = sqlite3_column_text(stmt, column);
  return text ? std::string(reinterpret_cast<const char*>(text))
              : std::string();
}

// Case-insensitive for ASCII, which is what a name sort wants: "zelda" must
// not land after "Zoo" just because of its case.
bool CompareNoCase(const std::string& a, const std::string& b) {
  const size_t shared = std::min(a.size(), b.size());
  for (size_t i = 0; i < shared; ++i) {
    const unsigned char left = static_cast<unsigned char>(a[i]);
    const unsigned char right = static_cast<unsigned char>(b[i]);
    const unsigned char lower_left =
        left >= 'A' && left <= 'Z' ? left + 32 : left;
    const unsigned char lower_right =
        right >= 'A' && right <= 'Z' ? right + 32 : right;
    if (lower_left != lower_right) {
      return lower_left < lower_right;
    }
  }
  return a.size() < b.size();
}

std::vector<uint8_t> ColumnBlob(sqlite3_stmt* stmt, int column) {
  const void* data = sqlite3_column_blob(stmt, column);
  const int size = sqlite3_column_bytes(stmt, column);
  if (!data || size <= 0) {
    return {};
  }
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  return std::vector<uint8_t>(bytes, bytes + size);
}

constexpr const char* kSelectColumns =
    "SELECT rowid, title_id, media_id, title_name, path, content_type, is_xna,"
    "       first_run_utc, last_run_utc, run_count, play_seconds, icon,"
    "       category"
    "  FROM titles";

PlayedTitle ReadRow(sqlite3_stmt* stmt) {
  PlayedTitle title;
  title.row = sqlite3_column_int64(stmt, 0);
  title.title_id = uint32_t(sqlite3_column_int64(stmt, 1));
  title.media_id = ColumnText(stmt, 2);
  title.title_name = ColumnText(stmt, 3);
  title.path = xe::to_path(ColumnText(stmt, 4));
  title.content_type = uint32_t(sqlite3_column_int64(stmt, 5));
  title.is_xna = sqlite3_column_int(stmt, 6) != 0;
  title.first_run_utc = sqlite3_column_int64(stmt, 7);
  title.last_run_utc = sqlite3_column_int64(stmt, 8);
  title.run_count = sqlite3_column_int64(stmt, 9);
  title.play_seconds = sqlite3_column_int64(stmt, 10);
  title.icon = ColumnBlob(stmt, 11);
  title.category = uint32_t(sqlite3_column_int64(stmt, 12));
  return title;
}

}  // namespace

PlayedDB::~PlayedDB() {
  std::lock_guard lock(mutex_);
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool PlayedDB::ApplySchema() {
  const auto exec = [&](const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
      XELOGE("PlayedDB: {}", error ? error : "unknown error");
      sqlite3_free(error);
      return false;
    }
    return true;
  };

  // WAL keeps a reader (a list being drawn) from blocking the writer (a
  // launch being recorded).
  exec("PRAGMA journal_mode=WAL;");
  exec("PRAGMA synchronous=NORMAL;");
  exec("PRAGMA foreign_keys=ON;");

  // One row per copy: the same title id from two containers is two copies and
  // does not share a title update, which is why media_id and path are part of
  // the key rather than title_id alone.
  if (!exec("CREATE TABLE IF NOT EXISTS titles ("
            "  title_id      INTEGER NOT NULL DEFAULT 0,"
            "  media_id      TEXT    NOT NULL DEFAULT '',"
            "  title_name    TEXT    NOT NULL DEFAULT '',"
            "  path          TEXT    NOT NULL,"
            "  content_type  INTEGER NOT NULL DEFAULT 0,"
            "  is_xna        INTEGER NOT NULL DEFAULT 0,"
            "  category      INTEGER NOT NULL DEFAULT 0,"
            "  first_run_utc INTEGER NOT NULL DEFAULT 0,"
            "  last_run_utc  INTEGER NOT NULL DEFAULT 0,"
            "  run_count     INTEGER NOT NULL DEFAULT 0,"
            "  play_seconds  INTEGER NOT NULL DEFAULT 0,"
            "  icon          BLOB,"
            "  UNIQUE (title_id, media_id, path)"
            ");")) {
    return false;
  }

  // Added after the table first shipped, so a database made by an earlier
  // build gains it here. sqlite has no IF NOT EXISTS for a column and the
  // failure when it is already there is the expected case, not an error.
  sqlite3_exec(db_,
               "ALTER TABLE titles ADD COLUMN category INTEGER NOT NULL"
               " DEFAULT 0;",
               nullptr, nullptr, nullptr);

  exec(
      "CREATE INDEX IF NOT EXISTS idx_titles_last_run"
      " ON titles(last_run_utc DESC);");

  if (!exec("CREATE TABLE IF NOT EXISTS mounts ("
            "  title_rowid INTEGER NOT NULL,"
            "  mount       TEXT    NOT NULL,"
            "  path        TEXT    NOT NULL,"
            "  UNIQUE (title_rowid, mount, path)"
            ");")) {
    return false;
  }

  exec(
      "CREATE INDEX IF NOT EXISTS idx_mounts_title"
      " ON mounts(title_rowid);");
  return true;
}

bool PlayedDB::Open(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);

  if (db_) {
    return true;
  }

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  // sqlite takes a UTF-8 path on every platform; std::filesystem on Windows
  // hands back UTF-16, so convert rather than passing native characters.
  const std::string utf8_path = xe::path_to_utf8(path);

  if (sqlite3_open(utf8_path.c_str(), &db_) != SQLITE_OK) {
    XELOGE("PlayedDB: could not open {}: {}", utf8_path,
           db_ ? sqlite3_errmsg(db_) : "out of memory");
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    return false;
  }

  if (!ApplySchema()) {
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }
  return true;
}

int64_t PlayedDB::BeginSession(const PlayedTitle& title) {
  std::lock_guard lock(mutex_);
  if (!db_ || title.path.empty()) {
    return 0;
  }
  const std::string path = xe::path_to_utf8(title.path);
  const int64_t now = NowUtc();

  {
    // The name, content type and xna flag are refreshed on every launch: the
    // first record of a title can predate knowing any of them, and an entry
    // migrated out of recent.toml has none.
    Stmt stmt(db_,
              "INSERT INTO titles (title_id, media_id, title_name, path,"
              "                    content_type, is_xna, first_run_utc,"
              "                    last_run_utc, run_count, play_seconds,"
              "                    category)"
              " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?7, 1, 0, ?8)"
              " ON CONFLICT (title_id, media_id, path) DO UPDATE SET"
              "   title_name   = CASE WHEN length(excluded.title_name) > 0"
              "                       THEN excluded.title_name"
              "                       ELSE titles.title_name END,"
              "   content_type = CASE WHEN excluded.content_type != 0"
              "                       THEN excluded.content_type"
              "                       ELSE titles.content_type END,"
              "   is_xna       = excluded.is_xna,"
              "   category     = excluded.category,"
              "   last_run_utc = excluded.last_run_utc,"
              "   run_count    = titles.run_count + 1;");
    if (!stmt) {
      return 0;
    }
    sqlite3_bind_int64(stmt.get(), 1, title.title_id);
    sqlite3_bind_text(stmt.get(), 2, title.media_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, title.title_name.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 5, title.content_type);
    sqlite3_bind_int(stmt.get(), 6, title.is_xna ? 1 : 0);
    sqlite3_bind_int64(stmt.get(), 7, now);
    sqlite3_bind_int64(stmt.get(), 8, title.category);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      XELOGE("PlayedDB: could not record launch: {}", sqlite3_errmsg(db_));
      return 0;
    }
  }

  Stmt find(db_,
            "SELECT rowid FROM titles"
            " WHERE title_id = ?1 AND media_id = ?2 AND path = ?3;");
  if (!find) {
    return 0;
  }
  sqlite3_bind_int64(find.get(), 1, title.title_id);
  sqlite3_bind_text(find.get(), 2, title.media_id.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(find.get(), 3, path.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(find.get()) != SQLITE_ROW) {
    return 0;
  }
  return sqlite3_column_int64(find.get(), 0);
}

bool PlayedDB::EndSession(int64_t row, int64_t seconds) {
  std::lock_guard lock(mutex_);
  if (!db_ || !row || seconds <= 0) {
    return false;
  }
  Stmt stmt(db_,
            "UPDATE titles SET play_seconds = play_seconds + ?2"
            " WHERE rowid = ?1;");
  if (!stmt) {
    return false;
  }
  sqlite3_bind_int64(stmt.get(), 1, row);
  sqlite3_bind_int64(stmt.get(), 2, seconds);
  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool PlayedDB::SetIcon(int64_t row, std::span<const uint8_t> png) {
  std::lock_guard lock(mutex_);
  if (!db_ || !row || png.empty()) {
    return false;
  }
  Stmt stmt(db_, "UPDATE titles SET icon = ?2 WHERE rowid = ?1;");
  if (!stmt) {
    return false;
  }
  sqlite3_bind_int64(stmt.get(), 1, row);
  sqlite3_bind_blob(stmt.get(), 2, png.data(), int(png.size()),
                    SQLITE_TRANSIENT);
  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool PlayedDB::SetMounts(int64_t row, const std::vector<PlayedMount>& mounts) {
  std::lock_guard lock(mutex_);
  if (!db_ || !row) {
    return false;
  }
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  {
    Stmt clear(db_, "DELETE FROM mounts WHERE title_rowid = ?1;");
    if (clear) {
      sqlite3_bind_int64(clear.get(), 1, row);
      sqlite3_step(clear.get());
    }
  }
  for (const PlayedMount& mount : mounts) {
    Stmt stmt(db_,
              "INSERT OR IGNORE INTO mounts (title_rowid, mount, path)"
              " VALUES (?1, ?2, ?3);");
    if (!stmt) {
      continue;
    }
    sqlite3_bind_int64(stmt.get(), 1, row);
    sqlite3_bind_text(stmt.get(), 2, mount.mount.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, mount.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt.get());
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

std::vector<PlayedMount> PlayedDB::GetMounts(int64_t row) const {
  std::lock_guard lock(mutex_);
  std::vector<PlayedMount> mounts;
  if (!db_ || !row) {
    return mounts;
  }
  Stmt stmt(db_,
            "SELECT mount, path FROM mounts WHERE title_rowid = ?1"
            " ORDER BY mount;");
  if (!stmt) {
    return mounts;
  }
  sqlite3_bind_int64(stmt.get(), 1, row);
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    mounts.push_back({ColumnText(stmt.get(), 0), ColumnText(stmt.get(), 1)});
  }
  return mounts;
}

std::vector<PlayedTitle> PlayedDB::GetRecent(size_t limit,
                                             PlayedSort sort) const {
  std::lock_guard lock(mutex_);
  std::vector<PlayedTitle> titles;
  if (!db_) {
    return titles;
  }
  // Always read most-recent-first, whatever the caller asked for: that is the
  // order the dedupe below depends on. Re-ordering happens after it, so which
  // copy of a title is offered never changes with the sort.
  const std::string sql =
      std::string(kSelectColumns) + " ORDER BY last_run_utc DESC, rowid DESC;";
  Stmt stmt(db_, sql.c_str());
  if (!stmt) {
    return titles;
  }
  // A title id can have several copies on disk; the list wants the game once,
  // as whichever copy was played last. Rows arrive newest first, so the first
  // one wins. Id zero is "unknown", which is not an identity - those stay.
  std::set<uint32_t> seen;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    PlayedTitle title = ReadRow(stmt.get());
    if (title.title_id && !seen.insert(title.title_id).second) {
      continue;
    }
    titles.push_back(std::move(title));
  }

  // Ordered before the limit is applied, so a name sort gives the first N
  // names rather than sorting the N most recent among themselves.
  switch (sort) {
    case PlayedSort::kTitleName:
      std::stable_sort(titles.begin(), titles.end(),
                       [](const PlayedTitle& a, const PlayedTitle& b) {
                         // A title with no name has nothing to sort on and
                         // would otherwise take the whole front of the list.
                         if (a.title_name.empty() != b.title_name.empty()) {
                           return b.title_name.empty();
                         }
                         return CompareNoCase(a.title_name, b.title_name);
                       });
      break;
    case PlayedSort::kMostPlayed:
      std::stable_sort(titles.begin(), titles.end(),
                       [](const PlayedTitle& a, const PlayedTitle& b) {
                         return a.play_seconds > b.play_seconds;
                       });
      break;
    case PlayedSort::kFirstPlayed:
      std::stable_sort(titles.begin(), titles.end(),
                       [](const PlayedTitle& a, const PlayedTitle& b) {
                         return a.first_run_utc < b.first_run_utc;
                       });
      break;
    case PlayedSort::kMostRecentlyPlayed:
    case PlayedSort::kByDate:
    default:
      // Already in that order - the query produced it.
      break;
  }

  if (limit && titles.size() > limit) {
    titles.resize(limit);
  }
  return titles;
}

std::optional<PlayedTitle> PlayedDB::Get(int64_t row) const {
  std::lock_guard lock(mutex_);
  if (!db_ || !row) {
    return std::nullopt;
  }
  const std::string sql = std::string(kSelectColumns) + " WHERE rowid = ?1;";
  Stmt stmt(db_, sql.c_str());
  if (!stmt) {
    return std::nullopt;
  }
  sqlite3_bind_int64(stmt.get(), 1, row);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return std::nullopt;
  }
  return ReadRow(stmt.get());
}

bool PlayedDB::Remove(int64_t row) {
  std::lock_guard lock(mutex_);
  if (!db_ || !row) {
    return false;
  }
  {
    Stmt stmt(db_, "DELETE FROM mounts WHERE title_rowid = ?1;");
    if (stmt) {
      sqlite3_bind_int64(stmt.get(), 1, row);
      sqlite3_step(stmt.get());
    }
  }
  Stmt stmt(db_, "DELETE FROM titles WHERE rowid = ?1;");
  if (!stmt) {
    return false;
  }
  sqlite3_bind_int64(stmt.get(), 1, row);
  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool PlayedDB::ImportRecentToml(const std::filesystem::path& toml_path) {
  {
    std::lock_guard lock(mutex_);
    if (!db_) {
      return false;
    }
    Stmt any(db_, "SELECT 1 FROM titles LIMIT 1;");
    if (!any || sqlite3_step(any.get()) == SQLITE_ROW) {
      return false;
    }
  }

  std::ifstream file(toml_path);
  if (!file.is_open()) {
    return false;
  }
  toml::parse_result parsed;
  try {
    parsed = toml::parse(file);
  } catch (toml::parse_error& exception) {
    XELOGE("PlayedDB: cannot parse {}: {}", xe::path_to_utf8(toml_path),
           exception.what());
    return false;
  }
  if (!parsed.is_table()) {
    return false;
  }

  size_t imported = 0;
  for (const auto& [index, entry] : *parsed.as_table()) {
    const toml::table* table = entry.as_table();
    if (!table) {
      continue;
    }
    const auto path_node = table->get_as<std::string>("path");
    if (!path_node || path_node->get().empty()) {
      continue;
    }
    PlayedTitle title;
    title.path = xe::to_path(path_node->get());
    if (const auto node = table->get_as<std::string>("title_name")) {
      title.title_name = node->get();
    }
    if (const auto node = table->get_as<std::string>("media_id")) {
      title.media_id = node->get();
    }
    if (const auto node = table->get_as<int64_t>("title_id")) {
      title.title_id = uint32_t(node->get());
    }
    int64_t last_run = 0;
    if (const auto node = table->get_as<int64_t>("last_run_time")) {
      last_run = node->get();
    }

    std::lock_guard lock(mutex_);
    Stmt stmt(db_,
              "INSERT OR IGNORE INTO titles (title_id, media_id, title_name,"
              "                              path, first_run_utc,"
              "                              last_run_utc, run_count)"
              " VALUES (?1, ?2, ?3, ?4, ?5, ?5, 1);");
    if (!stmt) {
      continue;
    }
    const std::string path = xe::path_to_utf8(title.path);
    sqlite3_bind_int64(stmt.get(), 1, title.title_id);
    sqlite3_bind_text(stmt.get(), 2, title.media_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, title.title_name.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 5, last_run);
    if (sqlite3_step(stmt.get()) == SQLITE_DONE) {
      ++imported;
    }
  }
  if (imported) {
    XELOGI("PlayedDB: took in {} entries from recent.toml", imported);
  }
  return imported != 0;
}

}  // namespace kernel
}  // namespace xe
