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
#include "xenia/base/utf8.h"
#include "xenia/cpu/lzx.h"
#include "xenia/kernel/xam/xam_ui_new.h"
#include "xenia/kernel/xam/xui_font.h"
#include "xenia/kernel/xam/xui_runtime.h"

// Defined in xex_module.cc, which is where the Rijndael tables are compiled
// in - including them here as well would define them twice. CBC with a zero
// IV, which is what both of this file's uses want.
void aes_decrypt_buffer(const uint8_t* session_key, const uint8_t* input_buffer,
                        const size_t input_size, uint8_t* output_buffer,
                        const size_t output_size);

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

constexpr size_t kPageSize = 0x200;
constexpr size_t kClusterSize = 0x1000;

constexpr uint32_t kOptFileFormatInfo = 0x000003FF;
constexpr uint32_t kOptResourceInfo = 0x000002FF;
constexpr uint32_t kOptExecutionInfo = 0x00040006;
constexpr uint32_t kOptOriginalPeName = 0x000183FF;
constexpr uint32_t kSecurityLoadAddress = 0x110;
constexpr uint32_t kSecurityAesKey = 0x150;

// The XEX2 retail key, as xex_module.cc has it. A flash module's own key sits
// wrapped in its security info and this is what unwraps it.
constexpr uint8_t kXex2RetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28,
                                        0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB,
                                        0x08, 0x96, 0xBF, 0x91};

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

// How much of a page a raw dump of a part this big spends on data and how
// much on the out-of-band bytes. The 16 MB parts are small block, 16 spare
// per 512; everything above is big block, 64 per 2048. Those two are the same
// ratio, so a raw dump's SIZE cannot say which it is - the capacity has to,
// and reading the spare to find out would be exactly the per-revision
// knowledge this path exists to avoid.
struct SpareGeometry {
  size_t page;
  size_t spare;
};

SpareGeometry GeometryFor(size_t capacity) {
  return capacity <= 16 * kMiB ? SpareGeometry{0x200, 0x10}
                               : SpareGeometry{0x800, 0x40};
}

// The capacity a raw dump of this size holds, or 0 if no part matches.
size_t RawCapacity(size_t size) {
  for (size_t capacity : kCapacities) {
    const SpareGeometry geometry = GeometryFor(capacity);
    if (size == capacity / geometry.page * (geometry.page + geometry.spare)) {
      return capacity;
    }
  }
  return 0;
}

// Detection is by size, never by spare content.
ImageKind ClassifyImage(size_t size) {
  if (RawCapacity(size)) {
    return ImageKind::kRaw;
  }
  if (IsKnownCapacity(size)) {
    return ImageKind::kFlat;
  }
  return ImageKind::kUnknown;
}

