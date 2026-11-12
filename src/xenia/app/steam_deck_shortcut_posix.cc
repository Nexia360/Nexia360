/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/steam_deck_shortcut.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"

DEFINE_bool(steam_deck_shortcut, true,
            "On a Steam Deck, put Nexia in Steam's library as a non-Steam "
            "game the first time it runs.",
            "General");

namespace xe {
namespace app {

namespace {

// Binary VDF markers.
constexpr uint8_t kVdfMap = 0x00;
constexpr uint8_t kVdfString = 0x01;
constexpr uint8_t kVdfInt32 = 0x02;
constexpr uint8_t kVdfUInt64 = 0x07;
constexpr uint8_t kVdfEnd = 0x08;

// A node carries its own key so the children can be a plain vector: a vector
// of an incomplete type is allowed, a pair of one is not.
struct VdfValue {
  enum class Type { kMap, kString, kInt32, kUInt64 };

  std::string key;
  Type type = Type::kMap;
  std::vector<VdfValue> children;
  std::string text;
  int32_t int32 = 0;
  uint64_t uint64 = 0;

  VdfValue* Find(const std::string_view name) {
    for (auto& child : children) {
      if (child.key == name) {
        return &child;
      }
    }
    return nullptr;
  }
};

VdfValue MakeString(std::string key, std::string text) {
  VdfValue value;
  value.key = std::move(key);
  value.type = VdfValue::Type::kString;
  value.text = std::move(text);
  return value;
}

VdfValue MakeInt32(std::string key, int32_t number) {
  VdfValue value;
  value.key = std::move(key);
  value.type = VdfValue::Type::kInt32;
  value.int32 = number;
  return value;
}

class VdfReader {
 public:
  VdfReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool ReadRoot(VdfValue* out) {
    uint8_t marker = 0;
    if (!ReadByte(&marker) || marker != kVdfMap) {
      return false;
    }
    VdfValue top;
    if (!ReadString(&top.key)) {
      return false;
    }
    if (!ReadMap(&top)) {
      return false;
    }
    // One more end marker closes the outermost map.
    if (!ReadByte(&marker) || marker != kVdfEnd) {
      return false;
    }
    out->type = VdfValue::Type::kMap;
    out->children.clear();
    out->children.push_back(std::move(top));
    return position_ == size_;
  }

 private:
  bool ReadByte(uint8_t* out) {
    if (position_ >= size_) {
      return false;
    }
    *out = data_[position_++];
    return true;
  }

  bool ReadString(std::string* out) {
    const size_t start = position_;
    while (position_ < size_ && data_[position_] != 0) {
      ++position_;
    }
    if (position_ >= size_) {
      return false;
    }
    out->assign(reinterpret_cast<const char*>(data_ + start),
                position_ - start);
    ++position_;
    return true;
  }

  bool ReadMap(VdfValue* out) {
    out->type = VdfValue::Type::kMap;
    for (;;) {
      uint8_t marker = 0;
      if (!ReadByte(&marker)) {
        return false;
      }
      if (marker == kVdfEnd) {
        return true;
      }
      VdfValue child;
      if (!ReadString(&child.key)) {
        return false;
      }
      switch (marker) {
        case kVdfMap:
          if (!ReadMap(&child)) {
            return false;
          }
          break;
        case kVdfString:
          child.type = VdfValue::Type::kString;
          if (!ReadString(&child.text)) {
            return false;
          }
          break;
        case kVdfInt32: {
          if (position_ + 4 > size_) {
            return false;
          }
          uint32_t raw = 0;
          std::memcpy(&raw, data_ + position_, 4);
          position_ += 4;
          child.type = VdfValue::Type::kInt32;
          child.int32 = static_cast<int32_t>(raw);
          break;
        }
        case kVdfUInt64: {
          if (position_ + 8 > size_) {
            return false;
          }
          uint64_t raw = 0;
          std::memcpy(&raw, data_ + position_, 8);
          position_ += 8;
          child.type = VdfValue::Type::kUInt64;
          child.uint64 = raw;
          break;
        }
        default:
          return false;
      }
      out->children.push_back(std::move(child));
    }
  }

