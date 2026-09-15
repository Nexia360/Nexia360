#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "third_party/mspack/lzx.h"
#include "third_party/mspack/mspack.h"
}

extern "C" void xenia_log(const char*, ...) {}

namespace {

struct Context {
  const uint8_t* input = nullptr;
  size_t input_size = 0;
  size_t input_at = 0;
  std::vector<uint8_t> output;
};

int Read(mspack_file* file, void* buffer, int bytes) {
  auto* c = reinterpret_cast<Context*>(file);
  const size_t take =
      std::min<size_t>(size_t(bytes), c->input_size - c->input_at);
  std::memcpy(buffer, c->input + c->input_at, take);
  c->input_at += take;
  return int(take);
}

int Write(mspack_file* file, void* buffer, int bytes) {
  auto* c = reinterpret_cast<Context*>(file);
  auto* in = static_cast<uint8_t*>(buffer);
  c->output.insert(c->output.end(), in, in + bytes);
  return bytes;
}

void* Alloc(mspack_system*, size_t bytes) { return std::malloc(bytes); }
void Free(void* p) { std::free(p); }
void Copy(void* src, void* dst, size_t bytes) { std::memcpy(dst, src, bytes); }

bool Decompress(const uint8_t* input, size_t input_size, size_t output_size,
                std::vector<uint8_t>* out) {
  Context c;
  c.input = input;
  c.input_size = input_size;
  mspack_system sys = {};
  sys.read = Read;
  sys.write = Write;
  sys.alloc = Alloc;
  sys.free = Free;
  sys.copy = Copy;
  lzxd_stream* lzx = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&c),
                               reinterpret_cast<mspack_file*>(&c), 15, 0,
                               32 * 1024, off_t(output_size), 0);
  if (!lzx) {
    return false;
  }
  const int status = lzxd_decompress(lzx, off_t(output_size));
  lzxd_free(lzx);
  *out = std::move(c.output);
  return status == MSPACK_ERR_OK && out->size() == output_size;
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

std::string Utf16BeName(const std::vector<uint8_t>& d, size_t at) {
  std::string name;
  while (at + 1 < d.size() && name.size() < 80) {
    const uint16_t unit = uint16_t(d[at] << 8) | d[at + 1];
    if (!unit) {
      break;
    }
    const bool safe = (unit >= '0' && unit <= '9') ||
                      (unit >= 'A' && unit <= 'Z') ||
                      (unit >= 'a' && unit <= 'z');
    name.push_back(safe ? char(unit) : '_');
    at += 2;
  }
  return name;
}

void WriteFile(const std::filesystem::path& path, const uint8_t* data,
               size_t size) {
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    return;
  }
  std::fwrite(data, 1, size, f);
  std::fclose(f);
}

