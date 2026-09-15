/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The HLCB command stream: what XNA actually sends the console to draw with.
//
// Every packet is a 32-bit header word, `(type << 24) | (handle & 0x00FFFFFF)`,
// followed by a fixed number of 32-bit arguments for its type - or, for the
// draws that carry their own geometry, a length field and that many bytes. The
// layouts below were read out of Microsoft.Xna.Framework.Graphics.PacketHelpers
// with tools/xna/dump-il.ps1: each Send*Packet method declares its own size to
// EnsurePacketSize and then writes its fields in order, so the wire format is
// stated outright rather than inferred.
//
// THE STREAM IS SELF-CHECKING. A packet decoded at the wrong size does not fail
// where the mistake is - it shifts everything after it, and the next header
// word is read out of the middle of the previous packet's arguments. So every
// header is validated against the known types, and the first one that is not
// recognised stops the walk and reports the byte offset. A decoder that guessed
// its way past a bad size would produce a plausible-looking frame built from
// garbage.

#include "xenia/kernel/xna/xna_packets.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "third_party/fmt/include/fmt/format.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/xna/xna_exports.h"
#include "xenia/kernel/xna/xna_guest_heap.h"

namespace xe {
namespace kernel {
namespace xna {

namespace {

// PacketHelpers::MaskPacketType is `(uint)type << 24`, and every handle is
// masked with 0x00FFFFFF before being merged in.
constexpr uint32_t kHandleMask = 0x00FFFFFF;

uint32_t PacketType(uint32_t header) { return header >> 24; }
uint32_t PacketHandle(uint32_t header) { return header & kHandleMask; }

// Total packet size in bytes, header included, for the fixed-size types. Zero
// means the size depends on the packet contents and is worked out in Walk.
// These are the constants each sender passes to EnsurePacketSize.
uint32_t FixedSize(HlcbPacketType type) {
  switch (type) {
    case HlcbPacketType::kClear:
      return 28;  // header + Vector4 colour + float depth + int stencil
    case HlcbPacketType::kSetIndexBuffer:
    case HlcbPacketType::kSetVertexDeclaration:
    case HlcbPacketType::kSetBlendState:
    case HlcbPacketType::kSetDepthStencilState:
    case HlcbPacketType::kSetRasterizerState:
    case HlcbPacketType::kBeginQuery:
    case HlcbPacketType::kEndQuery:
      return 4;  // the handle in the header is the whole packet
    case HlcbPacketType::kSetSamplerState:
      // Not 4 like the other three state objects: a sampler state is bound to
      // a numbered slot, so the slot index follows the handle. Read out of a
      // real buffer, which opened with one Blend, one DepthStencil and one
      // Rasterizer at 4 bytes each and then twenty SetSamplerState packets for
      // slots 0..0x13 - 12 + 20 * 8 = 172, exactly the buffer length.
      return 8;
    case HlcbPacketType::kSetStreamSource:
      return 20;  // + stream index, offset, stride, instance frequency
    case HlcbPacketType::kSetTexture:
      return 8;  // + sampler index
    case HlcbPacketType::kSetHighFrequencyState:
      return 8;  // + value
    case HlcbPacketType::kSetScissorRect:
      return 20;  // + Rectangle
    case HlcbPacketType::kSetViewPort:
      return 28;  // + Viewport (x, y, w, h, minDepth, maxDepth)
    case HlcbPacketType::kDrawPrimitives:
      return 16;  // + primitive type, start vertex, primitive count
    case HlcbPacketType::kDrawIndexedPrimitives:
      return 28;  // + type, base vertex, min index, num vertices, start, count
    case HlcbPacketType::kDrawInstancedPrimitives:
      return 32;
    case HlcbPacketType::kEffectApply:
      return 16;
    case HlcbPacketType::kEffectSetTexture:
      return 12;
    case HlcbPacketType::kEffectSetTechnique:
      return 8;
    default:
      return 0;
  }
}

bool IsKnownType(uint32_t type) {
  switch (static_cast<HlcbPacketType>(type)) {
    case HlcbPacketType::kClear:
    case HlcbPacketType::kSetIndexBuffer:
    case HlcbPacketType::kSetVertexDeclaration:
    case HlcbPacketType::kSetStreamSource:
    case HlcbPacketType::kSetTexture:
    case HlcbPacketType::kEffectSetValueBool:
    case HlcbPacketType::kEffectSetValueFloat:
    case HlcbPacketType::kEffectSetValueInt:
    case HlcbPacketType::kEffectSetValueMatrix:
    case HlcbPacketType::kEffectSetValueString:
    case HlcbPacketType::kEffectSetValueTexture:
    case HlcbPacketType::kEffectSetValueVector:
    case HlcbPacketType::kEffectSetValueMatrixTranspose:
    case HlcbPacketType::kEffectSetValueBoolArray:
    case HlcbPacketType::kEffectSetValueFloatArray:
    case HlcbPacketType::kEffectSetValueIntArray:
    case HlcbPacketType::kEffectSetValueMatrixArray:
    case HlcbPacketType::kEffectSetValueVectorArray:
    case HlcbPacketType::kEffectSetValueMatrixTransposeArray:
    case HlcbPacketType::kEffectApply:
    case HlcbPacketType::kEffectSetTexture:
    case HlcbPacketType::kSetBlendState:
    case HlcbPacketType::kSetDepthStencilState:
    case HlcbPacketType::kSetRasterizerState:
    case HlcbPacketType::kSetSamplerState:
    case HlcbPacketType::kSetHighFrequencyState:
    case HlcbPacketType::kSetRenderTargets:
    case HlcbPacketType::kSetScissorRect:
    case HlcbPacketType::kSetViewPort:
    case HlcbPacketType::kDrawPrimitives:
    case HlcbPacketType::kDrawIndexedPrimitives:
    case HlcbPacketType::kDrawUserIndexedPrimitives:
    case HlcbPacketType::kDrawSprites:
    case HlcbPacketType::kEffectSetTechnique:
    case HlcbPacketType::kDrawUserPrimitives:
    case HlcbPacketType::kBeginQuery:
    case HlcbPacketType::kEndQuery:
    case HlcbPacketType::kDrawInstancedPrimitives:
      return true;
    default:
      return false;
  }
}

const char* TypeName(uint32_t type) {
  switch (static_cast<HlcbPacketType>(type)) {
    case HlcbPacketType::kClear:
      return "Clear";
    case HlcbPacketType::kSetIndexBuffer:
      return "SetIndexBuffer";
    case HlcbPacketType::kSetVertexDeclaration:
      return "SetVertexDeclaration";
    case HlcbPacketType::kSetStreamSource:
      return "SetStreamSource";
    case HlcbPacketType::kSetTexture:
      return "SetTexture";
    case HlcbPacketType::kEffectApply:
      return "EffectApply";
    case HlcbPacketType::kEffectSetTexture:
      return "EffectSetTexture";
    case HlcbPacketType::kEffectSetTechnique:
      return "EffectSetTechnique";
    case HlcbPacketType::kSetBlendState:
      return "SetBlendState";
    case HlcbPacketType::kSetDepthStencilState:
      return "SetDepthStencilState";
    case HlcbPacketType::kSetRasterizerState:
      return "SetRasterizerState";
    case HlcbPacketType::kSetSamplerState:
      return "SetSamplerState";
    case HlcbPacketType::kSetHighFrequencyState:
      return "SetHighFrequencyState";
    case HlcbPacketType::kSetRenderTargets:
      return "SetRenderTargets";
    case HlcbPacketType::kSetScissorRect:
      return "SetScissorRect";
    case HlcbPacketType::kSetViewPort:
      return "SetViewPort";
    case HlcbPacketType::kDrawPrimitives:
      return "DrawPrimitives";
    case HlcbPacketType::kDrawIndexedPrimitives:
      return "DrawIndexedPrimitives";
    case HlcbPacketType::kDrawUserIndexedPrimitives:
      return "DrawUserIndexedPrimitives";
    case HlcbPacketType::kDrawUserPrimitives:
      return "DrawUserPrimitives";
    case HlcbPacketType::kDrawSprites:
      return "DrawSprites";
    case HlcbPacketType::kDrawInstancedPrimitives:
      return "DrawInstancedPrimitives";
    case HlcbPacketType::kBeginQuery:
      return "BeginQuery";
    case HlcbPacketType::kEndQuery:
      return "EndQuery";
    default:
      return "EffectSetValue";
  }
}

uint32_t ReadWord(const uint8_t* data, size_t offset) {
  uint32_t value;
  std::memcpy(&value, data + offset, sizeof(value));
  return value;
}

float ReadFloat(const uint8_t* data, size_t offset) {
  float value;
  std::memcpy(&value, data + offset, sizeof(value));
  return value;
}

}  // namespace

// Bytes per element for the EffectSetValue family, straight out of
// PacketHelpers::GetEffectPacketTypeSize - a switch on (type - 8) over the
// fourteen types 8..21. UINT32_MAX means "not one of these".
//
// This is also the stride the title's own data has: a Matrix array arrives as
// sixty-four bytes per element whatever the shader reserved for it, so it is
// the only thing that says where element e of an array begins.
uint32_t EffectValueElementSize(uint32_t type) {
  switch (static_cast<HlcbPacketType>(type)) {
    case HlcbPacketType::kEffectSetValueBool:
    case HlcbPacketType::kEffectSetValueFloat:
    case HlcbPacketType::kEffectSetValueInt:
    case HlcbPacketType::kEffectSetValueBoolArray:
    case HlcbPacketType::kEffectSetValueFloatArray:
    case HlcbPacketType::kEffectSetValueIntArray:
      return 4;
    case HlcbPacketType::kEffectSetValueMatrix:
    case HlcbPacketType::kEffectSetValueMatrixTranspose:
    case HlcbPacketType::kEffectSetValueMatrixArray:
    case HlcbPacketType::kEffectSetValueMatrixTransposeArray:
      return 64;
    case HlcbPacketType::kEffectSetValueVector:
    case HlcbPacketType::kEffectSetValueVectorArray:
      return 16;
    case HlcbPacketType::kEffectSetValueString:
      return 2;
    case HlcbPacketType::kEffectSetValueTexture:
      return 0;
    default:
      return UINT32_MAX;
  }
}

bool HlcbDecoder::Walk(const uint8_t* data, uint32_t size, HlcbSink* sink) {
  if (!data || size < 4) {
    return true;
  }
  // Counted per buffer, for the report at the end of the walk.
  std::map<uint32_t, uint32_t> seen;

  size_t offset = 0;
  while (offset + 4 <= size) {
    const uint32_t header = ReadWord(data, offset);
    const uint32_t type = PacketType(header);
    // The sender wrote `pComPtr & 0x00FFFFFF`, so the top byte of the handle
    // was truncated into the packet type. Every resource descriptor lives in
    // one 16 MB guest arena precisely so that byte is a constant and can be put
    // back here - once, for every packet, rather than in each sink.
    const uint32_t handle = XnaGuestResolveHandle(PacketHandle(header));

    if (!IsKnownType(type)) {
      XELOGE(
          "[xna] HLCB desync at byte {} of {}: packet type {} is not a "
          "command. "
          "Some packet before this one was decoded at the wrong size",
          offset, size, type);
      // The buffer itself is the evidence: the header words are readable in the
      // dump, so the packet whose size is wrong can be identified from it
      // without another run.
      std::string dump;
      for (size_t i = 0; i < size; i += 4) {
        if (i % 32 == 0) {
          dump += "\n    " + std::to_string(i) + ":";
        }
        char word[16];
        std::snprintf(word, sizeof(word), " %08X", ReadWord(data, i));
        dump += word;
      }
      XELOGE("[xna] command buffer was:{}", dump);
      return false;
    }

    uint32_t packet_size = FixedSize(static_cast<HlcbPacketType>(type));
    if (packet_size == 0) {
      // The variable-length packets carry their own byte count. Each is laid
      // out with its length ahead of the payload, so the size is readable
      // before the payload is.
      switch (static_cast<HlcbPacketType>(type)) {
        case HlcbPacketType::kDrawUserPrimitives: {
          // header, primitive type, primitive count, data size, type size,
          // then the vertex data itself.
          if (offset + 20 > size) {
            return false;
          }
          packet_size = 20 + ReadWord(data, offset + 12);
          break;
        }
        case HlcbPacketType::kDrawUserIndexedPrimitives: {
          if (offset + 28 > size) {
            return false;
          }
          const uint32_t vertex_bytes = ReadWord(data, offset + 16);
          const uint32_t index_bytes = ReadWord(data, offset + 20);
          if (vertex_bytes > size - offset || index_bytes > size - offset) {
            XELOGE(
                "[xna] HLCB DrawUserIndexedPrimitives at byte {} claims {} "
                "vertex and {} index byte(s), which does not fit in {}",
                offset, vertex_bytes, index_bytes, size);
            return false;
          }
          packet_size = 28 + vertex_bytes + ((index_bytes + 3) & ~3u);
          break;
        }
        case HlcbPacketType::kDrawSprites: {
          // header, SPRITE COUNT, texture width, texture height, then that many
          // sprite records.
          //
          // The byte count is NOT in the packet: SendDrawSpritesPacket passes
          // it to EnsurePacketSize and then advances currentPacketSize by it,
          // but never writes it. Reading word 3 as a length - which is what
          // desynced the buffer - actually read the texture height, so a 1280 x
          // 720 sprite batch claimed a 720-byte payload.
          //
          // A record is 56 bytes: destination rect, source rect, five words the
          // observed batches leave zero, and a packed colour. Measured off a
          // real buffer, where consecutive SetTexture packets sat exactly 72
          // bytes apart with a count of one - 16 + 56.
          constexpr uint32_t kSpriteRecordBytes = 56;
          if (offset + 16 > size) {
            return false;
          }
          const uint32_t sprites = ReadWord(data, offset + 4);
          if (sprites > (size - offset) / kSpriteRecordBytes) {
            XELOGE(
                "[xna] HLCB DrawSprites at byte {} claims {} sprites, which "
                "does not fit in {}",
                offset, sprites, size);
            return false;
          }
          packet_size = 16 + sprites * kSpriteRecordBytes;
          break;
        }
        case HlcbPacketType::kSetRenderTargets: {
          // The header carries the count, and each target is a handle plus a
          // cube map face. A count that cannot be right means the header was
          // not a header.
          if (handle > 4) {
            XELOGE("[xna] HLCB SetRenderTargets claims {} targets at byte {}",
                   handle, offset);
            return false;
          }
          packet_size = 4 + handle * 8;
          break;
        }
        default: {
          // The EffectSetValue family. The size IS in the packet, contrary to
          // what stopped the walk here before: both senders lay the packet out
          // as header, parameter handle, ELEMENT COUNT, is-array flag, payload,
          // and reserve 16 + elementSize * count bytes. The scalar sender puts
          // its multiplier in that third word and the array sender puts
          // length * multiplier there, so one rule covers the whole family.
          const uint32_t element = EffectValueElementSize(type);
          if (element == UINT32_MAX) {
            XELOGW(
                "[xna] HLCB {} ({}) has no size rule yet - stopping the walk "
                "at byte {} of {}",
                TypeName(type), type, offset, size);
            return false;
          }
          if (offset + 16 > size) {
            XELOGE("[xna] HLCB {} at byte {} has no room for its header",
                   TypeName(type), offset);
            return false;
          }
          const uint32_t count = ReadWord(data, offset + 8);
          // A count that cannot fit the buffer means this was not a header, and
          // multiplying it out would overflow into a plausible-looking size.
          if (element && count > (size - offset) / element) {
            XELOGE(
                "[xna] HLCB {} at byte {} claims {} elements of {} bytes, "
                "which "
                "does not fit in {}",
                TypeName(type), offset, count, element, size);
            return false;
          }
          packet_size = 16 + element * count;
          break;
        }
      }
    }

    if (offset + packet_size > size) {
      XELOGE("[xna] HLCB {} at byte {} runs {} bytes past the end of {}",
             TypeName(type), offset, offset + packet_size - size, size);
      return false;
    }

    const uint8_t* body = data + offset;
    seen[type]++;
    if (sink) {
      Dispatch(static_cast<HlcbPacketType>(type), handle, body, packet_size,
               sink);
    }
    offset += packet_size;
  }

  // WHAT THE TITLE IS ACTUALLY ASKING FOR. A screen that stays black while the
  // loop keeps running is either a title drawing nothing or a title drawing
  // through a path that is counted and discarded - and those need completely
  // different work. Reported for the first few buffers that carry a draw of any
  // kind, because the buffers before the first draw say nothing useful.
  {
    static std::atomic<uint32_t> reported{0};
    bool has_draw = false;
    for (const auto& pair : seen) {
      switch (static_cast<HlcbPacketType>(pair.first)) {
        case HlcbPacketType::kDrawPrimitives:
        case HlcbPacketType::kDrawIndexedPrimitives:
        case HlcbPacketType::kDrawUserPrimitives:
        case HlcbPacketType::kDrawUserIndexedPrimitives:
        case HlcbPacketType::kDrawInstancedPrimitives:
        case HlcbPacketType::kDrawSprites:
          has_draw = true;
          break;
        default:
          break;
      }
    }
    if (has_draw && reported.fetch_add(1) < 4) {
      std::string mix;
      for (const auto& pair : seen) {
        mix += fmt::format(" {}x{}", pair.second, TypeName(pair.first));
      }
      XELOGI("[xna] packet mix in {} byte(s):{}", size, mix);
    }
  }
  return true;
}

void HlcbDecoder::Dispatch(HlcbPacketType type, uint32_t handle,
                           const uint8_t* body, uint32_t size, HlcbSink* sink) {
  switch (type) {
    case HlcbPacketType::kClear: {
      // AddClearPacket writes the options in the header handle, then a Vector4
      // colour, a float depth and an int stencil.
      HlcbClear clear;
      clear.options = handle;
      clear.color[0] = ReadFloat(body, 4);
      clear.color[1] = ReadFloat(body, 8);
      clear.color[2] = ReadFloat(body, 12);
      clear.color[3] = ReadFloat(body, 16);
      clear.depth = ReadFloat(body, 20);
      clear.stencil = static_cast<int32_t>(ReadWord(body, 24));
      sink->Clear(clear);
      break;
    }
    case HlcbPacketType::kSetViewPort: {
      HlcbViewport viewport;
      viewport.x = static_cast<int32_t>(ReadWord(body, 4));
      viewport.y = static_cast<int32_t>(ReadWord(body, 8));
      viewport.width = static_cast<int32_t>(ReadWord(body, 12));
      viewport.height = static_cast<int32_t>(ReadWord(body, 16));
      viewport.min_depth = ReadFloat(body, 20);
      viewport.max_depth = ReadFloat(body, 24);
      sink->SetViewport(viewport);
      break;
    }
    case HlcbPacketType::kSetScissorRect: {
      HlcbRect rect;
      rect.x = static_cast<int32_t>(ReadWord(body, 4));
      rect.y = static_cast<int32_t>(ReadWord(body, 8));
      rect.width = static_cast<int32_t>(ReadWord(body, 12));
      rect.height = static_cast<int32_t>(ReadWord(body, 16));
      sink->SetScissorRect(rect);
      break;
    }
    case HlcbPacketType::kSetStreamSource: {
      HlcbStreamSource stream;
      stream.vertex_buffer = handle;
      stream.stream_index = ReadWord(body, 4);
      stream.vertex_offset = ReadWord(body, 8);
      stream.stride = ReadWord(body, 12);
      stream.instance_frequency = ReadWord(body, 16);
      sink->SetStreamSource(stream);
      break;
    }
    case HlcbPacketType::kSetTexture:
      sink->SetTexture(ReadWord(body, 4), handle);
      break;
    // SendEffectSetTexturePacket writes three dwords: the header carrying the
    // effect handle, then the PARAMETER, then the texture. Known and sized
    // since this decoder was written, but never dispatched - so every texture a
    // title bound through an effect parameter was silently dropped, and only
    // the ones bound by raw sampler slot ever reached the GPU.
    case HlcbPacketType::kEffectSetTexture:
      sink->EffectTexture(handle, ReadWord(body, 4), ReadWord(body, 8));
      break;
    case HlcbPacketType::kEffectSetTechnique:
      sink->EffectTechnique(handle, ReadWord(body, 4));
      break;
    case HlcbPacketType::kSetIndexBuffer:
      sink->SetIndexBuffer(handle);
      break;
    case HlcbPacketType::kSetVertexDeclaration:
      sink->SetVertexDeclaration(handle);
      break;
    case HlcbPacketType::kSetBlendState:
    case HlcbPacketType::kSetDepthStencilState:
    case HlcbPacketType::kSetRasterizerState:
      sink->SetState(type, handle, 0);
      break;
    case HlcbPacketType::kSetSamplerState:
      sink->SetState(type, handle, ReadWord(body, 4));
      break;
    case HlcbPacketType::kSetHighFrequencyState:
      sink->SetHighFrequencyState(PacketHandle(ReadWord(body, 0)),
                                  ReadWord(body, 4));
      break;
    case HlcbPacketType::kDrawPrimitives: {
      HlcbDraw draw;
      draw.primitive_type = ReadWord(body, 4);
      draw.start_vertex = ReadWord(body, 8);
      draw.primitive_count = ReadWord(body, 12);
      sink->Draw(draw);
      break;
    }
    case HlcbPacketType::kDrawIndexedPrimitives: {
      HlcbDraw draw;
      draw.indexed = true;
      draw.primitive_type = ReadWord(body, 4);
      draw.base_vertex = ReadWord(body, 8);
      draw.min_vertex_index = ReadWord(body, 12);
      draw.vertex_count = ReadWord(body, 16);
      draw.start_index = ReadWord(body, 20);
      draw.primitive_count = ReadWord(body, 24);
      sink->Draw(draw);
      break;
    }
    case HlcbPacketType::kDrawUserPrimitives: {
      HlcbDraw draw;
      draw.primitive_type = ReadWord(body, 4);
      draw.primitive_count = ReadWord(body, 8);
      draw.user_data = body + 20;
      draw.user_data_size = ReadWord(body, 12);
      draw.vertex_stride = ReadWord(body, 16);
      sink->Draw(draw);
      break;
    }
    case HlcbPacketType::kDrawUserIndexedPrimitives: {
      HlcbDraw draw;
      draw.indexed = true;
      draw.primitive_type = ReadWord(body, 4);
      draw.vertex_count = ReadWord(body, 8);
      draw.primitive_count = ReadWord(body, 12);
      draw.user_data = body + 28;
      draw.user_data_size = ReadWord(body, 16);
      draw.vertex_stride = ReadWord(body, 24);
      sink->Draw(draw);
      break;
    }
    case HlcbPacketType::kDrawSprites: {
      HlcbSprites sprites;
      // Word 1 is the sprite COUNT, and the texture dimensions follow it. The
      // payload length is implied by the count rather than stated - see the
      // size rule in Walk.
      sprites.texture = handle;
      sprites.count = ReadWord(body, 4);
      sprites.texture_width = static_cast<int32_t>(ReadWord(body, 8));
      sprites.texture_height = static_cast<int32_t>(ReadWord(body, 12));
      sprites.data = body + 16;
      sprites.data_size = size > 16 ? size - 16 : 0;
      sink->DrawSprites(sprites);
      break;
    }
    case HlcbPacketType::kSetRenderTargets: {
      // The header's handle is the count; each target is a handle then a cube
      // map face.
      uint32_t handles[4] = {};
      const uint32_t count = std::min<uint32_t>(handle, 4);
      for (uint32_t i = 0; i < count; ++i) {
        if (4 + i * 8 + 4 > size) {
          break;
        }
        handles[i] = ReadWord(body, 4 + i * 8);
      }
      sink->SetRenderTargets(handles, count);
      break;
    }
    default:
      if (EffectValueElementSize(static_cast<uint32_t>(type)) != UINT32_MAX &&
          size >= 16) {
        sink->EffectValue(type, handle, ReadWord(body, 4), ReadWord(body, 8),
                          body + 16, size - 16);
        break;
      }
      sink->Other(type, handle);
      break;
  }
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
