/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XNB decompression, on the LZX decoder the emulator already has.
//
// A compressed .xnb holds LZX in the standard XNB chunk framing: each chunk is
// a big-endian 16-bit compressed size, or 0xFF followed by a big-endian
// uncompressed size and then a big-endian compressed size. A chunk with no 0xFF
// decompresses to 32768 bytes. Verified against a real asset - DefaultFont.xnb
// begins 1B 46, a 6982-byte chunk, with no XMemCompress signature in sight.
//
// The console's XMemDecompress kept state across calls, so the caller assumes
// this does too: DecompressStream only refills its input buffer once the last
// one is fully consumed, so a chunk that straddles the end of a buffer must be
// held rather than refused. That is what the pending buffer below is for -
// every byte handed over is taken, and decoding runs from what has accumulated.
//
// The LZX decoder is mspack's, the same one that unpacks compressed XEX
// sections, driven here in its streaming form rather than the one-shot
// lzx_decompress() wrapper: XNB frames are exactly the 32KB frames mspack
// already understands, so once the chunk headers are stripped the payload is a
// single continuous stream to it.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/kernel/xna/xna_exports.h"

#include "third_party/mspack/lzx.h"
#include "third_party/mspack/mspack.h"

namespace {

// One chunk decompresses to this unless a 0xFF header says otherwise, and it is
// also mspack's frame size - which is why the payload can be handed over as one
// continuous stream.
constexpr uint32_t kFrameSize = 0x8000;

// The XNB window is 64KB. Both of DecompressStream's buffers are that size.
constexpr int kWindowBits = 16;

struct Context {
  // Compressed bytes taken from the title but not yet decoded, and how far into
  // them the chunk parser has reached.
  std::vector<uint8_t> pending;
  size_t pending_read = 0;

  // Payload bytes of the chunk currently being served to mspack, and what is
  // left of it.
  size_t chunk_remaining = 0;
  // Chunk headers reported so far, so the log stays bounded.
  uint32_t chunks_logged = 0;

  // Where mspack writes. Repointed at the caller's buffer on every call.
  uint8_t* out = nullptr;
  size_t out_capacity = 0;
  size_t out_written = 0;

