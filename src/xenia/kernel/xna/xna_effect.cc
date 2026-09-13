/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 ******************************************************************************
 */

// Loading an effect the way an image is loaded, and pulling the shaders out of
// it.
//
// An XNA effect built for the console is a container of compiled Xenos shader
// microcode - the same instruction set this emulator already translates. That
// is the whole reason to run one of these titles here rather than port it: a
// desktop port has to throw the microcode away and hand-author replacement HLSL
// for every effect, because no PC can execute Xenos. Nexia can.
//
// So an effect is treated as an image: its magic is checked, it is byte-swapped
// into host order if it was authored for the console CPU, the shaders inside it
// are located, and each is handed to xe::gpu::Shader - the same class the GPU
// builds from microcode found in guest memory. From there it is an ordinary
// Nexia shader.
//
// FINDING THE SHADERS. The container layout is undocumented, so nothing here
// assumes one. What IS documented, in this tree, is the microcode itself: a
// Xenos shader begins with a control-flow program of paired instructions packed
// three dwords at a time, every opcode is one of a known set, and the program
// must terminate. That is a strong enough shape to search for. Each aligned
// offset is decoded as a control-flow program and accepted only if it parses
// cleanly to an end - so a shader is identified by being a valid shader, not by
// sitting where a guessed header said it would.
//
// WHY THE BYTE ORDER IS THE FIRST PROBLEM. The container was written for a
// big-endian PowerPC, and Effect..ctor reads its magic with a native-order
// 32-bit load. That code now runs little-endian on x64, so the first word it
// reads is byte-reversed and it refuses the effect before any of this runs. The
// XEX loader has the same problem and solves it the same way: swap on load,
// then everything above reads normally.

#include "xenia/kernel/xna/xna_effect.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/ucode.h"
#include "xenia/kernel/xna/xna_exports.h"

// Xenia's dump_shaders hangs off AnalyzeUcode and has not been producing files
// for this path, so the shaders an effect actually yields are written here
// instead - from the same bytes that were handed to the translator, next to the
// disassembly it produced from them. This is the only way to see whether a
// shader we located is a real program or a piece of the constant table.
DEFINE_path(xna_dump_shaders, "",
            "Directory to write each shader an XNA effect yields into, as "
            "microcode and as disassembly.",
            "XNA");

namespace xe {
namespace kernel {
namespace xna {

namespace {

// The first word of a compiled console effect. Effect..ctor compares against
// this exact value with a native-order load, so a container still in console
// byte order reads as the reversed constant instead.
constexpr uint32_t kEffectMagic = 0xBCF00BCF;
constexpr uint32_t kEffectMagicSwapped = 0xCF0BF0BC;

// A control-flow program shorter than this is not a shader - it is three dwords
// of something else that happened to decode. Two instruction pairs is the
// smallest thing that can set up and end a real program.
constexpr uint32_t kMinControlFlowDwords = 3;

// A real control-flow program is short - a few dozen instructions at most.
// The candidates that crashed the analyzer claimed over a thousand dwords,
// which is string data that happened to decode, not a shader.
constexpr uint32_t kMaxControlFlowDwords = 512;

uint32_t ReadWord(const uint8_t* data, size_t offset) {
  uint32_t value;
  std::memcpy(&value, data + offset, sizeof(value));
  return value;
}

bool IsKnownControlFlowOpcode(gpu::ucode::ControlFlowOpcode opcode) {
  using gpu::ucode::ControlFlowOpcode;
  switch (opcode) {
    case ControlFlowOpcode::kNop:
    case ControlFlowOpcode::kExec:
    case ControlFlowOpcode::kExecEnd:
    case ControlFlowOpcode::kCondExec:
    case ControlFlowOpcode::kCondExecEnd:
    case ControlFlowOpcode::kCondExecPred:
    case ControlFlowOpcode::kCondExecPredEnd:
    case ControlFlowOpcode::kLoopStart:
    case ControlFlowOpcode::kLoopEnd:
    case ControlFlowOpcode::kCondCall:
    case ControlFlowOpcode::kReturn:
    case ControlFlowOpcode::kCondJmp:
    case ControlFlowOpcode::kAlloc:
    case ControlFlowOpcode::kCondExecPredClean:
    case ControlFlowOpcode::kCondExecPredCleanEnd:
    case ControlFlowOpcode::kMarkVsFetchDone:
      return true;
    default:
      return false;
  }
}

// Decodes a control-flow program at `dwords` and reports how many dwords it
// occupies, or 0 if this is not one.
//
// The test is deliberately strict. A loose one finds "shaders" everywhere in a
// 200KB container, and every false positive costs a translation and a wrong
// binding later, where it is far harder to recognise than here.
uint32_t MeasureControlFlow(const uint32_t* dwords, uint32_t available) {
  using gpu::ucode::ControlFlowInstruction;
  using gpu::ucode::ControlFlowOpcode;

  if (available < 3) {
    return 0;
  }

  bool saw_exec = false;
  bool executes_instructions = false;
  uint32_t first_exec_address = UINT32_MAX;
  // The furthest dword any exec instruction reaches, so the shader can be
  // handed exactly what it addresses instead of the rest of the container.
  uint32_t highest_dword = 0;
  const uint32_t limit = std::min(available, kMaxControlFlowDwords);
  for (uint32_t offset = 0; offset + 3 <= limit; offset += 3) {
    ControlFlowInstruction pair[2];
    gpu::ucode::UnpackControlFlowInstructions(dwords + offset, pair);

    for (int index = 0; index < 2; ++index) {
      const ControlFlowOpcode opcode = pair[index].opcode();
      if (!IsKnownControlFlowOpcode(opcode)) {
        return 0;
      }
      if (gpu::ucode::IsControlFlowOpcodeExec(opcode)) {
        // EVERY ADDRESS THIS PROGRAM REACHES MUST BE INSIDE THE BLOB.
        //
        // Without this the scan accepts control flow whose exec instructions
        // point anywhere, and AnalyzeUcode then follows them straight off the
        // end of the container - which is exactly what crashed inside
        // InteropCreateEffect. A real shader only ever addresses its own body,
        // so a program that does not is not one, and the fact that its opcodes
        // decoded is a coincidence.
        //
        // Instructions are three dwords each, addressed in instruction slots.
        // kExec* and kCondExec* carry the same two fields in different
        // layouts, so the right member has to be read for each.
        const bool plain = opcode == ControlFlowOpcode::kExec ||
                           opcode == ControlFlowOpcode::kExecEnd;
        const uint32_t address = plain ? pair[index].exec.address()
                                       : pair[index].cond_exec.address();
        const uint32_t count =
            plain ? pair[index].exec.count() : pair[index].cond_exec.count();
        const uint64_t end_dword =
            (static_cast<uint64_t>(address) + count) * 3;
        if (end_dword > available) {
          return 0;
        }
        highest_dword = std::max<uint32_t>(highest_dword,
                                           static_cast<uint32_t>(end_dword));
        saw_exec = true;
        executes_instructions |= count != 0;
        if (first_exec_address == UINT32_MAX) {
          first_exec_address = address;
        }
      }
      // A jump or a call names an instruction slot too, and one pointing
      // outside the blob is the same coincidence an exec would be - it just
      // was not being checked, so a program could pass this test and still
      // branch off the end of the container.
      if (opcode == ControlFlowOpcode::kCondJmp ||
          opcode == ControlFlowOpcode::kCondCall) {
        const uint64_t target =
            static_cast<uint64_t>(opcode == ControlFlowOpcode::kCondJmp
                                      ? pair[index].cond_jmp.address()
                                      : pair[index].cond_call.address()) *
            3;
        if (target >= available) {
          return 0;
        }
        highest_dword =
            std::max<uint32_t>(highest_dword, static_cast<uint32_t>(target) + 3);
      }
      if (gpu::ucode::DoesControlFlowOpcodeEndShader(opcode)) {
        const uint32_t used = offset + 3;
        // A program that ends immediately, or never executes anything, is
        // three dwords of coincidence rather than a shader.
        if (!saw_exec || used < kMinControlFlowDwords ||
            !executes_instructions || highest_dword <= used ||
            first_exec_address * 3 != used) {
          return 0;
        }
        // The shader spans its control flow AND everything that flow
        // executes. Control flow comes in groups of three dwords, so an extent
        // that is not a multiple of three did not come from one.
        const uint32_t extent = std::max(used, highest_dword);
        if (extent % 3 != 0 || extent > kMaxControlFlowDwords) {
          return 0;
        }
        return extent;
      }
    }
  }
  // Ran to the end of the blob without terminating - not a shader.
  return 0;
}

// Whether AnalyzeUcode can walk this program without reading outside it.
//
// MeasureControlFlow is not enough on its own, because it and AnalyzeUcode walk
// the program differently. MeasureControlFlow stops at the first opcode that
// ends the shader; AnalyzeUcode instead bounds itself by the address of the
// FIRST exec instruction and walks every pair up to it. When the end opcode
// comes first - which it does - AnalyzeUcode walks pairs that were never
// validated, and one bad exec address in them reads off the end of the shader.
//
// This mirrors AnalyzeUcode's traversal exactly, so anything it accepts can be
// analyzed safely.
bool IsAnalyzableUcode(const uint32_t* dwords, uint32_t dword_count) {
  using gpu::ucode::ControlFlowInstruction;
  using gpu::ucode::ControlFlowOpcode;

  if (dword_count < 3) {
    return false;
  }
  // The same bound AnalyzeUcode computes: the lowest exec address seen while
  // scanning the whole program, capped by its length in pairs.
  uint32_t pair_bound = dword_count / 3;
  for (uint32_t i = 0; i < pair_bound; ++i) {
    ControlFlowInstruction pair[2];
    gpu::ucode::UnpackControlFlowInstructions(dwords + i * 3, pair);
    for (uint32_t j = 0; j < 2; ++j) {
      if (gpu::ucode::IsControlFlowOpcodeExec(pair[j].opcode())) {
        pair_bound = std::min(pair_bound, pair[j].exec.address());
      }
    }
  }
  // Every pair it will actually walk has to decode, and every instruction range
  // an exec names has to lie inside the program.
  for (uint32_t i = 0; i < pair_bound; ++i) {
    ControlFlowInstruction pair[2];
    gpu::ucode::UnpackControlFlowInstructions(dwords + i * 3, pair);
    for (uint32_t j = 0; j < 2; ++j) {
      const ControlFlowOpcode opcode = pair[j].opcode();
      if (!IsKnownControlFlowOpcode(opcode)) {
        return false;
      }
      if (!gpu::ucode::IsControlFlowOpcodeExec(opcode)) {
        continue;
      }
      const bool plain = opcode == ControlFlowOpcode::kExec ||
                         opcode == ControlFlowOpcode::kExecEnd;
      const uint64_t end =
          (static_cast<uint64_t>(plain ? pair[j].exec.address()
                                       : pair[j].cond_exec.address()) +
           (plain ? pair[j].exec.count() : pair[j].cond_exec.count())) *
          3;
      if (end > dword_count) {
        return false;
      }
    }
  }
  return true;
}

// Effect..ctor prepares the container at its top and then calls
// D3D_Effect_CreateHandle further down, on the same thread, with a DIFFERENT
// buffer - the inner D3DX effect the container wraps, which carries the
// fx_2_0 signature 0xFEFF0901 rather than the console magic. Matching by
// content therefore never connects the two. What does connect them is that
// they are two points in one constructor call, so the image prepared a moment
// ago on this thread is the one being created now.
thread_local uint32_t prepared_on_this_thread = 0;

std::mutex effects_mutex;
std::unordered_map<uint32_t, std::unique_ptr<EffectImage>> effects;
// Content hash of a prepared blob to the handle its image was given. The
// managed hook sees the bytes first, at Effect..ctor; CreateHandle sees the
// same bytes moments later, and this is what connects the two without parsing
// a 200KB container twice.
std::unordered_map<uint64_t, uint32_t> effects_by_content;

uint64_t ContentHash(const uint8_t* data, uint32_t size) {
  // The size plus the head of the blob. Two distinct effects that agree on both
  // would have to be the same shader program.
  size_t hash = xe::memory::hash_combine(0, size);
  const uint32_t words = std::min<uint32_t>(size / 4, 16);
  for (uint32_t i = 0; i < words; ++i) {
    hash = xe::memory::hash_combine(hash, ReadWord(data, i * 4));
  }
  return static_cast<uint64_t>(hash);
}

}  // namespace

// A shader found inside an effect. Subclassed only so the base class can be
// instantiated - all the behaviour that matters is xe::gpu::Shader's, which is
// the point: once the microcode is out of the container it is the same object
// the GPU builds for a guest draw.
class EffectShader final : public gpu::Shader {
 public:
  EffectShader(gpu::xenos::ShaderType type, uint64_t hash, const uint32_t* dwords,
               size_t count)
      // The container has already been swapped into host order by the time the
      // shaders are located in it, so the microcode is native here - saying
      // otherwise would swap it a second time.
      : gpu::Shader(type, hash, dwords, count, std::endian::native) {}
};

bool PrepareEffectImage(uint8_t* data, uint32_t size, EffectImage* image) {
  if (!data || size < 8 || !image) {
    XELOGE("[xna] effect image is {} bytes, too small to hold a header", size);
    return false;
  }

  const uint32_t first = ReadWord(data, 0);
  if (first == kEffectMagic) {
    // ALREADY SWAPPED - BY US, ON A PREVIOUS LOAD OF THE SAME ARRAY.
    //
    // The swap below is in place on the caller's byte[], and a title reuses one
    // static array for every construction of the same effect: BasicEffectCode
    // .Code is shared by every BasicEffect, and SpriteBatch reloads its effect
    // per batch. So the second construction finds the magic already correct.
    //
    // That does NOT mean the strings are in host order. A console container's
    // characters are reversed within each word by the swap, and they stay that
    // way - so names still have to be read back through it. Treating this as
    // "not swapped" is what produced "txeT" and "irpSaBet" on every load after
    // the first, and with them a parameter table nothing could look up by name.
    image->was_byte_swapped = true;
    image->was_already_swapped = true;
  } else if (first == kEffectMagicSwapped) {
    // Authored for the console CPU. Swapping every 32-bit word puts the whole
    // container into host order in one pass, which is what everything above -
    // managed and native alike - assumes from here on. A trailing partial word
    // would mean this is not the shape it claims to be, so the size is checked
    // rather than rounded.
    if (size % 4 != 0) {
      XELOGE(
          "[xna] effect image is {} bytes, not a whole number of words - "
          "refusing to byte-swap a container this cannot be the shape of",
          size);
      return false;
    }
    for (uint32_t offset = 0; offset < size; offset += 4) {
      uint32_t word = xe::byte_swap(ReadWord(data, offset));
      std::memcpy(data + offset, &word, sizeof(word));
    }
    image->was_byte_swapped = true;
  } else {
    XELOGE(
        "[xna] effect image starts {:08X}, which is neither the effect magic "
        "{:08X} nor its console-order form {:08X} - not a compiled effect, or "
        "it arrived truncated",
        first, kEffectMagic, kEffectMagicSwapped);
    return false;
  }

  image->size = size;
  image->valid = true;
  XELOGI("[xna] effect image: {} bytes{}", size,
         image->was_already_swapped ? ", already swapped by an earlier load"
                                    : ", byte-swapped from console order");
  return true;
}

void FindEffectShaders(const uint8_t* data, uint32_t size, EffectImage* image) {
  const uint32_t total_dwords = size / 4;
  if (total_dwords < 3) {
    return;
  }
  // The container is word-aligned after the swap above, so the microcode can be
  // read straight out of it rather than copied to align it.
  const auto* dwords = reinterpret_cast<const uint32_t*>(data);

  uint32_t at = 1;  // dword 0 is the magic
  while (at + 3 <= total_dwords) {
    const uint32_t used = MeasureControlFlow(dwords + at, total_dwords - at);
    if (used == 0) {
      ++at;
      continue;
    }

    // The control-flow program is the head of the shader; the ALU and fetch
    // instructions it executes follow it, and MeasureControlFlow has already
    // bounded both.
    EffectShaderInfo info;
    info.dword_offset = at;
    info.dword_count = used;
    info.found = true;

    // NO ANALYSIS HERE, DELIBERATELY. AnalyzeUcode is written for microcode the
    // GPU has already been told to trust - it follows jumps and reads operands
    // without treating the buffer as hostile. A scan over a container feeds it
    // whatever happens to decode, and that is what faulted inside
    // InteropCreateEffect. Candidates are recorded and nothing more; a shader
    // is analyzed once a pass names it, where it is a real shader rather than a
    // guess. Which of vertex or pixel it is comes from that pass too, instead
    // of from trying both and seeing which survives.
    XELOGD("[xna]    shader candidate at dword {}, {} dword(s)", at, used);
    image->shaders.push_back(std::move(info));
    at += used;
  }

  XELOGI("[xna] effect image: {} bytes{}, {} shader candidate(s)", size,
         image->was_already_swapped ? ", already swapped by an earlier load"
                                    : ", byte-swapped from console order",
         image->shaders.size());
}


// ---- the D3DX effect inside the container ------------------------------------
//
// Effect..ctor reads the console container as two words - the magic, then an
// offset - and hands what sits at that offset to CreateEffect, having first
// checked that it starts with 0xFEFF0901. That inner blob is a D3DX fx_2_0
// effect, and its header is what EFFECT_DESC has to be filled from: the counts
// there drive EffectTechniqueCollection and EffectParameterCollection, and the
// constructor immediately does `CurrentTechnique = techniques[0]`. Reporting
// zero techniques is what made that index null.
//
// The counts sit at the offset the second header word points to. That is stated
// here rather than assumed: the alternative reading - counts immediately after
// the two header words - is tried as well, and whichever produces a plausible
// header wins. Both are logged, so a wrong guess names itself in one run
// instead of turning into a silently wrong effect.

namespace {

constexpr uint32_t kD3dxEffectTag = 0xFEFF0901;

// A header is believed only if every count could belong to a real effect.
// Reading the wrong offset produces enormous or absurd values, which is exactly
// what this rejects.
bool PlausibleD3dxHeader(uint32_t parameters, uint32_t techniques,
                         uint32_t objects, uint32_t words_available) {
  if (techniques == 0 || techniques > 256) {
    return false;
  }
  if (parameters > 4096 || objects > 65536) {
    return false;
  }
  // An effect with techniques has objects to draw with - at minimum the shaders
  // its passes bind. A header reporting techniques but no objects is not this
  // effect's header.
  if (objects == 0) {
    return false;
  }
  // Every parameter and technique needs several words of record after the
  // header, so a header claiming more than the blob can hold is not one.
  return (parameters + techniques) < words_available;
}

}  // namespace

bool ParseD3dxEffect(const uint8_t* data, uint32_t size, EffectImage* image) {
  if (!data || size < 24 || !image) {
    return false;
  }
  const uint32_t tag = ReadWord(data, 0);
  if (tag != kD3dxEffectTag) {
    XELOGW("[xna] effect body starts {:08X}, not the D3DX tag {:08X}", tag,
           kD3dxEffectTag);
    return false;
  }

  const uint32_t offset = ReadWord(data, 4);
  const uint32_t words = size / 4;

  // The offset is relative to the position AFTER the two header words, not to
  // the start of the blob. Reading it as absolute landed in the string table -
  // "tara" as a parameter count - and reading the counts at a fixed +8 landed
  // on something that is the same in every effect, which is how that mistake
  // announced itself: a 1212-byte effect and a 29796-byte one both reported
  // 7 techniques and 28 objects. Counts that do not vary with the effect are
  // not counts.
  //
  // The fixed readings are kept behind it only as a last resort, and the
  // "same in every effect" trap is now caught by the caller instead.
  const uint32_t candidates[] = {8 + offset, offset, 8};
  for (const uint32_t at : candidates) {
    if (at + 16 > size) {
      continue;
    }
    const uint32_t parameters = ReadWord(data, at);
    const uint32_t techniques = ReadWord(data, at + 4);
    const uint32_t objects = ReadWord(data, at + 12);
    if (!PlausibleD3dxHeader(parameters, techniques, objects, words)) {
      XELOGD(
          "[xna]    d3dx header at {} reads {} parameter(s), {} technique(s), "
          "{} object(s) - rejected as implausible",
          at, parameters, techniques, objects);
      continue;
    }
    image->parameter_count = parameters;
    image->technique_count = techniques;
    image->object_count = objects;
    image->header_offset = at;
    XELOGI(
        "[xna] effect body: {} parameter(s), {} technique(s), {} object(s) "
        "(header at byte {}, offset field {}, body {} bytes)",
        parameters, techniques, objects, at, offset, size);
    return true;
  }

  XELOGE(
      "[xna] could not read the D3DX effect header - first words are "
      "{:08X} {:08X} {:08X} {:08X}, offset field {}",
      ReadWord(data, 0), ReadWord(data, 4), ReadWord(data, 8),
      ReadWord(data, 12), offset);
  return false;
}


// ---- the technique, pass and object tables -----------------------------------
//
// This is what replaces guessing. The scan in FindEffectShaders can only say
// "these bytes decode as a control-flow program"; the tables say which shader a
// pass actually uses, and whether it is the vertex or the pixel one. Once a
// pass names a shader, that shader is real and can be analyzed - which is why
// nothing is analyzed before this point.
//
// Record shapes, all little-endian dwords after the container was swapped:
//
//   header     parameters, techniques, (unused), objects
//   technique  name offset, data offset, annotations, passes, (unused)
//              then its annotations, then its passes
//   pass       name offset, data offset, annotations, states
//              then its annotations, then its states
//   state      operation, index, type, parameter offset
//
// The type field is a D3DXPARAMETER_TYPE, and two of its values are the whole
// point of this walk: 15 is a pixel shader and 16 is a vertex shader. Those are
// fixed by the D3DX parameter-type enumeration rather than by anything
// title-specific, so a state carrying one is a shader binding whatever the
// effect is.
//
// EVERY READ IS BOUNDS-CHECKED AND EVERY COUNT IS RANGE-CHECKED. A layout that
// is wrong by one dword produces enormous counts almost immediately, so the
// walk stops at the first thing that cannot be true and reports where. It never
// half-succeeds: either the tables are believed whole, or the effect keeps the
// candidates the scan found and says so.

namespace {

// D3DXPARAMETER_TYPE, the two values that matter here.
constexpr uint32_t kD3dxTypePixelShader = 15;
constexpr uint32_t kD3dxTypeVertexShader = 16;

// Bounds beyond which a count cannot have come from a real effect. These are
// generous - the point is to catch a misread offset, which yields values in the
// millions, not to police unusual effects.
constexpr uint32_t kMaxTechniques = 256;
constexpr uint32_t kMaxPasses = 256;
constexpr uint32_t kMaxAnnotations = 256;
constexpr uint32_t kMaxShaderTableEntries = 256;
constexpr uint32_t kMaxShaderRecordBytes = 0x5FF4;
constexpr uint32_t kMaxStates = 1024;

// A cursor over the effect body that refuses to read past the end.
class Reader {
 public:
  Reader(const uint8_t* data, uint32_t size, uint32_t at)
      : data_(data), size_(size), at_(at) {}