void StripSpare(std::vector<uint8_t>* data) {
  const size_t capacity = RawCapacity(data->size());
  if (!capacity) {
    return;
  }
  const SpareGeometry geometry = GeometryFor(capacity);
  const size_t stride = geometry.page + geometry.spare;
  const size_t pages = data->size() / stride;
  for (size_t page = 0; page < pages; ++page) {
    std::memmove(data->data() + page * geometry.page,
                 data->data() + page * stride, geometry.page);
  }
  data->resize(pages * geometry.page);
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

// A module's PE data in the clear. A retail console's NAND holds its modules
// plainly, but an image built for one carries them encrypted with the retail
// key, and until that is undone nothing in them - the SHA-1 block chain, the
// LZX windows - reads as anything.
bool DecryptPeData(const uint8_t* data, size_t size, uint32_t security_offset,
                   uint32_t pe_offset, std::vector<uint8_t>* out) {
  if (size_t(security_offset) + kSecurityAesKey + 16 > size ||
      pe_offset >= size) {
    return false;
  }
  uint8_t session_key[16];
  aes_decrypt_buffer(kXex2RetailKey, data + security_offset + kSecurityAesKey,
                     sizeof(session_key), session_key, sizeof(session_key));
  // Whole blocks only: a trailing partial one is padding either way.
  const size_t body = (size - pe_offset) & ~size_t(0xF);
  if (!body) {
    return false;
  }
  out->resize(body);
  aes_decrypt_buffer(session_key, data + pe_offset, body, out->data(), body);
  return true;
}

// Turn a XEX2 into its loaded image.
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
  if (header.pe_offset >= size || !header.image_size) {
    return false;
  }
  const uint8_t* pe = data + header.pe_offset;
  size_t pe_size = size - header.pe_offset;
  std::vector<uint8_t> plain;
  if (encryption != 0) {
    if (!DecryptPeData(data, size, header.security_offset, header.pe_offset,
                       &plain)) {
      return false;
    }
    pe = plain.data();
    pe_size = plain.size();
  }

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

// THE NAND'S OWN FILESYSTEM. A root block is 0x20 pages: the even ones hold
// the next-block map, one big-endian uint16 per 0x4000-byte block (bit 15 is a
// flag), the odd ones 16 directory entries of 0x20 bytes - name[22], start
// block, size, timestamp. A name whose first byte is 0x05 is a superseded
// entry. A file follows the map from its start block, which is why xam
// (0x250000 bytes from block 0x3BE) breaks off at 0xF70000 and goes on at
// 0x9C000. The chip keeps several generations of the root, and a logical
// image has lost the spare area that says which is live, so each is scored by
// how many of its modules' SHA-1 block chains verify when read through its
// map: a stale root reads blocks that have since been rewritten. On the
// reference dump that picks the root every one of whose 33 files matches a
// known-good extraction.
constexpr size_t kFlashBlockSize = 0x4000;
constexpr size_t kFlashMapBlocks = (kFlashBlockSize / kPageSize / 2) * 0x100;

struct FlashFile {
  std::string name;
  uint16_t start_block = 0;
  uint32_t size = 0;
};

size_t FlashBlockCount(const std::vector<uint8_t>& image) {
  return std::min(image.size() / kFlashBlockSize, kFlashMapBlocks);
}

uint16_t FlashNextBlock(const std::vector<uint8_t>& image, size_t root,
                        size_t block) {
  const size_t at =
      root + (block / 0x100) * 2 * kPageSize + (block % 0x100) * 2;
  return Read16(image.data() + at) & 0x7FFF;
}

bool ReadFlashDirectory(const std::vector<uint8_t>& image, size_t root,
                        std::vector<FlashFile>* out) {
  out->clear();
  const size_t blocks = FlashBlockCount(image);
  for (size_t page = 1; page < kFlashBlockSize / kPageSize; page += 2) {
    const uint8_t* base = image.data() + root + page * kPageSize;
    for (size_t i = 0; i < kPageSize / 0x20; ++i) {
      const uint8_t* entry = base + i * 0x20;
      if (entry[0] == 0x00 || entry[0] == 0xFF || entry[0] == 0x05) {
        continue;
      }
      FlashFile file;
      for (size_t c = 0; c < 22 && entry[c]; ++c) {
        const char value = static_cast<char>(entry[c]);
        if (!std::isalnum(static_cast<unsigned char>(value)) && value != '.' &&
            value != '_' && value != '-' && value != '$') {
          return false;
        }
        file.name.push_back(value);
      }
      file.start_block = Read16(entry + 0x16);
      file.size = Read32(entry + 0x18);
      if (file.name.front() == '.' ||
          file.name.find('.') == std::string::npos ||
          file.start_block >= blocks || file.size > image.size()) {
        return false;
      }
      out->push_back(std::move(file));
    }
  }
  return !out->empty();
}

bool ReadFlashFile(const std::vector<uint8_t>& image, size_t root,
                   const FlashFile& file, std::vector<uint8_t>* out) {
  out->clear();
  out->reserve(file.size);
  const size_t blocks = FlashBlockCount(image);
  size_t block = file.start_block;
  size_t steps = 0;
  while (out->size() < file.size) {
    if (block >= blocks || ++steps > blocks) {
      return false;
    }
    const size_t take =
        std::min(kFlashBlockSize, size_t(file.size) - out->size());
    const auto from = image.begin() + block * kFlashBlockSize;
    out->insert(out->end(), from, from + take);
    block = FlashNextBlock(image, root, block);
  }
  return true;
}

// True when a compressed XEX's own SHA-1 block chain holds end to end.
bool XexChainVerifies(const std::vector<uint8_t>& data) {
  if (data.size() < 0x18 || std::memcmp(data.data(), "XEX2", 4) != 0) {
    return false;
  }
  const uint32_t count = Read32(data.data() + 0x14);
  if (!count || count > 256 || size_t(0x18) + size_t(count) * 8 > data.size()) {
    return false;
  }
  uint32_t info = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = data.data() + 0x18 + size_t(i) * 8;
    if (Read32(entry) == kOptFileFormatInfo) {
      info = Read32(entry + 4);
    }
  }
  if (!info || size_t(info) + 0x24 > data.size() ||
      Read16(data.data() + info + 6) != 2) {
    return false;
  }
  const uint32_t pe_offset = Read32(data.data() + 0x08);
  if (pe_offset >= data.size()) {
    return false;
  }
  const uint8_t* body = data.data() + pe_offset;
  size_t body_size = data.size() - pe_offset;
  std::vector<uint8_t> plain;
  if (Read16(data.data() + info + 4) != 0) {
    if (!DecryptPeData(data.data(), data.size(), Read32(data.data() + 0x10),
                       pe_offset, &plain)) {
      return false;
    }
    body = plain.data();
    body_size = plain.size();
  }
  size_t pos = 0;
  uint32_t size = Read32(data.data() + info + 0xC);
  uint8_t digest[20];
  std::memcpy(digest, data.data() + info + 0x10, sizeof(digest));
  while (size) {
    if (size < 24 || pos + size > body_size) {
      return false;
    }
    uint8_t actual[20];
    Sha1(body + pos, size, actual);
    if (std::memcmp(actual, digest, sizeof(digest)) != 0) {
      return false;
    }
    std::memcpy(digest, body + pos + 4, sizeof(digest));
    const uint32_t next = Read32(body + pos);
    pos += size;
    size = next;
  }
  return true;
}

