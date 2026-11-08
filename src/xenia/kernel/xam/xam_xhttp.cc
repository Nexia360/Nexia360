/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include "xenia/base/platform.h"

#include <curl/curl.h>

#include "third_party/stb/stb_image.h"
#include "third_party/stb/stb_image_write.h"
#include "xenia/apu/sdl/voice_chat.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"
#include "xenia/base/string_util.h"
#include "xenia/base/threading.h"
#include "xenia/base/utf8.h"
#include "xenia/config.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/json/base_object_json.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/util/friends_util.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xam_xhttp.h"
#include "xenia/kernel/xnet.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

XHttpManager* XHttpManager::Get() {
  static XHttpManager instance;
  return &instance;
}

uint32_t XHttpManager::Create(XHttpHandleType type, uint32_t parent) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto handle = std::make_unique<XHttpHandle>();
  handle->type = type;
  handle->handle = next_handle_++;
  handle->parent = parent;
  const uint32_t id = handle->handle;
  handles_.emplace(id, std::move(handle));
  return id;
}

XHttpHandle* XHttpManager::Lookup(uint32_t handle) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto it = handles_.find(handle);
  return it == handles_.end() ? nullptr : it->second.get();
}

XHttpHandle* XHttpManager::Lookup(uint32_t handle, XHttpHandleType type) {
  auto* object = Lookup(handle);
  if (object && object->type != type) {
    return nullptr;
  }
  return object;
}

bool XHttpManager::Close(uint32_t handle) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto it = handles_.find(handle);
  if (it == handles_.end()) {
    return false;
  }
  std::vector<uint32_t> children;
  for (const auto& entry : handles_) {
    if (entry.second->parent == handle) {
      children.push_back(entry.first);
    }
  }
  handles_.erase(it);
  for (uint32_t child : children) {
    Close(child);
  }
  return true;
}

void XHttpManager::CloseAll() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  handles_.clear();
}

XHttpHandle* XHttpManager::ConnectOf(XHttpHandle* handle) {
  if (!handle) {
    return nullptr;
  }
  if (handle->type == XHttpHandleType::kConnect) {
    return handle;
  }
  return Lookup(handle->parent, XHttpHandleType::kConnect);
}

XHttpHandle* XHttpManager::SessionOf(XHttpHandle* handle) {
  if (!handle) {
    return nullptr;
  }
  if (handle->type == XHttpHandleType::kSession) {
    return handle;
  }
  auto* connect = ConnectOf(handle);
  return connect ? Lookup(connect->parent, XHttpHandleType::kSession) : nullptr;
}

bool XHttpManager::IsAsync(XHttpHandle* handle) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto* session = SessionOf(handle);
  return session && (session->flags & XHTTP_FLAG_ASYNC) != 0;
}

void XHttpManager::Notify(XHttpHandle* request, uint32_t status,
                          uint32_t info_ptr, uint32_t info_length,
                          uint32_t free_info) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!request || !IsAsync(request)) {
    if (free_info) {
      kernel_memory()->SystemHeapFree(free_info);
    }
    return;
  }
  // The callback may be set on the request or inherited from the session.
  auto* session = SessionOf(request);
  uint32_t callback = request->callback;
  uint32_t flags = request->callback_flags;
  if (!callback && session) {
    callback = session->callback;
    flags = session->callback_flags;
  }
  if (!callback || !(flags & status)) {
    if (free_info) {
      kernel_memory()->SystemHeapFree(free_info);
    }
    return;
  }

  XHttpNotification notification = {};
  notification.session = session ? session->handle : 0;
  notification.handle = request->handle;
  notification.callback = callback;
  notification.context = request->context;
  notification.status = status;
  notification.info_ptr = info_ptr;
  notification.info_length = info_length;
  notification.free_info = free_info;
  notifications_.push_back(notification);
}

bool XHttpManager::TakeNotification(uint32_t session, XHttpNotification& out) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (auto it = notifications_.begin(); it != notifications_.end(); ++it) {
    if (session && it->session != session && it->handle != session) {
      continue;
    }
    out = *it;
    notifications_.erase(it);
    return true;
  }
  return false;
}

namespace {

std::string GuestString(uint32_t address) {
  if (!address) {
    return "";
  }
  return kernel_memory()->TranslateVirtual<const char*>(address);
}

std::string GuestStringW(uint32_t address) {
  if (!address) {
    return "";
  }
  const auto* host_ptr =
      kernel_memory()->TranslateVirtual<const char16_t*>(address);
  return xe::to_utf8(xe::load_and_swap<std::u16string>(host_ptr));
}

size_t WriteBody(char* data, size_t size, size_t count, void* userdata) {
  auto* body = reinterpret_cast<std::vector<uint8_t>*>(userdata);
  const size_t total = size * count;
  body->insert(body->end(), data, data + total);
  return total;
}

size_t WriteHeader(char* data, size_t size, size_t count, void* userdata) {
  auto* handle = reinterpret_cast<XHttpHandle*>(userdata);
  const size_t total = size * count;
  std::string line(data, total);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.pop_back();
  }
  if (line.empty()) {
    return total;
  }
  if (line.rfind("HTTP/", 0) == 0) {
    handle->raw_headers.clear();
    handle->status_text.clear();
    const size_t code_at = line.find(' ');
    if (code_at != std::string::npos) {
      const size_t text_at = line.find(' ', code_at + 1);
      if (text_at != std::string::npos) {
        handle->status_text = line.substr(text_at + 1);
      }
    }
  }
  handle->raw_headers += line;
  handle->raw_headers += "\r\n";
  return total;
}

std::string HeaderValue(const XHttpHandle* handle, const std::string& name) {
  size_t at = 0;
  while (at < handle->raw_headers.size()) {
    const size_t end = handle->raw_headers.find("\r\n", at);
    if (end == std::string::npos) {
      break;
    }
    const std::string line = handle->raw_headers.substr(at, end - at);
    const size_t colon = line.find(':');
    if (colon != std::string::npos &&
        xe::utf8::equal_case(line.substr(0, colon), name)) {
      size_t value_at = colon + 1;
      while (value_at < line.size() && line[value_at] == ' ') {
        ++value_at;
      }
      return line.substr(value_at);
    }
    at = end + 2;
  }
  return "";
}

std::string BuildUrl(XHttpHandle* request) {
  auto* manager = XHttpManager::Get();
  auto* connect = manager->ConnectOf(request);
  if (!connect) {
    return "";
  }
  uint16_t port = connect->port;
  // The dash's XMan client connects to port 443 but does not pass
  // XHTTP_FLAG_SECURE on the request (on hardware the Live stack picks the
  // scheme from the port). Sending plaintext to 443 gets a 400 out of nginx
  // before the request ever reaches the hub, so treat 443 as https.
  const bool secure = request->secure || port == 443;
  if (!port) {
    port = secure ? 443 : 80;
  }
  std::string url = secure ? "https://" : "http://";
  url += connect->host;
  if ((secure && port != 443) || (!secure && port != 80)) {
    url += ":" + std::to_string(port);
  }
  std::string object_name = request->object_name;
  if (object_name.empty()) {
    object_name = "/";
  }
  if (object_name.front() != '/') {
    url += "/";
  }
  url += object_name;
  return url;
}

// One GET, body and status. Empty body on any failure.
std::vector<uint8_t> HttpGet(const std::string& url, long* status,
                             long timeout_ms = 10000) {
  std::vector<uint8_t> body;
  *status = 0;
  CURL* curl = curl_easy_init();
  if (!curl) {
    return body;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,
                   static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  const CURLcode result = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK || *status != 200) {
    body.clear();
  }
  return body;
}

std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
}

void WriteWholeFile(const std::filesystem::path& path,
                    const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
}

uint32_t ParseHexTitleId(const char* text) {
  char* end = nullptr;
  const unsigned long id = std::strtoul(text, &end, 16);
  return end != text && *end == '\0' ? static_cast<uint32_t>(id) : 0;
}

constexpr std::string_view kX360dbBases[] = {
    "https://xenia-manager.github.io/x360db/",
    "https://raw.githubusercontent.com/xenia-manager/x360db/main/"};

struct X360dbTitle {
  uint32_t primary_id = 0;
  std::string boxart;
};

// x360db files art under a game's primary id; games.json maps its other ids.
const X360dbTitle* LookupX360dbTitle(uint32_t title_id) {
  static std::mutex mutex;
  static bool loaded = false;
  static std::deque<X360dbTitle> titles;
  static std::unordered_map<uint32_t, const X360dbTitle*> by_id;
  std::lock_guard<std::mutex> lock(mutex);
  if (!loaded) {
    loaded = true;
    const std::filesystem::path path =
        kernel_state()->emulator()->cache_root() / "x360db" / "games.json";
    std::error_code ec;
    const bool cached = std::filesystem::is_regular_file(path, ec);
    const bool stale =
        !cached || std::filesystem::last_write_time(path, ec) +
                           std::chrono::hours(24 * 7) <
                       std::filesystem::file_time_type::clock::now();
    std::vector<uint8_t> json;
    if (stale) {
      long status = 0;
      for (std::string_view base : kX360dbBases) {
        json = HttpGet(std::string(base) + "games.json", &status, 30000);
        if (!json.empty()) {
          break;
        }
      }
      if (!json.empty()) {
        WriteWholeFile(path, json);
      } else {
        XELOGI("x360db: games.json unavailable (HTTP {})", status);
      }
    }
    if (json.empty() && cached) {
      json = ReadWholeFile(path);
    }
    rapidjson::Document document;
    if (!json.empty()) {
      document.Parse(reinterpret_cast<const char*>(json.data()), json.size());
    }
    if (!json.empty() && !document.HasParseError() && document.IsArray()) {
      std::vector<std::pair<uint32_t, const X360dbTitle*>> alternatives;
      for (const auto& entry : document.GetArray()) {
        if (!entry.IsObject() || !entry.HasMember("id") ||
            !entry["id"].IsString()) {
          continue;
        }
        const uint32_t id = ParseHexTitleId(entry["id"].GetString());
        if (!id) {
          continue;
        }
        X360dbTitle& title = titles.emplace_back();
        title.primary_id = id;
        if (entry.HasMember("boxart") && entry["boxart"].IsString()) {
          title.boxart = entry["boxart"].GetString();
        }
        by_id.emplace(id, &title);
        if (entry.HasMember("alternative_id") &&
            entry["alternative_id"].IsArray()) {
          for (const auto& alt : entry["alternative_id"].GetArray()) {
            const uint32_t alt_id =
                alt.IsString() ? ParseHexTitleId(alt.GetString()) : 0;
            if (alt_id) {
              alternatives.emplace_back(alt_id, &title);
            }
          }
        }
      }
      for (const auto& [id, title] : alternatives) {
        by_id.emplace(id, title);
      }
      XELOGI("x360db: {} titles, {} ids", titles.size(), by_id.size());
    }
  }
  auto it = by_id.find(title_id);
  return it == by_id.end() ? nullptr : it->second;
}

// Box art from x360db as Edge takes it, then the marketplace URL it mirrors.
std::vector<uint8_t> FetchDiscBoxArt(uint32_t title_id) {
  const X360dbTitle* title = LookupX360dbTitle(title_id);
  const uint32_t primary_id = title ? title->primary_id : title_id;
  const std::filesystem::path cache =
      kernel_state()->emulator()->cache_root() / "x360db" / "titles" /
      fmt::format("{:08X}", primary_id) / "artwork" / "boxart.jpg";
  std::error_code ec;
  if (std::filesystem::is_regular_file(cache, ec)) {
    return ReadWholeFile(cache);
  }

  static std::mutex missing_mutex;
  static std::set<uint32_t> missing;
  {
    std::lock_guard<std::mutex> lock(missing_mutex);
    if (missing.contains(primary_id)) {
      return {};
    }
  }

  std::vector<std::string> urls;
  for (std::string_view base : kX360dbBases) {
    urls.push_back(
        fmt::format("{}titles/{:08X}/artwork/boxart.jpg", base, primary_id));
  }
  if (title && !title->boxart.empty()) {
    urls.push_back(title->boxart);
  }
  const std::string marketplace = fmt::format(
      "http://download.xbox.com/content/images/"
      "66acd000-77fe-1000-9115-d802{:08x}/1033/boxartlg.jpg",
      title_id);
  if (std::find(urls.begin(), urls.end(), marketplace) == urls.end()) {
    urls.push_back(marketplace);
  }
  for (const std::string& url : urls) {
    long status = 0;
    std::vector<uint8_t> art = HttpGet(url, &status, 8000);
    int width = 0;
    int height = 0;
    int channels = 0;
    if (!art.empty() &&
        stbi_info_from_memory(art.data(), static_cast<int>(art.size()), &width,
                              &height, &channels)) {
      WriteWholeFile(cache, art);
      XELOGI("x360db: box art for {:08X} from {} ({}x{})", title_id, url, width,
             height);
      return art;
    }
  }
  XELOGI("x360db: no box art for {:08X}", title_id);
  std::lock_guard<std::mutex> lock(missing_mutex);
  missing.insert(primary_id);
  return {};
}

