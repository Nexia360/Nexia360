/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_editor/navigation.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "third_party/libcurl/include/curl/curl.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/xam/avatar_editor/image_constants.h"
#include "xenia/kernel/xam/avatar_editor/scene.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

namespace {

constexpr uint32_t kRemoteImageWidgetBytes = 0x84C;
constexpr uint32_t kLocalImageWidgetBytes = 0x134;

// Long enough for the editor's own asset paths and for the cache file a
// download lands in.
constexpr uint32_t kWidgetPathChars = 260;

constexpr uint32_t kImageNotLoaded = 0;
constexpr uint32_t kImageLoaded = 1;
constexpr uint32_t kImageUnavailable = 2;

constexpr long kDownloadTimeoutSeconds = 20;
constexpr curl_off_t kDownloadByteLimit = 8 * 1024 * 1024;

#pragma pack(push, 1)
// The editor's image widget drew the picture itself. Here the renderer draws
// the background, so the object carries what the renderer needs to find the
// picture: which slot it is in, whether the picture has arrived, and the path
// to open. For a local path that path is the one it was asked for; for a
// remote one it is empty until the download lands and then names the file on
// disk the bytes went into.
//
// The first two words are the class id and reference count every scene object
// starts with, because the navigation releases these through ReleaseScene.
struct BackgroundWidget {
  xe::be<uint32_t> class_id;
  xe::be<uint32_t> ref_count;
  xe::be<int32_t> slot;
  xe::be<uint32_t> load_state;
  uint8_t remote;
  uint8_t reserved[3];
  xe::be<uint16_t> path[kWidgetPathChars];
};
#pragma pack(pop)

static_assert(offsetof(BackgroundWidget, class_id) == 0x00, "");
static_assert(offsetof(BackgroundWidget, ref_count) == 0x04, "");
static_assert(offsetof(BackgroundWidget, slot) == 0x08, "");
static_assert(offsetof(BackgroundWidget, load_state) == 0x0C, "");
static_assert(offsetof(BackgroundWidget, path) == 0x14, "");

std::u16string WidePathAt(const Guest& guest, uint32_t address) {
  std::u16string path;
  const auto* text = guest.At<xe::be<uint16_t>>(address);
  if (!text) {
    return path;
  }
  for (uint32_t i = 0; i + 1 < kBackgroundImageChars; ++i) {
    const uint16_t character = text[i];
    if (!character) {
      break;
    }
    path.push_back(char16_t(character));
  }
  return path;
}

void StoreWidgetPath(BackgroundWidget* widget, const std::u16string& path) {
  const size_t length =
      path.size() < kWidgetPathChars ? path.size() : kWidgetPathChars - 1;
  for (size_t i = 0; i < length; ++i) {
    widget->path[i] = uint16_t(path[i]);
  }
  widget->path[length] = 0;
}

bool PathIsRemote(const std::u16string& path) {
  return path.compare(0, kRemoteImagePrefixChars, kRemoteImagePrefix) == 0;
}

// --- the remote image cache --------------------------------------------------
//
// One entry per URL, shared by every widget that asks for it, so returning to
// a screen shows its background straight away. The worker only ever touches
// the entry; guest memory is written on the editor's own thread, when the
// transition asks whether the picture has arrived.

struct RemoteImage {
  std::atomic<uint32_t> state{kImageNotLoaded};
  std::u16string file;
};

uint64_t HashOfUrl(const std::string& url) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (unsigned char character : url) {
    hash = (hash ^ character) * 0x100000001B3ull;
  }
  return hash;
}

std::string ExtensionOfUrl(const std::string& url) {
  const size_t query = url.find_first_of("?#");
  const std::string path = url.substr(0, query);
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
    return ".img";
  }
  const std::string extension = path.substr(dot);
  if (extension.size() > 5) {
    return ".img";
  }
  for (char character : extension.substr(1)) {
    if (!isalnum(static_cast<unsigned char>(character))) {
      return ".img";
    }
  }
  return extension;
}

std::filesystem::path CacheFolder() {
  std::error_code error;
  std::filesystem::path folder =
      std::filesystem::temp_directory_path(error) / "nexia_avatar_backgrounds";
  if (error) {
    return std::filesystem::path();
  }
  std::filesystem::create_directories(folder, error);
  return folder;
}