// A file that came out of the map beginning with something the chip's own
// directory holds. Weaker than the block chain, and the only thing left on an
// image whose modules have been patched: a patched module keeps its name and
// its place while its chain no longer closes.
bool FlashFileReadsBack(const std::vector<uint8_t>& data) {
  if (data.size() < 4) {
    return false;
  }
  if (!std::memcmp(data.data(), "XEX2", 4)) {
    if (data.size() < 0x18) {
      return false;
    }
    const uint32_t count = Read32(data.data() + 0x14);
    return count && count <= 256 &&
           size_t(0x18) + size_t(count) * 8 <= data.size() &&
           Read32(data.data() + 0x08) < data.size() &&
           Read32(data.data() + 0x10) < data.size();
  }
  for (const char* magic : {"xttf", "CRLP", "DAEP", "PIRS", "LIVE", "CON "}) {
    if (!std::memcmp(data.data(), magic, 4)) {
      return true;
    }
  }
  return false;
}

// The live root, or SIZE_MAX when no block reads as one. A stale generation
// reads blocks that have since been rewritten, so it is ranked below one
// whose modules verify, and below one whose files at least read back.
size_t FindFlashRoot(const std::vector<uint8_t>& image,
                     std::vector<FlashFile>* out_files) {
  size_t best_root = SIZE_MAX;
  std::tuple<size_t, size_t, size_t> best_rank{0, 0, 0};
  std::vector<FlashFile> files;
  std::vector<uint8_t> data;
  for (size_t block = 0; block < FlashBlockCount(image); ++block) {
    const size_t root = block * kFlashBlockSize;
    if (!ReadFlashDirectory(image, root, &files)) {
      continue;
    }
    size_t verified = 0;
    size_t reads_back = 0;
    for (const FlashFile& file : files) {
      if (!ReadFlashFile(image, root, file, &data)) {
        continue;
      }
      if (FlashFileReadsBack(data)) {
        ++reads_back;
      }
      if (file.name.size() > 4 &&
          file.name.compare(file.name.size() - 4, 4, ".xex") == 0 &&
          XexChainVerifies(data)) {
        ++verified;
      }
    }
    const std::tuple<size_t, size_t, size_t> rank{verified, reads_back,
                                                  files.size()};
    if ((verified || reads_back) && rank > best_rank) {
      best_rank = rank;
      best_root = root;
      *out_files = files;
    }
  }
  return best_root;
}

