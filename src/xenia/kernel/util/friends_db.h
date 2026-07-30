/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_FRIENDS_DB_H_
#define XENIA_KERNEL_UTIL_FRIENDS_DB_H_

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct sqlite3;

namespace xe {
namespace kernel {

struct FriendRecord {
  uint64_t friend_xuid = 0;
  std::string gamertag;
  // The value of XPROFILE_GAMERCARD_PICTURE_KEY the cached tiles were fetched
  // for. A friend who changes their picture gets a different key, which is how
  // a stale cache is detected without asking the hub every time.
  std::string gamerpic_key;
  std::vector<uint8_t> gamerpic;
  std::vector<uint8_t> gamerpic_small;
};

// Per-profile friends list backed by <storage_root>/friends.sqlite.
//
// Friends used to live in the global `friends_xuids` cvar, which every profile
// shared - signing in as somebody else showed the same list. Rows here are
// keyed by the owning profile instead, and carry the cached gamerpic so the
// list renders offline and a gamercard does not re-download a tile it already
// has.
class FriendsDB {
 public:
  FriendsDB() = default;
  ~FriendsDB();

  FriendsDB(const FriendsDB&) = delete;
  FriendsDB& operator=(const FriendsDB&) = delete;

  // Opens (creating if needed) the database and applies the schema. Safe to
  // call more than once; later calls are no-ops.
  bool Open(const std::filesystem::path& path);
  bool is_open() const { return db_ != nullptr; }

  bool AddFriend(uint64_t owner_xuid, uint64_t friend_xuid);
  bool RemoveFriend(uint64_t owner_xuid, uint64_t friend_xuid);
  bool HasFriend(uint64_t owner_xuid, uint64_t friend_xuid) const;

  std::set<uint64_t> GetFriendXUIDs(uint64_t owner_xuid) const;
  std::optional<FriendRecord> GetFriend(uint64_t owner_xuid,
                                        uint64_t friend_xuid) const;

  bool SetGamertag(uint64_t owner_xuid, uint64_t friend_xuid,
                   const std::string& gamertag);

  // Stores the tiles together with the key they belong to. Pass the key the
  // tiles were actually fetched for - GamerpicKeyMatches() compares against it
  // to decide whether a refetch is needed.
  bool SetGamerpic(uint64_t owner_xuid, uint64_t friend_xuid,
                   const std::string& gamerpic_key,
                   const std::vector<uint8_t>& tile,
                   const std::vector<uint8_t>& small_tile);

  bool GamerpicKeyMatches(uint64_t owner_xuid, uint64_t friend_xuid,
                          const std::string& gamerpic_key) const;

  // Inserts every XUID that is not already present for this profile, and
  // returns how many were added. Existing rows keep their cached gamerpic -
  // the import never overwrites, which is what makes it safe to run on every
  // launch.
  uint32_t ImportXUIDs(uint64_t owner_xuid, const std::set<uint64_t>& xuids);

 private:
  bool Exec(const char* sql);

  sqlite3* db_ = nullptr;
  // sqlite is built threadsafe, but the friends list is touched from the guest,
  // the UI and the presence worker - one lock keeps statement lifetimes simple.
  mutable std::mutex mutex_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_FRIENDS_DB_H_