// Guest URLs can hold invalid UTF-8; utf8::lower_ascii reads past the end on
// it.
std::string LowerBytes(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// The disc in the tray's art. The moment a disc goes in, the dashboard asks
// Live's tile service - GameImageAssetUriRoot, pointed at the hub in
// xam_info.cc - for <root>/<title id>/<locale>/discintray.jpg. Nothing online
// has that file for a disc image off the user's drive, so the title's box art
// answers it, cut to the tile's 4:3. Without the box art, the disc's own title
// image stands in - but that is a 64x64 icon, and the tile stretches it three
// times over.
bool AnswerDiscInTray(XHttpHandle* request, const std::string& verb,
                      const std::string& url) {
  if (verb != "GET") {
    return false;
  }
  std::string lowered = LowerBytes(url);
  lowered = lowered.substr(0, lowered.find('?'));
  constexpr std::string_view kImages = "/images/";
  constexpr std::string_view kName = "/discintray.jpg";
  const size_t images = lowered.find(kImages);
  if (images == std::string::npos || !lowered.ends_with(kName)) {
    return false;
  }
  const std::string title_hex = lowered.substr(images + kImages.size(), 8);
  char* end = nullptr;
  const uint32_t title_id =
      static_cast<uint32_t>(std::strtoul(title_hex.c_str(), &end, 16));
  if (title_hex.size() != 8 || end != title_hex.c_str() + 8) {
    return false;
  }
  Emulator* emulator = kernel_state()->emulator();
  std::vector<uint8_t> png;
  if (!emulator || !emulator->GetInsertedDiscImage(title_id, &png)) {
    return false;
  }

  const std::vector<uint8_t> box_art = FetchDiscBoxArt(title_id);
  const bool from_box_art = !box_art.empty();
  const std::vector<uint8_t>& source = from_box_art ? box_art : png;
  int width = 0;
  int height = 0;
  int channels = 0;
  uint8_t* pixels =
      stbi_load_from_memory(source.data(), static_cast<int>(source.size()),
                            &width, &height, &channels, 3);
  if (!pixels) {
    return false;
  }
  // Box art is portrait; the tile is 4:3. The band starts just under the
  // "XBOX 360" strip across the top 12% of every cover, because the game's
  // logo sits anywhere from straight under it (Black Ops II) to the middle
  // (Modern Warfare 3) - a centred band cut the first one's off.
  int top = 0;
  int rows = height;
  if (from_box_art && height * 4 > width * 3) {
    rows = width * 3 / 4;
    top = std::min(height * 13 / 100, height - rows);
  }
  std::vector<uint8_t> jpeg;
  stbi_write_jpg_to_func(
      [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        const auto* bytes = static_cast<const uint8_t*>(data);
        out->insert(out->end(), bytes, bytes + size);
      },
      &jpeg, width, rows, 3, pixels + size_t(top) * size_t(width) * 3, 92);
  stbi_image_free(pixels);
  height = rows;
  if (jpeg.empty()) {
    return false;
  }

  request->raw_headers = fmt::format(
      "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: {}\r\n",
      jpeg.size());
  request->status_text = "OK";
  request->status_code = 200;
  request->response_body = std::move(jpeg);
  request->read_pos = 0;
  request->auth_schemes = 0;
  request->received = true;
  XELOGI("XHttp: {} {} -> answered from the {} ({}x{}, {} bytes)", verb, url,
         from_box_art ? "title's box art" : "disc's title image", width, height,
         request->response_body.size());
  return true;
}

void AnswerWith(XHttpHandle* request, uint32_t status, const char* status_text,
                const std::string& body) {
  request->raw_headers = fmt::format(
      "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\n"
      "Content-Length: {}\r\n",
      status, status_text, body.size());
  request->status_text = status_text;
  request->status_code = status;
  request->response_body.assign(body.begin(), body.end());
  request->read_pos = 0;
  request->auth_schemes = 0;
  request->received = true;
}

// The dashboard's Privacy app (our Dash.PrivacyUI.lex) reads and writes the
// friend-request setting at <hub>/players/friendprivacy. Answered here through
// XLiveAPI so it gets exactly what the Friends dialog gets: the hub's value
// with the local copy behind it, and a save that lands locally first, then on
// the hub. A POST carrying a "friends" list is not the app's and goes out
// untouched - SetFriendPrivacy deliberately sends no list, because an empty
// one would wipe the hub's.
bool AnswerFriendPrivacy(XHttpHandle* request, const std::string& verb,
                         const std::string& url) {
  std::string path = LowerBytes(url);
  path = path.substr(0, path.find('?'));
  constexpr std::string_view kRoute = "/players/friendprivacy";
  const size_t route = path.find(kRoute);
  if (route == std::string::npos) {
    return false;
  }
  std::string rest = path.substr(route + kRoute.size());
  auto* xlive_api = kernel_state()->GetXboxLiveAPI();
  if (!xlive_api) {
    return false;
  }

  if (verb == "GET") {
    static const std::regex kXuid("^/([0-9a-f]{16})$");
    std::smatch match;
    if (!std::regex_match(rest, match, kXuid)) {
      return false;
    }
    const uint64_t xuid = std::strtoull(match[1].str().c_str(), nullptr, 16);
    const auto privacy = xlive_api->GetFriendPrivacy(xuid);
    AnswerWith(
        request, 200, "OK",
        fmt::format("{{\"privacy\":{}}}", static_cast<uint32_t>(privacy)));
    XELOGI("XHttp: GET {} -> friend privacy {} for {:016X}", url,
           static_cast<uint32_t>(privacy), xuid);
    return true;
  }

  if (verb != "POST" || !(rest.empty() || rest == "/")) {
    return false;
  }
  const std::string body(request->request_body.begin(),
                         request->request_body.end());
  if (body.find("\"friends\"") != std::string::npos) {
    return false;
  }
  static const std::regex kXuidField("\"xuid\"\\s*:\\s*\"([0-9A-Fa-f]{16})\"");
  static const std::regex kPrivacyField("\"privacy\"\\s*:\\s*([0-2])");
  std::smatch xuid_match;
  std::smatch privacy_match;
  if (!std::regex_search(body, xuid_match, kXuidField) ||
      !std::regex_search(body, privacy_match, kPrivacyField)) {
    return false;
  }
  const uint64_t xuid = std::strtoull(xuid_match[1].str().c_str(), nullptr, 16);
  const auto privacy =
      static_cast<XLiveAPI::FriendPrivacy>(std::stoul(privacy_match[1].str()));
  const bool on_hub = xlive_api->SetFriendPrivacy(xuid, privacy);
  const std::string answer =
      fmt::format("{{\"privacy\":{}}}", static_cast<uint32_t>(privacy));
  // Saved locally either way; the app is told the truth about the hub,
  // because the hub is what gates friend requests.
  if (on_hub) {
    AnswerWith(request, 201, "Created", answer);
  } else {
    AnswerWith(request, 503, "Service Unavailable", answer);
  }
  XELOGI("XHttp: POST {} -> friend privacy {} for {:016X}, {}", url,
         static_cast<uint32_t>(privacy), xuid,
         on_hub ? "local and hub" : "local only (hub did not take it)");
  return true;
}

// ---- The dashboard hub apps' local routes ---------------------------------
// Dash.NexiaAddFriend/Messages/Voice.lex are Lua, and the dashboard's Lua has
// no API for the keyboard, another player's gamercard, a friend request or
// voice mail (checked against all 264 of its .lub files). What it has is
// Xbox.Net, and every request it makes comes through here, so those things
// are asked for as requests to <hub>/nexia/... and answered locally - the
// network never sees them. Every route takes its parameters from the query
// string (GET, which the Privacy app proved) or a JSON body (POST).

std::string PercentDecode(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < text.size() &&
               std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      out += static_cast<char>(
          std::strtoul(text.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

class HubParams {
 public:
  HubParams(const std::string& url, const std::vector<uint8_t>& body) {
    const size_t q = url.find('?');
    if (q != std::string::npos) {
      size_t at = q + 1;
      while (at <= url.size()) {
        size_t end = url.find('&', at);
        if (end == std::string::npos) {
          end = url.size();
        }
        const std::string pair = url.substr(at, end - at);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos) {
          query_[PercentDecode(pair.substr(0, eq))] =
              PercentDecode(pair.substr(eq + 1));
        }
        at = end + 1;
      }
    }
    if (!body.empty()) {
      const std::string text(body.begin(), body.end());
      json_.Parse(text.c_str());
    }
  }

  std::string Get(const char* name) const {
    auto it = query_.find(name);
    if (it != query_.end()) {
      return it->second;
    }
    if (json_.IsObject() && json_.HasMember(name)) {
      const auto& value = json_[name];
      if (value.IsString()) {
        return value.GetString();
      }
      if (value.IsUint64()) {
        return std::to_string(value.GetUint64());
      }
    }
    return "";
  }

  uint64_t Xuid(const char* name) const {
    std::string text = Get(name);
    if (text.rfind("Xuid:", 0) == 0) {
      text = text.substr(5);
    }
    if (text.size() != 16) {
      return 0;
    }
    char* end = nullptr;
    const uint64_t xuid = std::strtoull(text.c_str(), &end, 16);
    return end == text.c_str() + 16 ? xuid : 0;
  }

 private:
  std::map<std::string, std::string> query_;
  rapidjson::Document json_;
};

std::string JsonString(const std::string& text) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.String(text.c_str(), static_cast<rapidjson::SizeType>(text.size()));
  return buffer.GetString();
}

// At most `limit` characters (code points, not bytes) of UTF-8 text.
std::string TruncateUtf8(const std::string& text, size_t limit) {
  size_t count = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
      if (count == limit) {
        return text.substr(0, i);
      }
      ++count;
    }
  }
  return text;
}

// The Voice Messages app's recorder: the Nexia Voice Messages dialog's own
// recording steps (app/messages_dialog.cc), run on a thread of their own
// because nothing here draws a frame to pump them. The mixer captures at 16 kHz
// and mail is stored at 8 kHz, each pair averaged; a minute at most. The voice
// device is held while recording and while anything is playing, then let go.
class HubVoice {
 public:
  static constexpr uint32_t kCaptureRate = 16000;
  static constexpr uint32_t kStoreRate = 8000;
  static constexpr size_t kMaxSamples = 60 * kStoreRate;

  static HubVoice& Get() {
    static HubVoice* instance = new HubVoice();  // never torn down at exit
    return *instance;
  }

  bool Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_) {
      return true;
    }
    HoldLocked(std::chrono::hours(1));
    pcm_.clear();
    cursor_ = apu::sdl::VoiceChat::Get().capture_position();
    have_pending_ = false;
    recording_ = true;
    EnsureThreadLocked();
    return true;
  }

  // Leaves the clip for preview and send; the device stays up a while for it.
  uint32_t Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_) {
      PumpLocked();
      recording_ = false;
    }
    HoldLocked(std::chrono::minutes(2));
    return DurationMsLocked();
  }

  uint32_t Preview() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_ || pcm_.empty()) {
      return 0;
    }
    PlayLocked(pcm_, kStoreRate);
    return DurationMsLocked();
  }

  void Discard() {
    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = false;
    pcm_.clear();
    apu::sdl::VoiceChat::Get().StopPcmMessage();
    release_at_ = std::chrono::steady_clock::now();
  }

  // The clip, for sending; empty while still recording.
  std::vector<int16_t> Clip() {
    std::lock_guard<std::mutex> lock(mutex_);
    return recording_ ? std::vector<int16_t>() : pcm_;
  }

  void State(bool* out_recording, uint32_t* out_duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    *out_recording = recording_;
    *out_duration_ms = DurationMsLocked();
  }

  // A message from the hub (stored at its own rate).
  uint32_t Play(const std::vector<int16_t>& pcm, uint32_t sample_rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    PlayLocked(pcm, sample_rate ? sample_rate : kStoreRate);
    const uint32_t rate = sample_rate ? sample_rate : kStoreRate;
    return static_cast<uint32_t>(uint64_t(pcm.size()) * 1000 / rate);
  }

 private:
  uint32_t DurationMsLocked() const {
    return static_cast<uint32_t>(uint64_t(pcm_.size()) * 1000 / kStoreRate);
  }

  void HoldLocked(std::chrono::steady_clock::duration for_at_least) {
    if (!referenced_) {
      apu::sdl::VoiceChat::Get().AddRef();
      apu::sdl::VoiceChat::Get().SetMessagePlayback(true);
      referenced_ = true;
    }
    const auto until = std::chrono::steady_clock::now() + for_at_least;
    if (until > release_at_) {
      release_at_ = until;
    }
    EnsureThreadLocked();
  }

  void PlayLocked(const std::vector<int16_t>& pcm, uint32_t sample_rate) {
    const uint64_t ms = uint64_t(pcm.size()) * 1000 / sample_rate;
    HoldLocked(std::chrono::milliseconds(ms + 2000));
    if (sample_rate == kCaptureRate) {
      apu::sdl::VoiceChat::Get().PlayPcmMessage(pcm.data(), pcm.size());
      return;
    }
    // 8 kHz back up to what the mixer plays, each sample held twice - as the
    // dialog does.
    std::vector<int16_t> playable;
    playable.reserve(pcm.size() * 2);
    for (const int16_t sample : pcm) {
      playable.push_back(sample);
      playable.push_back(sample);
    }
    apu::sdl::VoiceChat::Get().PlayPcmMessage(playable.data(), playable.size());
  }

  // Everything captured since the last pump, exactly once (the dialog's
  // PumpRecording).
  void PumpLocked() {
    int16_t buffer[4096];
    while (recording_) {
      const size_t read = apu::sdl::VoiceChat::Get().ReadCaptureSince(
          cursor_, buffer, sizeof(buffer) / sizeof(buffer[0]));
      if (!read) {
        break;
      }
      size_t i = 0;
      if (have_pending_) {
        pcm_.push_back(
            static_cast<int16_t>((int32_t(pending_) + int32_t(buffer[0])) / 2));
        have_pending_ = false;
        i = 1;
      }
      for (; i + 1 < read; i += 2) {
        pcm_.push_back(static_cast<int16_t>(
            (int32_t(buffer[i]) + int32_t(buffer[i + 1])) / 2));
      }
      if (i < read) {
        pending_ = buffer[i];
        have_pending_ = true;
      }
      if (pcm_.size() >= kMaxSamples) {
        pcm_.resize(kMaxSamples);
        recording_ = false;
        release_at_ =
            std::chrono::steady_clock::now() + std::chrono::minutes(2);
      }
    }
  }

  void EnsureThreadLocked() {
    if (thread_running_) {
      return;
    }
    thread_running_ = true;
    std::thread([this]() {
      while (true) {
        xe::threading::Sleep(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> lock(mutex_);
        PumpLocked();
        if (!recording_ && referenced_ &&
            std::chrono::steady_clock::now() >= release_at_) {
          apu::sdl::VoiceChat::Get().SetMessagePlayback(false);
          apu::sdl::VoiceChat::Get().Release();
          referenced_ = false;
        }
        if (!recording_ && !referenced_) {
          thread_running_ = false;
          return;
        }
      }
    }).detach();
  }

  std::mutex mutex_;
  bool recording_ = false;
  bool referenced_ = false;
  bool thread_running_ = false;
  std::chrono::steady_clock::time_point release_at_;
  std::vector<int16_t> pcm_;
  uint64_t cursor_ = 0;
  bool have_pending_ = false;
  int16_t pending_ = 0;
};