// A XEX's execution-info version, 0 when it has none.
uint32_t XexVersionOf(const std::vector<uint8_t>& data) {
  if (data.size() < 0x18 || std::memcmp(data.data(), "XEX2", 4) != 0) {
    return 0;
  }
  const uint32_t count = Read32(data.data() + 0x14);
  if (!count || count > 256 || size_t(0x18) + size_t(count) * 8 > data.size()) {
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = data.data() + 0x18 + size_t(i) * 8;
    if (Read32(entry) != kOptExecutionInfo) {
      continue;
    }
    const uint32_t at = Read32(entry + 4);
    return size_t(at) + 8 <= data.size() ? Read32(data.data() + at + 4) : 0;
  }
  return 0;
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
// A module's ORIGINAL_PE_NAME, e.g. "bootanim.dll". Empty when it has none.
//
// ScanModules cannot be used to find every module: ParseXexHeader refuses one
// with no resources at all (it ends in `return !out->resources.empty()`), and
// bootanim.xex has none - it is pure code with a .XBMOVIE section. Its PE name
// is the one thing in its header that names it.
std::string ModulePeName(const uint8_t* data, size_t size) {
  if (size < 0x18 || std::memcmp(data, "XEX2", 4) != 0) {
    return {};
  }
  const uint32_t count = Read32(data + 0x14);
  if (!count || count > 256 || size_t(0x18) + size_t(count) * 8 > size) {
    return {};
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = data + 0x18 + size_t(i) * 8;
    if (Read32(entry) != kOptOriginalPeName) {
      continue;
    }
    const uint32_t at = Read32(entry + 4);
    if (size_t(at) + 8 > size) {
      return {};
    }
    const uint32_t length = Read32(data + at);
    if (length < 4 || size_t(at) + length > size) {
      return {};
    }
    std::string name(reinterpret_cast<const char*>(data + at + 4), length - 4);
    const size_t end = name.find('\0');
    if (end != std::string::npos) {
      name.resize(end);
    }
    return name;
  }
  return {};
}

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

// How far an object at `at` reaches ON THE FLASH: to the first cluster that is
// erased, zeroed, or starts another object. Exact only when free space or the
// next object follows, which on a NAND it does.
size_t ObjectExtent(const std::vector<uint8_t>& image, size_t at) {
  static const char* kMagics[] = {"XEX2", "xttf", "XTAF",
                                  "CON ", "LIVE", "PIRS"};
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
  return end - at;
}

// Flash fonts are loose files with no directory entry we can read, so the
// extent is whatever the object reaches to.
void ScanFonts(const std::vector<uint8_t>& image,
               const std::vector<std::pair<size_t, size_t>>& claimed,
               std::vector<std::pair<size_t, size_t>>* out) {
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
    out->push_back({at, at + ObjectExtent(image, at)});
  }
}

