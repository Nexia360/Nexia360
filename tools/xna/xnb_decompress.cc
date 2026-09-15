#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "third_party/mspack/lzx.h"
#include "third_party/mspack/mspack.h"
}

// The vendored mspack logs through this; the emulator supplies it normally.
extern "C" void xenia_log(const char*, ...) {}

namespace {

constexpr int kWindowBits = 16;
constexpr int kFrameSize = 32 * 1024;

struct Context {
  const uint8_t* input = nullptr;
  size_t input_size = 0;
  size_t input_at = 0;
  size_t chunk_remaining = 0;
  bool chunked = false;
  std::vector<uint8_t> output;
};

struct Chunk {
  size_t data = 0;
  uint32_t compressed = 0;
  uint32_t uncompressed = 0;
};

std::vector<Chunk> ParseChunks(const uint8_t* input, size_t size) {
  std::vector<Chunk> chunks;
  size_t at = 0;
  while (at + 2 <= size) {
    Chunk chunk;
    chunk.uncompressed = kFrameSize;
    if (input[at] == 0xFF) {
      if (at + 5 > size) {
        break;
      }
      chunk.uncompressed = (uint32_t(input[at + 1]) << 8) | input[at + 2];
      chunk.compressed = (uint32_t(input[at + 3]) << 8) | input[at + 4];
      at += 5;
    } else {
      chunk.compressed = (uint32_t(input[at]) << 8) | input[at + 1];
      at += 2;
    }
    if (!chunk.compressed || at + chunk.compressed > size) {
      break;
    }
    chunk.data = at;
    chunks.push_back(chunk);
    at += chunk.compressed;
  }
  return chunks;
}

int Read(mspack_file* file, void* buffer, int bytes) {
  auto* c = reinterpret_cast<Context*>(file);
  auto* out = static_cast<uint8_t*>(buffer);
  int written = 0;
  if (c->chunked) {
    const size_t take = std::min<size_t>(c->chunk_remaining, size_t(bytes));
    std::memcpy(out, c->input + c->input_at, take);
    c->input_at += take;
    c->chunk_remaining -= take;
    return int(take);
  }
  while (written < bytes) {
    if (!c->chunk_remaining) {
      if (c->input_at + 2 > c->input_size) {
        break;
      }
      uint32_t compressed;
      if (c->input[c->input_at] == 0xFF) {
        if (c->input_at + 5 > c->input_size) {
          break;
        }
        // 0xFF, big-endian uncompressed size, big-endian compressed size.
        compressed = (uint32_t(c->input[c->input_at + 3]) << 8) |
                     c->input[c->input_at + 4];
        c->input_at += 5;
      } else {
        compressed = (uint32_t(c->input[c->input_at]) << 8) |
                     c->input[c->input_at + 1];
        c->input_at += 2;
      }
      if (!compressed || c->input_at + compressed > c->input_size) {
        break;
      }
      c->chunk_remaining = compressed;
    }
    const size_t take =
        std::min<size_t>(c->chunk_remaining, size_t(bytes - written));
    std::memcpy(out + written, c->input + c->input_at, take);
    c->input_at += take;
    c->chunk_remaining -= take;
    written += int(take);
  }
  return written;
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: xnb_decompress <in.xnb> <out.bin> [chunked]\n");
    return 1;
  }
  const bool chunked = argc > 3 && std::strcmp(argv[3], "chunked") == 0;
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) {
    std::printf("cannot open %s\n", argv[1]);
    return 1;
  }
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> file(size);
  std::fread(file.data(), 1, size, f);
  std::fclose(f);

  if (size < 14 || std::memcmp(file.data(), "XNB", 3) != 0) {
    std::printf("not an XNB\n");
    return 1;
  }
  const uint8_t flags = file[5];
  // Header is 10 bytes, then a 4-byte decompressed size when compressed.
  const size_t payload = (flags & 0x80) ? 14 : 10;
  if (!(flags & 0x80)) {
    FILE* o = std::fopen(argv[2], "wb");
    std::fwrite(file.data() + payload, 1, size - payload, o);
    std::fclose(o);
    std::printf("stored, %zu bytes\n", size - payload);
    return 0;
  }

  Context c;
  c.input = file.data() + payload;
  c.input_size = size - payload;

  mspack_system sys = {};
  sys.read = Read;
  sys.write = Write;
  sys.alloc = Alloc;
  sys.free = Free;
  sys.copy = Copy;

  const uint32_t decompressed_size =
      uint32_t(file[10]) | (uint32_t(file[11]) << 8) |
      (uint32_t(file[12]) << 16) | (uint32_t(file[13]) << 24);
  lzxd_stream* lzx =
      lzxd_init(&sys, reinterpret_cast<mspack_file*>(&c),
                reinterpret_cast<mspack_file*>(&c), kWindowBits, 0, kFrameSize,
                off_t(decompressed_size), 0);
  if (!lzx) {
    std::printf("lzxd_init failed\n");
    return 1;
  }
  // The header's second size is what the whole file decompresses to; mspack
  // wants the real length, not an upper bound.
  if (!chunked) {
    const int status = lzxd_decompress(lzx, off_t(decompressed_size));
    if (status != MSPACK_ERR_OK) {
      std::printf("lzxd_decompress returned %d after %zu bytes\n", status,
                  c.output.size());
    }
  } else {
    const std::vector<Chunk> chunks = ParseChunks(c.input, c.input_size);
    c.chunked = true;
    uint64_t target = 0;
    uint64_t requested = 0;
    for (size_t k = 0; k < chunks.size(); ++k) {
      c.input_at = chunks[k].data;
      c.chunk_remaining = chunks[k].compressed;
      lzx->i_ptr = lzx->i_end = lzx->inbuf;
      lzx->bit_buffer = 0;
      lzx->bits_left = 0;
      lzx->input_end = 0;
      target += chunks[k].uncompressed;
      const uint64_t goal = k + 1 < chunks.size() ? target - 1 : target;
      const int status = lzxd_decompress(lzx, off_t(goal - requested));
      requested = goal;
      std::printf(
          "chunk %zu: %u -> %u, %d byte(s) and %u bit(s) left unused, status "
          "%d, output %zu\n",
          k, chunks[k].compressed, chunks[k].uncompressed,
          int(lzx->i_end - lzx->i_ptr) + int(c.chunk_remaining),
          lzx->bits_left, status, c.output.size());
      if (status != MSPACK_ERR_OK) {
        break;
      }
    }
  }
  lzxd_free(lzx);

  FILE* o = std::fopen(argv[2], "wb");
  std::fwrite(c.output.data(), 1, c.output.size(), o);
  std::fclose(o);
  std::printf("decompressed %zu bytes\n", c.output.size());
  return 0;
}