  const uint8_t* data_;
  size_t size_;
  size_t position_ = 0;
};

void AppendKey(const std::string& key, std::vector<uint8_t>* out) {
  out->insert(out->end(), key.begin(), key.end());
  out->push_back(0);
}

void WriteMap(const VdfValue& value, std::vector<uint8_t>* out) {
  for (const auto& child : value.children) {
    switch (child.type) {
      case VdfValue::Type::kMap:
        out->push_back(kVdfMap);
        AppendKey(child.key, out);
        WriteMap(child, out);
        out->push_back(kVdfEnd);
        break;
      case VdfValue::Type::kString:
        out->push_back(kVdfString);
        AppendKey(child.key, out);
        out->insert(out->end(), child.text.begin(), child.text.end());
        out->push_back(0);
        break;
      case VdfValue::Type::kInt32: {
        out->push_back(kVdfInt32);
        AppendKey(child.key, out);
        const auto raw = static_cast<uint32_t>(child.int32);
        for (int i = 0; i < 4; ++i) {
          out->push_back(static_cast<uint8_t>(raw >> (i * 8)));
        }
        break;
      }
      case VdfValue::Type::kUInt64: {
        out->push_back(kVdfUInt64);
        AppendKey(child.key, out);
        for (int i = 0; i < 8; ++i) {
          out->push_back(static_cast<uint8_t>(child.uint64 >> (i * 8)));
        }
        break;
      }
    }
  }
}

std::vector<uint8_t> SerializeRoot(const VdfValue& root) {
  std::vector<uint8_t> out;
  WriteMap(root, &out);
  out.push_back(kVdfEnd);
  return out;
}

uint32_t Crc32(const std::string& text) {
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char byte : text) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
  }
  return ~crc;
}

std::string ReadTrimmedFile(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream) {
    return "";
  }
  std::string contents((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
  return std::string(xe::string_util::trim(contents));
}

std::string ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return "";
  }
  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

bool IsSteamDeck() {
  const std::string vendor = ReadTrimmedFile("/sys/class/dmi/id/sys_vendor");
  const std::string product = ReadTrimmedFile("/sys/class/dmi/id/product_name");
  // Jupiter is the LCD deck, Galileo the OLED one.
  if (vendor == "Valve" && (product == "Jupiter" || product == "Galileo")) {
    return true;
  }
  // A reimaged deck, or SteamOS elsewhere, still wants the shortcut.
  std::ifstream release("/etc/os-release");
  std::string line;
  while (std::getline(release, line)) {
    if (line == "VARIANT_ID=steamdeck") {
      return true;
    }
  }
  return false;
}

bool IsSteamRunning() {
  std::error_code ec;
  std::filesystem::directory_iterator it("/proc", ec);
  if (ec) {
    return false;
  }
  for (const auto& entry : it) {
    const std::string name = entry.path().filename().string();
    if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) {
      continue;
    }
    if (ReadTrimmedFile(entry.path() / "comm") == "steam") {
      return true;
    }
  }
  return false;
}

// Every account signed in on this machine; a shortcut belongs to one.
std::vector<std::filesystem::path> SteamUserConfigDirs() {
  std::vector<std::filesystem::path> dirs;
  const char* home = std::getenv("HOME");
  if (!home) {
    return dirs;
  }
  const std::filesystem::path roots[] = {
      std::filesystem::path(home) / ".steam" / "steam" / "userdata",
      std::filesystem::path(home) / ".local" / "share" / "Steam" / "userdata",
  };
  std::error_code ec;
  for (const auto& root : roots) {
    if (!std::filesystem::is_directory(root, ec)) {
      continue;
    }
    std::filesystem::directory_iterator it(root, ec);
    if (ec) {
      continue;
    }
    for (const auto& entry : it) {
      const std::string name = entry.path().filename().string();
      // "0" is the logged-out placeholder rather than an account.
      if (name == "0" ||
          name.find_first_not_of("0123456789") != std::string::npos) {
        continue;
      }
      auto config = entry.path() / "config";
      if (std::filesystem::is_directory(config, ec)) {
        dirs.push_back(std::move(config));
      }
    }
    if (!dirs.empty()) {
      break;
    }
  }
  return dirs;
}