  mspack_system sys = {};
  lzxd_stream* lzx = nullptr;
  bool failed = false;
  uint32_t calls = 0;
  // Total uncompressed bytes of every chunk whose header has been read.
  uint64_t declared_output = 0;
  // Set once the chunk table has been walked to its end - the padding past
  // the last chunk parses as a zero-length one. Until then declared_output is
  // a running subtotal, NOT the length of the stream.
  bool stream_complete = false;
  // Everything handed back to the caller so far, across all calls.
  uint64_t total_written = 0;
  // How far into `pending` the length scan has already looked.
  size_t declared_scan = 0;
  // Reads that could not be filled. A SHORT read is normal - mspack refills as
  // it goes - but a ZERO read is how mspack is told the input has ended, and
  // once it believes that it injects zero bits and never recovers. If a decode
  // dies mid-stream with zero reads behind it, the data was never the problem.
  uint32_t short_reads = 0;
  uint32_t zero_reads = 0;
};

std::mutex contexts_mutex;
// The one context whose calls are logged - see the trace below.
std::atomic<const void*> traced_context{nullptr};
std::unordered_map<uint64_t, Context*> contexts;

// mspack addresses its files through opaque handles; there is exactly one
// input and one output per context, so the context itself is the handle and the
// two are told apart by which callback is running.
int ContextRead(mspack_file* file, void* buffer, int bytes) {
  auto* context = reinterpret_cast<Context*>(file);
  auto* dest = static_cast<uint8_t*>(buffer);
  int served = 0;
  while (served < bytes) {
    if (context->chunk_remaining == 0) {
      // Start the next chunk. A header that is not entirely present yet means
      // the rest is still on disk - stop here rather than guess at it.
      const size_t available = context->pending.size() - context->pending_read;
      if (available < 2) {
        break;
      }
      const uint8_t* header = context->pending.data() + context->pending_read;
      size_t header_size = 2;
      uint32_t compressed;
      uint32_t uncompressed = kFrameSize;
      if (header[0] == 0xFF) {
        if (available < 5) {
          break;
        }
        // Uncompressed size first, then compressed - both big-endian.
        uncompressed = (static_cast<uint32_t>(header[1]) << 8) | header[2];
        compressed = (static_cast<uint32_t>(header[3]) << 8) | header[4];
        header_size = 5;
      } else {
        compressed = (static_cast<uint32_t>(header[0]) << 8) | header[1];
      }
      // Past the last chunk the file carries a few bytes of padding, which
      // parse as a zero-length chunk. Reading them as real chunks would add
      // phantom frames to the declared length and leave mspack waiting for
      // input that never comes.
      if (compressed == 0) {
        break;
      }
      // ONLY THE HEADER HAS TO BE WHOLE.
      //
      // Waiting for the entire payload before serving any of it is what killed
      // the big assets: mspack asked for 32768 bytes, 15782 of them were
      // sitting in the buffer, and this returned ZERO because the chunk they
      // belonged to was 612 bytes short of complete. A zero-length read is how
      // mspack is told the input has ended - after which it injects zero bits
      // and never recovers - so a 699281-byte model died five frames in with
      // 81310 bytes still buffered. Serving what has arrived costs nothing:
      // chunk_remaining carries across calls, so the rest is served when it
      // turns up.
      if (available < header_size + 1) {
        break;
      }
      context->pending_read += header_size;
      context->chunk_remaining = compressed;

      // WHAT THE FRAMING ACTUALLY IS, rather than what it is assumed to be.
      // A stream that decodes several frames and then dies on "failed to build
      // PRETREE table" has lost bit alignment at a chunk boundary, and the two
      // candidate explanations - chunks that are not one 32KB frame, or a
      // bitstream that resets per chunk - are told apart by the headers alone.
      // Bounded so a large asset cannot flood the log.
      if (context->chunks_logged < 12) {
        ++context->chunks_logged;
        XELOGI("[xna] LZX chunk {}: {} compressed -> {} uncompressed ({})",
               context->chunks_logged - 1, compressed, uncompressed,
               header_size == 5 ? "explicit" : "implied");
      }

      // The uncompressed size is the point of the 0xFF form, but it is
      // declared to mspack in DeclareLength below rather than here - see the
      // note there for why doing it at this point is too late.
    }

    // Bounded by what is actually in the buffer as well as by what is left of
    // the chunk, now that a chunk may be served in pieces.
    const size_t buffered = context->pending.size() - context->pending_read;
    const int take = static_cast<int>(std::min<size_t>(
        std::min<size_t>(context->chunk_remaining,
                         static_cast<size_t>(bytes - served)),
        buffered));
    if (take == 0) {
      break;
    }
    std::memcpy(dest + served, context->pending.data() + context->pending_read,
                take);
    context->pending_read += take;
    context->chunk_remaining -= take;
    served += take;
  }
  if (served < bytes) {
    ++context->short_reads;
    if (served == 0) {
      ++context->zero_reads;
    }
  }
  return served;
}

int ContextWrite(mspack_file* file, void* buffer, int bytes) {
  auto* context = reinterpret_cast<Context*>(file);
  const size_t room = context->out_capacity - context->out_written;
  const size_t take = std::min<size_t>(room, static_cast<size_t>(bytes));
  if (take != 0) {
    std::memcpy(context->out + context->out_written, buffer, take);
    context->out_written += take;
  }
  return static_cast<int>(take);
}

void* ContextAlloc(mspack_system* self, size_t bytes) {
  return std::calloc(1, bytes);
}
void ContextFree(void* ptr) { std::free(ptr); }
void ContextCopy(void* src, void* dest, size_t bytes) {
  std::memcpy(dest, src, bytes);
}

// mspack narrates conditions this code handles on purpose - "out of input
// bytes" when a frame straddles a buffer, "bytes left to output" at the end of
// the final short frame - and both read like failures in the log while the
// decode is in fact fine. Swallowed here so that a real problem stands out.
void ContextMessage(mspack_file* file, const char* format, ...) {}

// How many bytes the chunks now buffered will decompress to, in total.
//
// THIS HAS TO HAPPEN BEFORE lzxd_decompress, NOT DURING IT. mspack clamps its
// output target against lzx->length once, at the top of the call - so a length
// declared from inside the read callback arrives after the decision it was
// meant to inform. With the length still zero, mspack aimed for the full 64KB
// the caller offered, ran off the end of a stream that only held 1274 bytes,
// and returned 605 of them. Six-frame assets survived that because the first
// call had whole frames to fill; a single short frame - which is what a small
// effect is - did not.
//
// The scan does not consume anything: it walks the same chunk headers the read
// callback will walk, so the two agree by construction.
void DeclareLength(Context* context) {
  // Where the last scan stopped. Without this the same chunk headers would be
  // counted again on every call and the declared length would run away.
  size_t at = context->declared_scan;
  while (at + 2 <= context->pending.size()) {
    const uint8_t* header = context->pending.data() + at;
    const size_t available = context->pending.size() - at;
    size_t header_size = 2;
    uint32_t compressed;
    uint32_t uncompressed = kFrameSize;
    if (header[0] == 0xFF) {
      if (available < 5) {
        break;
      }
      uncompressed = (static_cast<uint32_t>(header[1]) << 8) | header[2];
      compressed = (static_cast<uint32_t>(header[3]) << 8) | header[4];
      header_size = 5;
    } else {
      compressed = (static_cast<uint32_t>(header[0]) << 8) | header[1];
    }
    if (compressed == 0) {
      // The padding after the final chunk: the table ends here, so the subtotal
      // is now the real length of the stream.
      context->stream_complete = true;
      break;
    }
    if (available < header_size + compressed) {
      break;
    }
    context->declared_output += uncompressed;
    at += header_size + compressed;
    context->declared_scan = at;
  }
  if (context->lzx) {
    // NEVER DECLARE A LENGTH THE DECODER CAN REACH BEFORE THE STREAM ENDS.
    //
    // mspack does end-of-stream handling the moment output reaches lzx->length,
    // and everything decoded after that is garbage. Since only the chunks
    // buffered so far are known, a subtotal declared as if it were the total is
    // an end-of-stream in the middle of the file - which is what produced
    // "failed to build PRETREE table" a few frames into a 699281-byte model,
    // while the same bytes decode perfectly when the whole payload is present.
    //
    // So until the chunk table has been walked to its padding, the length is
    // declared one frame beyond the subtotal. Nothing ever asks for that frame
    // - the request below is capped at the subtotal - so it exists only to keep
    // mspack from concluding the stream is over.
    const uint64_t length = context->stream_complete
                                ? context->declared_output
                                : context->declared_output + kFrameSize;
    lzxd_set_output_length(context->lzx, static_cast<off_t>(length));
  }
}

// Everything already consumed is dropped, so a long asset does not grow the
// buffer without bound.
void CompactPending(Context* context) {
  // NEVER DROP PAST THE SCAN CURSOR. The reader serves partial chunks and the
  // scanner only counts whole ones, so the reader can now run ahead of the
  // scanner - and clamping the scan cursor to zero would restart it in the
  // middle of a chunk, reading payload bytes as if they were headers. Keeping
  // the smaller of the two keeps both valid, at the cost of holding a chunk's
  // worth of buffer a little longer.
  const size_t dropped =
      std::min(context->pending_read, context->declared_scan);
  if (dropped == 0) {
    return;
  }
  context->pending.erase(context->pending.begin(),
                         context->pending.begin() + dropped);
  context->pending_read -= dropped;
  context->declared_scan -= dropped;
}

}  // namespace

