/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_EXPORTS_H_
#define XENIA_KERNEL_XNA_XNA_EXPORTS_H_

#include <cstdint>
#include <string>

namespace xe {
namespace kernel {
namespace xna {

// Records an entry point a title called that has no implementation yet, and
// returns. Reported once per entry point, so a per-frame call does not fill
// the log.
void XnaExportUnimplemented(const char* entry);

// Records an entry point that ANSWERED - with success and zeroed out
// parameters - rather than refusing. Kept apart from the above so a run says
// which defaults a title actually relied on, since a wrong default is far
// harder to spot than a refusal.
void XnaExportDefaulted(const char* entry);

// A resource handle for the generated shims. Shares the table and the 24-bit
// limit with the graphics resources - see xna_exports_gpu.cc.
uint32_t XnaAllocateHandle();

// One mip level of a texture the title has uploaded, as it was given to us:
// linear, in the format the texture was created with. It is NOT tiled - the
// console runtime hands SetData linear pixels and lets the GPU tile them, and
// since a hosted title never reaches the guest GPU, linear is exactly what a
// host texture wants.
struct XnaTextureView {
  const uint8_t* data = nullptr;
  uint32_t size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // XNA SurfaceFormat.
  uint32_t format = 0;
  // Changes whenever the title writes to the texture, so an uploaded copy can
  // tell it has gone stale.
  uint32_t version = 0;
};

// Looks a texture up for the presenter. False if the handle is not a texture or
// the level was never filled.
bool XnaLookupTexture(uint32_t handle, uint32_t level, XnaTextureView* out);

// The same, for one face of a cube map. A cube's six faces each carry the whole
// mip chain and they share one guest allocation, so the mip number alone does
// not name a face's pixels - TextureCube_CreateHandle stores them at
// `face * levels + level`. Face 0 of a 2D texture is the texture itself, which
// is why XnaLookupTexture is this with face 0.
bool XnaLookupTextureFace(uint32_t handle, uint32_t face, uint32_t level,
                          XnaTextureView* out);

// Frames presented, and command packets accepted - reported alongside the
// entry-point usage so a run says whether the title is actually drawing.
void CountPresent();
void CountPackets(uint32_t size);

// Draws that bypass the packet stream: SpriteBatch and DrawUserPrimitives hand
// their geometry over directly, so a run that reports no packets can still be
// drawing plenty.
void CountSprites(uint32_t count);
void CountUserPrimitives(uint32_t count);

// Every console entry point reached so far and how often, most-used first.
// This is a far better guide to what to implement next than the export list:
// it says what a real title actually asks for.
std::string DescribeXnaExportUsage();

// What the command stream actually did: clears, draws, primitives, and the
// state it left behind. Reported alongside the entry-point usage, because a run
// that reaches Present having issued no draws is a very different problem from
// one that draws and shows nothing.
std::string DescribeXnaDeviceState();

// Where a shader's microcode was first seen, by the ucode hash a draw carries.
// The hash is the only identity a draw has, and on its own it says nothing -
// this turns it back into the effect, pass and stage it came from.
std::string HostedShaderOriginFor(uint64_t hash);

// The full path of the title executable on the host, recorded when the title
// is hosted. The console reports this through STORAGE_GetStorageLocation, and
// XNA takes the DIRECTORY of it to resolve every relative content path - so it
// has to be the .exe itself, not the folder it sits in.
void SetXnaTitlePath(const std::string& path);
std::string XnaTitlePath();

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_EXPORTS_H_