std::string Unquote(const std::string& text) {
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    return text.substr(1, text.size() - 2);
  }
  return text;
}

VdfValue MakeShortcut(const std::string& index, const std::string& exe,
                      const std::string& start_dir, const std::string& icon,
                      const std::string& launch_options) {
  const std::string quoted_exe = "\"" + exe + "\"";
  VdfValue entry;
  entry.key = index;
  entry.type = VdfValue::Type::kMap;
  const uint32_t appid =
      Crc32(quoted_exe + kSteamDeckShortcutName) | 0x80000000u;
  entry.children.push_back(MakeInt32("appid", static_cast<int32_t>(appid)));
  entry.children.push_back(MakeString("AppName", kSteamDeckShortcutName));
  entry.children.push_back(MakeString("Exe", quoted_exe));
  entry.children.push_back(MakeString("StartDir", "\"" + start_dir + "\""));
  entry.children.push_back(MakeString("icon", icon));
  entry.children.push_back(MakeString("ShortcutPath", ""));
  entry.children.push_back(MakeString("LaunchOptions", launch_options));
  entry.children.push_back(MakeInt32("IsHidden", 0));
  entry.children.push_back(MakeInt32("AllowDesktopConfig", 1));
  entry.children.push_back(MakeInt32("AllowOverlay", 1));
  entry.children.push_back(MakeInt32("OpenVR", 0));
  entry.children.push_back(MakeInt32("Devkit", 0));
  entry.children.push_back(MakeString("DevkitGameID", ""));
  entry.children.push_back(MakeInt32("DevkitOverrideAppID", 0));
  entry.children.push_back(MakeInt32("LastPlayTime", 0));
  entry.children.push_back(MakeString("FlatpakAppID", ""));
  VdfValue tags;
  tags.key = "tags";
  tags.type = VdfValue::Type::kMap;
  entry.children.push_back(std::move(tags));
  return entry;
}

// Already there, by the name the user sees or by what it runs. Either is
// enough: matching only one would add a second copy of something already
// present under the other.
bool AlreadyPresent(VdfValue* shortcuts, const std::string& exe) {
  for (auto& entry : shortcuts->children) {
    if (entry.type != VdfValue::Type::kMap) {
      continue;
    }
    if (auto* name = entry.Find("AppName")) {
      if (name->type == VdfValue::Type::kString &&
          name->text == kSteamDeckShortcutName) {
        return true;
      }
    }
    if (auto* target = entry.Find("Exe")) {
      if (target->type == VdfValue::Type::kString &&
          Unquote(target->text) == exe) {
        return true;
      }
    }
  }
  return false;
}

bool ParseShortcuts(const std::string& data, VdfValue* out) {
  VdfReader reader(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  return reader.ReadRoot(out);
}

}  // namespace