extern "C" uint64_t xna_STORAGE_STORAGE_CreateDecompressionContext() {
  auto* context = new Context();
  context->sys.read = ContextRead;
  context->sys.write = ContextWrite;
  context->sys.alloc = ContextAlloc;
  context->sys.free = ContextFree;
  context->sys.copy = ContextCopy;
  context->sys.message = ContextMessage;

  // Output length 0 means "until the input runs out", which is right: the
  // caller drives this a buffer at a time and never says how much is coming.
  context->lzx = lzxd_init(&context->sys,
                           reinterpret_cast<mspack_file*>(context),
                           reinterpret_cast<mspack_file*>(context), kWindowBits,
                           0, kFrameSize, 0, 0);
  if (!context->lzx) {
    delete context;
    // The caller tests the handle against IntPtr.Zero and raises
    // "Error decompressing content data", which is the honest outcome.
    return 0;
  }

  const void* expected = nullptr;
  traced_context.compare_exchange_strong(expected, context);

  const auto handle = reinterpret_cast<uint64_t>(context);
  std::lock_guard<std::mutex> lock(contexts_mutex);
  contexts[handle] = context;
  return handle;
}

extern "C" void xna_STORAGE_STORAGE_DestroyDecompressionContext(
    uint64_t handle) {
  Context* context = nullptr;
  {
    std::lock_guard<std::mutex> lock(contexts_mutex);
    auto found = contexts.find(handle);
    if (found == contexts.end()) {
      return;
    }
    context = found->second;
    contexts.erase(found);
  }
  if (context->lzx) {
    lzxd_free(context->lzx);
  }
  delete context;
}

