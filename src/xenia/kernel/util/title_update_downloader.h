/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_TITLE_UPDATE_DOWNLOADER_H_
#define XENIA_KERNEL_UTIL_TITLE_UPDATE_DOWNLOADER_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace util {

// One title update offered by the online catalogue.
struct RemoteTitleUpdate {
  uint32_t id = 0;  // catalogue id, used to fetch the package
  std::string media_id;
  std::string version;
  std::string base_version;
  std::string name;
  std::string upload_date;
  // SHA-1 the catalogue lists for the package. A download is only installed
  // when its own hash matches this, so a truncated or wrong file is refused
  // rather than written into the library.
  std::string hash;
  // Size as listed. The catalogue reports this in KiB while the HTTP response
  // reports bytes, so treat it as a display hint and trust Content-Length for
  // the transfer itself.
  uint64_t listed_size = 0;
};

// Fetches title updates for a title from the online catalogue.
//
// Matching is strict: a title update is only offered when BOTH the title id
// and the media id match the running game. The catalogue groups its updates by
// media id precisely because a package built for another media id does not
// belong to this copy of the game.
class TitleUpdateDownloader {
 public:
  // Media id of a game on disk, as an 8-digit uppercase hex string, read from
  // the XEX's execution-info header. The update selector runs BEFORE the title
  // is loaded, so there is no module to ask - the file is the only source.
  // Empty if the file is not a XEX2 or carries no execution info.
  static std::string ReadMediaId(const std::filesystem::path& xex_path);

  // Updates published for this exact (title id, media id) pair, newest
  // version first. Empty when the catalogue has none, or is unreachable.
  static std::vector<RemoteTitleUpdate> List(uint32_t title_id,
                                             const std::string& media_id);

  // Streams one update to dest_path and verifies it before returning true.
  //
  // progress is called with (received_bytes, total_bytes); total is 0 while
  // the server has not said how big the body is. Setting cancel aborts the
  // transfer and removes the partial file.
  static bool Download(const RemoteTitleUpdate& update,
                       const std::filesystem::path& dest_path,
                       const std::function<void(uint64_t, uint64_t)>& progress,
                       const std::atomic<bool>* cancel);

  static bool DownloadUrl(
      const std::string& url, const std::filesystem::path& dest_path,
      const std::function<void(uint64_t, uint64_t)>& progress,
      const std::atomic<bool>* cancel);

 private:
  static std::string BuildListUrl(uint32_t title_id);
  static std::string BuildDownloadUrl(uint32_t update_id);
};

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif
