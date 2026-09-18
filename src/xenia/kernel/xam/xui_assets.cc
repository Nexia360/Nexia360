/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_assets.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"
#include "xenia/cpu/lzx.h"
#include "xenia/kernel/xam/xam_ui_new.h"
#include "xenia/kernel/xam/xui_font.h"
#include "xenia/kernel/xam/xui_runtime.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

constexpr size_t kPageSize = 0x200;
constexpr size_t kSparePage = 0x210;
constexpr size_t kClusterSize = 0x1000;

constexpr uint32_t kOptFileFormatInfo = 0x000003FF;
constexpr uint32_t kOptResourceInfo = 0x000002FF;
constexpr uint32_t kOptExecutionInfo = 0x00040006;
constexpr uint32_t kSecurityLoadAddress = 0x110;

constexpr size_t kMiB = 1024 * 1024;
constexpr size_t kCapacities[] = {16 * kMiB, 64 * kMiB, 256 * kMiB, 512 * kMiB};

uint32_t Read32(const uint8_t* data) {
  return xe::load_and_swap<uint32_t>(data);
}
uint16_t Read16(const uint8_t* data) {
  return xe::load_and_swap<uint16_t>(data);
}

bool IsKnownCapacity(size_t size) {
  for (size_t capacity : kCapacities) {
    if (size == capacity) {
      return true;
    }
  }
  return false;
}

enum class ImageKind {
  kRaw,
  kFlat,
  kUnknown,
};

// Detection is by size, never by spare content: the spare layout and the ECC
// polynomial differ between small- and big-block parts, and that is exactly
// the per-revision knowledge this path exists to avoid.
ImageKind ClassifyImage(size_t size) {
  if (size % kSparePage == 0 &&
      IsKnownCapacity(size / kSparePage * kPageSize)) {
    return ImageKind::kRaw;
  }
  if (IsKnownCapacity(size)) {
    return ImageKind::kFlat;
  }
  return ImageKind::kUnknown;
}

void StripSpare(std::vector<uint8_t>* data) {
  const size_t pages = data->size() / kSparePage;
  for (size_t page = 0; page < pages; ++page) {
    std::memmove(data->data() + page * kPageSize,
                 data->data() + page * kSparePage, kPageSize);
  }
  data->resize(pages * kPageSize);
}

bool ReadWholeFile(const std::filesystem::path& path,
                   std::vector<uint8_t>* out_data) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || !size) {
    return false;
  }
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  out_data->resize(static_cast<size_t>(size));
  const size_t read = fread(out_data->data(), 1, out_data->size(), file);
  fclose(file);
  out_data->resize(read);
  return read != 0;
}

bool WriteWholeFile(const std::filesystem::path& path, const uint8_t* data,
                    size_t size) {
  FILE* file = xe::filesystem::OpenFile(path, "wb");
  if (!file) {
    return false;
  }
  const size_t written = size ? fwrite(data, 1, size, file) : 0;
  fclose(file);
  return written == size;
}

struct XexResource {
  std::string name;
  uint32_t address = 0;
  uint32_t size = 0;
};

struct XexHeader {
  uint32_t pe_offset = 0;
  uint32_t security_offset = 0;
  std::map<uint32_t, uint32_t> optional;
  uint32_t image_size = 0;
  uint32_t load_address = 0;
  uint32_t version = 0;
  std::vector<XexResource> resources;
  std::set<std::string> resource_names;
};

bool ParseXexHeader(const uint8_t* data, size_t size, XexHeader* out) {
  if (size < 0x18 || std::memcmp(data, "XEX2", 4) != 0) {
    return false;
  }
  out->pe_offset = Read32(data + 0x08);
  out->security_offset = Read32(data + 0x10);
  const uint32_t count = Read32(data + 0x14);
  if (!count || count > 256) {
    return false;
  }
  if (size_t(0x18) + size_t(count) * 8 > size) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = data + 0x18 + size_t(i) * 8;
    out->optional[Read32(entry)] = Read32(entry + 4);
  }
  if (size_t(out->security_offset) + kSecurityLoadAddress + 4 > size) {
    return false;
  }
  out->image_size = Read32(data + out->security_offset + 4);
  out->load_address =
      Read32(data + out->security_offset + kSecurityLoadAddress);
  const auto execution = out->optional.find(kOptExecutionInfo);
  if (execution != out->optional.end() &&
      size_t(execution->second) + 8 <= size) {
    out->version = Read32(data + execution->second + 4);
  }

  const auto resource = out->optional.find(kOptResourceInfo);
  if (resource == out->optional.end() || size_t(resource->second) + 4 > size) {
    return false;
  }
  const uint32_t total = Read32(data + resource->second);
  if (total < 20 || size_t(resource->second) + total > size) {
    return false;
  }
  const uint32_t entries = (total - 4) / 16;
  for (uint32_t i = 0; i < entries; ++i) {
    const uint8_t* entry = data + resource->second + 4 + size_t(i) * 16;
    std::string name(reinterpret_cast<const char*>(entry), 8);
    const size_t end = name.find('\0');
    if (end != std::string::npos) {
      name.resize(end);
    }
    while (!name.empty() && name.back() == ' ') {
      name.pop_back();
    }
    if (name.empty()) {
      continue;
    }
    out->resources.push_back({name, Read32(entry + 8), Read32(entry + 12)});
    out->resource_names.insert(name);
  }
  return !out->resources.empty();
}

// Turn a non-encrypted XEX2 into its loaded image.
bool FlattenXex(const uint8_t* data, size_t size, const XexHeader& header,
                std::vector<uint8_t>* out) {
  const auto format = header.optional.find(kOptFileFormatInfo);
  if (format == header.optional.end() || size_t(format->second) + 16 > size) {
    return false;
  }
  const uint8_t* info = data + format->second;
  const uint32_t info_size = Read32(info);
  const uint16_t encryption = Read16(info + 4);
  const uint16_t compression = Read16(info + 6);
  if (encryption != 0) {
    return false;
  }
  if (header.pe_offset >= size || !header.image_size) {
    return false;
  }
  const uint8_t* pe = data + header.pe_offset;
  const size_t pe_size = size - header.pe_offset;

  if (compression == 0) {
    out->assign(header.image_size, 0);
    std::memcpy(out->data(), pe, std::min<size_t>(header.image_size, pe_size));
    return true;
  }

  if (compression == 1) {
    if (info_size < 8) {
      return false;
    }
    out->clear();
    out->reserve(header.image_size);
    const uint32_t blocks = (info_size - 8) / 8;
    size_t at = 0;
    for (uint32_t i = 0; i < blocks; ++i) {
      const uint32_t data_size = Read32(info + 8 + size_t(i) * 8);
      const uint32_t zero_size = Read32(info + 8 + size_t(i) * 8 + 4);
      if (at + data_size > pe_size) {
        break;
      }
      out->insert(out->end(), pe + at, pe + at + data_size);
      out->insert(out->end(), zero_size, 0);
      at += data_size;
    }
    out->resize(header.image_size, 0);
    return true;
  }

  if (compression != 2) {
    return false;
  }
  const uint32_t window = Read32(info + 8);
  uint32_t block_size = Read32(info + 0xC);
  std::vector<uint8_t> chunks;
  size_t at = 0;
  while (block_size) {
    if (at + 24 > pe_size || at + block_size > pe_size) {
      break;
    }
    const uint32_t next_size = Read32(pe + at);
    const size_t next_at = at + block_size;
    size_t cursor = at + 24;
    while (cursor + 2 <= pe_size) {
      const uint32_t chunk = Read16(pe + cursor);
      cursor += 2;
      if (!chunk || cursor + chunk > pe_size) {
        break;
      }
      chunks.insert(chunks.end(), pe + cursor, pe + cursor + chunk);
      cursor += chunk;
    }
    at = next_at;
    block_size = next_size;
  }
  if (chunks.empty()) {
    return false;
  }
  out->assign(header.image_size, 0);
  return lzx_decompress(chunks.data(), chunks.size(), out->data(), out->size(),
                        window, nullptr, 0) == 0;
}

// The fragment search below hashes hundreds of megabytes of candidates, so
// this compresses whole 64-byte blocks rather than going a byte at a time, and
// a partly-fed stream can be copied so a shared prefix is hashed once.
class Sha1Stream {
 public:
  void Update(const uint8_t* data, size_t size) {
    length_ += size;
    if (buffered_) {
      const size_t take = std::min<size_t>(64 - buffered_, size);
      std::memcpy(buffer_ + buffered_, data, take);
      buffered_ += take;
      data += take;
      size -= take;
      if (buffered_ < 64) {
        return;
      }
      Compress(state_, buffer_);
      buffered_ = 0;
    }
    while (size >= 64) {
      Compress(state_, data);
      data += 64;
      size -= 64;
    }
    std::memcpy(buffer_, data, size);
    buffered_ = size;
  }

  void Digest(uint8_t out[20]) const {
    uint32_t state[5];
    std::memcpy(state, state_, sizeof(state));
    uint8_t block[64];
    std::memcpy(block, buffer_, buffered_);
    size_t at = buffered_;
    block[at++] = 0x80;
    if (at > 56) {
      std::memset(block + at, 0, 64 - at);
      Compress(state, block);
      at = 0;
    }
    std::memset(block + at, 0, 56 - at);
    const uint64_t bits = length_ * 8;
    for (int i = 0; i < 8; ++i) {
      block[56 + i] = uint8_t(bits >> (56 - i * 8));
    }
    Compress(state, block);
    for (int i = 0; i < 5; ++i) {
      out[i * 4 + 0] = uint8_t(state[i] >> 24);
      out[i * 4 + 1] = uint8_t(state[i] >> 16);
      out[i * 4 + 2] = uint8_t(state[i] >> 8);
      out[i * 4 + 3] = uint8_t(state[i]);
    }
  }

 private:
  static uint32_t Rotate(uint32_t value, int count) {
    return (value << count) | (value >> (32 - count));
  }

  static void Compress(uint32_t state[5], const uint8_t block[64]) {
    uint32_t words[80];
    for (int i = 0; i < 16; ++i) {
      words[i] = Read32(block + i * 4);
    }
    for (int i = 16; i < 80; ++i) {
      words[i] = Rotate(
          words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3],
             e = state[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const uint32_t temp = Rotate(a, 5) + f + e + k + words[i];
      e = d;
      d = c;
      c = Rotate(b, 30);
      b = a;
      a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
  }

  uint32_t state_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                        0xC3D2E1F0};
  uint8_t buffer_[64] = {};
  size_t buffered_ = 0;
  uint64_t length_ = 0;
};

void Sha1(const uint8_t* data, size_t size, uint8_t out[20]) {
  Sha1Stream stream;
  stream.Update(data, size);
  stream.Digest(out);
}

bool FinishSha1(const Sha1Stream& prefix, const uint8_t* data, size_t size,
                const uint8_t* expected) {
  Sha1Stream stream = prefix;
  stream.Update(data, size);
  uint8_t digest[20];
  stream.Digest(digest);
  return !std::memcmp(digest, expected, sizeof(digest));
}

// A block that does not hash straddles a NAND fragment boundary. Both the
// break and the continuation are cluster-aligned, so search those; the
// continuation is usually ahead of the break, so start there and wrap.
bool FindBrokenBlock(const std::vector<uint8_t>& image, size_t at,
                     uint32_t size, const uint8_t* digest,
                     std::vector<uint8_t>* out_block, size_t* out_next) {
  const size_t clusters = image.size() / kClusterSize;
  for (size_t cut = (at / kClusterSize + 1) * kClusterSize;
       cut < at + size && cut < image.size(); cut += kClusterSize) {
    Sha1Stream prefix;
    prefix.Update(image.data() + at, cut - at);
    const size_t remaining = size - (cut - at);
    if (remaining > image.size()) {
      break;
    }
    const size_t start = cut / kClusterSize;
    for (size_t step = 0; step < clusters; ++step) {
      const size_t candidate = (start + step) % clusters;
      const size_t source = candidate * kClusterSize;
      if (source + remaining > image.size()) {
        continue;
      }
      if (!FinishSha1(prefix, image.data() + source, remaining, digest)) {
        continue;
      }
      out_block->assign(image.begin() + at, image.begin() + cut);
      out_block->insert(out_block->end(), image.begin() + source,
                        image.begin() + source + remaining);
      *out_next = source + remaining;
      return true;
    }
  }
  return false;
}

// Rebuild a module that the NAND stored in more than one extent, using the
// XEX's own per-block SHA-1 chain: every payload byte is covered by a hash
// stored before it.
bool ReassembleXex(const std::vector<uint8_t>& image, size_t at,
                   std::vector<uint8_t>* out) {
  XexHeader header;
  if (!ParseXexHeader(image.data() + at, image.size() - at, &header)) {
    return false;
  }
  const auto format = header.optional.find(kOptFileFormatInfo);
  if (format == header.optional.end() ||
      at + format->second + 0x24 > image.size()) {
    return false;
  }
  const uint8_t* info = image.data() + at + format->second;
  if (Read16(info + 6) != 2) {
    return false;
  }
  if (at + header.pe_offset > image.size()) {
    return false;
  }
  out->assign(image.begin() + at, image.begin() + at + header.pe_offset);

  size_t pos = at + header.pe_offset;
  uint32_t size = Read32(info + 0xC);
  uint8_t digest[20];
  std::memcpy(digest, info + 0x10, sizeof(digest));

  std::vector<uint8_t> block;
  while (size) {
    if (size < 24) {
      return false;
    }
    bool ok = false;
    if (pos + size <= image.size()) {
      uint8_t actual[20];
      Sha1(image.data() + pos, size, actual);
      if (!std::memcmp(actual, digest, sizeof(digest))) {
        block.assign(image.begin() + pos, image.begin() + pos + size);
        pos += size;
        ok = true;
      }
    }
    if (!ok) {
      size_t next = 0;
      if (!FindBrokenBlock(image, pos, size, digest, &block, &next)) {
        return false;
      }
      pos = next;
    }
    out->insert(out->end(), block.begin(), block.end());
    size = Read32(block.data());
    std::memcpy(digest, block.data() + 4, sizeof(digest));
  }
  return true;
}

struct ResourceSpec {
  std::string resource;
  std::string out_name;
};

struct ModuleSpec {
  std::string module;
  std::set<std::string> names;
  bool exact = true;
  std::vector<ResourceSpec> take;
};

// Resource names are NOT unique across modules - "skin" and "xam" are in both
// huduiskin.xex and xam.xex with different content - so a module is identified
// by its whole resource-name set, never by one name.
const std::vector<ModuleSpec>& ModuleSpecs() {
  static const std::vector<ModuleSpec> specs = {
      {"vk", {"vk"}, true, {{"vk", "vk"}}},
      {"gamerprofile", {"gp"}, true, {{"gp", "gp"}}},
      {"createprofile", {"cp"}, true, {{"cp", "cp"}}},
      {"signin", {"signin"}, true, {{"signin", "signin"}}},
      {"huduiskin",
       {"skin", "xam"},
       true,
       {{"skin", "huduiskin"}, {"xam", "xamstrings"}}},
      {"xam",
       {"shrdres", "xam", "controlp", "fusion"},
       false,
       {{"shrdres", "sharedres"}, {"xam", "xam"}, {"controlp", "controlpack"}}},
      // The dashboard. Its scenes are Lua programs, so the scripts come out
      // alongside the scenes: luaxbox is the XUI class library every scene is
      // built on, soclua and hubapp are the dashboard's own. Its "controlp"
      // is NOT xam's, so every name here is prefixed.
      {"dash",
       {"luaxbox", "dashcomm", "dashlua", "dashuisk", "hubapp"},
       false,
       {{"luaxbox", "dashluaxbox"},
        {"controlp", "dashcontrolpack"},
        {"socxzp", "dashsocial"},
        {"soclua", "dashsociallua"},
        {"hubui", "dashhubui"},
        {"hubapp", "dashhubapp"},
        {"dashcomm", "dashcommon"},
        {"dashlua", "dashlua"},
        {"SharedUI", "dashsharedui"},
        {"gamer", "dashgamer"},
        {"dashuisk", "dashskin"}}},
  };
  return specs;
}

const ModuleSpec* MatchModule(const XexHeader& header) {
  for (const ModuleSpec& spec : ModuleSpecs()) {
    if (spec.exact) {
      if (header.resource_names == spec.names) {
        return &spec;
      }
      continue;
    }
    if (std::includes(header.resource_names.begin(),
                      header.resource_names.end(), spec.names.begin(),
                      spec.names.end())) {
      return &spec;
    }
  }
  return nullptr;
}

struct Candidate {
  size_t offset = 0;
  XexHeader header;
  const ModuleSpec* spec = nullptr;
  uint32_t build = 0;
  uint32_t qfe = 0;
};

// Flash keeps pre-update copies of some modules (the reference dump has build
// 16547 vk/signin beside the live 17559), so keep the newest per module.
std::map<std::string, Candidate> ScanModules(
    const std::vector<uint8_t>& image) {
  std::map<std::string, Candidate> best;
  for (size_t at = 0; at + 0x18 < image.size(); at += kPageSize) {
    if (std::memcmp(image.data() + at, "XEX2", 4) != 0) {
      continue;
    }
    Candidate candidate;
    candidate.offset = at;
    if (!ParseXexHeader(image.data() + at, image.size() - at,
                        &candidate.header)) {
      continue;
    }
    candidate.spec = MatchModule(candidate.header);
    if (!candidate.spec) {
      continue;
    }
    candidate.build = (candidate.header.version >> 8) & 0xFFFF;
    candidate.qfe = candidate.header.version & 0xFF;
    const auto existing = best.find(candidate.spec->module);
    if (existing == best.end() ||
        std::make_tuple(candidate.build, candidate.qfe, candidate.offset) >
            std::make_tuple(existing->second.build, existing->second.qfe,
                            existing->second.offset)) {
      best[candidate.spec->module] = std::move(candidate);
    }
  }
  return best;
}

// Flash fonts are loose files with no directory entry we can read, so the
// extent runs to the first cluster that is erased, zeroed, or starts another
// object. That is exact only when free space follows.
void ScanFonts(const std::vector<uint8_t>& image,
               const std::vector<std::pair<size_t, size_t>>& claimed,
               std::vector<std::pair<size_t, size_t>>* out) {
  static const char* kMagics[] = {"XEX2", "xttf", "XTAF",
                                  "CON ", "LIVE", "PIRS"};
  for (size_t at = 0; at + kClusterSize <= image.size(); at += kClusterSize) {
    if (std::memcmp(image.data() + at, "xttf", 4) != 0) {
      continue;
    }
    bool inside = false;
    for (const auto& range : claimed) {
      if (at >= range.first && at < range.second) {
        inside = true;
        break;
      }
    }
    if (inside) {
      continue;
    }
    size_t end = at + kClusterSize;
    while (end + kClusterSize <= image.size()) {
      const uint8_t* cluster = image.data() + end;
      bool bounded = false;
      for (const char* magic : kMagics) {
        if (!std::memcmp(cluster, magic, 4)) {
          bounded = true;
          break;
        }
      }
      if (!bounded) {
        bounded = true;
        for (size_t i = 0; i < kClusterSize; ++i) {
          if (cluster[i] != 0x00) {
            bounded = false;
            break;
          }
        }
        if (!bounded) {
          bounded = true;
          for (size_t i = 0; i < kClusterSize; ++i) {
            if (cluster[i] != 0xFF) {
              bounded = false;
              break;
            }
          }
        }
      }
      if (bounded) {
        break;
      }
      end += kClusterSize;
    }
    out->push_back({at, end});
  }
}

std::string SafeFileName(const std::string& name) {
  std::string out;
  for (char value : name) {
    if (std::isalnum(static_cast<unsigned char>(value)) || value == '-' ||
        value == '_' || value == '.') {
      out.push_back(value);
    }
  }
  return out;
}

}  // namespace

std::filesystem::path DefaultAssetDirectory() {
  return xe::filesystem::GetExecutableFolder() / "Dashboard" / "UI";
}

bool IsFlashImage(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || ClassifyImage(size) == ImageKind::kUnknown) {
    return false;
  }
  // Size alone is not enough - a 16 MiB anything would pass - so require the
  // NAND header magic or at least one page-aligned XEX2 near the front.
  std::vector<uint8_t> head;
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  head.resize(size_t(4) * 1024 * 1024);
  const size_t read = fread(head.data(), 1, head.size(), file);
  fclose(file);
  head.resize(read);
  if (read >= 2 && head[0] == 0xFF && (head[1] == 0x4F || head[1] == 0x4E)) {
    return true;
  }
  for (size_t at = 0; at + 4 <= read; at += kPageSize) {
    if (!std::memcmp(head.data() + at, "XEX2", 4)) {
      return true;
    }
  }
  return false;
}

AssetInstallReport InstallFromFlashImage(
    const std::filesystem::path& image_path,
    const std::filesystem::path& out_dir) {
  AssetInstallReport report;

  std::vector<uint8_t> image;
  if (!ReadWholeFile(image_path, &image)) {
    report.text = "Could not read " + xe::path_to_utf8(image_path) + "\n";
    return report;
  }
  const ImageKind kind = ClassifyImage(image.size());
  if (kind == ImageKind::kRaw) {
    StripSpare(&image);
    XELOGI("xui assets: stripped NAND spare area, {} logical bytes",
           image.size());
  }

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    report.text = "Could not create " + xe::path_to_utf8(out_dir) + "\n";
    return report;
  }

  const auto modules = ScanModules(image);
  std::vector<std::pair<size_t, size_t>> claimed;
  std::string manifest;

  for (const ModuleSpec& spec : ModuleSpecs()) {
    const auto found = modules.find(spec.module);
    if (found == modules.end()) {
      report.missing.push_back(spec.module);
      continue;
    }
    const Candidate& candidate = found->second;
    std::vector<uint8_t> flat;
    if (!FlattenXex(image.data() + candidate.offset,
                    image.size() - candidate.offset, candidate.header, &flat)) {
      std::vector<uint8_t> rebuilt;
      XexHeader rebuilt_header;
      if (!ReassembleXex(image, candidate.offset, &rebuilt) ||
          !ParseXexHeader(rebuilt.data(), rebuilt.size(), &rebuilt_header) ||
          !FlattenXex(rebuilt.data(), rebuilt.size(), rebuilt_header, &flat)) {
        XELOGE("xui assets: {} at {:08X} could not be flattened", spec.module,
               candidate.offset);
        report.missing.push_back(spec.module);
        continue;
      }
      XELOGI(
          "xui assets: {} was fragmented in the NAND; rebuilt from its "
          "SHA-1 block chain",
          spec.module);
    }

    report.build = std::max(report.build, candidate.build);
    claimed.push_back(
        {candidate.offset, candidate.offset + candidate.header.image_size});

    for (const ResourceSpec& take : spec.take) {
      const XexResource* resource = nullptr;
      for (const XexResource& entry : candidate.header.resources) {
        if (entry.name == take.resource) {
          resource = &entry;
          break;
        }
      }
      if (!resource) {
        report.missing.push_back(spec.module + "/" + take.resource);
        continue;
      }
      const size_t at = resource->address - candidate.header.load_address;
      if (resource->address < candidate.header.load_address ||
          at + resource->size > flat.size()) {
        report.missing.push_back(spec.module + "/" + take.resource);
        continue;
      }
      const std::filesystem::path target = out_dir / (take.out_name + ".xzp");
      if (!WriteWholeFile(target, flat.data() + at, resource->size)) {
        report.missing.push_back(spec.module + "/" + take.resource);
        continue;
      }
      ++report.package_count;
      manifest += fmt::format(
          "    {{ \"module\": \"{}\", \"resource\": \"{}\", \"file\": "
          "\"{}.xzp\", \"size\": {}, \"build\": {} }},\n",
          spec.module, take.resource, take.out_name, resource->size,
          candidate.build);

      // The skin scene is read loose, not through a package.
      if (take.out_name == "huduiskin") {
        std::vector<uint8_t> payload(flat.begin() + at,
                                     flat.begin() + at + resource->size);
        Package package;
        if (package.LoadFromMemory(std::move(payload))) {
          if (const PackageEntry* entry = package.Find("skin.xur")) {
            WriteWholeFile(out_dir / "skin.xur", entry->data, entry->size);
          }
        }
      }
    }
  }

  std::vector<std::pair<size_t, size_t>> fonts;
  ScanFonts(image, claimed, &fonts);
  for (size_t i = 0; i < fonts.size(); ++i) {
    const uint8_t* data = image.data() + fonts[i].first;
    const size_t size = fonts[i].second - fonts[i].first;
    Font font;
    std::string name;
    if (font.Load(data, size)) {
      name = SafeFileName(font.name());
    }
    if (name.empty()) {
      name = fmt::format("font{}", i);
    }
    const std::filesystem::path target = out_dir / (name + ".xtt");
    if (!WriteWholeFile(target, data, size)) {
      continue;
    }
    ++report.font_count;
    manifest += fmt::format(
        "    {{ \"font\": \"{}.xtt\", \"size\": {}, \"source\": \"{:08X}\" "
        "}},\n",
        name, size, fonts[i].first);
  }

  if (!manifest.empty()) {
    manifest.resize(manifest.size() - 2);
  }
  const std::string json = fmt::format(
      "{{\n  \"manifest\": \"nexia-ui\",\n  \"format_version\": 1,\n"
      "  \"dashboard_build\": {},\n  \"files\": [\n{}\n  ]\n}}\n",
      report.build, manifest);
  WriteWholeFile(out_dir / "nexia-ui.json",
                 reinterpret_cast<const uint8_t*>(json.data()), json.size());

  report.ok = report.package_count != 0;
  if (report.ok) {
    report.text = fmt::format(
        "Installed {} UI package(s) and {} font(s) from dashboard build {}\n"
        "to {}\n",
        report.package_count, report.font_count, report.build,
        xe::path_to_utf8(out_dir));
  } else {
    report.text = "No Xbox 360 UI modules were found in " +
                  xe::path_to_utf8(image_path) + "\n";
  }
  if (!report.missing.empty()) {
    report.text += "Not present in this image: ";
    for (size_t i = 0; i < report.missing.size(); ++i) {
      report.text += (i ? ", " : "") + report.missing[i];
    }
    report.text += "\n";
  }
  return report;
}

namespace {

// Resources carried by the system update's module XEXs. The names are ours,
// chosen so nothing collides with the flash set - AvatarEditor has its own
// "controlp" that is NOT xam's.
struct UpdateResource {
  const char* module;
  const char* resource;
  const char* out_name;
};

const UpdateResource kUpdateResources[] = {
    {"AvatarEditor.xex", "MEDIA", "avatareditor"},
    {"AvatarEditor.xex", "common", "avatarcommon"},
    {"AvatarEditor.xex", "controlp", "avatarcontrolpack"},
    {"Guide.AvatarMiniCreator.xex", "avatarmc", "avatarmc"},
};

std::filesystem::path FindModule(const std::filesystem::path& directory,
                                 const std::string& name) {
  std::error_code ec;
  const std::string wanted = xe::utf8::lower_ascii(name);
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           directory,
           std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    if (xe::utf8::lower_ascii(xe::path_to_utf8(entry.path().filename())) ==
        wanted) {
      return entry.path();
    }
  }
  return std::filesystem::path();
}

}  // namespace

const std::vector<std::string>& SystemUpdatePackageNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    for (const UpdateResource& resource : kUpdateResources) {
      out.push_back(std::string(resource.out_name) + ".xzp");
    }
    return out;
  }();
  return names;
}