// The dashboard's Account, Profile and Preferences apps. They act on the
// profile signed in to slot 0 - the one registered with the hub.
UserProfile* HubAppProfile() {
  return kernel_state()->xam_state()->GetUserProfile(uint32_t(0));
}

std::string ProfileText(UserProfile* profile, UserSettingId id) {
  const auto setting = kernel_state()->xam_state()->user_tracker()->GetSetting(
      profile, kDashboardID, static_cast<uint32_t>(id));
  if (!setting) {
    return {};
  }
  const auto data = setting->get_host_data();
  if (const auto* text = std::get_if<std::u16string>(&data)) {
    std::u16string trimmed = *text;
    while (!trimmed.empty() && trimmed.back() == u'\0') {
      trimmed.pop_back();
    }
    return xe::to_utf8(trimmed);
  }
  return {};
}

int32_t ProfileInt(UserProfile* profile, UserSettingId id) {
  const auto setting = kernel_state()->xam_state()->user_tracker()->GetSetting(
      profile, kDashboardID, static_cast<uint32_t>(id));
  if (!setting) {
    return 0;
  }
  const auto data = setting->get_host_data();
  if (const auto* value = std::get_if<int32_t>(&data)) {
    return *value;
  }
  if (const auto* value = std::get_if<uint32_t>(&data)) {
    return static_cast<int32_t>(*value);
  }
  return 0;
}

// Writes a dashboard setting of the signed-in profile and sends it to the
// hub (the gamercard others see is built from what the hub holds).
void StoreProfileSetting(UserProfile* profile, UserSettingId id,
                         UserDataTypes value) {
  UserSetting setting(id, value);
  kernel_state()->xam_state()->user_tracker()->UpsertSetting(
      profile->xuid(), kDashboardID, &setting);
  kernel_state()->BroadcastNotification(
      kXNotificationSystemProfileSettingChanged, 1);
  if (const uint64_t online_xuid = profile->GetOnlineXUID()) {
    user_settingids_map settings = {};
    settings[online_xuid][kDashboardID].push_back(id);
    kernel_state()->GetXboxLiveAPI()->SetUsersSettings(settings);
  }
}

// Rewrites the account record of a profile the way a gamertag change on the
// console does: signed out, rewritten, signed back in - which registers with
// the hub again under the new name or xuid. Runs on its own thread after the
// app has its answer, because signing out ends the app.
void RewriteAccountLater(uint64_t xuid,
                         std::function<void(X_XAMACCOUNTINFO&)> change) {
  std::thread([xuid, change]() {
    xe::threading::Sleep(std::chrono::milliseconds(500));
    auto* manager = kernel_state()->xam_state()->profile_manager();
    const X_XAMACCOUNTINFO* current = manager->GetAccount(xuid);
    if (!current) {
      XELOGE("Account: profile {:016X} is gone; nothing rewritten", xuid);
      return;
    }
    X_XAMACCOUNTINFO account = *current;
    if (!manager->ModifyAccount(xuid, account,
                                [&change](X_XAMACCOUNTINFO& target) {
                                  change(target);
                                  return true;
                                })) {
      XELOGE("Account: could not rewrite profile {:016X}", xuid);
    }
  }).detach();
}

void SetAccountGamertag(X_XAMACCOUNTINFO& account,
                        const std::string& gamertag) {
  string_util::copy_and_swap_truncating(
      account.gamertag, xe::to_utf16(gamertag), std::size(account.gamertag));
}

// Preferences: Nexia settings a player would look for on the console.
struct HubPreference {
  const char* id;
  const char* name;
  const char* description;
};
constexpr HubPreference kHubPreferences[] = {
    {"voice_chat", "Voice Chat",
     "Talk with other players in games and parties."},
    {"show_achievement_notification", "Achievement Notifications",
     "Show a notification when you unlock an achievement."},
    {"vibration", "Controller Vibration", "Let games rumble your controller."},
    {"discord", "Share Activity on Discord",
     "Show the game you are playing on your Discord profile."},
    {"upnp", "Automatic Port Forwarding",
     "Open the ports online games need on your router (UPnP)."},
    {"auto_check_updates", "Check for Nexia Updates",
     "Look for a new version of Nexia when it starts."},
};

cvar::ConfigVar<bool>* BoolConfigVar(const char* name) {
  if (!cvar::ConfigVars) {
    return nullptr;
  }
  auto it = cvar::ConfigVars->find(name);
  if (it == cvar::ConfigVars->end()) {
    return nullptr;
  }
  return dynamic_cast<cvar::ConfigVar<bool>*>(it->second);
}

std::optional<bool> PreferenceValue(const std::string& id) {
  if (id == "voice_chat") {
    return apu::sdl::VoiceChat::Get().enabled();
  }
  if (auto* config_var = BoolConfigVar(id.c_str())) {
    return *config_var->current_value();
  }
  return std::nullopt;
}

bool SetPreference(const std::string& id, bool value) {
  if (id == "voice_chat") {
    apu::sdl::VoiceChat::Get().SetEnabled(value);
    return true;
  }
  for (const auto& preference : kHubPreferences) {
    if (id == preference.id) {
      auto* config_var = BoolConfigVar(preference.id);
      if (!config_var) {
        return false;
      }
      config_var->OverrideConfigValue(value);
      config::SaveConfig();
      return true;
    }
  }
  return false;
}

// Answers the Account / Profile / Preferences routes; false for any other.
bool AnswerHubAccountApps(XHttpHandle* request, const std::string& route,
                          const HubParams& params) {
  auto* xlive_api = kernel_state()->GetXboxLiveAPI();

  if (route == "account") {
    UserProfile* profile = HubAppProfile();
    if (!profile || !profile->GetOnlineXUID()) {
      AnswerWith(request, 200, "OK", "{\"signedIn\":false}");
      return true;
    }
    const uint64_t online_xuid = profile->GetOnlineXUID();
    const auto state = xlive_api->GetAccountState(online_xuid);
    AnswerWith(
        request, 200, "OK",
        fmt::format("{{\"signedIn\":true,\"xuid\":\"{:016X}\",\"gamertag\":{},"
                    "\"reachable\":{},\"claimed\":{},\"tokenValid\":{},"
                    "\"claimedAs\":{},\"refusal\":{}}}",
                    online_xuid, JsonString(profile->name()),
                    state.reachable ? "true" : "false",
                    state.claimed ? "true" : "false",
                    state.token_valid ? "true" : "false",
                    JsonString(state.gamertag),
                    JsonString(xlive_api->register_refusal())));
    return true;
  }

  if (route == "account/claim") {
    UserProfile* profile = HubAppProfile();
    if (!profile || !profile->GetOnlineXUID()) {
      AnswerWith(request, 200, "OK", "{\"outcome\":\"signedout\"}");
      return true;
    }
    const auto result = xlive_api->ClaimAccount(
        profile->GetOnlineXUID(), profile->name(), params.Get("password"));
    AnswerWith(
        request, 200, "OK",
        fmt::format("{{\"outcome\":{},\"field\":{}}}",
                    JsonString(result.outcome), JsonString(result.field)));
    XELOGI("Account: claim {} -> {}", profile->name(), result.outcome);
    return true;
  }

  if (route == "account/recover") {
    UserProfile* profile = HubAppProfile();
    if (!profile) {
      AnswerWith(request, 200, "OK", "{\"outcome\":\"signedout\"}");
      return true;
    }
    const auto result = xlive_api->RecoverAccount(params.Get("gamertag"),
                                                  params.Get("password"));
    AnswerWith(
        request, 200, "OK",
        fmt::format("{{\"outcome\":{},\"gamertag\":{}}}",
                    JsonString(result.outcome), JsonString(result.gamertag)));
    XELOGI("Account: recover -> {}", result.outcome);
    if (result.outcome == "recovered") {
      // This profile becomes the recovered account: its online xuid and its
      // gamertag. The token is already stored under that xuid, so the
      // sign-in that follows registers with it.
      const uint64_t recovered_xuid = result.xuid;
      const std::string gamertag = result.gamertag;
      RewriteAccountLater(profile->xuid(), [recovered_xuid, gamertag](
                                               X_XAMACCOUNTINFO& account) {
        account.xuid_online = recovered_xuid;
        account.ToggleLiveFlag(true);
        SetAccountGamertag(account, gamertag);
      });
    }
    return true;
  }

  if (route == "account/password") {
    UserProfile* profile = HubAppProfile();
    if (!profile || !profile->GetOnlineXUID()) {
      AnswerWith(request, 200, "OK", "{\"outcome\":\"signedout\"}");
      return true;
    }
    const auto result = xlive_api->ChangeAccountPassword(
        profile->GetOnlineXUID(), params.Get("password"),
        params.Get("newPassword"));
    AnswerWith(
        request, 200, "OK",
        fmt::format("{{\"outcome\":{},\"field\":{}}}",
                    JsonString(result.outcome), JsonString(result.field)));
    XELOGI("Account: password change -> {}", result.outcome);
    return true;
  }

  if (route == "profile") {
    UserProfile* profile = HubAppProfile();
    if (!profile) {
      AnswerWith(request, 200, "OK", "{\"signedIn\":false}");
      return true;
    }
    AnswerWith(
        request, 200, "OK",
        fmt::format(
            "{{\"signedIn\":true,\"xuid\":\"{:016X}\",\"gamertag\":{},"
            "\"motto\":{},\"name\":{},\"location\":{},\"bio\":{},"
            "\"zone\":{}}}",
            profile->GetOnlineXUID(), JsonString(profile->name()),
            JsonString(
                ProfileText(profile, UserSettingId::XPROFILE_GAMERCARD_MOTTO)),
            JsonString(ProfileText(
                profile, UserSettingId::XPROFILE_GAMERCARD_USER_NAME)),
            JsonString(ProfileText(
                profile, UserSettingId::XPROFILE_GAMERCARD_USER_LOCATION)),
            JsonString(ProfileText(profile,
                                   UserSettingId::XPROFILE_GAMERCARD_USER_BIO)),
            ProfileInt(profile, UserSettingId::XPROFILE_GAMERCARD_ZONE)));
    return true;
  }

  if (route == "profile/set") {
    UserProfile* profile = HubAppProfile();
    if (!profile) {
      AnswerWith(request, 200, "OK", "{\"outcome\":\"signedout\"}");
      return true;
    }
    const std::string field = params.Get("field");
    const std::string value = params.Get("value");

    struct TextField {
      const char* name;
      UserSettingId id;
      size_t limit;
    };
    // Character limits are the settings' own sizes, less the terminator.
    static constexpr TextField kTextFields[] = {
        {"motto", UserSettingId::XPROFILE_GAMERCARD_MOTTO, 21},
        {"name", UserSettingId::XPROFILE_GAMERCARD_USER_NAME, 64},
        {"location", UserSettingId::XPROFILE_GAMERCARD_USER_LOCATION, 40},
        {"bio", UserSettingId::XPROFILE_GAMERCARD_USER_BIO, 499},
    };
    for (const auto& text_field : kTextFields) {
      if (field == text_field.name) {
        StoreProfileSetting(
            profile, text_field.id,
            xe::to_utf16(TruncateUtf8(value, text_field.limit)));
        AnswerWith(request, 200, "OK", "{\"outcome\":\"saved\"}");
        return true;
      }
    }

    if (field == "zone") {
      const int32_t zone = std::atoi(value.c_str());
      if (zone < 0 || zone > 4) {
        AnswerWith(request, 200, "OK", "{\"outcome\":\"invalid\"}");
        return true;
      }
      StoreProfileSetting(profile, UserSettingId::XPROFILE_GAMERCARD_ZONE,
                          zone);
      AnswerWith(request, 200, "OK", "{\"outcome\":\"saved\"}");
      return true;
    }

    if (field == "gamertag") {
      if (!ProfileManager::IsGamertagValid(value)) {
        AnswerWith(request, 200, "OK", "{\"outcome\":\"invalid\"}");
        return true;
      }
      if (value == profile->name()) {
        AnswerWith(request, 200, "OK", "{\"outcome\":\"saved\"}");
        return true;
      }
      // A claimed account moves its claim first; anyone else may only take
      // a gamertag nobody has claimed.
      const uint64_t online_xuid = profile->GetOnlineXUID();
      const auto state = xlive_api->GetAccountState(online_xuid);
      if (state.claimed) {
        const auto result = xlive_api->RenameAccount(online_xuid, value);
        if (result.outcome != "renamed") {
          AnswerWith(
              request, 200, "OK",
              fmt::format("{{\"outcome\":{}}}", JsonString(result.outcome)));
          return true;
        }
      } else if (xlive_api->IsGamertagAvailable(value) == false) {
        AnswerWith(request, 200, "OK", "{\"outcome\":\"taken\"}");
        return true;
      }
      RewriteAccountLater(profile->xuid(), [value](X_XAMACCOUNTINFO& account) {
        SetAccountGamertag(account, value);
      });
      AnswerWith(request, 200, "OK", "{\"outcome\":\"renaming\"}");
      XELOGI("Profile: gamertag {} -> {}", profile->name(), value);
      return true;
    }

    AnswerWith(request, 200, "OK", "{\"outcome\":\"invalid\"}");
    return true;
  }

  if (route == "profile/gamerpic") {
    kernel_state()->emulator()->on_gamerpic_browser();
    AnswerWith(request, 200, "OK", "{\"shown\":true}");
    return true;
  }

  if (route == "prefs") {
    std::string body = "{\"items\":[";
    bool first = true;
    for (const auto& preference : kHubPreferences) {
      const auto value = PreferenceValue(preference.id);
      if (!value) {
        continue;
      }
      body += fmt::format(
          "{}{{\"id\":\"{}\",\"name\":{},\"description\":{},\"on\":{}}}",
          first ? "" : ",", preference.id, JsonString(preference.name),
          JsonString(preference.description), *value ? "true" : "false");
      first = false;
    }
    body += "]}";
    AnswerWith(request, 200, "OK", body);
    return true;
  }

  if (route == "prefs/set") {
    const std::string id = params.Get("id");
    const bool on = params.Get("on") == "1";
    const bool saved = SetPreference(id, on);
    AnswerWith(
        request, 200, "OK",
        fmt::format("{{\"outcome\":\"{}\"}}", saved ? "saved" : "invalid"));
    XELOGI("Preferences: {} -> {}", id, saved ? (on ? "on" : "off") : "?");
    return true;
  }

  return false;
}

