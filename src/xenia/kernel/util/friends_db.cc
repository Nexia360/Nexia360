/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/friends_db.h"

#include <chrono>

#include "third_party/sqlite/sqlite3.h"

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
      XELOGE("FriendsDB: prepare failed: {} ({})", sqlite3_errmsg(db), sql);
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

std::vector<uint8_t> ColumnBlob(sqlite3_stmt* stmt, int column) {
  const void* data = sqlite3_column_blob(stmt, column);
  const int size = sqlite3_column_bytes(stmt, column);
  if (!data || size <= 0) {
    return {};
  }
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  return std::vector<uint8_t>(bytes, bytes + size);
}

std::string ColumnText(sqlite3_stmt* stmt, int column) {
  const unsigned char* text = sqlite3_column_text(stmt, column);
  return text ? std::string(reinterpret_cast<const char*>(text))
              : std::string();
}

}  // namespace

FriendsDB::~FriendsDB() {
  std::lock_guard lock(mutex_);
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool FriendsDB::Exec(const char* sql) {
  char* error = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    XELOGE("FriendsDB: {}", error ? error : "unknown error");
    sqlite3_free(error);
    return false;
  }
  return true;
}

bool FriendsDB::Open(const std::filesystem::path& path) {
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
    XELOGE("FriendsDB: could not open {}: {}", utf8_path,
           db_ ? sqlite3_errmsg(db_) : "out of memory");
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    return false;
  }

  // WAL keeps a reader (the UI drawing the friends list) from blocking the
  // writer (presence updates arriving on a worker).
  Exec("PRAGMA journal_mode=WAL;");
  Exec("PRAGMA synchronous=NORMAL;");

  if (!Exec("CREATE TABLE IF NOT EXISTS friends ("
            "  owner_xuid     INTEGER NOT NULL,"
            "  friend_xuid    INTEGER NOT NULL,"
            "  gamertag       TEXT,"
            "  gamerpic_key   TEXT,"
            "  gamerpic       BLOB,"
            "  gamerpic_small BLOB,"
            "  updated_utc    INTEGER,"
            "  added_utc      INTEGER NOT NULL,"
            "  PRIMARY KEY (owner_xuid, friend_xuid)"
            ");")) {
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }

  Exec("CREATE INDEX IF NOT EXISTS idx_friends_owner ON friends(owner_xuid);");

  // Records that a profile's config list has already been taken in. Without
  // it the config would be re-imported on every launch, and a friend removed
  // here would come straight back - the config is no longer written, so it
  // stays stale by design.
  Exec(
      "CREATE TABLE IF NOT EXISTS config_import ("
      "  owner_xuid   INTEGER PRIMARY KEY,"
      "  imported_utc INTEGER NOT NULL"
      ");");

  XELOGI("FriendsDB: opened {}", utf8_path);
  return true;
}

bool FriendsDB::AddFriend(uint64_t owner_xuid, uint64_t friend_xuid) {
  std::lock_guard lock(mutex_);
  if (!db_) {
    return false;
  }

  // Never clobbers an existing row - a re-add must not drop the cached
  // gamerpic we already paid a download for.
  Stmt stmt(db_,
            "INSERT OR IGNORE INTO friends"
            " (owner_xuid, friend_xuid, added_utc) VALUES (?, ?, ?);");
  if (!stmt) {
    return false;
  }

  sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(owner_xuid));
  sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(friend_xuid));
  sqlite3_bind_int64(stmt.get(), 3, NowUtc());

  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool FriendsDB::RemoveFriend(uint64_t owner_xuid, uint64_t friend_xuid) {
  std::lock_guard lock(mutex_);
  if (!db_) {
    return false;
  }

  Stmt stmt(db_,
            "DELETE FROM friends WHERE owner_xuid = ? AND friend_xuid = ?;");
  if (!stmt) {
    return false;
  }

  sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(owner_xuid));
  sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(friend_xuid));

  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool FriendsDB::HasFriend(uint64_t owner_xuid, uint64_t friend_xuid) const {
  std::lock_guard lock(mutex_);
  if (!db_) {
    return false;
  }

  Stmt stmt(db_,
            "SELECT 1 FROM friends WHERE owner_xuid = ? AND friend_xuid = ?;");
  if (!stmt) {
    return false;
  }

  sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(owner_xuid));
  sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(friend_xuid));

  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

std::set<uint64_t> FriendsDB::GetFriendXUIDs(uint64_t owner_xuid) const {
  std::set<uint64_t> xuids;

  std::lock_guard lock(mutex_);
  if (!db_) {
    return xuids;
  }

  Stmt stmt(db_,
            "SELECT friend_xuid FROM friends WHERE owner_xuid = ?"
            " ORDER BY added_utc;");
  if (!stmt) {
    return xuids;
  }

  sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(owner_xuid));

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    xuids.insert(static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 0)));
  }

  return xuids;
}

std::optional<FriendRecord> FriendsDB::GetFriend(uint64_t owner_xuid,
                                                 uint64_t friend_xuid) const {
  std::lock_guard lock(mutex_);
  if (!db_) {
    return std::nullopt;
  }

  Stmt stmt(db_,
            "SELECT friend_xuid, gamertag, gamerpic_key, gamerpic,"
            " gamerpic_small FROM friends"
            " WHERE owner_xuid = ? AND friend_xuid = ?;");
  if (!stmt) {
    return std::nullopt;
  }

  sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(owner_xuid));
  sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(friend_xuid));

  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return std::nullopt;
  }

  FriendRecord record;
  record.friend_xuid =
      static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 0));
  record.gamertag = ColumnText(stmt.get(), 1);
  record.gamerpic_key = ColumnText(stmt.get(), 2);
  record.gamerpic = ColumnBlob(stmt.get(), 3);
  record.gamerpic_small = ColumnBlob(stmt.get(), 4);

  return record;
}