AssetInstallReport InstallFromSystemUpdate(
    const std::filesystem::path& directory,
    const std::filesystem::path& out_dir) {
  AssetInstallReport report;
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    report.text = "Could not create " + xe::path_to_utf8(out_dir) + "\n";
    return report;
  }

  std::string current_module;
  std::vector<uint8_t> flat;
  XexHeader header;
  bool module_ok = false;

  for (const UpdateResource& wanted : kUpdateResources) {
    if (current_module != wanted.module) {
      current_module = wanted.module;
      module_ok = false;
      flat.clear();
      const auto path = FindModule(directory, current_module);
      if (path.empty()) {
        report.missing.push_back(current_module);
        continue;
      }
      std::vector<uint8_t> image;
      if (!ReadWholeFile(path, &image)) {
        report.missing.push_back(current_module);
        continue;
      }
      header = XexHeader();
      if (!ParseXexHeader(image.data(), image.size(), &header) ||
          !FlattenXex(image.data(), image.size(), header, &flat)) {
        XELOGE("xui assets: {} could not be flattened", current_module);
        report.missing.push_back(current_module);
        continue;
      }
      module_ok = true;
    }
    if (!module_ok) {
      continue;
    }

    const XexResource* resource = nullptr;
    for (const XexResource& entry : header.resources) {
      if (entry.name == wanted.resource) {
        resource = &entry;
        break;
      }
    }
    const size_t at =
        resource ? size_t(resource->address - header.load_address) : 0;
    if (!resource || resource->address < header.load_address ||
        at + resource->size > flat.size()) {
      report.missing.push_back(std::string(wanted.module) + "/" +
                               wanted.resource);
      continue;
    }
    if (!WriteWholeFile(out_dir / (std::string(wanted.out_name) + ".xzp"),
                        flat.data() + at, resource->size)) {
      report.missing.push_back(std::string(wanted.module) + "/" +
                               wanted.resource);
      continue;
    }
    ++report.package_count;
  }

  report.ok = report.package_count != 0;
  report.text =
      report.ok ? fmt::format("Installed {} avatar UI package(s) to {}\n",
                              report.package_count, xe::path_to_utf8(out_dir))
                : "No avatar UI packages were found in the system update\n";
  if (!report.missing.empty()) {
    report.text += "Not present: ";
    for (size_t i = 0; i < report.missing.size(); ++i) {
      report.text += (i ? ", " : "") + report.missing[i];
    }
    report.text += "\n";
  }
  return report;
}