size_t AppendToFile(void* data, size_t size, size_t count, void* context) {
  return fwrite(data, size, count, static_cast<FILE*>(context));
}

int RefuseOversizedDownload(void*, curl_off_t total_download,
                            curl_off_t now_download, curl_off_t, curl_off_t) {
  return total_download > kDownloadByteLimit ||
                 now_download > kDownloadByteLimit
             ? 1
             : 0;
}

bool DownloadToFile(const std::string& url,
                    const std::filesystem::path& destination) {
  std::error_code error;
  std::filesystem::path partial = destination;
  partial += ".part";

  FILE* file = xe::filesystem::OpenFile(partial, "wb");
  if (!file) {
    XELOGE("avatar_editor: no cache file for the background at {}", url);
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    fclose(file);
    std::filesystem::remove(partial, error);
    return false;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "nexia360");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, kDownloadTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendToFile);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, RefuseOversizedDownload);

  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  fclose(file);

  if (result != CURLE_OK || status >= 400) {
    XELOGE(
        "avatar_editor: the background at {} did not arrive, curl {} http {}",
        url, static_cast<int>(result), status);
    std::filesystem::remove(partial, error);
    return false;
  }

  std::filesystem::remove(destination, error);
  std::filesystem::rename(partial, destination, error);
  if (error) {
    XELOGE("avatar_editor: the background at {} could not be kept", url);
    std::filesystem::remove(partial, error);
    return false;
  }
  return true;
}

std::shared_ptr<RemoteImage> RequestRemoteImage(const std::u16string& url) {
  static std::mutex mutex;
  static std::map<std::u16string, std::shared_ptr<RemoteImage>> requests;

  std::lock_guard<std::mutex> lock(mutex);
  auto existing = requests.find(url);
  if (existing != requests.end()) {
    return existing->second;
  }

  auto image = std::make_shared<RemoteImage>();
  requests.emplace(url, image);

  const std::string utf8_url = xe::to_utf8(url);
  const std::filesystem::path folder = CacheFolder();
  if (folder.empty()) {
    image->state = kImageUnavailable;
    XELOGE("avatar_editor: no cache folder, the background at {} stays blank",
           utf8_url);
    return image;
  }
  const std::filesystem::path destination =
      folder /
      (fmt::format("{:016X}", HashOfUrl(utf8_url)) + ExtensionOfUrl(utf8_url));

  std::thread([image, utf8_url, destination]() {
    xe::threading::set_name("Avatar Editor Background");
    const bool arrived = std::filesystem::exists(destination) ||
                         DownloadToFile(utf8_url, destination);
    if (arrived) {
      image->file = xe::path_to_utf16(destination);
      image->state = kImageLoaded;
    } else {
      image->state = kImageUnavailable;
    }
  }).detach();

  return image;
}

// Which download a widget is waiting on, for as long as the widget exists.
std::mutex& PendingMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<uint32_t, std::shared_ptr<RemoteImage>>& PendingByWidget() {
  static std::map<uint32_t, std::shared_ptr<RemoteImage>> pending;
  return pending;
}

std::shared_ptr<RemoteImage> PendingImageFor(uint32_t widget) {
  std::lock_guard<std::mutex> lock(PendingMutex());
  auto found = PendingByWidget().find(widget);
  return found == PendingByWidget().end() ? nullptr : found->second;
}

uint32_t BuildBackgroundWidget(const Guest& guest, uint32_t bytes,
                               int32_t slot) {
  const uint32_t allocation = bytes < sizeof(BackgroundWidget)
                                  ? uint32_t(sizeof(BackgroundWidget))
                                  : bytes;
  const uint32_t widget = AllocateGuestBytes(guest, allocation);
  if (!widget) {
    return 0;
  }
  memset(guest.At<uint8_t>(widget), 0, allocation);
  auto* object = guest.At<BackgroundWidget>(widget);
  object->class_id = kClassBackgroundImage;
  object->ref_count = 1;
  object->slot = slot;
  return widget;
}

}  // namespace

