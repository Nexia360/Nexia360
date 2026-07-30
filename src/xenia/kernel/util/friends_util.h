/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_FRIENDS_UTIL
#define XENIA_KERNEL_UTIL_FRIENDS_UTIL

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace xe {
namespace kernel {

std::vector<std::string> ParseDelimitedList(std::string_view csv,
                                            uint32_t count = 0);

std::string BuildCSVFromVector(std::vector<std::string>& data,
                               uint32_t count = 0);

std::set<uint64_t> ParseFriendsXUIDs();

// Friends for one profile, from friends.sqlite. Any XUID still listed in the
// config that the database does not already have is imported first, so an
// existing config-based list carries over once and additions made in either
// place survive. Existing rows are never overwritten - the import cannot lose
// a cached gamerpic. Falls back to the config list when the database is
// unavailable.
// The database is passed in rather than looked up: one caller runs inside
// XamState's constructor, where kernel_state()->xam_state() is still null.
// A null db falls back to the config list.
std::set<uint64_t> LoadProfileFriends(uint64_t owner_xuid, FriendsDB* db);

// Gamerpic tile for a friend, cached in friends.sqlite.
//
//  - we are offline    -> whatever the database has, even if empty
//  - friend is offline -> the cached tile; only fetched if nothing is cached
//  - friend is online  -> fetched from the hub, and the database is updated
//                         when the tile actually differs
//
// Only rows that exist (real friends) are written; a lookup for somebody who
// is not a friend still downloads, it just is not cached.
std::vector<uint8_t> GetFriendGamerpic(uint64_t owner_xuid,
                                       uint64_t friend_xuid, bool small_tile);

void AddFriendToConfig(uint64_t xuid);

void RemoveFriendFromConfig(uint64_t xuid);

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_FRIENDS_UTIL