bool FriendsDB::SetGamertag(uint64_t owner_xuid, uint64_t friend_xuid,
                            const std::string& gamertag) {
  std::lock_guard lock(mutex_);
  if (!db_) {
    return false;
  }

  Stmt stmt(db_,
            "UPDATE friends SET gamertag = ?, updated_utc = ?"
            " WHERE owner_xuid = ? AND friend_xuid = ?;");
  if (!stmt) {
    return false;
  }

  sqlite3_bind_text(stmt.get(), 1, gamertag.c_str(),
                    static_cast<int>(gamertag.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.get(), 2, NowUtc());
  sqlite3_bind_int64(stmt.get(), 3, static_cast<sqlite3_int64>(owner_xuid));
  sqlite3_bind_int64(stmt.get(), 4, static_cast<sqlite3_int64>(friend_xuid));

  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool FriendsDB::SetGamerpic(uint64_t owner_xuid, uint64_t friend_xuid,
                            const std::string& gamerpic_key,
                            const std::vector<uint8_t>& tile,
                            const std::vector<uint8_t>& small_tile) {
  std::lock_guard lock(mutex_);
  if (!db_) {
    return false;
  }

  Stmt stmt(db_,
            "UPDATE friends SET gamerpic_key = ?, gamerpic = ?,"
            " gamerpic_small = ?, updated_utc = ?"
            " WHERE owner_xuid = ? AND friend_xuid = ?;");
  if (!stmt) {
    return false;
  }

  sqlite3_bind_text(stmt.get(), 1, gamerpic_key.c_str(),
                    static_cast<int>(gamerpic_key.size()), SQLITE_TRANSIENT);
  if (tile.empty()) {
    sqlite3_bind_null(stmt.get(), 2);
  } else {
    sqlite3_bind_blob(stmt.get(), 2, tile.data(), static_cast<int>(tile.size()),
                      SQLITE_TRANSIENT);
  }
  if (small_tile.empty()) {
    sqlite3_bind_null(stmt.get(), 3);
  } else {
    sqlite3_bind_blob(stmt.get(), 3, small_tile.data(),
                      static_cast<int>(small_tile.size()), SQLITE_TRANSIENT);
  }
  sqlite3_bind_int64(stmt.get(), 4, NowUtc());
  sqlite3_bind_int64(stmt.get(), 5, static_cast<sqlite3_int64>(owner_xuid));
  sqlite3_bind_int64(stmt.get(), 6, static_cast<sqlite3_int64>(friend_xuid));

  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool FriendsDB::GamerpicKeyMatches(uint64_t owner_xuid, uint64_t friend_xuid,
                                   const std::string& gamerpic_key) const {
  // An empty key can never match - it means we have nothing cached, so the
  // caller should fetch.
  if (gamerpic_key.empty()) {
    return false;
  }

  std::lock_guard lock(mutex_);
  if (!db_) {
    return false;
  }

  Stmt stmt(db_,
            "SELECT 1 FROM friends WHERE owner_xuid = ? AND friend_xuid = ?"
            " AND gamerpic_key = ? AND gamerpic IS NOT NULL;");
  if (!stmt) {
    return false;
  }

  sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(owner_xuid));
  sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(friend_xuid));
  sqlite3_bind_text(stmt.get(), 3, gamerpic_key.c_str(),
                    static_cast<int>(gamerpic_key.size()), SQLITE_TRANSIENT);

  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

uint32_t FriendsDB::ImportXUIDs(uint64_t owner_xuid,
                                const std::set<uint64_t>& xuids) {
  std::lock_guard lock(mutex_);
  if (!db_) {
    return 0;
  }

  // One shot per profile. Re-running would resurrect friends removed since,
  // because removals no longer touch the config.
  {
    Stmt seen(db_, "SELECT 1 FROM config_import WHERE owner_xuid = ?;");
    if (seen) {
      sqlite3_bind_int64(seen.get(), 1, static_cast<sqlite3_int64>(owner_xuid));
      if (sqlite3_step(seen.get()) == SQLITE_ROW) {
        return 0;
      }
    }
  }

  Exec("BEGIN;");

  const int64_t now = NowUtc();
  uint32_t imported = 0;

  Stmt stmt(db_,
            "INSERT OR IGNORE INTO friends"
            " (owner_xuid, friend_xuid, added_utc) VALUES (?, ?, ?);");
  if (!stmt) {
    Exec("ROLLBACK;");
    return 0;
  }

  for (const uint64_t friend_xuid : xuids) {
    sqlite3_reset(stmt.get());
    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(owner_xuid));
    sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(friend_xuid));
    sqlite3_bind_int64(stmt.get(), 3, now);

    if (sqlite3_step(stmt.get()) == SQLITE_DONE) {
      // Only counts rows the INSERT actually created - an XUID already in the
      // table is ignored, which is the "only those not in the sqlite" part.
      imported += static_cast<uint32_t>(sqlite3_changes(db_));
    }
  }

  // Marked even when nothing was imported - an empty config list is still an
  // import that has happened, and must not be retried later against a config
  // that has since gone stale.
  {
    Stmt mark(db_,
              "INSERT OR REPLACE INTO config_import"
              " (owner_xuid, imported_utc) VALUES (?, ?);");
    if (mark) {
      sqlite3_bind_int64(mark.get(), 1, static_cast<sqlite3_int64>(owner_xuid));
      sqlite3_bind_int64(mark.get(), 2, now);
      sqlite3_step(mark.get());
    }
  }

  Exec("COMMIT;");

  return imported;
}

}  // namespace kernel
}  // namespace xe