bool AnswerNexiaHub(XHttpHandle* request, const std::string& verb,
                    const std::string& url) {
  const std::string lowered = LowerBytes(url);
  const std::string path = lowered.substr(0, lowered.find('?'));
  constexpr std::string_view kRoot = "/nexia/";
  const size_t root = path.find(kRoot);
  if (root == std::string::npos) {
    return false;
  }
  // No scheme or host before it: only <host>/nexia/..., never a path that
  // merely contains it.
  const size_t host_end = path.find('/', path.find("//") + 2);
  if (host_end != root) {
    return false;
  }
  const std::string route = path.substr(root + kRoot.size());
  const HubParams params(url, request->request_body);
  auto* xlive_api = kernel_state()->GetXboxLiveAPI();

  if (AnswerHubAccountApps(request, route, params)) {
    return true;
  }

  if (route == "keyboard") {
    const std::string title = params.Get("title");
    const uint32_t ticket =
        OpenHubKeyboard(title.empty() ? "Message" : title, params.Get("text"),
                        params.Get("password") == "1");
    if (!ticket) {
      AnswerWith(request, 200, "OK", "{\"ticket\":0}");
    } else {
      AnswerWith(request, 200, "OK", fmt::format("{{\"ticket\":{}}}", ticket));
    }
    XELOGI("XHttp: {} {} -> hub keyboard ticket {}", verb, url, ticket);
    return true;
  }

  if (route.rfind("keyboard/", 0) == 0) {
    const uint32_t ticket = static_cast<uint32_t>(
        std::strtoul(route.substr(9).c_str(), nullptr, 10));
    bool cancelled = false;
    std::string text;
    if (!PollHubKeyboard(ticket, &cancelled, &text)) {
      AnswerWith(request, 200, "OK", "{\"done\":false}");
      return true;
    }
    size_t limit = 240;
    const std::string max = params.Get("max");
    if (!max.empty()) {
      limit =
          std::clamp<size_t>(std::strtoul(max.c_str(), nullptr, 10), 1, 240);
    }
    text = TruncateUtf8(text, limit);
    AnswerWith(request, 200, "OK",
               fmt::format("{{\"done\":true,\"cancelled\":{},\"text\":{}}}",
                           cancelled ? "true" : "false", JsonString(text)));
    XELOGI("XHttp: hub keyboard {} {}", ticket,
           cancelled ? "cancelled" : "done");
    return true;
  }

  if (route == "gamercard") {
    const uint64_t xuid = params.Xuid("xuid");
    const uint32_t result = ShowHubGamerCard(0, xuid);
    if (result == X_ERROR_SUCCESS) {
      AnswerWith(request, 200, "OK", "{\"shown\":true}");
    } else {
      AnswerWith(request, 200, "OK", "{\"shown\":false}");
    }
    XELOGI("XHttp: {} {} -> gamercard {:016X}: {:08X}", verb, url, xuid,
           result);
    return true;
  }

  // Nexia's own friends list - the one the Friends dialog shows. The hub only
  // knows what a client has published, so the hub apps ask here.
  if (route == "friends") {
    std::string body = "{\"friends\":[";
    bool first = true;
    for (const auto& [xuid, gamertag] : HubFriends(0)) {
      body += fmt::format("{}{{\"xuid\":\"{:016X}\",\"gamertag\":{}}}",
                          first ? "" : ",", xuid, JsonString(gamertag));
      first = false;
    }
    body += "]}";
    AnswerWith(request, 200, "OK", body);
    return true;
  }

  // A player's gamerpic for the hub apps' card. An XUI image cannot take a
  // web address here (17559 has no LoadRemoteImage, and an https:// path is
  // never fetched), but it reads memory://<address>,<size> - the dash builds
  // those itself for pictures it has in memory. The tile comes from
  // friends.sqlite where it is cached (friends, and SeenPlayers for everyone
  // else), fetched and stored only when it is not (GetFriendGamerpic, the
  // ImGui dialogs' own source); it is copied into guest memory once and kept,
  // and the app gets that path.
  if (route == "gamerpic") {
    static std::mutex gamerpic_mutex;
    static std::map<uint64_t, std::pair<uint32_t, uint32_t>> gamerpics;
    const uint64_t xuid = params.Xuid("xuid");
    std::pair<uint32_t, uint32_t> found = {0, 0};
    if (xuid) {
      {
        std::lock_guard<std::mutex> lock(gamerpic_mutex);
        if (auto it = gamerpics.find(xuid); it != gamerpics.end()) {
          found = it->second;
        }
      }
      auto* profile = kernel_state()->xam_state()->GetUserProfile(uint32_t(0));
      if (!found.first && profile) {
        const std::vector<uint8_t> tile =
            GetFriendGamerpic(profile->xuid(), xuid, false);
        if (!tile.empty()) {
          const uint32_t size = static_cast<uint32_t>(tile.size());
          const uint32_t address =
              kernel_state()->memory()->SystemHeapAlloc(size);
          if (address) {
            std::memcpy(
                kernel_state()->memory()->TranslateVirtual<uint8_t*>(address),
                tile.data(), size);
            found = {address, size};
            std::lock_guard<std::mutex> lock(gamerpic_mutex);
            gamerpics[xuid] = found;
          }
        }
      }
    }
    if (found.first) {
      AnswerWith(request, 200, "OK",
                 fmt::format("{{\"path\":\"memory://{:X},{:X}\"}}", found.first,
                             found.second));
    } else {
      AnswerWith(request, 200, "OK", "{\"path\":\"\"}");
    }
    return true;
  }

  if (route == "friendrequest") {
    const uint64_t target = params.Xuid("target");
    // The name the app already shows, so the new friend is not "Friend N".
    const uint32_t outcome =
        target ? RequestHubFriend(0, target, params.Get("gamertag")) : 2;
    static const char* kOutcomes[] = {"accepted", "pending", "failed"};
    // Always 200: Xbox.Net.HttpGet hands Lua the body only on success, and
    // the outcome is the answer.
    AnswerWith(request, 200, "OK",
               fmt::format("{{\"outcome\":\"{}\"}}", kOutcomes[outcome]));
    XELOGI("XHttp: {} {} -> friend request to {:016X}: {}", verb, url, target,
           kOutcomes[outcome]);
    return true;
  }

  if (route == "unfriend") {
    const uint64_t target = params.Xuid("target");
    const bool removed = target && RemoveHubFriend(0, target);
    AnswerWith(request, 200, "OK",
               removed ? "{\"removed\":true}" : "{\"removed\":false}");
    return true;
  }

  // Join Game: whether the friend's game (title=<hex id>, update=<label>,
  // hash=<the update's content hash>) is here with the same update, and then
  // starting it. The answer goes out before the launch: the switch tears the
  // dash down.
  if (route == "joinable" || route == "joingame") {
    const uint32_t title_id = static_cast<uint32_t>(
        std::strtoul(params.Get("title").c_str(), nullptr, 16));
    const std::string update = params.Get("update");
    const std::string hash = params.Get("hash");
    const HubJoinTarget target = FindHubJoinTarget(title_id, update, hash);
    if (route == "joinable") {
      AnswerWith(
          request, 200, "OK",
          fmt::format("{{\"ok\":{},\"reason\":{}}}",
                      target.ok ? "true" : "false", JsonString(target.reason)));
      return true;
    }
    const uint64_t inviter = params.Xuid("target");
    const bool can = target.ok && inviter;
    AnswerWith(request, 200, "OK",
               fmt::format("{{\"started\":{},\"reason\":{}}}",
                           can ? "true" : "false", JsonString(target.reason)));
    if (can && !StartHubJoin(inviter, title_id, update, hash)) {
      XELOGE("XHttp: Join Game into {:08X} for {:016X} did not start", title_id,
             inviter);
    }
    return true;
  }

  // Sending goes through XLiveAPI for the same reason as the friend request:
  // the hub's rule comes back as the outcome the dialog already reports.
  auto outcome_answer = [&](SendMessageOutcome outcome) -> const char* {
    switch (outcome) {
      case SendMessageOutcome::kSent:
        AnswerWith(request, 200, "OK", "{\"outcome\":\"sent\"}");
        return "sent";
      case SendMessageOutcome::kDenied:
        AnswerWith(request, 200, "OK", "{\"outcome\":\"denied\"}");
        return "denied";
      default:
        AnswerWith(request, 200, "OK", "{\"outcome\":\"invalid\"}");
        return "invalid";
    }
  };

  if (route == "message/send") {
    const uint64_t from = params.Xuid("xuid");
    const uint64_t target = params.Xuid("target");
    const std::string text = TruncateUtf8(params.Get("text"), 240);
    const char* result = "invalid";
    if (!xlive_api || !from || !target || text.empty()) {
      AnswerWith(request, 200, "OK", "{\"outcome\":\"invalid\"}");
    } else {
      result = outcome_answer(xlive_api->SendTextMessage(from, target, text));
    }
    XELOGI("XHttp: hub text message to {:016X}: {}", target, result);
    return true;
  }

  if (route == "message/read") {
    const uint64_t xuid = params.Xuid("xuid");
    const std::string id = params.Get("id");
    if (xlive_api && xuid && !id.empty()) {
      xlive_api->MarkMessageRead(xuid, id);
      xlive_api->RefreshMessageCounts();
    }
    AnswerWith(request, 200, "OK", "{}");
    return true;
  }

  if (route == "voice/record") {
    HubVoice::Get().Start();
    AnswerWith(request, 200, "OK", "{\"recording\":true}");
    XELOGI("XHttp: hub voice recording started");
    return true;
  }
  if (route == "voice/stop") {
    const uint32_t ms = HubVoice::Get().Stop();
    AnswerWith(request, 200, "OK", fmt::format("{{\"durationMs\":{}}}", ms));
    XELOGI("XHttp: hub voice recording stopped, {} ms", ms);
    return true;
  }
  if (route == "voice/state") {
    bool recording = false;
    uint32_t ms = 0;
    HubVoice::Get().State(&recording, &ms);
    AnswerWith(request, 200, "OK",
               fmt::format("{{\"recording\":{},\"durationMs\":{}}}",
                           recording ? "true" : "false", ms));
    return true;
  }
  if (route == "voice/preview") {
    const uint32_t ms = HubVoice::Get().Preview();
    AnswerWith(request, 200, "OK", fmt::format("{{\"durationMs\":{}}}", ms));
    return true;
  }
  if (route == "voice/discard") {
    HubVoice::Get().Discard();
    AnswerWith(request, 200, "OK", "{}");
    return true;
  }
  if (route == "voice/send") {
    const uint64_t from = params.Xuid("xuid");
    const uint64_t target = params.Xuid("target");
    const std::vector<int16_t> clip = HubVoice::Get().Clip();
    const char* result = "invalid";
    if (!xlive_api || !from || !target || clip.empty()) {
      AnswerWith(request, 200, "OK", "{\"outcome\":\"invalid\"}");
    } else {
      result = outcome_answer(xlive_api->SendVoiceMessage(
          from, target, clip, HubVoice::kStoreRate));
      if (std::string_view(result) == "sent") {
        HubVoice::Get().Discard();
      }
    }
    XELOGI("XHttp: hub voice message to {:016X} ({} samples): {}", target,
           clip.size(), result);
    return true;
  }
  if (route == "voice/play") {
    const uint64_t xuid = params.Xuid("xuid");
    const std::string id = params.Get("id");
    std::vector<int16_t> pcm;
    uint32_t sample_rate = 0;
    if (!xlive_api || !xuid || id.empty() ||
        !xlive_api->GetMessageAudio(xuid, id, pcm, sample_rate) ||
        pcm.empty()) {
      AnswerWith(request, 200, "OK", "{\"durationMs\":0}");
      XELOGI("XHttp: hub voice message {} could not be fetched", id);
      return true;
    }
    const uint32_t ms = HubVoice::Get().Play(pcm, sample_rate);
    xlive_api->MarkMessageRead(xuid, id);
    xlive_api->RefreshMessageCounts();
    AnswerWith(request, 200, "OK", fmt::format("{{\"durationMs\":{}}}", ms));
    XELOGI("XHttp: hub voice message {} playing, {} ms", id, ms);
    return true;
  }

  AnswerWith(request, 404, "Not Found", "{}");
  XELOGW("XHttp: {} {} -> no such hub app route", verb, url);
  return true;
}

// The host part of scheme://host[:port]/..., lowercase.
std::string UrlHost(const std::string& url) {
  const size_t scheme = url.find("://");
  const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
  const size_t end = url.find_first_of(":/?", start);
  return LowerBytes(url.substr(
      start, end == std::string::npos ? std::string::npos : end - start));
}

// Our hub, as XLiveAPI addresses it.
bool IsHubUrl(const std::string& url) {
  const std::string hub = UrlHost(XLiveAPI::GetApiAddress());
  return !hub.empty() && UrlHost(url) == hub;
}