bool DecodeChunks(const uint8_t* b, size_t at, size_t end, size_t* chunks,
                  std::vector<uint8_t>* output, std::string* status) {
  while (at + 12 <= end) {
    const uint32_t compressed = Le32(b + at);
    const uint32_t start = Le32(b + at + 4);
    const uint32_t size = Le32(b + at + 8);
    at += 12;
    if (!compressed || at + compressed > end) {
      *status = "bad chunk header";
      return false;
    }
    std::vector<uint8_t> chunk;
    if (!Decompress(b + at, compressed, size, &chunk)) {
      *status = "lzx failed";
      return false;
    }
    if (output->size() < size_t(start) + chunk.size()) {
      output->resize(size_t(start) + chunk.size());
    }
    std::memcpy(output->data() + start, chunk.data(), chunk.size());
    at += compressed;
    ++*chunks;
  }
  if (at != end) {
    *status = "chunk list overruns its record";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: strb_extract <toc> <out_dir>\n");
    return 1;
  }
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) {
    std::printf("cannot open %s\n", argv[1]);
    return 1;
  }
  std::fseek(f, 0, SEEK_END);
  const long file_size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> d(file_size);
  std::fread(d.data(), 1, file_size, f);
  std::fclose(f);

  const std::filesystem::path out_dir = argv[2];
  std::filesystem::create_directories(out_dir);

  const uint32_t version = Be32(d.data() + 0x18);
  const uint32_t count = Be32(d.data() + 0x1C);
  const uint32_t locales = version >= 2 ? Be32(d.data() + 0x28) : 13;
  const size_t first = version >= 2 ? 0x30 : 0x28;
  const size_t stride = 8 + 0xB4 + 4 * locales + 8;

  FILE* index = std::fopen((out_dir / "index.csv").string().c_str(), "w");
  std::fprintf(index,
               "index,kind,flags,name,blob_offset,blob_size,records,status\n");

  size_t extracted = 0;
  size_t failed = 0;
  size_t empty = 0;
  std::map<std::string, size_t> record_kinds;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t base = first + i * stride;
    const uint32_t kind = Be32(d.data() + base);
    const uint32_t flags = Be32(d.data() + base + 4);
    const size_t pointers = base + 8 + 0xB4;
    const uint32_t name_at = Be32(d.data() + pointers);
    const uint32_t blob = Be32(d.data() + pointers + 4 * locales);
    const uint32_t blob_size = Be32(d.data() + pointers + 4 * locales + 4);
    const std::string name =
        name_at && name_at < d.size() ? Utf16BeName(d, name_at) : "";
    if (!blob || !blob_size || blob + blob_size > d.size() ||
        std::memcmp(d.data() + blob, "STRB", 4) != 0) {
      ++empty;
      continue;
    }
    char stem[160];
    std::snprintf(stem, sizeof(stem), "%04u_%08X_%s", i, kind, name.c_str());
    const uint8_t* b = d.data() + blob;
    WriteFile(out_dir / (std::string(stem) + ".hdr"), b,
              std::min<size_t>(blob_size, 0x3C));

    std::string status = "ok";
    std::string records;
    size_t at = 0x3C;
    int record = 0;
    while (at + 12 <= blob_size && status == "ok") {
      const uint8_t tag = b[at];
      const uint32_t length = Be32(b + at + 1);
      const size_t payload = at + 12;
      const size_t end = payload + length;
      if (!tag || end > blob_size) {
        break;
      }
      char file[200];
      std::vector<uint8_t> output;
      size_t chunks = 0;
      if (tag == 2 || tag == 3) {
        if (!DecodeChunks(b, payload, end, &chunks, &output, &status)) {
          break;
        }
        std::snprintf(file, sizeof(file), "%s_r%d_t%u.bin", stem, record, tag);
        WriteFile(out_dir / file, output.data(), output.size());
      } else {
        std::snprintf(file, sizeof(file), "%s_r%d_t%u.raw", stem, record, tag);
        WriteFile(out_dir / file, b + payload, length);
        output.resize(length);
      }
      char summary[64];
      std::snprintf(summary, sizeof(summary), "%st%u:%zu%s",
                    records.empty() ? "" : ";", tag, output.size(),
                    chunks ? (":c" + std::to_string(chunks)).c_str() : "");
      records += summary;
      ++record_kinds["t" + std::to_string(tag)];
      at = end;
      while (at < blob_size && (at & 3) && b[at] == 0) {
        ++at;
      }
      ++record;
    }
    if (status == "ok" && at < blob_size) {
      bool zeros = blob_size - at < 16;
      for (size_t z = at; zeros && z < blob_size; ++z) {
        zeros = b[z] == 0;
      }
      if (!zeros) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "leftover %zu bytes at +%zX: %02X",
                      size_t(blob_size - at), at, b[at]);
        status = detail;
      }
    }
    if (!record && status == "ok") {
      status = "no records";
    }
    std::fprintf(index, "%u,%08X,%08X,%s,%08X,%X,%s,%s\n", i, kind, flags,
                 name.c_str(), blob, blob_size, records.c_str(),
                 status.c_str());
    if (status == "ok") {
      ++extracted;
    } else {
      ++failed;
      std::printf("#%u %s: %s (records %s)\n", i, name.c_str(), status.c_str(),
                  records.c_str());
    }
  }
  std::fclose(index);
  std::printf("version %u, %u entries: %zu extracted, %zu failed, %zu empty\n",
              version, count, extracted, failed, empty);
  for (const auto& kind : record_kinds) {
    std::printf("  record %s x%zu\n", kind.first.c_str(), kind.second);
  }
  return failed ? 2 : 0;
}