  bool Read(uint32_t* out) {
    if (at_ + 4 > size_) {
      failed_ = true;
      return false;
    }
    std::memcpy(out, data_ + at_, sizeof(*out));
    at_ += 4;
    return true;
  }

  bool Skip(uint32_t dwords) {
    const uint64_t next = static_cast<uint64_t>(at_) + dwords * 4ull;
    if (next > size_) {
      failed_ = true;
      return false;
    }
    at_ = static_cast<uint32_t>(next);
    return true;
  }

  uint32_t at() const { return at_; }
  bool failed() const { return failed_; }

 private:
  const uint8_t* data_;
  uint32_t size_;
  uint32_t at_;
  bool failed_ = false;
};

// Annotations are FIXED-SIZE in the stream, which is what makes this walk
// possible: an annotation is two offsets - one to its type, one to its value -
// both pointing elsewhere in the body, so stepping over one costs two dwords
// and needs no understanding of what it describes.
constexpr uint32_t kAnnotationDwords = 2;

bool SkipAnnotations(Reader* reader, uint32_t count) {
  return count <= kMaxAnnotations && reader->Skip(count * kAnnotationDwords);
}

}  // namespace

// The D3DX effect tables, decoded against a real effect rather than inferred.
//
// MotionBlurBlend.xnb and MotionBlur.xnb were unpacked and walked byte by byte
// offline, and every field below is what those files actually contain:
//
//   container  magic BCF00BCF, offset to the body (16)
//   body       tag FEFF0901, offset field, then at 8 + offset:
//                parameters, techniques, (unused), objects
//   parameter  typedef offset, value offset, flags, annotation count
//   typedef    type, class, name offset, semantic offset, elements,
//              columns, rows
//   technique  name offset, annotation count, pass count
//   pass       name offset, annotation count, state count
//   state      operation, index, typedef offset, value offset
//   name       a length followed by that many characters
//
// EVERY OFFSET IS RELATIVE TO body + 8, not to the body. That is the one thing
// that cannot be guessed and the last thing that was wrong: reading typedefs
// from the body start gave a "type" of 432, which is the offset field.
//
// Operation 92 is the vertex shader and 93 the pixel shader, seen on both
// techniques of MotionBlur; the typedef those states point at confirms it with
// a D3DXPARAMETER_TYPE of 16 and 15 respectively. The type is what is trusted -
// the operation number is only corroboration.

namespace {

// All offsets inside the body are measured from here.
constexpr uint32_t kOffsetBase = 8;

// A counted string: a length followed by that many characters.
std::string ReadName(const uint8_t* body, uint32_t size, uint32_t offset,
                     bool image_was_swapped) {
  const uint32_t at = kOffsetBase + offset;
  if (at + 4 > size) {
    return std::string();
  }
  const uint32_t length = ReadWord(body, at);
  if (length == 0 || length > 256 || at + 4 + length > size) {
    return std::string();
  }
  // STRINGS ARE BYTE DATA, NOT WORDS. The container was byte-swapped a whole
  // 32-bit word at a time on load, which is right for every structure in it and
  // right for the shader microcode - but it reverses each group of four
  // characters in a name: "DiffuseColor" came back as "ffiDCesurolo". So the
  // characters are read back through the same swap that put them there, which
  // restores the original bytes exactly.
  std::string name;
  name.reserve(length);
  for (uint32_t i = 0; i < length; ++i) {
    const uint32_t byte_at =
        image_was_swapped ? (at + 4 + (i & ~3u) + (3u - (i & 3u)))
                          : (at + 4 + i);
    if (byte_at >= size) {
      break;
    }
    const uint8_t c = body[byte_at];
    if (c == 0 || c < 0x20 || c >= 0x7F) {
      break;
    }
    name.push_back(static_cast<char>(c));
  }
  return name;
}

}  // namespace

bool WalkEffectTables(const uint8_t* body, uint32_t size, EffectImage* image) {
  if (!body || !image || image->technique_count == 0 ||
      image->technique_count > kMaxTechniques) {
    return false;
  }

  Reader reader(body, size, image->header_offset + 16);

  // Parameters come first, and their names are what a title looks them up by -
  // BasicEffect caches every one of its parameters by name in its constructor
  // and dereferences them straight away, so an empty table is a null reference
  // the moment the first property is set.
  std::vector<EffectParameterInfo> parameters;
  for (uint32_t i = 0; i < image->parameter_count; ++i) {
    uint32_t type_offset = 0, value_offset = 0, flags = 0, annotations = 0;
    if (!reader.Read(&type_offset) || !reader.Read(&value_offset) ||
        !reader.Read(&flags) || !reader.Read(&annotations)) {
      return false;
    }
    if (!SkipAnnotations(&reader, annotations)) {
      return false;
    }
    const uint32_t typedef_at = kOffsetBase + type_offset;
    if (typedef_at + 28 > size) {
      return false;
    }
    EffectParameterInfo parameter;
    parameter.type = ReadWord(body, typedef_at);
    parameter.parameter_class = ReadWord(body, typedef_at + 4);
    parameter.name = ReadName(body, size, ReadWord(body, typedef_at + 8),
                                image->was_byte_swapped ||
                                    image->was_already_swapped);
    parameter.elements = ReadWord(body, typedef_at + 16);
    parameter.columns = ReadWord(body, typedef_at + 20);
    parameter.rows = ReadWord(body, typedef_at + 24);
    parameter.value_offset = value_offset;
    // A parameter with no name cannot be looked up, and every one in a real
    // effect has one - so a nameless parameter means the table is not here.
    if (parameter.name.empty()) {
      return false;
    }
    parameters.push_back(std::move(parameter));
  }

  std::vector<EffectTechniqueInfo> techniques;
  for (uint32_t t = 0; t < image->technique_count; ++t) {
    uint32_t name_offset = 0, annotations = 0, passes = 0;
    if (!reader.Read(&name_offset) || !reader.Read(&annotations) ||
        !reader.Read(&passes)) {
      return false;
    }
    if (annotations > kMaxAnnotations || passes == 0 || passes > kMaxPasses) {
      return false;
    }
    if (!SkipAnnotations(&reader, annotations)) {
      return false;
    }

    EffectTechniqueInfo technique;
    technique.name =
        ReadName(body, size, name_offset,
                 image->was_byte_swapped || image->was_already_swapped);
    if (technique.name.empty()) {
      return false;
    }

    for (uint32_t p = 0; p < passes; ++p) {
      uint32_t pass_name = 0, pass_annotations = 0, states = 0;
      if (!reader.Read(&pass_name) || !reader.Read(&pass_annotations) ||
          !reader.Read(&states)) {
        return false;
      }
      if (pass_annotations > kMaxAnnotations || states > kMaxStates) {
        return false;
      }
      if (!SkipAnnotations(&reader, pass_annotations)) {
        return false;
      }

      EffectPassInfo pass;
      pass.name =
          ReadName(body, size, pass_name,
                   image->was_byte_swapped || image->was_already_swapped);

      for (uint32_t s = 0; s < states; ++s) {
        uint32_t operation = 0, index = 0, type_offset = 0, value_offset = 0;
        if (!reader.Read(&operation) || !reader.Read(&index) ||
            !reader.Read(&type_offset) || !reader.Read(&value_offset)) {
          return false;
        }
        const uint32_t typedef_at = kOffsetBase + type_offset;
        if (typedef_at + 4 > size) {
          return false;
        }
        const uint32_t type = ReadWord(body, typedef_at);
        // The value offset points at a record whose first word is the OBJECT
        // ID and whose second repeats the type. That id is the exact link to
        // the microcode, replacing any nearest-match on offsets.
        const uint32_t value_at = kOffsetBase + value_offset;
        if (value_at + 8 > size) {
          return false;
        }
        const uint32_t object_id = ReadWord(body, value_at);
        if (type == kD3dxTypeVertexShader) {
          pass.vertex_object_id = object_id;
          pass.has_vertex_shader = true;
          pass.vertex_state_index = s;
        } else if (type == kD3dxTypePixelShader) {
          pass.pixel_object_id = object_id;
          pass.has_pixel_shader = true;
          pass.pixel_state_index = s;
        }
      }
      technique.passes.push_back(std::move(pass));
    }
    techniques.push_back(std::move(technique));
  }

  uint32_t bound = 0;
  for (const auto& technique : techniques) {
    for (const auto& pass : technique.passes) {
      bound += (pass.has_vertex_shader ? 1u : 0u) +
               (pass.has_pixel_shader ? 1u : 0u);
    }
  }

  // Everything after the techniques is the object section.
  image->objects_offset = reader.at();
  image->parameters = std::move(parameters);
  image->techniques = std::move(techniques);
  XELOGI("[xna] effect tables: {} parameter(s), {} technique(s), {} pass(es), "
         "{} shader binding(s)",
         image->parameters.size(), image->techniques.size(),
         TotalPasses(*image), bound);
  for (const auto& parameter : image->parameters) {
    XELOGD("[xna]    parameter \"{}\" type {} class {} {}x{} elements {}",
           parameter.name, parameter.type, parameter.parameter_class,
           parameter.rows, parameter.columns, parameter.elements);
  }
  for (const auto& technique : image->techniques) {
    XELOGI("[xna]    technique \"{}\", {} pass(es)", technique.name,
           technique.passes.size());
  }
  return true;
}

uint32_t TotalPasses(const EffectImage& image) {
  uint32_t total = 0;
  for (const auto& technique : image.techniques) {
    total += static_cast<uint32_t>(technique.passes.size());
  }
  return total;
}


// The object section, and resolving a pass to the microcode it binds.
//
// A pass state does not name a shader by position - it points at a record whose
// first word is an OBJECT ID and whose second is the D3DX type. Read out of
// MotionBlur.xnb, whose two techniques bind four shaders:
//
//   op=92 value@4444 -> id 3, type 0x10 (vertex)
//   op=93 value@4468 -> id 4, type 0x0F (pixel)
//   op=92 value@4520 -> id 5
//   op=93 value@4544 -> id 6
//
// The objects themselves follow the techniques as records of
// [id][size][data, padded to a word], after two counts. A shader object's data
// begins with the Xenos shader header 0x102A1100, and the size word before it
// always spans exactly to the end of the blob - verified on MotionBlur,
// MotionBlurBlend and MotionBlurMask.
//
// This replaces guessing entirely. The scan says where microcode COULD be; the
// object section says which of it is real and what its id is; the pass says
// which id it binds and whether as a vertex or a pixel shader.

namespace {

// The first word of a compiled shader object as an effect stores it. The low
// byte is the stage - 0 for a pixel shader, 1 for a vertex one - so this is
// matched on its top three bytes.
//
// It was compared whole, against the pixel value. Every vertex shader object
// therefore failed the test and never entered the object list, so a pass asking
// for its vertex shader by id resolved to a pixel shader instead. That shader
// runs, reads its constants and rasterizes, and fetches no vertices at all.
constexpr uint32_t kXenosShaderHeaderMask = 0xFFFFFF00;
constexpr uint32_t kXenosShaderHeader = 0x102A1100;

}  // namespace

uint32_t AnalyzeObject(EffectImage* image, const EffectObjectInfo& object,
                       gpu::xenos::ShaderType type, const uint8_t* body,
                       uint32_t body_size, uint32_t shader_index = 0);

void ParseEffectObjects(const uint8_t* body, uint32_t size,
                        EffectImage* image) {
  if (!body || !image) {
    return;
  }
  // The section begins where the technique walk stopped.
  uint32_t at = image->objects_offset;
  if (at == 0 || at + 8 > size) {
    return;
  }
  const uint32_t inline_count = ReadWord(body, at);
  const uint32_t resource_count = ReadWord(body, at + 4);
  at += 8;  // the two counts

  const auto align4 = [](uint32_t v) { return (v + 3u) & ~3u; };
  const auto is_shader = [&](uint32_t data, uint32_t bytes) {
    return bytes >= 4 && data <= size - 4 &&
           (ReadWord(body, data) & kXenosShaderHeaderMask) ==
               kXenosShaderHeader;
  };
  const bool swapped = image->was_byte_swapped || image->was_already_swapped;
  bool walked = inline_count <= 65536 && resource_count <= 65536;
  uint32_t direct = 0;
  uint32_t selectors = 0;
  for (uint32_t i = 0; walked && i < inline_count; ++i) {
    if (at + 8 > size) {
      walked = false;
      break;
    }
    const uint32_t id = ReadWord(body, at);
    const uint32_t bytes = ReadWord(body, at + 4);
    const uint32_t data = at + 8;
    if (bytes > size - data) {
      walked = false;
      break;
    }
    if (is_shader(data, bytes)) {
      EffectObjectInfo object;
      object.id = id;
      object.byte_offset = data;
      object.byte_size = bytes;
      image->objects.push_back(object);
      XELOGD("[xna]    object {} is a {} shader: {} bytes at {}", id,
             (ReadWord(body, data) & 0xFF) ? "vertex" : "pixel", bytes, data);
    }
    at = data + align4(bytes);
  }
  for (uint32_t i = 0; walked && i < resource_count; ++i) {
    if (at + 24 > size) {
      walked = false;
      break;
    }
    const uint32_t technique = ReadWord(body, at);
    const uint32_t pass_index = ReadWord(body, at + 4);
    const uint32_t state = ReadWord(body, at + 12);
    const uint32_t usage = ReadWord(body, at + 16);
    const uint32_t bytes = ReadWord(body, at + 20);
    const uint32_t data = at + 24;
    if (bytes > size - data) {
      walked = false;
      break;
    }
    EffectPassInfo* pass = nullptr;
    if (technique < image->techniques.size() &&
        pass_index < image->techniques[technique].passes.size()) {
      pass = &image->techniques[technique].passes[pass_index];
    }
    if (usage == 0 && is_shader(data, bytes)) {
      EffectObjectInfo object;
      object.id = UINT32_MAX;
      object.byte_offset = data;
      object.byte_size = bytes;
      object.technique = technique;
      object.pass = pass_index;
      object.state = state;
      const uint32_t index = static_cast<uint32_t>(image->objects.size());
      if (pass && state == pass->vertex_state_index) {
        object.id = pass->vertex_object_id;
        pass->vertex_object_index = index;
        ++direct;
      } else if (pass && state == pass->pixel_state_index) {
        object.id = pass->pixel_object_id;
        pass->pixel_object_index = index;
        ++direct;
      }
      image->objects.push_back(object);
      XELOGD(
          "[xna]    object {} is a {} shader: {} bytes at {} (technique {} "
          "pass {} state {})",
          object.id, (ReadWord(body, data) & 0xFF) ? "vertex" : "pixel", bytes,
          data, technique, pass_index, state);
    } else if (usage == 2 && pass && bytes >= 8) {
      const uint32_t name_length = ReadWord(body, data);
      const uint32_t preshader_at = data + 4 + align4(name_length);
      EffectShaderSelector* selector =
          state == pass->vertex_state_index  ? &pass->vertex_selector
          : state == pass->pixel_state_index ? &pass->pixel_selector
                                             : nullptr;
      if (selector && preshader_at < data + bytes) {
        selector->valid = true;
        selector->array_name = ReadName(body, size, data - kOffsetBase, swapped);
        selector->preshader_offset = preshader_at;
        selector->preshader_size = data + bytes - preshader_at;
        ++selectors;
        XELOGD("[xna]    pass \"{}\" selects its {} shader from \"{}\"",
               pass->name, state == pass->vertex_state_index ? "vertex" : "pixel",
               selector->array_name);
      }
    }
    at = data + align4(bytes);
  }
  if (walked) {
    for (auto& technique : image->techniques) {
      for (auto& pass : technique.passes) {
        for (auto* selector : {&pass.vertex_selector, &pass.pixel_selector}) {
          if (!selector->valid) {
            continue;
          }
          const EffectParameterInfo* array = nullptr;
          for (const auto& parameter : image->parameters) {
            if (parameter.name == selector->array_name) {
              array = &parameter;
              break;
            }
          }
          if (!array) {
            XELOGW("[xna]    selector names \"{}\", which is not a parameter",
                   selector->array_name);
            continue;
          }
          const uint32_t elements = array->elements ? array->elements : 1;
          uint32_t resolved = 0;
          for (uint32_t e = 0; e < elements; ++e) {
            const uint32_t value_at = kOffsetBase + array->value_offset + e * 4;
            uint32_t element_index = UINT32_MAX;
            if (value_at + 4 <= size) {
              const uint32_t id = ReadWord(body, value_at);
              for (uint32_t o = 0; o < image->objects.size(); ++o) {
                if (image->objects[o].id == id &&
                    image->objects[o].technique == UINT32_MAX) {
                  element_index = o;
                  break;
                }
              }
            }
            resolved += element_index != UINT32_MAX ? 1 : 0;
            selector->element_object_indices.push_back(element_index);
          }
          XELOGI("[xna]    selector \"{}\": {} of {} element(s) resolved",
                 selector->array_name, resolved, elements);
        }
      }
    }
    XELOGI(
        "[xna] effect objects: {} shader blob(s), {} bound to a pass directly, "
        "{} array selector(s)",
        image->objects.size(), direct, selectors);
    return;
  }
  image->objects.clear();
  for (auto& technique : image->techniques) {
    for (auto& pass : technique.passes) {
      pass.vertex_object_index = UINT32_MAX;
      pass.pixel_object_index = UINT32_MAX;
      pass.vertex_selector = EffectShaderSelector();
      pass.pixel_selector = EffectShaderSelector();
    }
  }
  XELOGW("[xna] effect object section did not walk - scanning for shaders");
  for (uint32_t probe = at; probe + 4 <= size; probe += 4) {
    if ((ReadWord(body, probe) & kXenosShaderHeaderMask) !=
        kXenosShaderHeader) {
      continue;
    }
    if (probe < 8) {
      continue;
    }
    const uint32_t id = ReadWord(body, probe - 8);
    uint32_t object_size = ReadWord(body, probe - 4);
    // A size that does not fit is not a reason to abandon the section - only
    // this record. The next magic bounds it either way.
    if (object_size > size - probe) {
      object_size = size - probe;
    }
    if (object_size < 4) {
      continue;
    }
    EffectObjectInfo object;
    object.id = id;
    object.byte_offset = probe;
    object.byte_size = object_size;
    image->objects.push_back(object);
    XELOGD("[xna]    object {} is a {} shader: {} bytes at {}", id,
           (ReadWord(body, probe) & 0xFF) ? "vertex" : "pixel", object_size,
           probe);
  }
  XELOGI("[xna] effect objects: {} shader blob(s)", image->objects.size());
}

void BuildEffectShaderTables(const uint8_t* body, uint32_t size,
                             EffectImage* image) {
  if (!body || !image) {
    return;
  }
  image->vertex_shader_table.clear();
  image->pixel_shader_table.clear();

  for (uint32_t i = 0; i < image->objects.size(); ++i) {
    const EffectObjectInfo& object = image->objects[i];
    if (object.byte_offset + 4 > size) {
      continue;
    }
    const bool is_vertex = (ReadWord(body, object.byte_offset) & 0xFF) != 0;
    auto& table =
        is_vertex ? image->vertex_shader_table : image->pixel_shader_table;
    if (table.size() >= kMaxShaderTableEntries) {
      continue;
    }
    EffectNamedShader entry;
    entry.element = static_cast<uint32_t>(table.size());
    entry.name = (is_vertex ? "vs[" : "ps[") + std::to_string(entry.element) +
                 "]";
    entry.object_id = object.id;
    entry.object_index = i;
    entry.pointer = body + object.byte_offset;
    table.push_back(std::move(entry));
  }
  XELOGI("[xna] effect shader tables: {} vertex, {} pixel entry(ies)",
         image->vertex_shader_table.size(), image->pixel_shader_table.size());
}

const EffectNamedShader* EffectShaderAt(const EffectImage& image,
                                        gpu::xenos::ShaderType stage,
                                        uint32_t index) {
  const auto& table = stage == gpu::xenos::ShaderType::kVertex
                          ? image.vertex_shader_table
                          : image.pixel_shader_table;
  return index < table.size() ? &table[index] : nullptr;
}

// Ties each pass to the microcode it binds, and analyzes only those.
void ResolvePassShaders(const uint8_t* body, uint32_t size,
                        EffectImage* image) {
  if (!image || image->objects.empty()) {
    return;
  }
  image->body = body;
  image->body_size = size;

  // Shader objects appear in the section in ascending id order, so the ids the
  // passes reference line up with the blobs found, in order. Matching on the
  // recorded id first keeps that from mattering when the id is exact.
  auto blob_for = [&](uint32_t object_id) -> const EffectObjectInfo* {
    for (const auto& object : image->objects) {
      if (object.id == object_id) {
        return &object;
      }
    }
    return nullptr;
  };

  // Every distinct id a pass binds, in ascending order, paired with the blobs
  // in the order they appear - the fallback when the recorded id is not the
  // one the state names.
  std::vector<uint32_t> referenced;
  for (const auto& technique : image->techniques) {
    for (const auto& pass : technique.passes) {
      if (pass.has_vertex_shader) referenced.push_back(pass.vertex_object_id);
      if (pass.has_pixel_shader) referenced.push_back(pass.pixel_object_id);
    }
  }
  std::sort(referenced.begin(), referenced.end());
  referenced.erase(std::unique(referenced.begin(), referenced.end()),
                   referenced.end());

  // Objects sit in the container in ascending offset order and the passes name
  // them in that same order, so the position of an id within the ids the passes
  // reference is the position of its object among the objects found. That is
  // the fallback when the id itself is not in the list - the previous one
  // indexed the object list by a position in a sorted-unique id array, which is
  // a different ordering and returned an unrelated blob.
  auto resolve = [&](uint32_t object_id) -> const EffectObjectInfo* {
    if (const auto* exact = blob_for(object_id)) {
      return exact;
    }
    const auto found =
        std::lower_bound(referenced.begin(), referenced.end(), object_id);
    if (found != referenced.end() && *found == object_id) {
      const size_t position = static_cast<size_t>(found - referenced.begin());
      if (position < image->objects.size()) {
        return &image->objects[position];
      }
    }
    return nullptr;
  };
  auto index_of = [&](const EffectObjectInfo* blob) -> uint32_t {
    if (!blob) {
      return UINT32_MAX;
    }
    return static_cast<uint32_t>(blob - image->objects.data());
  };

  // WHICH STAGE AN OBJECT IS COMES FROM THE OBJECT, NOT FROM THE FIELD IT WAS
  // FOUND IN.
  //
  // The two ids a pass carries were being taken in the order they appear, one
  // as the vertex shader and one as the pixel shader. Against the magic - which
  // agrees with the shader's own version token in every one of the hundred
  // objects in this effect - that ordering disagreed 1394 times out of 1394.
  // A pixel shader bound as the vertex shader writes no position at all, and
  // the rasterizer then covers an unbounded area with whatever was left in the
  // output registers, which hangs the device.
  auto stage_of = [&](const EffectObjectInfo& object) {
    return (ReadWord(body, object.byte_offset) & 0xFF)
               ? gpu::xenos::ShaderType::kVertex
               : gpu::xenos::ShaderType::kPixel;
  };

  for (auto& technique : image->techniques) {
    for (auto& pass : technique.passes) {
      // Both ids are taken whenever they are set, not only when the flag that
      // came from the same misread state says so. Those flags decide WHICH of
      // the two the pass thinks is the vertex shader, and that was wrong every
      // time - so a pass whose one flagged object turns out to be the other
      // stage would fill one slot and leave the other empty, which the managed
      // runtime refuses to draw with.
      const EffectObjectInfo* blobs[2] = {nullptr, nullptr};
      if (pass.vertex_object_id && !pass.vertex_selector.valid &&
          pass.vertex_object_index == UINT32_MAX) {
        blobs[0] = resolve(pass.vertex_object_id);
      }
      if (pass.pixel_object_id && !pass.pixel_selector.valid &&
          pass.pixel_object_index == UINT32_MAX) {
        blobs[1] = resolve(pass.pixel_object_id);
      }
      // Both ids resolving to one object means neither was found and the
      // positional fallback returned the same blob twice. Using it for both
      // stages puts a pixel shader in the vertex slot, which writes no position
      // and hangs the device - the same fault the stage fix removed. One shader
      // in its correct slot draws nothing; two wrong ones take the GPU down.
      if (blobs[0] && blobs[0] == blobs[1]) {
        blobs[1] = nullptr;
      }
      for (const auto* blob : blobs) {
        if (!blob) {
          continue;
        }
        const auto stage = stage_of(*blob);
        const uint32_t index = index_of(blob);
        if (index == UINT32_MAX) {
          continue;
        }
        if (stage == gpu::xenos::ShaderType::kVertex) {
          if (pass.vertex_object_index == UINT32_MAX) {
            pass.vertex_object_index = index;
          }
        } else if (pass.pixel_object_index == UINT32_MAX) {
          pass.pixel_object_index = index;
        }
      }
      if (pass.vertex_object_index == UINT32_MAX &&
          !pass.vertex_selector.valid && !image->vertex_shader_table.empty()) {
        pass.vertex_object_index = image->vertex_shader_table[0].object_index;
        XELOGW("[xna]    pass \"{}\" takes {} for its vertex stage", pass.name,
               image->vertex_shader_table[0].name);
      }
      if (pass.pixel_object_index == UINT32_MAX &&
          !pass.pixel_selector.valid && !image->pixel_shader_table.empty()) {
        pass.pixel_object_index = image->pixel_shader_table[0].object_index;
        XELOGW("[xna]    pass \"{}\" takes {} for its pixel stage", pass.name,
               image->pixel_shader_table[0].name);
      }
      pass.has_vertex_shader = pass.vertex_object_index != UINT32_MAX ||
                               pass.vertex_selector.valid;
      pass.has_pixel_shader =
          pass.pixel_object_index != UINT32_MAX || pass.pixel_selector.valid;
      if (!pass.has_vertex_shader || !pass.has_pixel_shader) {
        XELOGW(
            "[xna]    pass \"{}\" has only {} shader - the effect holds {} "
            "vertex and {} pixel object(s)",
            pass.name,
            pass.vertex_object_index != UINT32_MAX ? "a vertex" : "a pixel",
            image->vertex_shader_table.size(),
            image->pixel_shader_table.size());
      }
    }
  }
}

// The parameters a shader object uses, read out of the constant table that sits
// immediately in front of its microcode.
//
// The layout, confirmed against a decompressed DeferredObjectEffect: a run of
// null terminated names, then the target ("vs_3_0" or "ps_3_0"), then the
// compiler version ("2.0.11626.0"), then 0xAB padding, then the microcode. The
// names are what D3DX assigned registers to, for THIS shader only, which is why
// a register computed by running a total across the whole effect overshoots the
// 256 the hardware has.
// A shader object is not microcode. It begins with a header whose fields are
// offsets into it, and the microcode is one of the things they point at.
//
// Confirmed against a decompressed DeferredObjectEffect, where every shader
// object starts with the same magic:
//
//   +0   0x102A1100
//   +8   offset to the constant name table
//   +24  offset to the shader program block
//   +36  offset to the (count, register) records
//
// The program block is self describing - a dword offset to the instructions,
// then their byte count. Handing the translator the object's first byte fed it
// this header, which parsed as control flow that ends immediately, which is why
// every shader analyzed cleanly and read no constants at all.
// The low byte is the stage: 0 is a pixel shader, 1 a vertex shader. In the
// effect dissected there were 64 of the first and 36 of the second, which is
// exactly the number of target strings it contains.
constexpr uint32_t kShaderObjectMagicMask = 0xFFFFFF00;
constexpr uint32_t kShaderObjectMagic = 0x102A1100;

// THE OBJECT CARRIES A D3DX CONSTANT TABLE. IT IS NOT A GUESS.
//
// Forty bytes past the 0x102A11xx magic sits a plain D3DXSHADER_CONSTANTTABLE -
// the same structure D3DXGetShaderConstantTable returns on the PC, big-endian,
// with every offset relative to the table's own first byte:
//
//   +0  Size (always 28)    +4  Creator ("2.0.11626.0")   +8  Version (FFFE0300)
//   +12 Constants           +16 ConstantInfo offset       +20 Flags
//   +24 Target ("vs_3_0")
//
// followed by Constants x D3DXSHADER_CONSTANTINFO, twenty bytes each:
//
//   +0 Name offset  +4 RegisterSet  +6 RegisterIndex  +8 RegisterCount
//   +10 Reserved    +12 TypeInfo offset               +16 DefaultValue offset
//
// Everything that came before this scraped printable runs out of the object and
// then guessed each name's register by summing the sizes of the names in front
// of it. That guess cannot be right: the vertex shader of this effect puts
// _World at c0, _SkinBones at c9 for two hundred and twenty five registers,
// _View at c234 and _Projection at c241. Nothing that walks up from zero
// reaches c234, so oPos was computed from registers holding another
// parameter's data.
struct ShaderConstantRecord {
  std::string name;
  uint32_t register_set = 0;
  uint32_t register_index = 0;
  uint32_t register_count = 0;
  std::vector<float> default_value;
};

std::vector<ShaderConstantRecord> ReadShaderConstantTable(
    const uint8_t* body, uint32_t body_size, uint32_t object_begin,
    uint32_t object_end, bool image_was_swapped, bool is_vertex,
    bool* table_found_out) {
  std::vector<ShaderConstantRecord> records;
  *table_found_out = false;
  object_end = std::min(object_end, body_size);
  if (object_begin + 96 > object_end) {
    return records;
  }

  // Bytes, not words. The container was reversed a dword at a time when it was
  // loaded, and an object does not have to begin on a dword boundary - the
  // vertex object examined starts at 71541 - so every field is assembled from
  // un-swapped bytes rather than read as a host word.
  const auto byte_at = [&](uint32_t p) -> uint8_t {
    const uint32_t real =
        image_was_swapped ? ((p & ~3u) + (3u - (p & 3u))) : p;
    return real < body_size ? body[real] : 0;
  };
  const auto u32_at = [&](uint32_t p) -> uint32_t {
    return (uint32_t(byte_at(p)) << 24) | (uint32_t(byte_at(p + 1)) << 16) |
           (uint32_t(byte_at(p + 2)) << 8) | uint32_t(byte_at(p + 3));
  };
  const auto u16_at = [&](uint32_t p) -> uint32_t {
    return (uint32_t(byte_at(p)) << 8) | uint32_t(byte_at(p + 1));
  };
  const auto string_at = [&](uint32_t p) {
    std::string out;
    for (uint32_t i = 0; i < 256 && p + i < object_end; ++i) {
      const uint8_t c = byte_at(p + i);
      if (c == 0 || c < 0x20 || c >= 0x7F) {
        break;
      }
      out.push_back(static_cast<char>(c));
    }
    return out;
  };

  // Forty for every object seen, but validated rather than trusted: a table is
  // a table only if it says it is 28 bytes long, its records start immediately
  // after it, and its version is one of the two shader models this container
  // holds. A small window is searched so an object laid out slightly
  // differently is still found instead of silently yielding nothing.
  uint32_t table = UINT32_MAX;
  for (uint32_t probe = object_begin + 32;
       probe <= object_begin + 64 && probe + 28 <= object_end; ++probe) {
    if (u32_at(probe) != 28) {
      continue;
    }
    const uint32_t version = u32_at(probe + 8);
    if (version != (is_vertex ? 0xFFFE0300u : 0xFFFF0300u)) {
      continue;
    }
    // The records follow the table immediately, so ConstantInfo is 28 - unless
    // there are no constants at all, and then it is zero. Four of the hundred
    // objects in this effect are that shader, and requiring 28 threw them away.
    if (u32_at(probe + 12) && u32_at(probe + 16) != 28) {
      continue;
    }
    table = probe;
    break;
  }
  if (table == UINT32_MAX) {
    return records;
  }
  *table_found_out = true;

  const uint32_t count = u32_at(table + 12);
  if (!count || count > 512) {
    return records;
  }
  const uint32_t info = table + u32_at(table + 16);
  if (info < table || info + count * 20 > object_end) {
    return records;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t record = info + i * 20;
    ShaderConstantRecord out;
    out.name = string_at(table + u32_at(record));
    out.register_set = u16_at(record + 4);
    out.register_index = u16_at(record + 6);
    out.register_count = u16_at(record + 8);
    if (out.name.empty() || !out.register_count) {
      continue;
    }
    const uint32_t defaults = u32_at(record + 16);
    if (defaults && out.register_set == 2) {
      const uint32_t at = table + defaults;
      const uint32_t floats = out.register_count * 4;
      if (at >= table && at + floats * 4 <= object_end) {
        out.default_value.reserve(floats);
        for (uint32_t f = 0; f < floats; ++f) {
          const uint32_t bits = u32_at(at + f * 4);
          float value;
          std::memcpy(&value, &bits, sizeof(value));
          out.default_value.push_back(value);
        }
      }
    }
    records.push_back(std::move(out));
  }
  return records;
}

// THE SHADER IS NOT FINISHED. THE DECLARATION FINISHES IT.
//
// Every vfetch_full a compiled vertex shader carries is BLANK: fetch constant
// 95, offset 0, stride 0, format undefined. On the console, SetVertexDeclaration
// patches those three fields into the microcode from the vertex elements before
// the draw - the shader alone does not know how its vertices are laid out. With
// stride 0 every vertex reads the same bytes, so nothing has any extent and
// nothing rasterizes, which is exactly what a hosted title does without this.
//
// What the shader does carry is which semantic each fetch is waiting for.
// Header +24 is an offset from the constant table to a run of four-byte
// entries, one per blank fetch, in instruction order:
//
//   flags:16 | usage:4 | usage_index:4 | instruction:8
//
// usage is D3DDECLUSAGE. Verified against all 34 vertex objects of
// DeferredObjectEffect that have blank fetches: entry count matches fetch
// count, and every instruction index matches, in order, on all 34.
std::vector<EffectFetchSemantic> ReadFetchSemantics(
    const uint8_t* body, uint32_t body_size, uint32_t object_begin,
    uint32_t object_end, uint32_t microcode_begin, uint32_t microcode_bytes,
    bool image_was_swapped) {
  std::vector<EffectFetchSemantic> semantics;
  object_end = std::min(object_end, body_size);
  if (object_begin + 32 > object_end || !microcode_bytes) {
    return semantics;
  }
  const auto byte_at = [&](uint32_t p) -> uint8_t {
    const uint32_t real =
        image_was_swapped ? ((p & ~3u) + (3u - (p & 3u))) : p;
    return real < body_size ? body[real] : 0;
  };
  const auto u32_at = [&](uint32_t p) -> uint32_t {
    return (uint32_t(byte_at(p)) << 24) | (uint32_t(byte_at(p + 1)) << 16) |
           (uint32_t(byte_at(p + 2)) << 8) | uint32_t(byte_at(p + 3));
  };

  // Every blank fetch in the program, by instruction index. An instruction is
  // three dwords, and the index the signature uses counts those from the start
  // of the microcode - the same number the disassembly prints.
  const auto* dwords =
      reinterpret_cast<const uint32_t*>(body + microcode_begin);
  const uint32_t instructions = microcode_bytes / 12;
  std::vector<uint32_t> blank;
  for (uint32_t i = 0; i < instructions; ++i) {
    const gpu::ucode::VertexFetchInstruction& op =
        *reinterpret_cast<const gpu::ucode::VertexFetchInstruction*>(dwords +
                                                                     i * 3);
    if (op.opcode() != gpu::ucode::FetchOpcode::kVertexFetch ||
        op.is_mini_fetch() || op.fetch_constant_index() != 95 ||
        op.stride() || op.offset() ||
        op.data_format() != gpu::xenos::VertexFormat::kUndefined) {
      continue;
    }
    blank.push_back(i);
  }
  if (blank.empty()) {
    return semantics;
  }

  const uint32_t table = object_begin + 40 + u32_at(object_begin + 24);
  if (table < object_begin ||
      table + uint32_t(blank.size()) * 4 > object_end) {
    return semantics;
  }
  // The table is only believed if it describes exactly these fetches, in this
  // order. Anything else and the shader is left alone rather than patched with
  // a semantic that belongs to some other instruction.
  for (uint32_t i = 0; i < blank.size(); ++i) {
    const uint32_t entry = u32_at(table + i * 4);
    if ((entry & 0xFF) != blank[i]) {
      return {};
    }
    EffectFetchSemantic semantic;
    semantic.instruction = blank[i];
    semantic.usage = (entry >> 12) & 0xF;
    semantic.usage_index = (entry >> 8) & 0xF;
    semantics.push_back(semantic);
  }
  return semantics;
}

std::vector<std::string> ReadConstantNames(const uint8_t* body,
                                           uint32_t body_size,
                                           uint32_t object_begin,
                                           uint32_t object_size,
                                           uint32_t* microcode_begin_out,
                                           uint32_t* microcode_bytes_out,
                                           bool* is_vertex_out,
                                           bool image_was_swapped,
                                           std::vector<uint32_t>* registers_out,
                                           std::vector<uint32_t>* counts_out) {
  *is_vertex_out = false;
  registers_out->clear();
  counts_out->clear();
  std::vector<std::string> names;
  *microcode_begin_out = 0;
  *microcode_bytes_out = 0;
  // Written as a subtraction because the addition wraps: an object claiming a
  // huge size makes object_begin + object_size small again, the check passes,
  // and every bound derived from it points off the end of the container.
  if (object_size < 64 || object_begin > body_size ||
      object_size > body_size - object_begin) {
    return names;
  }
  // The container was swapped into host order when it was loaded, so these are
  // read straight, the same way the parameter and technique tables are.
  auto word = [&](uint32_t at) { return ReadWord(body, at); };
  const uint32_t magic = word(object_begin);
  if ((magic & kShaderObjectMagicMask) != kShaderObjectMagic) {
    return names;
  }
  // Every shader carries the same magic; its low byte is the stage. In the
  // effect examined, all 64 objects ending 00 held a ps_3_0 token at +48 and
  // all 36 ending 01 held vs_3_0 - so the object states what it is, and that
  // can be checked against what the pass claimed it bound.
  *is_vertex_out = (magic & 0xFF) != 0;
  const uint32_t object_end = object_begin + object_size;

  // The field at +4 is the size of the DESCRIPTOR - the header, the sampler
  // table, the names and the records - not of the shader. The shader follows
  // it, after a run of float literals whose length is not stated anywhere.
  //
  // Guessing that run at sixteen dwords put the start a few dwords out, and a
  // control flow program read from the wrong dword decodes into a backward
  // jump: the GPU ran it until the watchdog killed the device. So the start is
  // not guessed. MeasureControlFlow accepts only a program whose every opcode
  // is real and whose every exec address stays inside the blob, and it reports
  // how long that program is - so the first offset it accepts is the shader,
  // and if it accepts none then this object holds no program to run.
  const uint32_t descriptor_size = word(object_begin + 4);
  if (descriptor_size < object_size) {
    const uint32_t search_begin = (object_begin + descriptor_size + 3) & ~3u;

    // Where the next object begins is where this one's data has to stop.
    // Objects are stored as (id, size, data) records back to back, so the eight
    // bytes in front of the next magic are that record's header. This bound
    // comes from the file, unlike the size field, which has already been seen
    // to arrive impossibly large. Found once for the object, not per candidate.
    uint32_t limit = object_end;
    for (uint32_t probe = search_begin; probe + 4 <= body_size; probe += 4) {
      if ((ReadWord(body, probe) & kShaderObjectMagicMask) ==
          kShaderObjectMagic) {
        const uint32_t record_begin = probe >= 8 ? probe - 8 : probe;
        if (record_begin > search_begin) {
          limit = std::min(limit, record_begin);
        }
        break;
      }
    }

    for (uint32_t at = search_begin; at + 12 <= limit; at += 4) {
      const uint32_t available = (limit - at) / 4;
      const auto* dwords = reinterpret_cast<const uint32_t*>(body + at);

      // A program does not begin with a nop. The float literals that sit in
      // front of the microcode decode as one - zero is a nop, and so is any
      // float whose top bits happen to land there - so a candidate started in
      // the middle of that pool walks through it into the real program and
      // measures as valid. The control flow then looks right while every
      // instruction address is off by the literals that were swallowed, and the
      // fetches that decodes produce index off the end of their bitmaps.
      gpu::ucode::ControlFlowInstruction first[2];
      gpu::ucode::UnpackControlFlowInstructions(dwords, first);
      if (first[0].opcode() == gpu::ucode::ControlFlowOpcode::kNop) {
        continue;
      }
      if (MeasureControlFlow(dwords, available) &&
          IsAnalyzableUcode(dwords, available)) {
        // The shader gets everything MeasureControlFlow checked, not the
        // length it measured. It validates each address against the dwords
        // remaining, so an address can be legal by that test and still sit past
        // the measured program - and analysis then reads off the end of the
        // shader's own copy. That is what crashed on object 46, whose
        // instructions genuinely run past the measured extent.
        *microcode_begin_out = at;
        *microcode_bytes_out = available * 4;
        break;
      }
    }
  }

  // ANCHOR ON THE TARGET STRING, NOT ON THE HEADER FIELD.
  //
  // +8 points at the names for a pixel object - 15401, first name at 15413 -
  // but for a vertex object at 71541 it points at 72553 while its names
  // (_View, _ViewToWorld, _World, _WorldToObject) sit at 75528. The field does
  // not mean the same thing for both stages. Trusting it returned fragments
  // like "ampler" and "0" and never a single matrix, which is exactly what a
  // vertex shader needs. "vs_3_0"/"ps_3_0" always follows the names, so it is
  // the anchor and the names are taken from in front of it.
  // STRINGS ARE BYTE DATA, NOT WORDS - the same trap ReadName documents.
  //
  // The container was byte-swapped a whole word at a time on load, which is
  // right for every structure in it and reverses each group of four characters
  // in a name. Reading these raw produced "tucepPralrewodnA_omA_" where the
  // effect declares "_SpecularPower_And_Amount", so no name ever matched a
  // parameter and not one constant was written. ReadName already un-swaps for
  // the parameter table; this reader did not.
  auto text_byte = [&](uint32_t p) -> uint8_t {
    const uint32_t real = image_was_swapped
                              ? ((p & ~3u) + (3u - (p & 3u)))
                              : p;
    return real < body_size ? body[real] : 0;
  };

  uint32_t target_at = UINT32_MAX;
  for (uint32_t probe = object_begin; probe + 7 <= object_end; ++probe) {
    const uint8_t first = text_byte(probe);
    if (first != 'v' && first != 'p') {
      continue;
    }
    char candidate[7];
    for (uint32_t i = 0; i < 7; ++i) {
      candidate[i] = static_cast<char>(text_byte(probe + i));
    }
    if (!std::memcmp(candidate, "vs_3_0", 7) ||
        !std::memcmp(candidate, "ps_3_0", 7)) {
      target_at = probe;
      break;
    }
  }
  if (target_at == UINT32_MAX) {
    return names;
  }
  // Back over the run of names to the first byte that is not part of one.
  // The names come in groups separated by runs of zeros - one vertex object
  // has _View and _ViewToWorld at 75529 and _World and _WorldToObject at
  // 75597, with a gap between. Stopping at the first gap found half of them,
  // so the walk continues back over zeros and only stops at a byte that is
  // neither text nor padding.
  uint32_t at = target_at;
  while (at > object_begin) {
    const uint8_t b = text_byte(at - 1);
    if (b != 0x00 && b != 0xAB && (b < 0x20 || b >= 0x7F)) {
      break;
    }
    --at;
  }
  while (at < target_at &&
         (text_byte(at) == 0x00 || text_byte(at) == 0xAB)) {
    ++at;
  }
  // Bounded by the target, not the object - everything past it is the creator
  // string, the padding and the microcode.
  while (at < target_at) {
    if (text_byte(at) < 0x20 || text_byte(at) >= 0x7F) {
      break;
    }
    std::string name;
    while (at < target_at) {
      const uint8_t c = text_byte(at);
      if (c < 0x20 || c >= 0x7F) {
        break;
      }
      name.push_back(static_cast<char>(c));
      ++at;
    }
    if (at >= target_at || text_byte(at) != 0x00) {
      break;
    }
    ++at;
    names.push_back(std::move(name));
    // Names are packed one after another, but the record block sits among them
    // and is not printable - step over it.
    while (at < target_at &&
           (text_byte(at) == 0x00 || text_byte(at) == 0xAB)) {
      ++at;
    }
  }

  // THE REGISTER EACH NAME OCCUPIES, STATED BY THE FILE.
  //
  // Header +36 points at a run of big-endian (u16 count, u16 register) pairs,
  // after 0xAB padding, ending at a zero pair. There is one per name and they
  // pair in order - object 14725 declares _ParallaxScale_And_Offset,
  // _SpecularPower_And_Amount and _TargetWidthHeight and carries (1,3) (1,2)
  // (1,0), and object 16277 repeats both lists identically.
  //
  // Everything before this inferred the register by summing parameter sizes in
  // declaration order, which produced numbers past the 256 the hardware has.
  // The mapping was in the file the whole time.
  // Read as whole words, not bytes: the container is dword-swapped, so a record
  // taken bytewise comes back scrambled the same way the names did. ReadWord
  // gives the value the file actually holds - count in the high half, register
  // in the low.
  // Read as BYTES through the same un-swap the names use. The records are not
  // dword aligned - one object's run begins at an offset of 1 mod 4, and
  // another's starts in the middle of a word after "ht\0" and three 0xAB - so
  // reading them as whole words worked for one object and returned nothing for
  // the next.
  const uint32_t records_offset = word(object_begin + 36);
  if (records_offset + 4 <= object_size) {
    // Find the run by validating it, not by guessing where the padding ends.
    // The offset lands a few bytes before the records and the amount varies -
    // one object needed none skipped, another three - so each starting byte in
    // a small window is tried and the first that yields a well formed record
    // followed by another record or the terminator is taken.
    const auto record_at = [&](uint32_t q, uint32_t* count_out,
                               uint32_t* reg_out) {
      *count_out = (uint32_t(text_byte(q)) << 8) | text_byte(q + 1);
      *reg_out = (uint32_t(text_byte(q + 2)) << 8) | text_byte(q + 3);
    };
    const auto plausible = [&](uint32_t q) {
      if (q + 4 > object_end) {
        return false;
      }
      uint32_t count = 0, reg = 0;
      record_at(q, &count, &reg);
      return count && count <= 256 && reg < 256;
    };

    uint32_t p = object_begin + records_offset;
    const uint32_t search_end =
        std::min<uint32_t>(p + 32, object_end > 8 ? object_end - 8 : p);
    for (; p < search_end; ++p) {
      if (!plausible(p)) {
        continue;
      }
      uint32_t count = 0, reg = 0;
      record_at(p + 4, &count, &reg);
      if (plausible(p + 4) || (!count && !reg)) {
        break;
      }
    }
    while (p + 4 <= object_end) {
      const uint32_t count =
          (uint32_t(text_byte(p)) << 8) | text_byte(p + 1);
      const uint32_t reg =
          (uint32_t(text_byte(p + 2)) << 8) | text_byte(p + 3);
      if (!count && !reg) {
        break;
      }
      if (!count || count > 256 || reg >= 256 || reg + count > 256) {
        break;
      }
      // The count is how many registers the parameter spans - four for a
      // matrix. Discarding it let _View land at c0 and _World at c1, so the
      // second overwrote three quarters of the first.
      registers_out->push_back(reg);
      counts_out->push_back(count);
      p += 4;
    }
  }
  return names;
}

bool ShaderRecordForObject(const uint8_t* body, uint32_t body_size,
                           const EffectObjectInfo& object, uint32_t index,
                           uint32_t* begin_out, uint32_t* bytes_out) {
  if (object.byte_size < 32 || object.byte_offset > body_size ||
      object.byte_size > body_size - object.byte_offset) {
    return false;
  }
  if ((ReadWord(body, object.byte_offset) & kXenosShaderHeaderMask) !=
      kXenosShaderHeader) {
    return false;
  }
  const uint32_t table_at = (index + 3) * 8;
  if (table_at + 4 > object.byte_size) {
    return false;
  }
  const uint32_t record_at = ReadWord(body, object.byte_offset + table_at);
  if (!record_at || record_at + 8 > object.byte_size) {
    return false;
  }
  const uint32_t base = ReadWord(body, object.byte_offset + 4);
  const uint32_t within = ReadWord(body, object.byte_offset + record_at);
  const uint32_t bytes = ReadWord(body, object.byte_offset + record_at + 4);
  if (!bytes || bytes % 12 || bytes > kMaxShaderRecordBytes) {
    return false;
  }
  const uint64_t at = uint64_t(base) + within;
  if (at + bytes > object.byte_size) {
    return false;
  }
  *begin_out = object.byte_offset + static_cast<uint32_t>(at);
  *bytes_out = bytes;
  return true;
}

// Analyzes one shader object, now that a pass has named it and said which kind
// it is. This is the only place a Shader is built - nothing found by the scan
// alone is ever analyzed.
uint32_t AnalyzeObject(EffectImage* image, const EffectObjectInfo& object,
                       gpu::xenos::ShaderType type, const uint8_t* body,
                       uint32_t body_size, uint32_t shader_index) {
  for (uint32_t i = 0; i < image->shaders.size(); ++i) {
    if (image->shaders[i].dword_offset == object.byte_offset / 4 &&
        image->shaders[i].shader) {
      return i;
    }
  }
  const auto* dwords = reinterpret_cast<const uint32_t*>(body);
  EffectShaderInfo info;
  info.type = type;
  info.found = true;

  // A shader object is its constant table followed by its microcode, not
  // microcode alone. Starting at the object's first byte handed the translator
  // the table - which analyzed as a shader that reads nothing and draws
  // nothing.
  uint32_t microcode_begin = 0;
  uint32_t microcode_bytes = 0;
  bool object_is_vertex = false;
  std::vector<uint32_t> declared_registers;
  std::vector<uint32_t> declared_counts;
  info.constant_names =
      ReadConstantNames(body, body_size, object.byte_offset, object.byte_size,
                        // EITHER flag means the bytes of a string are reversed
                        // within each word. was_already_swapped is set when an
                        // earlier load of the same array did the swapping - the
                        // dwords are then in host order but the characters are
                        // still four-at-a-time backwards, which is what its
                        // declaration says. Passing only was_byte_swapped left
                        // 36 effects reading scrambled names, so none of their
                        // parameters ever matched and their registers stayed
                        // empty.
                        &microcode_begin, &microcode_bytes, &object_is_vertex,
                        image->was_byte_swapped || image->was_already_swapped,
                        &declared_registers, &declared_counts);
  if (microcode_bytes &&
      object_is_vertex != (type == gpu::xenos::ShaderType::kVertex)) {
    // Reported, not acted on. The magic's low byte tracked the stage perfectly
    // across the hundred shaders of one effect, but it does not hold in
    // general - and refusing the object on that basis left a pass with no
    // vertex shader, which the managed runtime rejects outright at the first
    // draw. The pass is the authority on what it bound.
    XELOGW(
        "[xna]    object {}: the pass bound it as a {} shader while its magic "
        "suggests a {} one - keeping the pass's word",
        object.id, type == gpu::xenos::ShaderType::kVertex ? "vertex" : "pixel",
        object_is_vertex ? "vertex" : "pixel");
  }
  if (!microcode_bytes) {
    // Not laid out the way a 0x102A11xx object is. Refusing outright would
    // leave the effect with no shaders at all, so this keeps the old behaviour
    // of treating the object as microcode - and says so, because that is the
    // reading that has never produced a shader which uses a constant.
    // Says which of the two it was. This message claimed the header was
    // missing whatever the reason, and an object that has one but whose program
    // could not be located reads identically - which is a different problem
    // with a different fix.
    const bool has_header =
        object.byte_size >= 4 &&
        (ReadWord(body, object.byte_offset) & kXenosShaderHeaderMask) ==
            kXenosShaderHeader;
    XELOGW(
        "[xna]    object {}: {} - treating all {} bytes as microcode",
        object.id,
        has_header ? "has a header but no control flow program was found in it"
                   : "has no 0x102A11xx header",
        object.byte_size);
    if (object.byte_offset > body_size ||
        object.byte_size > body_size - object.byte_offset) {
      XELOGW("[xna]    object {} is out of range of the {} byte container",
             object.id, body_size);
      return UINT32_MAX;
    }
    microcode_begin = object.byte_offset;
    microcode_bytes = object.byte_size;
  }
  uint32_t record_offset = 0;
  uint32_t record_bytes = 0;
  if (ShaderRecordForObject(body, body_size, object, shader_index,
                            &record_offset, &record_bytes)) {
    if (microcode_begin != record_offset || microcode_bytes != record_bytes) {
      XELOGI(
          "[xna]    object {}: record {} says microcode at +{} for {}; the "
          "search said +{} for {}",
          object.id, shader_index, record_offset - object.byte_offset,
          record_bytes, microcode_begin - object.byte_offset, microcode_bytes);
    }
    microcode_begin = record_offset;
    microcode_bytes = record_bytes;
  }

  info.dword_offset = microcode_begin / 4;
  info.dword_count = microcode_bytes / 4;
  if (!info.dword_count) {
    return UINT32_MAX;
  }

  // Whatever lies between the descriptor and the microcode is the literal pool
  // the compiler emitted. It is addressed from the top of the constant file,
  // which is why the disassembly reads c255 and c253 that no parameter ever
  // writes.
  {
    const uint32_t descriptor_end =
        object.byte_offset + ReadWord(body, object.byte_offset + 4);
    if (microcode_begin > descriptor_end &&
        microcode_begin - descriptor_end >= 16) {
      const uint32_t literal_bytes =
          (microcode_begin - descriptor_end) & ~uint32_t(15);
      info.literals.resize(literal_bytes / 4);
      std::memcpy(info.literals.data(), body + descriptor_end, literal_bytes);
    }
  }

  // Last line before the microcode is touched. Analysis of a malformed program
  // takes the process with it, and a crash after this line names the object and
  // the exact range it died on instead of leaving it to be inferred from which
  // log line came last.

  // Nothing may be read outside the container, whatever the tables claimed.
  if (microcode_begin > body_size ||
      microcode_bytes > body_size - microcode_begin) {
    XELOGW("[xna]    object {}: microcode range is outside the container",
           object.id);
    return UINT32_MAX;
  }

  // An exec address is twelve bits, so no instruction the analysis can follow
  // reaches beyond this many dwords - the padding covers every address that can
  // be encoded, whether or not the program is what we believe it to be.
  // Held only long enough to construct the shader, which takes its own copy.
  constexpr uint32_t kUcodePaddingDwords = 4096 * 3;
  std::vector<uint32_t> padded_ucode(
      dwords + info.dword_offset,
      dwords + info.dword_offset + info.dword_count);
  padded_ucode.resize(info.dword_count + kUcodePaddingDwords, 0);

  // Kept so a draw can take a copy and fill its vertex fetches in from the
  // declaration in force. The Shader built below never gets that treatment -
  // it is the unpatched program, good for reading and for nothing else.
  info.ucode.assign(padded_ucode.begin(), padded_ucode.end());
  if (type == gpu::xenos::ShaderType::kVertex) {
    info.fetch_semantics = ReadFetchSemantics(
        body, body_size, object.byte_offset,
        object.byte_offset + object.byte_size, microcode_begin, microcode_bytes,
        image->was_byte_swapped || image->was_already_swapped);
  }

  auto shader = std::make_unique<EffectShader>(
      type, xe::memory::hash_combine(0, info.dword_offset, image->size),
      padded_ucode.data(), padded_ucode.size());
  StringBuffer disasm;
  // The microcode goes out BEFORE it is analyzed. Analysis of a malformed
  // program takes the process with it, and the shader that does that is the one
  // worth reading - dumping afterwards means the only bytes never written are
  // the ones that matter.
  const char* extension =
      type == gpu::xenos::ShaderType::kVertex ? "vert" : "frag";
  const auto stem =
      fmt::format("object_{:03d}_at_{}.{}", object.id, microcode_begin,
                  extension);
  if (!cvars::xna_dump_shaders.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(cvars::xna_dump_shaders, ec);
    if (FILE* binary = xe::filesystem::OpenFile(
            cvars::xna_dump_shaders / (stem + ".bin"), "wb")) {
      fwrite(dwords + info.dword_offset, sizeof(uint32_t), info.dword_count,
             binary);
      fclose(binary);
    }
  }

  shader->AnalyzeUcode(disasm);

  if (!cvars::xna_dump_shaders.empty()) {
    if (FILE* text = xe::filesystem::OpenFile(
            cvars::xna_dump_shaders / (stem + ".txt"), "w")) {
      const auto& listing = shader->ucode_disassembly();
      fwrite(listing.data(), 1, listing.size(), text);
      fclose(text);
    }
  }

  if (!shader->is_ucode_analyzed()) {
    XELOGW("[xna]    object {} did not analyze as a {} shader", object.id,
           type == gpu::xenos::ShaderType::kVertex ? "vertex" : "pixel");
    return UINT32_MAX;
  }

  // What the shader itself says it reads, against what its table claims it
  // was given. These come from two independent places - Xenia's analysis of the
  // microcode, and the effect's own bytes - so they agree only if the table was
  // read correctly. Disagreement is reported here rather than showing up later
  // as a black frame.
  uint32_t registers_read = 0;
  uint32_t highest_register = 0;
  const auto& map = shader->constant_register_map();
  for (uint32_t i = 0; i < 256; ++i) {
    if (map.float_bitmap[i / 64] & (uint64_t(1) << (i % 64))) {
      ++registers_read;
      highest_register = i;
    }
  }

  // The literals sit at the top of the constant file and belong to no
  // parameter - reported only so the two kinds of constant are distinguishable
  // in the log.
  const uint32_t literal_registers =
      static_cast<uint32_t>(info.literals.size() / 4);
  // THE TABLE FIRST. INFERENCE ONLY IF THERE IS NO TABLE.
  //
  // D3DXSHADER_CONSTANTINFO gives the register and the register count outright,
  // and RegisterSet says which file they belong to - a sampler at s3 and a
  // float4 at c3 are different registers with the same number, so a sampler
  // must not be allowed to claim a constant register.
  bool table_found = false;
  const auto table = ReadShaderConstantTable(
      body, body_size, object.byte_offset,
      object.byte_offset + object.byte_size,
      image->was_byte_swapped || image->was_already_swapped, object_is_vertex,
      &table_found);
  if (table_found) {
    info.constant_names.clear();
    for (const auto& record : table) {
      if (record.register_set == 3) {
        info.sampler_registers[record.name] = record.register_index;
        continue;
      }
      if (record.register_set != 2 || record.register_index >= 256) {
        continue;
      }
      info.constant_names.push_back(record.name);
      info.constant_registers[record.name] = record.register_index;
      info.constant_counts[record.name] = record.register_count;
      if (!record.default_value.empty()) {
        info.constant_defaults[record.name] = record.default_value;
      }
    }
  } else if (!info.constant_names.empty()) {
    XELOGW(
        "[xna]    object {}: no constant table - falling back to inferring "
        "registers from the order of {} scraped name(s)",
        object.id, info.constant_names.size());
    const uint32_t first_literal =
        literal_registers < 256 ? 256 - literal_registers : 256;
    uint32_t lowest = UINT32_MAX;
    for (uint32_t i = 0; i < first_literal; ++i) {
      if (map.float_bitmap[i / 64] & (uint64_t(1) << (i % 64))) {
        lowest = i;
        break;
      }
    }
    if (lowest != UINT32_MAX) {
      uint32_t next = lowest;
      for (const auto& name : info.constant_names) {
        const EffectParameterInfo* parameter = nullptr;
        for (const auto& candidate : image->parameters) {
          if (candidate.name == name) {
            parameter = &candidate;
            break;
          }
        }
        uint32_t span = 1;
        if (parameter) {
          const uint32_t elements =
              parameter->elements ? parameter->elements : 1;
          if (parameter->parameter_class == 2) {
            span = elements * (parameter->rows ? parameter->rows : 1);
          } else if (parameter->parameter_class == 3) {
            span = elements * (parameter->columns ? parameter->columns : 1);
          } else if (parameter->parameter_class == 4) {
            continue;  // a sampler takes no constant register
          } else {
            span = elements;
          }
        }
        if (next + span > first_literal) {
          break;
        }
        info.constant_registers[name] = next;
        next += span;
      }
    }
  }

  for (const auto& name : info.constant_names) {
    if (info.constant_registers.find(name) == info.constant_registers.end()) {
      // A named constant with nowhere to go is read from whatever the last
      // effect left in that register - the only case here worth a word.
      XELOGW("[xna]    object {}: constant \"{}\" is unmapped", object.id, name);
    }
  }
  info.shader = std::move(shader);
  image->shaders.push_back(std::move(info));
  return static_cast<uint32_t>(image->shaders.size() - 1);
}

namespace {

constexpr uint32_t kPreshaderCtab = 0x42415443;
constexpr uint32_t kPreshaderClit = 0x54494C43;
constexpr uint32_t kPreshaderFxlc = 0x434C5846;
constexpr uint32_t kPreshaderRegisters = 64;

struct PreshaderConstant {
  std::string name;
  uint32_t register_index = 0;
  uint32_t register_count = 1;
};

std::string PreshaderString(const uint8_t* body, uint32_t size, uint32_t at,
                            bool swapped) {
  std::string name;
  for (uint32_t i = 0; i < 128; ++i) {
    const uint32_t byte_at =
        swapped ? (at + (i & ~3u) + (3u - (i & 3u))) : (at + i);
    if (byte_at >= size) {
      break;
    }
    const uint8_t c = body[byte_at];
    if (c == 0 || c < 0x20 || c >= 0x7F) {
      break;
    }
    name.push_back(static_cast<char>(c));
  }
  return name;
}

}  // namespace

bool EvaluatePreshader(const EffectImage& image,
                       const EffectShaderSelector& selector,
                       const EffectParameterLookup& lookup, float* out,
                       std::string* inputs_out) {
  const uint8_t* body = image.body;
  const uint32_t size = image.body_size;
  if (!body || !selector.valid || !out ||
      selector.preshader_offset >= size ||
      selector.preshader_size > size - selector.preshader_offset ||
      selector.preshader_size < 8) {
    return false;
  }
  const bool swapped = image.was_byte_swapped || image.was_already_swapped;
  const uint32_t end = selector.preshader_offset + selector.preshader_size;
  std::vector<PreshaderConstant> constants;
  std::vector<double> literals;
  uint32_t fxlc_at = 0;
  uint32_t fxlc_dwords = 0;

  uint32_t at = selector.preshader_offset + 4;
  while (at + 8 <= end) {
    const uint32_t token = ReadWord(body, at);
    if (token == 0x0000FFFF) {
      break;
    }
    if ((token & 0xFFFF) != 0xFFFE) {
      at += 4;
      continue;
    }
    const uint32_t length = token >> 16;
    const uint32_t tag = ReadWord(body, at + 4);
    const uint32_t chunk = at + 8;
    if (length == 0 || length * 4 > end - at - 4) {
      break;
    }
    const uint32_t chunk_dwords = length - 1;
    if (tag == kPreshaderCtab && chunk_dwords >= 7) {
      const uint32_t count = ReadWord(body, chunk + 12);
      const uint32_t info = ReadWord(body, chunk + 16);
      for (uint32_t i = 0; i < count && i < 32; ++i) {
        const uint32_t record = chunk + info + i * 20;
        if (record + 20 > end) {
          break;
        }
        PreshaderConstant constant;
        constant.name =
            PreshaderString(body, size, chunk + ReadWord(body, record), swapped);
        const uint32_t set_and_index = ReadWord(body, record + 4);
        const uint32_t count_and_reserved = ReadWord(body, record + 8);
        constant.register_index = set_and_index & 0xFFFF;
        constant.register_count = count_and_reserved >> 16;
        if (!constant.register_count) {
          constant.register_count = 1;
        }
        constants.push_back(std::move(constant));
      }
    } else if (tag == kPreshaderClit && chunk_dwords >= 1) {
      const uint32_t count = ReadWord(body, chunk);
      for (uint32_t i = 0; i < count && chunk + 4 + i * 8 + 8 <= end; ++i) {
        const uint64_t hi = ReadWord(body, chunk + 4 + i * 8);
        const uint64_t lo = ReadWord(body, chunk + 8 + i * 8);
        const uint64_t bits = (hi << 32) | lo;
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        literals.push_back(value);
      }
    } else if (tag == kPreshaderFxlc) {
      fxlc_at = chunk;
      fxlc_dwords = chunk_dwords;
    }
    at += 4 + length * 4;
  }
  if (!fxlc_at) {
    XELOGW("[xna] selector \"{}\": no FXLC block in its preshader",
           selector.array_name);
    return false;
  }

  double consts[kPreshaderRegisters * 4] = {};
  double temps[kPreshaderRegisters * 4] = {};
  double outputs[kPreshaderRegisters * 4] = {};
  for (const auto& constant : constants) {
    for (uint32_t r = 0; r < constant.register_count; ++r) {
      const uint32_t base = (constant.register_index + r) * 4;
      if (base + 4 > kPreshaderRegisters * 4) {
        break;
      }
      float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      bool have = lookup && lookup(constant.name, r, value);
      bool from_default = false;
      if (!have) {
        for (const auto& parameter : image.parameters) {
          if (parameter.name != constant.name) {
            continue;
          }
          // ONLY THE COMPONENTS THE PARAMETER DECLARES. Reading four regardless
          // of shape walked off the end of a 1x1 int into whatever sat next to
          // it, which is why _LightCount read back as 0 12 1.6e9 1.75e9 - the
          // zero was real, the rest was the neighbouring parameter.
          const uint32_t rows = std::max<uint32_t>(1, parameter.rows);
          const uint32_t columns = std::max<uint32_t>(1, parameter.columns);
          const bool matrix =
              parameter.parameter_class == 2 || parameter.parameter_class == 3;
          const bool by_column = parameter.parameter_class == 3;
          const uint32_t per_element = !matrix ? 1 : (by_column ? columns : rows);
          const uint32_t element = r / per_element;
          const uint32_t line = r % per_element;
          const uint32_t declared = parameter.elements ? parameter.elements : 1;
          uint32_t components = std::min<uint32_t>(4, rows * columns);
          uint32_t first = 0;
          uint32_t step = 1;
          if (matrix && by_column) {
            components = std::min<uint32_t>(4, rows);
            first = line;
            step = columns;
          } else if (matrix) {
            components = std::min<uint32_t>(4, columns);
            first = line * columns;
          }
          const uint32_t value_at = kOffsetBase + parameter.value_offset +
                                    element * rows * columns * 4;
          for (uint32_t c = 0; element < declared && c < components; ++c) {
            const uint32_t word_at = value_at + (first + c * step) * 4;
            if (word_at + 4 > size) {
              break;
            }
            const uint32_t word = ReadWord(body, word_at);
            if (parameter.type == 3) {
              std::memcpy(&value[c], &word, sizeof(float));
            } else {
              value[c] = static_cast<float>(static_cast<int32_t>(word));
            }
          }
          have = true;
          from_default = true;
          break;
        }
      }
      for (uint32_t c = 0; c < 4; ++c) {
        consts[base + c] = value[c];
      }
      // Only when the caller asks, which it does when the index came out of
      // range - the value that produced a bad index is the whole diagnosis, and
      // printing every input of every healthy evaluation buries it.
      if (inputs_out) {
        // WHICH SOURCE, not just whether there was one. "set" covered both the
        // title's value and the effect's compiled-in default, and the whole
        // question here is which of the two the bad index came from.
        *inputs_out += fmt::format(
            "{}{} c{} = {} {} {} {} ({})", inputs_out->empty() ? "" : ", ",
            constant.name, constant.register_index + r, value[0], value[1],
            value[2], value[3],
            !have ? "NOTHING SET IT"
                  : (from_default ? "effect default - the title never set it"
                                  : "set by the title"));
      }
    }
  }

  const uint32_t instruction_count = ReadWord(body, fxlc_at);
  uint32_t cursor = fxlc_at + 4;
  const uint32_t fxlc_end = fxlc_at + fxlc_dwords * 4;
  struct Operand {
    uint32_t table = 0;
    uint32_t offset = 0;
    bool relative = false;
    uint32_t index_table = 0;
    uint32_t index_offset = 0;
  };
  const auto read_operand = [&](Operand* operand) -> bool {
    if (cursor + 12 > fxlc_end) {
      return false;
    }
    operand->relative = ReadWord(body, cursor) != 0;
    cursor += 4;
    if (operand->relative) {
      if (cursor + 16 > fxlc_end) {
        return false;
      }
      operand->index_table = ReadWord(body, cursor);
      operand->index_offset = ReadWord(body, cursor + 4);
      cursor += 8;
    }
    operand->table = ReadWord(body, cursor);
    operand->offset = ReadWord(body, cursor + 4);
    cursor += 8;
    return true;
  };
  const auto load = [&](uint32_t table, uint32_t offset) -> double {
    switch (table) {
      case 1:
        return offset < literals.size() ? literals[offset] : 0.0;
      case 2:
        return offset < kPreshaderRegisters * 4 ? consts[offset] : 0.0;
      case 4:
      case 5:
      case 6:
        return offset < kPreshaderRegisters * 4 ? outputs[offset] : 0.0;
      case 7:
        return offset < kPreshaderRegisters * 4 ? temps[offset] : 0.0;
      default:
        return 0.0;
    }
  };
  const auto read = [&](const Operand& operand, uint32_t component) -> double {
    uint32_t offset = operand.offset + component;
    if (operand.relative) {
      const uint32_t width = operand.table == 1 ? 1u : 4u;
      const int32_t index = static_cast<int32_t>(
          std::lround(load(operand.index_table, operand.index_offset)));
      offset = static_cast<uint32_t>(
          (static_cast<int32_t>(offset / width) + index) *
              static_cast<int32_t>(width) +
          static_cast<int32_t>(offset % width));
    }
    return load(operand.table, offset);
  };
  const auto store = [&](const Operand& operand, uint32_t component,
                         double value) {
    const uint32_t offset = operand.offset + component;
    if (offset >= kPreshaderRegisters * 4) {
      return;
    }
    if (operand.table == 7) {
      temps[offset] = value;
    } else if (operand.table >= 4 && operand.table <= 6) {
      outputs[offset] = value;
    }
  };

  for (uint32_t i = 0; i < instruction_count && i < 4096; ++i) {
    if (cursor + 8 > fxlc_end) {
      return false;
    }
    const uint32_t word = ReadWord(body, cursor);
    const uint32_t input_count = ReadWord(body, cursor + 4);
    cursor += 8;
    const bool scalar = (word >> 31) != 0;
    const uint32_t opcode = (word >> 20) & 0x7FF;
    uint32_t components = word & 0xFFFFF;
    if (!components || components > 4) {
      components = 1;
    }
    if (input_count > 8) {
      return false;
    }
    Operand inputs[8];
    for (uint32_t n = 0; n < input_count; ++n) {
      if (!read_operand(&inputs[n])) {
        return false;
      }
    }
    Operand output;
    if (!read_operand(&output)) {
      return false;
    }
    if (opcode == 0x500) {
      double sum = 0.0;
      for (uint32_t c = 0; c < components; ++c) {
        sum += read(inputs[0], c) * read(inputs[1], c);
      }
      store(output, 0, sum);
      continue;
    }
    for (uint32_t c = 0; c < components; ++c) {
      const uint32_t component = scalar ? 0 : c;
      const double a = input_count > 0 ? read(inputs[0], component) : 0.0;
      const double b = input_count > 1 ? read(inputs[1], component) : 0.0;
      const double d = input_count > 2 ? read(inputs[2], component) : 0.0;
      double result = 0.0;
      switch (opcode) {
        case 0x000: continue;
        case 0x100: result = a; break;
        case 0x101: result = -a; break;
        case 0x103: result = a != 0.0 ? 1.0 / a : 0.0; break;
        case 0x104: result = a - std::floor(a); break;
        case 0x105: result = std::exp2(a); break;
        case 0x106: result = a > 0.0 ? std::log2(a) : 0.0; break;
        case 0x107: result = a > 0.0 ? 1.0 / std::sqrt(a) : 0.0; break;
        case 0x108: result = std::sin(a); break;
        case 0x109: result = std::cos(a); break;
        case 0x10a: result = std::asin(a); break;
        case 0x10b: result = std::acos(a); break;
        case 0x10c: result = std::atan(a); break;
        case 0x200: result = std::min(a, b); break;
        case 0x201: result = std::max(a, b); break;
        case 0x202: result = a < b ? 1.0 : 0.0; break;
        case 0x203: result = a >= b ? 1.0 : 0.0; break;
        case 0x204: result = a + b; break;
        case 0x205: result = a * b; break;
        case 0x206: result = std::atan2(a, b); break;
        case 0x208: result = b != 0.0 ? a / b : 0.0; break;
        case 0x300: result = a >= 0.0 ? b : d; break;
        default:
          XELOGW("[xna] selector \"{}\": preshader opcode {:03X} is not handled",
                 selector.array_name, opcode);
          return false;
      }
      store(output, c, result);
    }
  }
  for (uint32_t c = 0; c < 4; ++c) {
    out[c] = static_cast<float>(outputs[c]);
  }
  return true;
}

void ResolvePassShadersOnUse(EffectImage* image, EffectPassInfo* pass,
                             const EffectParameterLookup& lookup) {
  if (!image || !pass) {
    return;
  }
  if (!image->body || !image->body_size) {
    return;
  }
  const auto select = [&](const EffectShaderSelector& selector,
                          const char* stage, uint32_t* object_index_out) {
    if (!selector.valid || selector.element_object_indices.empty()) {
      return;
    }
    float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!EvaluatePreshader(*image, selector, lookup, value, nullptr)) {
      XELOGW("[xna] pass \"{}\": could not evaluate the {} selector \"{}\"",
             pass->name, stage, selector.array_name);
      return;
    }
    int32_t index = static_cast<int32_t>(value[0]);
    const int32_t last =
        static_cast<int32_t>(selector.element_object_indices.size()) - 1;
    if (index < 0 || index > last) {
      // Re-run it collecting the inputs. An index outside the array means the
      // pass falls back to element 0 - for the deferred lighting array that is
      // the wrong light shader entirely, which leaves the lighting buffers at
      // their cleared black and every object lit by it comes out black. Which
      // parameter fed it is the only thing that says why.
      std::string inputs;
      float again[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      EvaluatePreshader(*image, selector, lookup, again, &inputs);
      XELOGW(
          "[xna] pass \"{}\": {} selector \"{}\" evaluated to {} of {} - "
          "clamped to element {}; inputs: {}",
          pass->name, stage, selector.array_name, value[0], last + 1,
          std::min(std::max(index, 0), last), inputs);
      index = std::min(std::max(index, 0), last);
    }
    const uint32_t object_index = selector.element_object_indices[index];
    if (object_index == UINT32_MAX) {
      XELOGW("[xna] pass \"{}\": {} selector \"{}\" element {} has no object",
             pass->name, stage, selector.array_name, index);
      return;
    }
    if (*object_index_out != object_index) {
      XELOGD("[xna] pass \"{}\": {} shader is \"{}\"[{}] -> object index {}",
             pass->name, stage, selector.array_name, index, object_index);
    }
    *object_index_out = object_index;
  };
  select(pass->vertex_selector, "vertex", &pass->vertex_object_index);
  select(pass->pixel_selector, "pixel", &pass->pixel_object_index);
  const bool selects =
      pass->vertex_selector.valid || pass->pixel_selector.valid;
  if (pass->shaders_resolved && !selects) {
    return;
  }
  pass->shaders_resolved = true;
  const auto analyze = [&](uint32_t object_index, gpu::xenos::ShaderType stage,
                           uint32_t* shader_index_out) {
    if (object_index >= image->objects.size()) {
      return;
    }
    const uint64_t key =
        (uint64_t(object_index) << 1) |
        (stage == gpu::xenos::ShaderType::kPixel ? 1u : 0u);
    auto found = image->analyzed_objects.find(key);
    if (found != image->analyzed_objects.end()) {
      *shader_index_out = found->second;
      return;
    }
    const uint32_t shader_index =
        AnalyzeObject(image, image->objects[object_index], stage, image->body,
                      image->body_size);
    if (shader_index != UINT32_MAX) {
      image->analyzed_objects.emplace(key, shader_index);
    }
    *shader_index_out = shader_index;
  };
  analyze(pass->vertex_object_index, gpu::xenos::ShaderType::kVertex,
          &pass->vertex_shader_index);
  analyze(pass->pixel_object_index, gpu::xenos::ShaderType::kPixel,
          &pass->pixel_shader_index);
}

void XnaSetEffectTechnique(uint32_t handle, uint32_t technique_handle) {
  std::lock_guard<std::mutex> lock(effects_mutex);
  auto found = effects.find(handle);
  if (found == effects.end() || !found->second) {
    return;
  }
  auto& image = *found->second;
  // D3D_Effect_GetTechnique hands the title index + 1, and the packet carries
  // that handle back.
  if (!technique_handle) {
    return;
  }
  const uint32_t index = technique_handle - 1;
  if (index >= image.techniques.size()) {
    XELOGW("[xna] effect {}: technique handle {} names index {} of {}", handle,
           technique_handle, index, image.techniques.size());
    return;
  }
  if (image.current_technique != index) {
    XELOGD("[xna] effect {}: technique {} \"{}\"", handle, index,
           image.techniques[index].name);
  }
  image.current_technique = index;
}

EffectImage* FindEffect(uint32_t handle) {
  std::lock_guard<std::mutex> lock(effects_mutex);
  auto found = effects.find(handle);
  return found == effects.end() ? nullptr : found->second.get();
}

uint32_t RegisterEffect(std::unique_ptr<EffectImage> image) {
  const uint32_t handle = XnaAllocateHandle();
  std::lock_guard<std::mutex> lock(effects_mutex);
  effects[handle] = std::move(image);
  return handle;
}

uint64_t ContentHashForRegistration(const uint8_t* data, uint32_t size) {
  return ContentHash(data, size);
}

void NoteEffectContent(uint64_t hash, uint32_t handle) {
  std::lock_guard<std::mutex> lock(effects_mutex);
  effects_by_content[hash] = handle;
}

uint32_t FindPreparedEffect(const uint8_t* data, uint32_t size) {
  // The handoff within one Effect..ctor comes first: it is exact, where a
  // content match cannot be, because the two calls are handed different
  // buffers.
  if (prepared_on_this_thread != 0) {
    const uint32_t handle = prepared_on_this_thread;
    prepared_on_this_thread = 0;
    return handle;
  }
  if (!data || size < 8) {
    return 0;
  }
  const uint64_t hash = ContentHash(data, size);
  std::lock_guard<std::mutex> lock(effects_mutex);
  auto found = effects_by_content.find(hash);
  return found == effects_by_content.end() ? 0 : found->second;
}

void NotePreparedOnThisThread(uint32_t handle) {
  prepared_on_this_thread = handle;
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe

// Called from the managed side with the effect bytes, before the console
// runtime looks at them. The array is pinned by the caller for the duration,
// because this writes back into the very bytes it is about to parse.
// A log line from managed code, reached the same way Prepare is.
//
// XnaOs::Log cannot be used from anything injected into a console assembly.
// It dispatches through a static function table bound once at bootstrap, and
// statics are per-AssemblyLoadContext: MXF.Graphics runs in the title's context,
// gets its own copy of that static, and never has it bound - so every Log call
// from a rewritten method returns silently at the null check. A diagnostic that
// cannot report is worse than none, because its silence reads as "the code did
// not run" when the code ran fine.
//
// Resolving an export off the running program has no such problem: there is one
// process, and NativeLibrary finds it from any load context.
extern "C" void Nexia_XnaLog(uint32_t level, const char* message) {
  if (!message) {
    return;
  }
  switch (level) {
    case 2: XELOGW("[xna] {}", message); break;
    case 3: XELOGE("[xna] {}", message); break;
    default: XELOGI("[xna] {}", message); break;
  }
}

extern "C" uint32_t Nexia_XnaPrepareEffect(uint8_t* data, uint32_t size) {
  auto image = std::make_unique<xe::kernel::xna::EffectImage>();
  if (!xe::kernel::xna::PrepareEffectImage(data, size, image.get())) {
    return 0;
  }
  // Kept so D3D_Effect_CreateHandle can find the shaders again by content
  // rather than re-parsing the blob it is handed. The hash is taken AFTER the
  // swap, which is the state CreateHandle will see the bytes in.
  const uint64_t hash = xe::kernel::xna::ContentHashForRegistration(data, size);
  const uint32_t handle = xe::kernel::xna::RegisterEffect(std::move(image));
  xe::kernel::xna::NoteEffectContent(hash, handle);
  xe::kernel::xna::NotePreparedOnThisThread(handle);
  return 1;
}