bool Perform(XHttpHandle* request) {
  auto* manager = XHttpManager::Get();
  auto* session = manager->SessionOf(request);

  const std::string url = BuildUrl(request);
  if (url.empty()) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_STATE);
    return false;
  }

  const std::string request_verb =
      request->verb.empty() ? "GET" : request->verb;
  if (AnswerDiscInTray(request, request_verb, url)) {
    return true;
  }
  if (AnswerFriendPrivacy(request, request_verb, url)) {
    return true;
  }
  if (AnswerNexiaHub(request, request_verb, url)) {
    return true;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    XThread::SetLastError(XHTTP_ERROR_INTERNAL_ERROR);
    return false;
  }

  request->response_body.clear();
  request->raw_headers.clear();
  request->read_pos = 0;

  struct curl_slist* headers = nullptr;
  for (const std::string& header : request->request_headers) {
    headers = curl_slist_append(headers, header.c_str());
  }
  // Tell the hub what this Nexia can do, so it only hands the dashboard what
  // it can open: channel-packages = a channel:// .xzp the hub publishes is
  // fetched and served (xam_info.cc DashChannelPackageRedirect), which the
  // online Social channel needs. Hub requests only.
  const bool hub_url = IsHubUrl(url);
  if (hub_url) {
    headers = curl_slist_append(headers, "X-Nexia-Features: channel-packages");
    if (auto* xlive_api = kernel_state()->GetXboxLiveAPI()) {
      const std::string token = xlive_api->hub_session_token();
      if (!token.empty()) {
        headers =
            curl_slist_append(headers, ("X-Nexia-Session: " + token).c_str());
      }
    }
  }

  std::string verb = request->verb.empty() ? "GET" : request->verb;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, verb.c_str());
  const auto disable = request->options.find(XHTTP_OPTION_DISABLE_FEATURE);
  const auto max_redirects = request->options.find(89);
  const bool no_redirects =
      (disable != request->options.end() && (disable->second & 0x2)) ||
      (max_redirects != request->options.end() && max_redirects->second == 0);
  XELOGI("XHttp: redirects {} (disable_feature {:X}, max_redirects {})",
         no_redirects ? "off" : "on",
         disable != request->options.end() ? disable->second : 0,
         max_redirects != request->options.end()
             ? static_cast<int64_t>(max_redirects->second)
             : -1);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, no_redirects ? 0L : 1L);
  if (!no_redirects && max_redirects != request->options.end()) {
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,
                     static_cast<long>(max_redirects->second));
  }
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (headers) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }
  if (hub_url) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nexia360");
  } else if (session && !session->agent.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, session->agent.c_str());
  } else {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Xbox");
  }
  if (session) {
    if (session->connect_timeout) {
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                       static_cast<long>(session->connect_timeout));
    }
    if (session->receive_timeout) {
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                       static_cast<long>(session->receive_timeout));
    }
    if (session->access_type == XHTTP_ACCESS_TYPE_NAMED_PROXY &&
        !session->proxy.empty()) {
      curl_easy_setopt(curl, CURLOPT_PROXY, session->proxy.c_str());
    } else if (session->access_type == XHTTP_ACCESS_TYPE_NO_PROXY) {
      curl_easy_setopt(curl, CURLOPT_PROXY, "");
    }
  }
  if (!request->user_name.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, request->user_name.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, request->password.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
  }
  if (!request->request_body.empty()) {
    curl_easy_setopt(
        curl, CURLOPT_POSTFIELDS,
        reinterpret_cast<const char*>(request->request_body.data()));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(request->request_body.size()));
  } else if (verb == "POST" || verb == "PUT") {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &request->response_body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, request);

  auto& counters = manager->counters();
  counters.requests_sent = counters.requests_sent + 1;
  counters.connects = counters.connects + 1;
  counters.name_resolutions = counters.name_resolutions + 1;
  counters.bytes_sent =
      counters.bytes_sent + static_cast<uint32_t>(request->request_body.size());

  const CURLcode result = curl_easy_perform(curl);

  if (result == CURLE_OK) {
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    request->status_code = static_cast<uint32_t>(code);
    // Every guest fetch, with the URL the guest actually built. This is how we
    // learn which files a title (the dash's XMan/Epix channel in particular)
    // expects the hub to serve.
    XELOGW("XHttp: {} {} -> {} ({} bytes)", verb, url, code,
           request->response_body.size());
    request->auth_schemes = 0;
    const std::string authenticate = HeaderValue(request, "WWW-Authenticate");
    if (!authenticate.empty()) {
      const std::string lowered = LowerBytes(authenticate);
      if (lowered.find("basic") != std::string::npos) {
        request->auth_schemes |= XHTTP_AUTH_SCHEME_BASIC;
      }
      if (lowered.find("digest") != std::string::npos) {
        request->auth_schemes |= XHTTP_AUTH_SCHEME_DIGEST;
      }
      if (lowered.find("ntlm") != std::string::npos) {
        request->auth_schemes |= XHTTP_AUTH_SCHEME_NTLM;
      }
      if (lowered.find("negotiate") != std::string::npos) {
        request->auth_schemes |= XHTTP_AUTH_SCHEME_NEGOTIATE;
      }
    }
    counters.responses_received = counters.responses_received + 1;
    counters.bytes_received =
        counters.bytes_received +
        static_cast<uint32_t>(request->response_body.size());
    request->received = true;
  } else {
    counters.errors = counters.errors + 1;
    uint32_t error = XHTTP_ERROR_CONNECTION_ERROR;
    switch (result) {
      case CURLE_UNSUPPORTED_PROTOCOL:
        error = XHTTP_ERROR_UNRECOGNIZED_SCHEME;
        break;
      case CURLE_COULDNT_RESOLVE_HOST:
      case CURLE_COULDNT_RESOLVE_PROXY:
        error = XHTTP_ERROR_NAME_NOT_RESOLVED;
        break;
      case CURLE_OPERATION_TIMEDOUT:
        error = XHTTP_ERROR_TIMEOUT;
        break;
      case CURLE_PEER_FAILED_VERIFICATION:
      case CURLE_SSL_CONNECT_ERROR:
        error = XHTTP_ERROR_SECURE_FAILURE;
        break;
      case CURLE_TOO_MANY_REDIRECTS:
        error = XHTTP_ERROR_REDIRECT_FAILED;
        break;
      default:
        break;
    }
    XThread::SetLastError(error);
    XELOGE("XHttp: {} {} failed: {}", verb, url, curl_easy_strerror(result));
  }

  if (headers) {
    curl_slist_free_all(headers);
  }
  curl_easy_cleanup(curl);

  return result == CURLE_OK;
}

bool EnsureReceived(XHttpHandle* request) {
  if (request->received) {
    return true;
  }
  if (!request->sent) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_STATE);
    return false;
  }
  return Perform(request);
}

bool CopyOut(uint32_t buffer_ptr, uint32_t length_ptr, const void* data,
             uint32_t size) {
  auto* length =
      length_ptr
          ? kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(length_ptr)
          : nullptr;
  if (!buffer_ptr || !length || *length < size) {
    if (length) {
      *length = size;
    }
    XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
    return false;
  }
  std::memcpy(kernel_memory()->TranslateVirtual<void*>(buffer_ptr), data, size);
  *length = size;
  return true;
}

}  // namespace

std::vector<uint8_t> FetchTitleBoxArt(uint32_t title_id) {
  return FetchDiscBoxArt(title_id);
}

namespace {

constexpr std::string_view kIndieCoverArchive =
    "https://archive.org/download/xbox-360-indie-games-rom/Cover/";

std::string IndieKey(std::string_view name) {
  std::string key;
  for (const unsigned char c : name) {
    if (std::isalnum(c)) {
      key.push_back(char(std::tolower(c)));
    }
  }
  return key;
}

std::string PercentEncode(std::string_view text) {
  std::string out;
  for (const unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(char(c));
    } else {
      out += fmt::format("%{:02X}", c);
    }
  }
  return out;
}

std::string HtmlUnescape(std::string text) {
  static constexpr std::pair<std::string_view, std::string_view> kEntities[] = {
      {"&amp;", "&"},   {"&lt;", "<"},  {"&gt;", ">"},
      {"&quot;", "\""}, {"&#39;", "'"}, {"&#039;", "'"}};
  for (const auto& [entity, plain] : kEntities) {
    for (size_t at = text.find(entity); at != std::string::npos;
         at = text.find(entity, at + plain.size())) {
      text.replace(at, entity.size(), plain);
    }
  }
  return text;
}

std::string ArchiveCoverNames(const std::string& html) {
  std::string names;
  constexpr std::string_view kOpen = "<a href=\"";
  for (size_t at = html.find(kOpen); at != std::string::npos;
       at = html.find(kOpen, at + 1)) {
    const size_t text = html.find("\">", at);
    const size_t close =
        text == std::string::npos ? std::string::npos : html.find("</a>", text);
    if (close == std::string::npos) {
      break;
    }
    std::string name = HtmlUnescape(html.substr(text + 2, close - text - 2));
    if (name.size() > 4 && LowerBytes(name.substr(name.size() - 4)) == ".jpg") {
      names += name;
      names += '\n';
    }
  }
  return names;
}

// Cover file names by IndieKey: the hub's list, else the archive's listing.
const std::unordered_map<std::string, std::string>& IndieCoverIndex() {
  static std::mutex mutex;
  static bool loaded = false;
  static std::unordered_map<std::string, std::string> index;
  std::lock_guard<std::mutex> lock(mutex);
  if (loaded) {
    return index;
  }
  loaded = true;
  const std::filesystem::path path =
      kernel_state()->emulator()->cache_root() / "xblig" / "covers.txt";
  std::error_code ec;
  const bool cached = std::filesystem::is_regular_file(path, ec);
  const bool stale =
      !cached ||
      std::filesystem::last_write_time(path, ec) + std::chrono::hours(24 * 7) <
          std::filesystem::file_time_type::clock::now();
  std::string names;
  if (stale) {
    long status = 0;
    const auto hub = HttpGet(
        XLiveAPI::GetApiAddress() + "xblig/covers/index.txt", &status, 15000);
    if (!hub.empty() && hub.front() != '<') {
      names.assign(hub.begin(), hub.end());
    } else {
      const auto listing =
          HttpGet(std::string(kIndieCoverArchive), &status, 60000);
      names = ArchiveCoverNames(std::string(listing.begin(), listing.end()));
    }
    if (!names.empty()) {
      WriteWholeFile(path, std::vector<uint8_t>(names.begin(), names.end()));
    } else {
      XELOGI("XBLIG: cover index unavailable (HTTP {})", status);
    }
  }
  if (names.empty() && cached) {
    const auto bytes = ReadWholeFile(path);
    names.assign(bytes.begin(), bytes.end());
  }
  size_t start = 0;
  while (start < names.size()) {
    size_t end = names.find('\n', start);
    if (end == std::string::npos) {
      end = names.size();
    }
    std::string name = names.substr(start, end - start);
    while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) {
      name.pop_back();
    }
    if (name.size() > 4) {
      index.emplace(IndieKey(name.substr(0, name.size() - 4)), name);
    }
    start = end + 1;
  }
  XELOGI("XBLIG: {} covers indexed", index.size());
  return index;
}

}  // namespace

std::vector<uint8_t> FetchIndieBoxArt(const std::vector<std::string>& names) {
  const auto& index = IndieCoverIndex();
  std::string file;
  for (const auto& name : names) {
    const std::string key = IndieKey(name);
    if (key.empty()) {
      continue;
    }
    if (const auto it = index.find(key); it != index.end()) {
      file = it->second;
      break;
    }
  }
  if (file.empty()) {
    XELOGI("XBLIG: no cover for '{}'", names.empty() ? "" : names.front());
    return {};
  }
  const std::string key = IndieKey(file.substr(0, file.size() - 4));
  const std::filesystem::path cache = kernel_state()->emulator()->cache_root() /
                                      "xblig" / "covers" / (key + ".jpg");
  std::error_code ec;
  if (std::filesystem::is_regular_file(cache, ec)) {
    return ReadWholeFile(cache);
  }

  static std::mutex missing_mutex;
  static std::set<std::string> missing;
  {
    std::lock_guard<std::mutex> lock(missing_mutex);
    if (missing.contains(key)) {
      return {};
    }
  }
  const std::string urls[] = {
      XLiveAPI::GetApiAddress() + "xblig/covers/" + PercentEncode(file),
      std::string(kIndieCoverArchive) + PercentEncode(file)};
  for (const std::string& url : urls) {
    long status = 0;
    std::vector<uint8_t> art = HttpGet(url, &status, 15000);
    int width = 0;
    int height = 0;
    int channels = 0;
    if (!art.empty() &&
        stbi_info_from_memory(art.data(), static_cast<int>(art.size()), &width,
                              &height, &channels)) {
      WriteWholeFile(cache, art);
      XELOGI("XBLIG: cover '{}' from {} ({}x{})", file, url, width, height);
      return art;
    }
  }
  XELOGI("XBLIG: cover '{}' could not be fetched", file);
  std::lock_guard<std::mutex> lock(missing_mutex);
  missing.insert(key);
  return {};
}