bool IsAssetArchiveRoot(const std::filesystem::path& directory) {
  std::error_code ec;
  if (std::filesystem::exists(directory / "nexia-ui.json", ec)) {
    return true;
  }
  ec.clear();
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           directory,
           std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (entry.is_regular_file(ec) &&
        entry.path().filename() == "nexia-ui.json") {
      return true;
    }
  }
  return false;
}

AssetInstallReport InstallFromArchiveRoot(
    const std::filesystem::path& directory,
    const std::filesystem::path& out_dir) {
  AssetInstallReport report;
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    report.text = "Could not create " + xe::path_to_utf8(out_dir) + "\n";
    return report;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           directory,
           std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string extension =
        xe::utf8::lower_ascii(xe::path_to_utf8(entry.path().extension()));
    if (extension != ".xzp" && extension != ".xtt" && extension != ".xur" &&
        extension != ".json") {
      continue;
    }
    std::filesystem::copy_file(
        entry.path(), out_dir / entry.path().filename(),
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (extension == ".xzp") {
      ++report.package_count;
    } else if (extension == ".xtt") {
      ++report.font_count;
    }
  }
  report.ok = report.package_count != 0;
  report.text =
      report.ok
          ? fmt::format("Installed {} UI package(s) and {} font(s) to {}\n",
                        report.package_count, report.font_count,
                        xe::path_to_utf8(out_dir))
          : "The archive held no Xbox 360 UI packages\n";
  return report;
}

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