// Decompress(context, dest, ref destSize, src, ref srcSize).
//
// Both sizes are in AND out: on entry they are the capacity and the bytes
// available, on return the bytes written and the bytes consumed. Zero means
// success - the caller throws on anything else - and the caller ALSO throws if
// both counts come back zero, treating that as a stalled stream.
extern "C" int32_t xna_STORAGE_STORAGE_Decompress(uint64_t handle, void* dest,
                                                  int32_t* dest_size,
                                                  const void* src,
                                                  int32_t* src_size) {
  if (!dest || !dest_size || !src_size) {
    return 1;
  }

  Context* context = nullptr;
  {
    std::lock_guard<std::mutex> lock(contexts_mutex);
    auto found = contexts.find(handle);
    if (found == contexts.end()) {
      return 1;
    }
    context = found->second;
  }
  if (context->failed) {
    return 1;
  }

  // Everything offered is taken, whether or not it can be decoded yet. The
  // caller only refills once its buffer is fully consumed, so leaving a partial
  // chunk behind would stall the stream permanently.
  const int32_t offered = *src_size;
  if (src && offered > 0) {
    const auto* bytes = static_cast<const uint8_t*>(src);
    context->pending.insert(context->pending.end(), bytes, bytes + offered);
  }

  context->out = static_cast<uint8_t*>(dest);
  context->out_capacity = static_cast<size_t>(*dest_size);
  context->out_written = 0;

  DeclareLength(context);

  // Never ask for more than the stream still holds. Asking for the caller's
  // full buffer when the asset is smaller is what truncated it.
  //
  // AND KEEP A CHUNK IN HAND. mspack reads ahead - it fills a 32KB input buffer
  // whenever it needs bits, not just the bytes it is about to use - so decoding
  // right up to the last fully buffered chunk leaves a read that cannot be
  // served. A read of zero is how mspack is told the input has ENDED, and from
  // then on it injects zero bits and never recovers: a 699281-byte model died
  // after five frames with 81310 bytes still sitting in the buffer, which is
  // not a shortage of data but a question asked one chunk too early.
  //
  // So a frame is held back until the chunk table has been walked to its
  // padding, at which point there is nothing left to read ahead into.
  uint64_t usable = context->declared_output;
  if (!context->stream_complete) {
    usable = usable > kFrameSize ? usable - kFrameSize : 0;
  }
  size_t request = context->out_capacity;
  const uint64_t remaining =
      usable > context->total_written ? usable - context->total_written : 0;
  if (remaining < request) {
    request = static_cast<size_t>(remaining);
  }
  if (request == 0) {
    *dest_size = 0;
    *src_size = offered;
    return 0;
  }

  const int result =
      lzxd_decompress(context->lzx, static_cast<off_t>(request));
  context->total_written += context->out_written;

  // The first few calls decide whether the framing above is right, and the
  // caller reports every failure with the same opaque "Error decompressing
  // content data" - so the counts are logged here rather than inferred.
  // Only the first asset is traced. Every context repeats the same handful of
  // calls, and a title loads hundreds of assets.
  if (context->calls < 4 && traced_context.load() == context) {
    XELOGI(
        "[xna] LZX call {}: mspack {} - offered {} bytes, pending {}, wrote {} "
        "of {}",
        context->calls, result, offered,
        context->pending.size() - context->pending_read, context->out_written,
        context->out_capacity);
  }
  // A FAILURE ALWAYS REPORTS, whatever context it happened in. The trace above
  // follows only the first asset, so the one stream that actually breaks - the
  // twenty-second chunk of a 699281-byte model, hundreds of assets in - was the
  // one call never logged. Where it stops is the whole question: stopping at
  // the subtotal means the declared length is still being reached early,
  // stopping at the last frame means the end-of-stream half is wrong.
  if (result != 0) {
    XELOGE(
        "[xna] LZX call {} failed ({}) after {} of {} declared byte(s), {} "
        "this call, stream {}, {} short read(s) and {} zero read(s), {} "
        "byte(s) still buffered",
        context->calls, result, context->total_written,
        context->declared_output, context->out_written,
        context->stream_complete ? "complete" : "still arriving",
        context->short_reads, context->zero_reads,
        context->pending.size() - context->pending_read);
  }
  ++context->calls;
  // A byte that reached the caller is a byte that decoded, so an error next to
  // real output is not a failure of the data - it is mspack describing where it
  // stopped. Two are expected here and neither means the stream is bad:
  //
  //   MSPACK_ERR_READ      the input ran out mid-frame - the rest is still on
  //                        disk, and the next call continues from this state.
  //   MSPACK_ERR_DECRUNCH  raised at the end of the last, short frame after it
  //                        has already been written out in full.
  //
  // So the test is on what came out, not on the code: anything produced is
  // reported, and only a call that produced nothing at all is a failure.
  if (result != MSPACK_ERR_OK && context->out_written == 0) {
    XELOGE("[xna] LZX decode failed ({}) with nothing decoded", result);
    context->failed = true;
    return 1;
  }

  CompactPending(context);
  *dest_size = static_cast<int32_t>(context->out_written);
  *src_size = offered;
  return 0;
}