SteamShortcutResult EnsureSteamDeckShortcut(
    const std::filesystem::path& storage_root) {
  if (!cvars::steam_deck_shortcut || !IsSteamDeck()) {
    return SteamShortcutResult::kNotADeck;
  }

  const auto exe_path = xe::filesystem::GetExecutablePath();
  const std::string exe = exe_path.string();
  const std::string start_dir = exe_path.parent_path().string();

  auto config_dirs = SteamUserConfigDirs();
  if (config_dirs.empty()) {
    XELOGW("Steam Deck: no Steam account to add '{}' to",
           kSteamDeckShortcutName);
    return SteamShortcutResult::kFailed;
  }

  // Settle this before touching anything: already in the library for every
  // account means there is no reason to open Steam's files for writing.
  std::vector<std::filesystem::path> missing_from;
  for (const auto& config_dir : config_dirs) {
    auto path = config_dir / "shortcuts.vdf";
    const std::string data = ReadBinaryFile(path);
    if (!data.empty()) {
      VdfValue root;
      if (!ParseShortcuts(data, &root)) {
        XELOGW("Steam Deck: {} is not a shortcuts file this understands",
               xe::path_to_utf8(path));
        return SteamShortcutResult::kFailed;
      }
      auto* shortcuts = root.Find("shortcuts");
      if (shortcuts && AlreadyPresent(shortcuts, exe)) {
        continue;
      }
    }
    missing_from.push_back(config_dir);
  }
  if (missing_from.empty()) {
    return SteamShortcutResult::kAlreadyPresent;
  }

  // Steam keeps the list in memory and writes it over the file on the way
  // out, so adding it now would simply be thrown away.
  if (IsSteamRunning()) {
    XELOGW(
        "Steam Deck: '{}' is not in Steam's library yet. Close Steam and "
        "start Nexia once more and it will be added.",
        kSteamDeckShortcutName);
    return SteamShortcutResult::kSteamRunning;
  }

  // Steam launches this straight from the desktop session, which is Wayland,
  // and the Vulkan surface needs an X11 window - so pin the backend the same
  // way a launcher would. The storage root travels with it so the library
  // entry lands where every other way of starting it does.
  const std::string launch_options =
      "GDK_BACKEND=x11 %command% --storage_root=\"" + storage_root.string() +
      "\"";

  std::string icon;
  std::error_code ec;
  for (const char* candidate :
       {"Nexia On Steamdeck.png", "Nexia360.png", "icon.png"}) {
    auto path = exe_path.parent_path() / candidate;
    if (std::filesystem::exists(path, ec)) {
      icon = path.string();
      break;
    }
  }

  bool added = false;
  for (const auto& config_dir : missing_from) {
    auto path = config_dir / "shortcuts.vdf";
    const std::string data = ReadBinaryFile(path);

    VdfValue root;
    if (data.empty()) {
      VdfValue shortcuts;
      shortcuts.key = "shortcuts";
      shortcuts.type = VdfValue::Type::kMap;
      root.type = VdfValue::Type::kMap;
      root.children.push_back(std::move(shortcuts));
    } else {
      if (!ParseShortcuts(data, &root)) {
        continue;
      }
      // NOTHING IS WRITTEN UNTIL THIS FILE CAN BE REPRODUCED EXACTLY.
      // Writing out what was read has to give back the same bytes; if it
      // does not, this does not understand the format well enough to be
      // editing somebody's Steam library.
      const auto round_trip = SerializeRoot(root);
      if (round_trip.size() != data.size() ||
          std::memcmp(round_trip.data(), data.data(), data.size()) != 0) {
        XELOGE(
            "Steam Deck: refusing to edit {} - reading and writing it back "
            "does not reproduce it",
            xe::path_to_utf8(path));
        return SteamShortcutResult::kFailed;
      }
    }

    auto* shortcuts = root.Find("shortcuts");
    if (!shortcuts || shortcuts->type != VdfValue::Type::kMap) {
      continue;
    }

    size_t index = 0;
    while (shortcuts->Find(std::to_string(index))) {
      ++index;
    }
    shortcuts->children.push_back(MakeShortcut(
        std::to_string(index), exe, start_dir, icon, launch_options));

    if (!data.empty()) {
      auto backup = path;
      backup += ".nexia-bak";
      std::filesystem::copy_file(
          path, backup, std::filesystem::copy_options::overwrite_existing, ec);
    }

    const auto bytes = SerializeRoot(root);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      XELOGE("Steam Deck: could not write {}", xe::path_to_utf8(path));
      return SteamShortcutResult::kFailed;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    added = true;
    XELOGI("Steam Deck: added '{}' to {}", kSteamDeckShortcutName,
           xe::path_to_utf8(path));
  }

  return added ? SteamShortcutResult::kAdded : SteamShortcutResult::kFailed;
}

}  // namespace app
}  // namespace xe