// The title's official retail cover from XboxUnity: the whole 900x600 case
// jacket, back, spine and front. Kept on disk once fetched; a title with no
// official cover, or no network, is tried once per session.
std::vector<uint8_t> FetchTitleCoverJacket(uint32_t title_id) {
  Emulator* emulator = kernel_state()->emulator();
  const std::filesystem::path cache = emulator->cache_root() / "disc_art" /
                                      fmt::format("{:08X}_cover.img", title_id);
  std::vector<uint8_t> jacket;
  std::error_code ec;
  if (std::filesystem::is_regular_file(cache, ec)) {
    std::ifstream file(cache, std::ios::binary);
    jacket.assign(std::istreambuf_iterator<char>(file),
                  std::istreambuf_iterator<char>());
    return jacket;
  }

  static std::mutex missing_mutex;
  static std::set<uint32_t> missing;
  {
    std::lock_guard<std::mutex> lock(missing_mutex);
    if (missing.contains(title_id)) {
      return {};
    }
  }
  auto give_up = [&](std::string_view why) {
    XELOGI("XboxUnity: no official cover for {:08X} ({})", title_id, why);
    std::lock_guard<std::mutex> lock(missing_mutex);
    missing.insert(title_id);
    return std::vector<uint8_t>();
  };

  long status = 0;
  const std::vector<uint8_t> info = HttpGet(
      fmt::format(
          "http://xboxunity.net/Resources/Lib/CoverInfo.php?titleid={:08X}",
          title_id),
      &status);
  if (info.empty()) {
    return give_up(fmt::format("cover list HTTP {}", status));
  }
  rapidjson::Document document;
  document.Parse(reinterpret_cast<const char*>(info.data()), info.size());
  if (document.HasParseError() || !document.IsObject() ||
      !document.HasMember("Covers") || !document["Covers"].IsArray()) {
    return give_up("cover list did not parse");
  }
  std::string cover_id;
  for (const auto& cover : document["Covers"].GetArray()) {
    if (cover.IsObject() && cover.HasMember("Official") &&
        cover["Official"].IsString() &&
        std::string_view(cover["Official"].GetString()) == "1" &&
        cover.HasMember("CoverID") && cover["CoverID"].IsString()) {
      cover_id = cover["CoverID"].GetString();
      break;
    }
  }
  if (cover_id.empty()) {
    return give_up("none marked official");
  }
  jacket = HttpGet(
      fmt::format(
          "http://xboxunity.net/Resources/Lib/Cover.php?size=large&cid={}",
          cover_id),
      &status);
  int width = 0;
  int height = 0;
  int channels = 0;
  if (jacket.empty() ||
      !stbi_info_from_memory(jacket.data(), static_cast<int>(jacket.size()),
                             &width, &height, &channels)) {
    return give_up(fmt::format("cover {} HTTP {}", cover_id, status));
  }
  std::filesystem::create_directories(cache.parent_path(), ec);
  std::ofstream file(cache, std::ios::binary);
  file.write(reinterpret_cast<const char*>(jacket.data()),
             static_cast<std::streamsize>(jacket.size()));
  XELOGI("XboxUnity: official cover {} for {:08X} ({}x{})", cover_id, title_id,
         width, height);
  return jacket;
}

dword_result_t NetDll_XHttpStartup_entry(dword_t caller, dword_t reserved,
                                         dword_t reserved_ptr) {
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpStartup, kNetworking, kImplemented);

dword_result_t NetDll_XHttpShutdown_entry(dword_t caller) {
  XHttpManager::Get()->CloseAll();
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpShutdown, kNetworking, kImplemented);

dword_result_t NetDll_XHttpOpen_entry(dword_t caller, lpu16string_t agent,
                                      dword_t access_type, lpu16string_t proxy,
                                      lpu16string_t proxy_bypass,
                                      dword_t flags) {
  auto* manager = XHttpManager::Get();
  const uint32_t handle = manager->Create(XHttpHandleType::kSession, 0);
  auto* session = manager->Lookup(handle);
  session->agent = GuestStringW(agent.guest_address());
  session->access_type = access_type;
  session->proxy = GuestStringW(proxy.guest_address());
  session->proxy_bypass = GuestStringW(proxy_bypass.guest_address());
  session->flags = flags;

  XThread::SetLastError(X_ERROR_SUCCESS);
  return handle;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpen, kNetworking, kImplemented);

dword_result_t NetDll_XHttpCloseHandle_entry(dword_t caller, dword_t handle) {
  if (!XHttpManager::Get()->Close(handle)) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCloseHandle, kNetworking, kImplemented);

dword_result_t NetDll_XHttpConnect_entry(dword_t caller, dword_t session_handle,
                                         lpstring_t host, dword_t port,
                                         dword_t flags) {
  auto* manager = XHttpManager::Get();
  auto* session = manager->Lookup(session_handle, XHttpHandleType::kSession);
  if (!session) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }

  const uint32_t handle =
      manager->Create(XHttpHandleType::kConnect, session_handle);
  auto* connect = manager->Lookup(handle);
  connect->host = host ? host.value() : "";
  connect->port = static_cast<uint16_t>(port.value());
  connect->flags = flags;

  XELOGW("XHttpConnect: {}:{}", connect->host, connect->port);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return handle;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpConnect, kNetworking, kImplemented);

static uint32_t CreateRequest(uint32_t connect_handle, const std::string& verb,
                              const std::string& path,
                              const std::string& version,
                              const std::string& referrer, uint32_t flags) {
  auto* manager = XHttpManager::Get();
  auto* connect = manager->Lookup(connect_handle, XHttpHandleType::kConnect);
  if (!connect) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }

  const uint32_t handle =
      manager->Create(XHttpHandleType::kRequest, connect_handle);
  auto* request = manager->Lookup(handle);
  request->verb = verb.empty() ? "GET" : verb;
  request->object_name = path.empty() ? "/" : path;
  request->version = version;
  request->referrer = referrer;
  request->flags = flags;
  request->secure = (flags & XHTTP_FLAG_SECURE) != 0;
  if (!referrer.empty()) {
    request->request_headers.push_back("Referer: " + referrer);
  }

  XELOGI("XHttpOpenRequest: {} {}", request->verb, request->object_name);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return handle;
}

dword_result_t NetDll_XHttpOpenRequest_entry(
    dword_t caller, dword_t connect_handle, lpstring_t verb, lpstring_t path,
    lpstring_t version, lpstring_t referrer, lpstring_t accept_types,
    dword_t flags) {
  return CreateRequest(connect_handle, verb ? verb.value() : "",
                       path ? path.value() : "", version ? version.value() : "",
                       referrer ? referrer.value() : "", flags);
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpenRequest, kNetworking, kImplemented);

dword_result_t NetDll_XHttpOpenRequestUsingMemory_entry(
    dword_t caller, dword_t connect_handle, lpstring_t verb, lpstring_t path,
    lpstring_t version, lpstring_t referrer, lpvoid_t buffer,
    dword_t buffer_size, dword_t flags) {
  return CreateRequest(connect_handle, verb ? verb.value() : "",
                       path ? path.value() : "", version ? version.value() : "",
                       referrer ? referrer.value() : "", flags);
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpenRequestUsingMemory, kNetworking,
                    kImplemented);

dword_result_t NetDll_XHttpSetStatusCallback_entry(dword_t caller,
                                                   dword_t handle,
                                                   dword_t callback,
                                                   dword_t flags,
                                                   dword_t reserved) {
  auto* object = XHttpManager::Get()->Lookup(handle);
  if (!object) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0xFFFFFFFF;
  }
  const uint32_t previous = object->callback;
  object->callback = callback;
  object->callback_flags = flags;
  return previous;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetStatusCallback, kNetworking, kImplemented);

dword_result_t NetDll_XHttpSendRequest_entry(
    dword_t caller, dword_t request_handle, lpstring_t headers,
    dword_t headers_length, lpvoid_t optional, dword_t optional_length,
    dword_t total_length, dword_t context) {
  auto* manager = XHttpManager::Get();
  auto* request = manager->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }

  if (headers) {
    std::string block = headers.value();
    if (headers_length && headers_length != 0xFFFFFFFF &&
        headers_length < block.size()) {
      block = block.substr(0, headers_length);
    }
    size_t at = 0;
    while (at < block.size()) {
      size_t end = block.find("\r\n", at);
      if (end == std::string::npos) {
        end = block.size();
      }
      const std::string line = block.substr(at, end - at);
      if (!line.empty()) {
        request->request_headers.push_back(line);
      }
      at = end + 2;
    }
  }

  request->request_body.clear();
  if (optional && optional_length) {
    const auto* data = kernel_memory()->TranslateVirtual<const uint8_t*>(
        optional.guest_address());
    request->request_body.assign(data, data + optional_length);
  }
  request->expected_body_length = total_length;
  request->context = context;
  request->sent = true;
  request->received = false;

  if (total_length && request->request_body.size() < total_length) {
    XThread::SetLastError(X_ERROR_SUCCESS);
    return 1;
  }

  const bool ok = Perform(request);

  if (manager->IsAsync(request)) {
    // Async: the call always succeeds and the outcome is reported through the
    // callback the next time the guest pumps XHttpDoWork. We fetch
    // synchronously, so the completion is ready immediately.
    if (ok) {
      manager->Notify(request, XHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, 0,
                      0);
    } else {
      // XHTTP_ASYNC_RESULT { dwResult; dwError; } in guest memory.
      const uint32_t block = kernel_memory()->SystemHeapAlloc(8);
      if (block) {
        auto* result =
            kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(block);
        result[0] = XHTTP_API_SEND_REQUEST;
        result[1] = XThread::GetLastError();
        manager->Notify(request, XHTTP_CALLBACK_STATUS_REQUEST_ERROR, block, 8,
                        block);
      }
    }
    XThread::SetLastError(X_ERROR_SUCCESS);
    return 1;
  }

  if (!ok) {
    return 0;
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSendRequest, kNetworking, kImplemented);

dword_result_t NetDll_XHttpWriteData_entry(dword_t caller,
                                           dword_t request_handle,
                                           lpvoid_t buffer, dword_t size,
                                           lpdword_t out_written) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  if (!request->sent) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_STATE);
    return 0;
  }

  if (buffer && size) {
    const auto* data = kernel_memory()->TranslateVirtual<const uint8_t*>(
        buffer.guest_address());
    request->request_body.insert(request->request_body.end(), data,
                                 data + size);
  }
  if (out_written) {
    *out_written = size.value();
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpWriteData, kNetworking, kImplemented);

dword_result_t NetDll_XHttpReceiveResponse_entry(dword_t caller,
                                                 dword_t request_handle,
                                                 lpvoid_t reserved) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  if (!EnsureReceived(request)) {
    return 0;
  }
  XHttpManager::Get()->Notify(request, XHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,
                              0, 0);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpReceiveResponse, kNetworking, kImplemented);

dword_result_t NetDll_XHttpQueryHeaders_entry(
    dword_t caller, dword_t request_handle, dword_t info_level, lpstring_t name,
    lpvoid_t buffer, lpdword_t buffer_length, lpdword_t index) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  if (!EnsureReceived(request)) {
    return 0;
  }

  const uint32_t level = info_level & XHTTP_QUERY_HEADER_MASK;
  const bool as_number = (info_level & XHTTP_QUERY_FLAG_NUMBER) != 0;

  // Every level from ACCEPT (0) to X_ERR (41) names one header, so a table
  // reads better than forty cases. The gap at RAW_HEADERS_CRLF is not a header
  // and is handled below; MAX (42) is the end of the range, not a level.
  static const char* const kHeaderForLevel[XHTTP_QUERY_MAX] = {
      "Accept",
      "Accept-Charset",
      "Accept-Encoding",
      "Accept-Language",
      "Accept-Ranges",
      "Allow",
      "Cache-Control",
      "Connection",
      "Content-Language",
      "Content-Length",
      "Content-Transfer-Encoding",
      "Content-Type",
      "Date",
      "Expires",
      "Ext",
      "Host",
      "If-Match",
      "If-Modified-Since",
      "If-None-Match",
      "If-Range",
      "If-Unmodified-Since",
      "Last-Modified",
      nullptr,  // XHTTP_QUERY_RAW_HEADERS_CRLF
      "Man",
      "MIME-Version",
      "MX",
      "NT",
      "NTS",
      "Range",
      "Referer",
      "Server",
      "Seq",
      "SID",
      "ST",
      "Timeout",
      "Transfer-Encoding",
      "Unless-Modified-Since",
      "User-Agent",
      "USN",
      "X-Delay",
      "X-Delayflags",
      "X-Err",
  };

  std::string value;
  switch (level) {
    case XHTTP_QUERY_STATUS_CODE:
      value = std::to_string(request->status_code);
      break;
    case XHTTP_QUERY_RAW_HEADERS_CRLF:
      value = request->raw_headers;
      break;
    case XHTTP_QUERY_CUSTOM:
      if (!name) {
        XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
        return 0;
      }
      value = HeaderValue(request, name.value());
      break;
    default: {
      if (level >= XHTTP_QUERY_MAX || !kHeaderForLevel[level]) {
        XELOGW("XHttpQueryHeaders: unhandled level {:X} (info_level {:08X})",
               level, info_level.value());
        XThread::SetLastError(XHTTP_ERROR_HEADER_NOT_FOUND);
        return 0;
      }
      value = HeaderValue(request, kHeaderForLevel[level]);
      // curl can consume Content-Length on a chunked or decompressed response;
      // what we actually hold is authoritative.
      if (value.empty() && level == XHTTP_QUERY_CONTENT_LENGTH) {
        value = std::to_string(request->response_body.size());
      }
      break;
    }
  }

  if (value.empty() && level != XHTTP_QUERY_RAW_HEADERS_CRLF) {
    XELOGW("XHttpQueryHeaders: level {:X} not present in the response", level);
    XThread::SetLastError(XHTTP_ERROR_HEADER_NOT_FOUND);
    return 0;
  }

  XELOGW("XHttpQueryHeaders: level {:X}{} -> '{}'", level,
         as_number ? " (number)" : "", value);

  if (as_number) {
    const uint64_t number = std::strtoull(value.c_str(), nullptr, 10);
    // The same level answers 32- or 64-bit depending on the buffer the caller
    // sizes: GetResponseContentLength passes 4, GetResponseContentLength64
    // passes 8. Writing 4 bytes into the 64-bit caller's buffer left it
    // reading the high half and seeing a zero-length body.
    const uint32_t capacity =
        buffer_length ? static_cast<uint32_t>(*buffer_length) : 0;
    if (capacity >= sizeof(uint64_t)) {
      xe::be<uint64_t> encoded = number;
      if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                   &encoded, sizeof(encoded))) {
        return 0;
      }
    } else {
      xe::be<uint32_t> encoded = static_cast<uint32_t>(number);
      if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                   &encoded, sizeof(encoded))) {
        return 0;
      }
    }
  } else {
    if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                 value.c_str(), static_cast<uint32_t>(value.size() + 1))) {
      return 0;
    }
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryHeaders, kNetworking, kImplemented);