// The three file names XamGetLanguageTypeface hands a system title, against
// what each face calls itself in its own sfnt name table. Nothing about
// "Xbox TC" or "Xbox JK" resembles the names the console asks for, and only
// Segoe carries a PostScript name at all - the other two have a family name
// and nothing else. TC is the Chinese Latin face (xenonclatin, which the
// dashboard also uses as its CJK and symbol fallback) and JK the
// Japanese/Korean one.
std::string TypefaceFileFor(const Font& font) {
  static const struct {
    const char* identity;
    const char* file;
  } kFaces[] = {
      {"SegoeXbox-Light", "SegoeXbox-Light.xtt"},
      {"Segoe Xbox Light", "SegoeXbox-Light.xtt"},
      {"Xbox TC", "xenonclatin.xtt"},
      {"Xbox JK", "xenonjklatin.xtt"},
  };
  std::string file;
  for (const auto& face : kFaces) {
    if (font.name() == face.identity) {
      file = face.file;
      break;
    }
  }
  if (file.empty()) {
    return {};
  }
  // THE CHIP CARRIES TWO FACES PER FAMILY. "Xbox TC" is both a 1.1 MB face
  // covering 5806 codepoints including the CJK ideographs and a 96 KB one
  // covering 790 with none; "Xbox JK" is 1.7 MB against 28 KB the same way.
  // The console asks for xenonCLATIN and xenonJKLATIN - the Latin subsets - so
  // the full face keeps its own name and does not overwrite them.
  if (font.GlyphIndex(U'一') || font.GlyphIndex(U'あ') ||
      font.GlyphIndex(U'가')) {
    return {};
  }
  return file;
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

std::string DashboardTypefaceFor(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  if (!ReadWholeFile(path, &data) || data.empty()) {
    return {};
  }
  Font font;
  if (!font.Load(data.data(), data.size())) {
    return {};
  }
  return TypefaceFileFor(font);
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

bool ExtractFlashModule(const std::filesystem::path& image_path,
                        const std::string& pe_name,
                        const std::filesystem::path& out_file) {
  std::vector<uint8_t> image;
  if (!ReadWholeFile(image_path, &image)) {
    return false;
  }
  if (ClassifyImage(image.size()) == ImageKind::kRaw) {
    StripSpare(&image);
  }

  for (size_t at = 0; at + 0x18 < image.size(); at += kPageSize) {
    if (std::memcmp(image.data() + at, "XEX2", 4) != 0) {
      continue;
    }
    if (ModulePeName(image.data() + at, image.size() - at) != pe_name) {
      continue;
    }
    // The ORIGINAL bytes, not a flattened image: this is written out as a xex
    // for the loader to load normally, exactly as it sat on the chip.
    const size_t extent = ObjectExtent(image, at);
    std::vector<uint8_t> module(image.begin() + at,
                                image.begin() + at + extent);
    // A module can be fragmented across the NAND, in which case the run above
    // is not the whole thing - the same rebuild the asset extractor uses puts
    // it back together from its SHA-1 block chain. The rebuild is taken
    // whenever it succeeds: every byte of it is hash-verified, while a run
    // that spills into whatever follows the break can still flatten. xam
    // (xamc.dll) is one - 0x78000 bytes, then the rest 15 MB earlier.
    std::vector<uint8_t> rebuilt;
    XexHeader rebuilt_header;
    std::vector<uint8_t> flat;
    if (ReassembleXex(image, at, &rebuilt) &&
        ParseXexHeader(rebuilt.data(), rebuilt.size(), &rebuilt_header) &&
        FlattenXex(rebuilt.data(), rebuilt.size(), rebuilt_header, &flat)) {
      if (rebuilt.size() > module.size() ||
          !std::equal(rebuilt.begin(), rebuilt.end(), module.begin())) {
        XELOGI("xui assets: {} was fragmented in the NAND; rebuilt it",
               pe_name);
      }
      module = std::move(rebuilt);
    }
    // A module with no resources cannot be parsed or rebuilt by those -
    // bootanim is one - so a failure here is not proof of anything. The
    // bytes on the chip are written out either way and the loader decides.

    std::error_code ec;
    std::filesystem::create_directories(out_file.parent_path(), ec);
    if (!WriteWholeFile(out_file, module.data(), module.size())) {
      XELOGW("xui assets: could not write {}", xe::path_to_utf8(out_file));
      return false;
    }
    XELOGI("xui assets: {} -> {} ({} bytes)", pe_name,
           xe::path_to_utf8(out_file), module.size());
    return true;
  }

  // A chip whose modules carry no original-PE-name header cannot be searched
  // by PE name at all - a 1888-era image is one, where only launch.xex has
  // the header. Its own directory names them the way the caller asks for
  // them, so take it from there instead.
  std::vector<FlashFile> files;
  const size_t root = FindFlashRoot(image, &files);
  if (root == SIZE_MAX) {
    return false;
  }
  const std::string wanted =
      xe::utf8::lower_ascii(xe::path_to_utf8(out_file.filename()));
  for (const FlashFile& file : files) {
    std::vector<uint8_t> data;
    if (xe::utf8::lower_ascii(file.name) != wanted ||
        !ReadFlashFile(image, root, file, &data)) {
      continue;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_file.parent_path(), ec);
    if (!WriteWholeFile(out_file, data.data(), data.size())) {
      XELOGW("xui assets: could not write {}", xe::path_to_utf8(out_file));
      return false;
    }
    XELOGI("xui assets: {} from the chip's directory -> {} ({} bytes)",
           file.name, xe::path_to_utf8(out_file), data.size());
    return true;
  }
  return false;
}

AssetInstallReport ExtractFlashFiles(const std::filesystem::path& image_path,
                                     const std::filesystem::path& out_dir) {
  AssetInstallReport report;
  std::vector<uint8_t> image;
  if (!ReadWholeFile(image_path, &image)) {
    report.text =
        "Flash: could not read " + xe::path_to_utf8(image_path) + "\n";
    return report;
  }
  if (ClassifyImage(image.size()) == ImageKind::kRaw) {
    StripSpare(&image);
  }
  std::vector<FlashFile> files;
  const size_t root = FindFlashRoot(image, &files);
  if (root == SIZE_MAX) {
    report.text = "Flash: no filesystem root found in this image\n";
    return report;
  }
  report.ok = true;

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  std::string written;
  std::vector<uint8_t> data;
  for (const FlashFile& file : files) {
    if (!ReadFlashFile(image, root, file, &data)) {
      report.missing.push_back(file.name);
      continue;
    }
    // An older chip must not take a module back past the system update, so
    // where the copy already here is newer the chip's goes in as $flash_<name>
    // - where the loader looks after the plain name - rather than over it.
    // EVERY file comes off the chip either way: which name it lands under is
    // the only thing the version decides, never whether it is written.
    std::string name = file.name;
    const uint32_t version = XexVersionOf(data);
    if (version) {
      std::vector<uint8_t> current;
      if (ReadWholeFile(out_dir / file.name, &current) &&
          XexVersionOf(current) > version) {
        name = "$flash_" + file.name;
        report.text += "Flash: the " + file.name +
                       " already here is newer, so the chip's went in as " +
                       name + "\n";
      }
    }
    if (!WriteWholeFile(out_dir / name, data.data(), data.size())) {
      report.missing.push_back(name);
      continue;
    }
    ++report.package_count;
    written += (written.empty() ? "" : ", ") + name;
  }
  XELOGI("xui assets: flash root at {:X}, {} of {} file(s) written: {}", root,
         report.package_count, files.size(), written);
  report.text +=
      fmt::format("Flash: {} file(s) from the chip's directory: {}\n",
                  report.package_count, written);
  for (const auto& name : report.missing) {
    report.text += "Flash: could not write " + name + "\n";
  }
  return report;
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
    // WHAT THE MODULE OCCUPIES ON THE CHIP, NOT HOW BIG IT LOADS. image_size
    // is the DECOMPRESSED size: dash.xex reports 0x1028000 against a few
    // hundred KB of flash, so claiming that much from its offset covered the
    // whole rest of the NAND and hid every loose font behind it - the console's
    // own typefaces sit past the dashboard, and the extractor reported
    // "0 font(s)" on any image laid out that way.
    claimed.push_back(
        {candidate.offset,
         candidate.offset +
             std::min<size_t>(candidate.header.image_size,
                              ObjectExtent(image, candidate.offset))});

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
  std::set<std::string> placed_fonts;
  ScanFonts(image, claimed, &fonts);
  for (size_t i = 0; i < fonts.size(); ++i) {
    const uint8_t* data = image.data() + fonts[i].first;
    const size_t size = fonts[i].second - fonts[i].first;
    Font font;
    std::string name;
    if (font.Load(data, size)) {
      // Under the name the console asks for when this is one of the three it
      // asks for, so an end user ends up with xenonclatin.xtt rather than a
      // file called after the face inside it. XamGetLanguageTypeface names
      // these by file, and the update does not ship two of them at all.
      const std::string typeface = TypefaceFileFor(font);
      name = typeface.empty() ? SafeFileName(font.name())
                              : typeface.substr(0, typeface.size() - 4);
    }
    if (name.empty()) {
      name = fmt::format("font{}", i);
    }
    const std::filesystem::path target = out_dir / (name + ".xtt");
    if (!WriteWholeFile(target, data, size)) {
      continue;
    }
    ++report.font_count;
    placed_fonts.insert(name + ".xtt");
    manifest += fmt::format(
        "    {{ \"font\": \"{}.xtt\", \"size\": {}, \"source\": \"{:08X}\" "
        "}},\n",
        name, size, fonts[i].first);
  }

  // A FACE THE CHIP'S OWN DIRECTORY NAMES IS ALREADY UNDER THE NAME THE
  // CONSOLE ASKS FOR. The scan above has to guess, because the full faces
  // call themselves "Xbox TC" and "Xbox JK" and a chip that carries both the
  // full face and the Latin subset can only be told apart by what is inside
  // them - which is why the full one is left under its own name. A chip that
  // carries ONE face per family has no subset for it to lose to, and names it
  // xenonclatin.xtt in its directory, so the directory is taken at its word
  // for whatever the scan did not already name. Second, so the identities win.
  std::vector<FlashFile> font_files;
  const size_t font_root = FindFlashRoot(image, &font_files);
  if (font_root != SIZE_MAX) {
    std::vector<uint8_t> data;
    for (const FlashFile& file : font_files) {
      if (file.name.size() < 5 ||
          xe::utf8::lower_ascii(file.name.substr(file.name.size() - 4)) !=
              ".xtt" ||
          placed_fonts.count(file.name) ||
          !ReadFlashFile(image, font_root, file, &data)) {
        continue;
      }
      if (!WriteWholeFile(out_dir / file.name, data.data(), data.size())) {
        continue;
      }
      ++report.font_count;
      placed_fonts.insert(file.name);
      manifest += fmt::format(
          "    {{ \"font\": \"{}\", \"size\": {}, \"source\": \"{:08X}\" "
          "}},\n",
          file.name, data.size(), file.start_block * kFlashBlockSize);
    }
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