/* sub_920EC4A8 */
void Navigation::SetBackgroundImage(uint32_t path) {
  auto* nav = state();
  const uint32_t shown =
      kNavigationAddress + offsetof(NavigationState, background_image_shown);

  const std::u16string wanted =
      path ? WidePathAt(guest_, path) : std::u16string();
  if (path && wanted == WidePathAt(guest_, shown)) {
    return;
  }

  const int32_t slot = nav->background_front_slot;
  const uint32_t previous = nav->background_widgets[slot];
  if (previous) {
    ReleaseBackgroundWidget(previous);
    nav->background_widgets[slot] = 0;
  }

  if (wanted.empty()) {
    nav->background_image_shown[0] = 0;
    return;
  }

  for (size_t i = 0; i < wanted.size(); ++i) {
    nav->background_image_shown[i] = uint16_t(wanted[i]);
  }
  nav->background_image_shown[wanted.size()] = 0;

  if (PathIsRemote(wanted)) {
    nav->background_widgets[slot] = BuildRemoteBackgroundWidget(path, slot);
    nav->background_widget_ready = 0;
  } else {
    nav->background_widgets[slot] = BuildLocalBackgroundWidget(path, slot);
  }
}

/* the first phase of sub_920ECEB8 */
//
// Two widgets and two slot numbers: the one just built is attached, the one it
// replaces is torn down, and the slots trade places so the next build has an
// empty one to use.
void Navigation::PresentPendingBackground() {
  auto* nav = state();
  const uint32_t front = nav->background_widgets[nav->background_front_slot];
  if (!front) {
    return;
  }

  if (!nav->background_widget_ready) {
    nav->background_widget_ready = BackgroundWidgetIsReady(front) ? 1 : 0;
  }
  AttachScene(front);

  const uint32_t back = nav->background_widgets[nav->background_back_slot];
  if (back) {
    ReleaseBackgroundWidget(back);
    nav->background_widgets[nav->background_back_slot] = 0;
  }

  const int32_t front_slot = nav->background_front_slot;
  nav->background_front_slot = nav->background_back_slot;
  nav->background_back_slot = front_slot;
}

/* sub_920DA5F8 then sub_920F69D0, then sub_920A9430 */
void Navigation::ReleaseBackgroundWidget(uint32_t widget) {
  {
    std::lock_guard<std::mutex> lock(PendingMutex());
    PendingByWidget().erase(widget);
  }
  ReleaseScene(widget);
}

/* sub_920F44E0, named from L"BGImage%i" by sub_920C29B0 */
//
// The download runs off the editor's thread. The widget goes out not loaded
// and the transition keeps asking, so nothing here waits on the network.
uint32_t Navigation::BuildRemoteBackgroundWidget(uint32_t path, int32_t slot) {
  const uint32_t widget =
      BuildBackgroundWidget(guest_, kRemoteImageWidgetBytes, slot);
  if (!widget) {
    return 0;
  }
  auto* object = guest_.At<BackgroundWidget>(widget);
  object->remote = 1;
  object->load_state = kImageNotLoaded;

  auto image = RequestRemoteImage(WidePathAt(guest_, path));
  {
    std::lock_guard<std::mutex> lock(PendingMutex());
    PendingByWidget()[widget] = std::move(image);
  }
  return widget;
}

/* sub_920F3ED8, named from L"BGImage%i" by sub_920C29B0 */
uint32_t Navigation::BuildLocalBackgroundWidget(uint32_t path, int32_t slot) {
  const uint32_t widget =
      BuildBackgroundWidget(guest_, kLocalImageWidgetBytes, slot);
  if (!widget) {
    return 0;
  }
  auto* object = guest_.At<BackgroundWidget>(widget);
  StoreWidgetPath(object, WidePathAt(guest_, path));
  object->load_state = kImageLoaded;
  return widget;
}

/* sub_920F4638 */
//
// Ready means settled, not fetched: a download that failed answers ready with
// load_state unavailable, so the transition it gates runs on rather than
// waiting for a picture that is never coming.
bool Navigation::BackgroundWidgetIsReady(uint32_t widget) {
  auto* object = guest_.At<BackgroundWidget>(widget);
  if (!object) {
    return false;
  }
  if (!object->remote) {
    return true;
  }
  if (object->load_state != kImageNotLoaded) {
    return true;
  }

  auto image = PendingImageFor(widget);
  if (!image) {
    object->load_state = kImageUnavailable;
    return true;
  }
  const uint32_t state = image->state.load();
  if (state == kImageNotLoaded) {
    return false;
  }
  if (state == kImageLoaded) {
    StoreWidgetPath(object, image->file);
  }
  object->load_state = state;
  return true;
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