dword_result_t NetDll_XHttpReadData_entry(dword_t caller,
                                          dword_t request_handle,
                                          lpvoid_t buffer, dword_t size,
                                          lpdword_t out_read) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  if (!EnsureReceived(request)) {
    return 0;
  }

  const size_t remaining = request->response_body.size() - request->read_pos;
  const size_t count = std::min(static_cast<size_t>(size.value()), remaining);
  if (count && buffer) {
    std::memcpy(
        kernel_memory()->TranslateVirtual<void*>(buffer.guest_address()),
        request->response_body.data() + request->read_pos, count);
    request->read_pos += count;
  }
  if (out_read) {
    *out_read = static_cast<uint32_t>(count);
  }

  // READ_COMPLETE hands the guest back its own buffer and the byte count; a
  // zero-length completion is how the transfer's end is signalled.
  XHttpManager::Get()->Notify(request, XHTTP_CALLBACK_STATUS_READ_COMPLETE,
                              buffer.guest_address(),
                              static_cast<uint32_t>(count));

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpReadData, kNetworking, kImplemented);

dword_result_t NetDll_XHttpQueryOption_entry(dword_t caller, dword_t handle,
                                             dword_t option, lpvoid_t buffer,
                                             lpdword_t buffer_length) {
  auto* manager = XHttpManager::Get();
  auto* object = manager->Lookup(handle);
  if (!object) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }

  auto write_dword = [&](uint32_t value) {
    xe::be<uint32_t> encoded = value;
    return CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                   &encoded, sizeof(encoded));
  };

  switch (option) {
    case XHTTP_OPTION_HANDLE_TYPE:
      if (!write_dword(static_cast<uint32_t>(object->type))) {
        return 0;
      }
      break;
    case XHTTP_OPTION_PARENT_HANDLE:
      if (!write_dword(object->parent)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_CONTEXT_VALUE:
      if (!write_dword(object->context)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_RESOLVE_TIMEOUT:
      if (!write_dword(object->resolve_timeout)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_CONNECT_TIMEOUT:
      if (!write_dword(object->connect_timeout)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_SEND_TIMEOUT:
      if (!write_dword(object->send_timeout)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_RECEIVE_TIMEOUT:
      if (!write_dword(object->receive_timeout)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_SECURITY_FLAGS:
      if (!write_dword(0)) {
        return 0;
      }
      break;
    case XHTTP_OPTION_URL: {
      if (object->type != XHttpHandleType::kRequest) {
        XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
        return 0;
      }
      const std::string url = BuildUrl(object);
      if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                   url.c_str(), static_cast<uint32_t>(url.size() + 1))) {
        return 0;
      }
      break;
    }
    case XHTTP_OPTION_USER_AGENT: {
      auto* session = manager->SessionOf(object);
      const std::string agent = session ? session->agent : "";
      if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
                   agent.c_str(), static_cast<uint32_t>(agent.size() + 1))) {
        return 0;
      }
      break;
    }
    default: {
      auto it = object->options.find(option);
      if (it != object->options.end()) {
        if (!write_dword(it->second)) {
          return 0;
        }
        break;
      }
      // An option we do not model. Answering INVALID_OPTION is how the dash's
      // XRL connection died: it queries option 0x17 on the request, and a
      // failure there sends it down the teardown-and-retry path (dash
      // 0x9226B710), which is the "Status Code: 80072ee9" box -- 0x2EE9 is
      // 12009, INVALID_OPTION, straight out of this branch. The options it
      // asks about are request-state flags it only acts on when set, so a zero
      // is the honest "not set" answer and keeps the transaction alive. Logged,
      // so an option that turns out to matter still names itself.
      XELOGW("XHttpQueryOption: unmodelled option {} on handle {:08X}; 0",
             option.value(), handle.value());
      if (!write_dword(0)) {
        return 0;
      }
      break;
    }
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryOption, kNetworking, kImplemented);

dword_result_t NetDll_XHttpSetOption_entry(dword_t caller, dword_t handle,
                                           dword_t option, lpvoid_t buffer,
                                           dword_t buffer_length) {
  auto* object = XHttpManager::Get()->Lookup(handle);
  if (!object) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }

  uint32_t value = 0;
  if (buffer && buffer_length >= sizeof(uint32_t)) {
    value = *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(
        buffer.guest_address());
  }

  switch (option) {
    case XHTTP_OPTION_HANDLE_TYPE:
    case XHTTP_OPTION_PARENT_HANDLE:
      XThread::SetLastError(XHTTP_ERROR_OPTION_NOT_SETTABLE);
      return 0;
    case XHTTP_OPTION_CONTEXT_VALUE:
      object->context = value;
      break;
    case XHTTP_OPTION_RESOLVE_TIMEOUT:
      object->resolve_timeout = value;
      break;
    case XHTTP_OPTION_CONNECT_TIMEOUT:
      object->connect_timeout = value;
      break;
    case XHTTP_OPTION_SEND_TIMEOUT:
      object->send_timeout = value;
      break;
    case XHTTP_OPTION_RECEIVE_TIMEOUT:
      object->receive_timeout = value;
      break;
    case XHTTP_OPTION_USER_AGENT:
      object->agent = GuestString(buffer.guest_address());
      break;
    case XHTTP_OPTION_PROXY:
      object->proxy = GuestString(buffer.guest_address());
      break;
    default:
      object->options[option] = value;
      break;
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetOption, kNetworking, kImplemented);

// The pump for an ASYNC session: the guest calls this from its own worker and
// expects every completion queued since the last call to be delivered to its
// status callback, on this thread, before returning. Without it the dash's XMan
// worker spins here forever waiting for a SENDREQUEST_COMPLETE that never
// arrives, and the home channel sits on a loading ring.
dword_result_t NetDll_XHttpDoWork_entry(dword_t caller, dword_t handle,
                                        dword_t reserved,
                                        const ppc_context_t& ctx) {
  auto* manager = XHttpManager::Get();

  XHttpNotification notification = {};
  while (manager->TakeNotification(handle.value(), notification)) {
    XELOGW("XHttpDoWork: callback {:08X} status {:08X} handle {:08X} len {}",
           notification.callback, notification.status, notification.handle,
           notification.info_length);
    uint64_t args[] = {notification.handle, notification.context,
                       notification.status, notification.info_ptr,
                       notification.info_length};
    ctx->processor->Execute(ctx->thread_state, notification.callback, args, 5);
    if (notification.free_info) {
      kernel_memory()->SystemHeapFree(notification.free_info);
    }
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpDoWork, kNetworking, kImplemented);

dword_result_t NetDll_XHttpSetCredentials_entry(
    dword_t caller, dword_t request_handle, dword_t auth_target,
    dword_t auth_scheme, lpstring_t user_name, lpstring_t password,
    lpvoid_t reserved) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  request->user_name = user_name ? user_name.value() : "";
  request->password = password ? password.value() : "";
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetCredentials, kNetworking, kImplemented);

dword_result_t NetDll_XHttpQueryAuthSchemes_entry(dword_t caller,
                                                  dword_t request_handle,
                                                  lpdword_t supported,
                                                  lpdword_t first,
                                                  lpdword_t target) {
  auto* request =
      XHttpManager::Get()->Lookup(request_handle, XHttpHandleType::kRequest);
  if (!request) {
    XThread::SetLastError(XHTTP_ERROR_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  if (!request->auth_schemes) {
    XThread::SetLastError(XHTTP_ERROR_INVALID_SERVER_RESPONSE);
    return 0;
  }

  uint32_t preferred = XHTTP_AUTH_SCHEME_BASIC;
  for (uint32_t scheme : {XHTTP_AUTH_SCHEME_NEGOTIATE, XHTTP_AUTH_SCHEME_NTLM,
                          XHTTP_AUTH_SCHEME_DIGEST, XHTTP_AUTH_SCHEME_BASIC}) {
    if (request->auth_schemes & scheme) {
      preferred = scheme;
      break;
    }
  }

  if (supported) {
    *supported = request->auth_schemes;
  }
  if (first) {
    *first = preferred;
  }
  if (target) {
    *target = XHTTP_AUTH_TARGET_SERVER;
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryAuthSchemes, kNetworking, kImplemented);

dword_result_t NetDll_XHttpCrackUrl_entry(
    dword_t caller, dword_t url_ptr, dword_t url_length, dword_t flags,
    pointer_t<XHTTP_URL_COMPONENTS> url_components_ptr) {
  if (!url_ptr || !url_components_ptr ||
      url_components_ptr->struct_size != sizeof(XHTTP_URL_COMPONENTS)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return false;
  }

  // X_ICU_ESCAPE is unsupported ignore it.

  bool insufficient_buffer =
      url_components_ptr->scheme_ptr && !url_components_ptr->scheme_length ||
      url_components_ptr->host_name_ptr &&
          !url_components_ptr->host_name_length ||
      url_components_ptr->user_name_ptr &&
          !url_components_ptr->user_name_length ||
      url_components_ptr->password_ptr &&
          !url_components_ptr->password_length ||
      url_components_ptr->url_path_ptr &&
          !url_components_ptr->url_path_length ||
      url_components_ptr->extra_info_ptr &&
          !url_components_ptr->extra_info_length;

  auto decode_string = [](const std::string encoded_component) -> std::string {
    CURL* curl = curl_easy_init();

    if (!curl) {
      return "";
    }

    std::string decoded_component;
    int component_length = 0;

    char* decoded_output =
        curl_easy_unescape(curl, encoded_component.c_str(),
                           decoded_component.size(), &component_length);

    if (decoded_output) {
      decoded_component = std::string(decoded_output, component_length);
      curl_free(decoded_output);
    }

    curl_easy_cleanup(curl);

    return decoded_component;
  };

  // A given length is read exactly: the buffer need not be terminated (see
  // XHttpCrackUrlW), and reading to a terminator could run off the page.
  std::string url_to_process;
  if (url_length) {
    const char* host_ptr =
        kernel_memory()->TranslateVirtual<const char*>(url_ptr.value());
    url_to_process.assign(host_ptr, strnlen(host_ptr, url_length));
  } else {
    // Unbounded only when the caller said it is terminated (no length).
    url_to_process =
        kernel_memory()->TranslateVirtual<const char*>(url_ptr.value());
  }

  CURLU* url = curl_url();

  if (url) {
    CURLUcode rc = curl_url_set(url, CURLUPART_URL, url_to_process.c_str(), 0);

    // Assert if URL is bad format
    assert_zero(rc);

    if (rc) {
      url_components_ptr->scheme = -1;
    }

    curl_url_cleanup(url);
  }

  std::regex url_regex(
      R"(^([a-zA-Z]+)://(?:([^:@]+)(?::([^:@]*))?@)?([^/:]+)(?::(\d+))?((/[^?#]*)(\?[^#]*)?(#[^ ]*)?)?$)",
      std::regex_constants::icase);

  std::smatch matches;

  auto ProcessComponent = [decode_string, flags, kernel_state = kernel_state()](
                              const uint32_t component_result_ptr,
                              uint32_t& component_ptr,
                              uint32_t& component_length_ptr, uint32_t size) {
    if (component_ptr) {
      // Include null terminator
      const uint32_t min_buffer_size = size + 1;

      if (!component_length_ptr || component_length_ptr < min_buffer_size) {
        component_length_ptr = min_buffer_size;
        return false;
      }

      char* result_dst_ptr =
          kernel_state->memory()->TranslateVirtual<char*>(component_ptr);

      char* result_src_ptr =
          kernel_state->memory()->TranslateVirtual<char*>(component_result_ptr);

      const std::string component_data(result_src_ptr, size);
      const std::string processed_data =
          flags & X_ICU_DECODE ? decode_string(component_data) : component_data;

      xe::string_util::copy_truncating(result_dst_ptr, processed_data.c_str(),
                                       component_length_ptr);
      component_length_ptr = processed_data.size();
    } else if (component_length_ptr) {
      component_ptr = component_result_ptr;
      component_length_ptr = size;
    }

    return true;
  };

  bool result = true;
  bool unrecognized_scheme = false;

  if (std::regex_match(url_to_process, matches, url_regex)) {
    for (size_t i = 0; i < matches.size(); ++i) {
      std::ssub_match sub_match = matches[i];

      if (sub_match.matched) {
        const uint32_t result_ptr =
            url_ptr.value() + static_cast<uint32_t>(matches.position(i));

        const uint32_t length = static_cast<uint32_t>(sub_match.length());

        const X_URL_COMPONENTS current_component =
            static_cast<X_URL_COMPONENTS>(i);

        switch (current_component) {
          case X_URL_COMPONENTS::Full: {
            // Skip
            continue;
          } break;
          case X_URL_COMPONENTS::Protocol: {
            uint32_t scheme_ptr_out = url_components_ptr->scheme_ptr;
            uint32_t scheme_length_out = url_components_ptr->scheme_length;

            const bool component_result = ProcessComponent(
                result_ptr, scheme_ptr_out, scheme_length_out, length);

            url_components_ptr->scheme_length = scheme_length_out;

            if (component_result) {
              if (!url_components_ptr->scheme_ptr) {
                url_components_ptr->scheme_ptr = scheme_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }

            const char* scheme_data_ptr =
                kernel_state()->memory()->TranslateVirtual<char*>(result_ptr);

            std::string schema_data = std::string(scheme_data_ptr, length);

            X_INTERNET_SCHEME scheme_type = {};

            // Set default scheme and port
            if (utf8::equal_case(schema_data.c_str(), "http")) {
              scheme_type = X_INTERNET_SCHEME::HTTP;
              url_components_ptr->port = 80;
            } else if (utf8::equal_case(schema_data.c_str(), "https")) {
              scheme_type = X_INTERNET_SCHEME::HTTPS;
              url_components_ptr->port = 443;
            } else {
              // XHTTP only speaks http and https; anything else is the
              // caller's own scheme and must be reported as unrecognised, the
              // way WinHTTP does. Succeeding here told the dash that
              // "epix://Offline_Slot_Home.jpg" was a remote URL, so it tried
              // to resolve a host named Offline_Slot_Home.jpg for every asset
              // in the manifest instead of loading them locally.
              unrecognized_scheme = true;
            }

            url_components_ptr->scheme = static_cast<uint32_t>(scheme_type);
          } break;
          case X_URL_COMPONENTS::Username: {
            uint32_t username_ptr_out = url_components_ptr->user_name_ptr;
            uint32_t username_length_out = url_components_ptr->user_name_length;

            const bool component_result = ProcessComponent(
                result_ptr, username_ptr_out, username_length_out, length);

            url_components_ptr->user_name_length = username_length_out;

            if (component_result) {
              if (!url_components_ptr->user_name_ptr) {
                url_components_ptr->user_name_ptr = username_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }
          } break;
          case X_URL_COMPONENTS::Password: {
            uint32_t password_ptr_out = url_components_ptr->password_ptr;
            uint32_t password_length_out = url_components_ptr->password_length;

            const bool component_result = ProcessComponent(
                result_ptr, password_ptr_out, password_length_out, length);

            url_components_ptr->password_length = password_length_out;

            if (component_result) {
              if (!url_components_ptr->password_ptr) {
                url_components_ptr->password_ptr = password_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }
          } break;
          case X_URL_COMPONENTS::Host: {
            uint32_t host_ptr_out = url_components_ptr->host_name_ptr;
            uint32_t host_length_out = url_components_ptr->host_name_length;

            const bool component_result = ProcessComponent(
                result_ptr, host_ptr_out, host_length_out, length);

            url_components_ptr->host_name_length = host_length_out;

            if (component_result) {
              if (!url_components_ptr->host_name_ptr) {
                url_components_ptr->host_name_ptr = host_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }
          } break;
          case X_URL_COMPONENTS::Port: {
            const char* port_str_ptr =
                kernel_memory()->TranslateVirtual<char*>(result_ptr);

            std::string port_str = std::string(port_str_ptr, length);

            const uint16_t port =
                xe::string_util::from_string<uint16_t>(port_str);

            url_components_ptr->port = port;
          } break;
          case X_URL_COMPONENTS::Path: {
            uint32_t path_ptr_out = url_components_ptr->url_path_ptr;
            uint32_t path_length_out = url_components_ptr->url_path_length;

            const bool component_result = ProcessComponent(
                result_ptr, path_ptr_out, path_length_out, length);

            url_components_ptr->url_path_length = path_length_out;

            if (component_result) {
              if (!url_components_ptr->url_path_ptr) {
                url_components_ptr->url_path_ptr = path_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }
          } break;
          case X_URL_COMPONENTS::Query: {
            uint32_t extra_ptr_out = url_components_ptr->extra_info_ptr;
            uint32_t extra_length_out = url_components_ptr->extra_info_length;

            const bool component_result = ProcessComponent(
                result_ptr, extra_ptr_out, extra_length_out, length);

            url_components_ptr->extra_info_length = extra_length_out;

            if (component_result) {
              if (!url_components_ptr->extra_info_ptr) {
                url_components_ptr->extra_info_ptr = extra_ptr_out;
              }
            } else {
              insufficient_buffer = true;
            }
          } break;
        }
      }
    }
  } else {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    result = false;
  }

  // Return after processing so the component length is set
  if (insufficient_buffer) {
    XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
    result = false;
  }

  if (unrecognized_scheme) {
    XELOGW("XHttpCrackUrl: unrecognised scheme in '{}'", url_to_process);
    XThread::SetLastError(XHTTP_ERROR_UNRECOGNIZED_SCHEME);
    result = false;
  }

  return result;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCrackUrl, kNetworking, kImplemented);

dword_result_t NetDll_XHttpCrackUrlW_entry(
    dword_t caller, dword_t url_ptr, dword_t url_length, dword_t flags,
    pointer_t<XHTTP_URL_COMPONENTS> url_components_ptr) {
  if (!url_ptr || !url_components_ptr ||
      url_components_ptr->struct_size != sizeof(XHTTP_URL_COMPONENTS)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }

  // With a length the URL need not be terminated - the dash cracks its own
  // buffers that way - and searching for the terminator ran off the end of the
  // page (Mark Unread: access violation at a page boundary, then a crash). So
  // a given length is read exactly, never past.
  std::string url;
  if (url_length) {
    const auto* host_ptr =
        kernel_memory()->TranslateVirtual<const char16_t*>(url_ptr.value());
    // Grown as read, never sized from the length up front: a caller can pass
    // a length far beyond the URL (or ~0 for "to the terminator").
    constexpr uint32_t kMaxUrlChars = 8192;
    const uint32_t limit = std::min<uint32_t>(url_length, kMaxUrlChars);
    std::u16string wide;
    for (uint32_t used = 0; used < limit; ++used) {
      const char16_t unit = static_cast<char16_t>(
          xe::byte_swap(static_cast<uint16_t>(host_ptr[used])));
      if (!unit) {
        break;
      }
      wide.push_back(unit);
    }
    url = xe::to_utf8(wide);
  } else {
    url = GuestStringW(url_ptr.value());
  }

  std::regex url_regex(
      R"(^([a-zA-Z]+)://(?:([^:@]+)(?::([^:@]*))?@)?([^/:]+)(?::(\d+))?((/[^?#]*)(\?[^#]*)?(#[^ ]*)?)?$)",
      std::regex_constants::icase);

  std::smatch matches;
  if (!std::regex_match(url, matches, url_regex)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }

  bool insufficient_buffer = false;

  auto store = [&](size_t index, xe::be<uint32_t>& component_ptr,
                   xe::be<uint32_t>& component_length) {
    std::ssub_match sub_match = matches[index];
    if (!sub_match.matched) {
      return;
    }
    const std::u16string value = xe::to_utf16(sub_match.str());
    const uint32_t units = static_cast<uint32_t>(value.size());
    if (component_ptr) {
      if (component_length < units + 1) {
        component_length = units + 1;
        insufficient_buffer = true;
        return;
      }
      auto* dst = kernel_memory()->TranslateVirtual<char16_t*>(component_ptr);
      xe::string_util::copy_and_swap_truncating(dst, value, component_length);
      component_length = units;
    } else {
      component_ptr =
          url_ptr.value() +
          static_cast<uint32_t>(matches.position(index) * sizeof(char16_t));
      component_length = units;
    }
  };

  xe::be<uint32_t> scheme_ptr = url_components_ptr->scheme_ptr;
  xe::be<uint32_t> scheme_length = url_components_ptr->scheme_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Protocol), scheme_ptr,
        scheme_length);
  url_components_ptr->scheme_ptr = scheme_ptr;
  url_components_ptr->scheme_length = scheme_length;

  const std::string scheme = matches[1].str();
  X_INTERNET_SCHEME scheme_type = {};
  if (utf8::equal_case(scheme, "http")) {
    scheme_type = X_INTERNET_SCHEME::HTTP;
    url_components_ptr->port = 80;
  } else if (utf8::equal_case(scheme, "https")) {
    scheme_type = X_INTERNET_SCHEME::HTTPS;
    url_components_ptr->port = 443;
  } else {
    // See the ANSI entry point: XHTTP knows only http and https, and the dash
    // cracks its own "epix://" asset URIs through here. Reporting success made
    // every manifest asset look like a remote host.
    url_components_ptr->scheme = static_cast<uint32_t>(scheme_type);
    XELOGW("XHttpCrackUrlW: unrecognised scheme in '{}'", url);
    XThread::SetLastError(XHTTP_ERROR_UNRECOGNIZED_SCHEME);
    return 0;
  }
  url_components_ptr->scheme = static_cast<uint32_t>(scheme_type);

  xe::be<uint32_t> user_ptr = url_components_ptr->user_name_ptr;
  xe::be<uint32_t> user_length = url_components_ptr->user_name_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Username), user_ptr, user_length);
  url_components_ptr->user_name_ptr = user_ptr;
  url_components_ptr->user_name_length = user_length;

  xe::be<uint32_t> password_ptr = url_components_ptr->password_ptr;
  xe::be<uint32_t> password_length = url_components_ptr->password_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Password), password_ptr,
        password_length);
  url_components_ptr->password_ptr = password_ptr;
  url_components_ptr->password_length = password_length;

  xe::be<uint32_t> host_ptr = url_components_ptr->host_name_ptr;
  xe::be<uint32_t> host_length = url_components_ptr->host_name_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Host), host_ptr, host_length);
  url_components_ptr->host_name_ptr = host_ptr;
  url_components_ptr->host_name_length = host_length;

  if (matches[static_cast<size_t>(X_URL_COMPONENTS::Port)].matched) {
    url_components_ptr->port = xe::string_util::from_string<uint16_t>(
        matches[static_cast<size_t>(X_URL_COMPONENTS::Port)].str());
  }

  xe::be<uint32_t> path_ptr = url_components_ptr->url_path_ptr;
  xe::be<uint32_t> path_length = url_components_ptr->url_path_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Path), path_ptr, path_length);
  url_components_ptr->url_path_ptr = path_ptr;
  url_components_ptr->url_path_length = path_length;

  xe::be<uint32_t> extra_ptr = url_components_ptr->extra_info_ptr;
  xe::be<uint32_t> extra_length = url_components_ptr->extra_info_length;
  store(static_cast<size_t>(X_URL_COMPONENTS::Query), extra_ptr, extra_length);
  url_components_ptr->extra_info_ptr = extra_ptr;
  url_components_ptr->extra_info_length = extra_length;

  if (insufficient_buffer) {
    XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCrackUrlW, kNetworking, kImplemented);

namespace {

std::string ComposeUrl(pointer_t<XHTTP_URL_COMPONENTS> components, bool wide) {
  auto read = [&](uint32_t address, uint32_t length) -> std::string {
    if (!address) {
      return "";
    }
    if (wide) {
      std::u16string value = xe::load_and_swap<std::u16string>(
          kernel_memory()->TranslateVirtual<const void*>(address));
      if (length && length < value.size()) {
        value = value.substr(0, length);
      }
      return xe::to_utf8(value);
    }
    std::string value = kernel_memory()->TranslateVirtual<const char*>(address);
    if (length && length < value.size()) {
      value = value.substr(0, length);
    }
    return value;
  };

  std::string scheme = read(components->scheme_ptr, components->scheme_length);
  if (scheme.empty()) {
    scheme =
        components->scheme == static_cast<uint32_t>(X_INTERNET_SCHEME::HTTPS)
            ? "https"
            : "http";
  }

  const std::string host =
      read(components->host_name_ptr, components->host_name_length);
  const std::string user =
      read(components->user_name_ptr, components->user_name_length);
  const std::string password =
      read(components->password_ptr, components->password_length);
  const std::string path =
      read(components->url_path_ptr, components->url_path_length);
  const std::string extra =
      read(components->extra_info_ptr, components->extra_info_length);

  std::string url = scheme + "://";
  if (!user.empty()) {
    url += user;
    if (!password.empty()) {
      url += ":" + password;
    }
    url += "@";
  }
  url += host;

  const uint16_t port = components->port;
  const bool secure = utf8::equal_case(scheme, "https");
  if (port && ((secure && port != 443) || (!secure && port != 80))) {
    url += ":" + std::to_string(port);
  }

  if (!path.empty() && path.front() != '/') {
    url += "/";
  }
  url += path.empty() ? "/" : path;
  url += extra;
  return url;
}

}  // namespace

dword_result_t NetDll_XHttpCreateUrl_entry(
    dword_t caller, pointer_t<XHTTP_URL_COMPONENTS> components, dword_t flags,
    lpvoid_t buffer, lpdword_t buffer_length) {
  if (!components || components->struct_size != sizeof(XHTTP_URL_COMPONENTS)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }

  const std::string url = ComposeUrl(components, false);
  if (!CopyOut(buffer.guest_address(), buffer_length.guest_address(),
               url.c_str(), static_cast<uint32_t>(url.size() + 1))) {
    return 0;
  }

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCreateUrl, kNetworking, kImplemented);

dword_result_t NetDll_XHttpCreateUrlW_entry(
    dword_t caller, pointer_t<XHTTP_URL_COMPONENTS> components, dword_t flags,
    lpvoid_t buffer, lpdword_t buffer_length) {
  if (!components || components->struct_size != sizeof(XHTTP_URL_COMPONENTS)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }

  const std::u16string url = xe::to_utf16(ComposeUrl(components, true));
  const uint32_t bytes =
      static_cast<uint32_t>((url.size() + 1) * sizeof(char16_t));

  auto* length = buffer_length
                     ? kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(
                           buffer_length.guest_address())
                     : nullptr;
  if (!buffer || !length || *length < url.size() + 1) {
    if (length) {
      *length = static_cast<uint32_t>(url.size() + 1);
    }
    XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }

  auto* dst =
      kernel_memory()->TranslateVirtual<char16_t*>(buffer.guest_address());
  xe::string_util::copy_and_swap_truncating(dst, url, *length);
  *length = static_cast<uint32_t>(url.size());

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCreateUrlW, kNetworking, kImplemented);

dword_result_t NetDll_XHttpResetPerfCounters_entry(dword_t caller) {
  XHttpManager::Get()->counters() = {};
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpResetPerfCounters, kNetworking, kImplemented);

dword_result_t NetDll_XHttpGetPerfCounters_entry(dword_t caller,
                                                 lpvoid_t counters_ptr) {
  if (!counters_ptr) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  std::memcpy(
      kernel_memory()->TranslateVirtual<void*>(counters_ptr.guest_address()),
      &XHttpManager::Get()->counters(), sizeof(XHttpPerfCounters));
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpGetPerfCounters, kNetworking, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(XHttp);
