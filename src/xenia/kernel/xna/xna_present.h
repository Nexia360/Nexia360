/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_PRESENT_H_
#define XENIA_KERNEL_XNA_XNA_PRESENT_H_

#include <cstddef>
#include <cstdint>

namespace xe {
namespace kernel {
namespace xna {

// Putting a hosted title's output on the screen.
//
// Everything up to here has been about understanding the command stream; this
// is where it finally becomes an image. A hosted title never touches the guest
// GPU - it hands Nexia an HLCB buffer - so nothing in the emulator was ever
// asked to produce a frame for it, and Present only counted calls. That is why
// the window stayed empty no matter how much of the stream decoded.
//
// The first thing to reach the screen is the clear colour, because it settles
// the whole chain end to end - packet decode, a real D3D12 command list, and
// the presenter's guest output - with the smallest amount that can be wrong.
// Draws build on exactly this path.

// The colour the title last asked to clear to, taken from the packet stream.
void XnaSetClearColor(const float rgba[4]);

// A DrawSprites packet, queued until the frame is presented.
//
// Each record is 56 bytes and the layout was measured off a real command
// buffer: a source rectangle in texture pixels, a destination rectangle in
// screen pixels, then rotation, origin x and y, depth, effects, and a packed
// colour. The full-screen sprite that opens a frame reads (0,0,1280,720) in
// both rectangles, which is what identified the pair.
// `texture_width` and `texture_height` come from the packet and are what the
// source rectangles are relative to.
void XnaQueueSprites(uint32_t texture_handle, uint32_t texture_width,
                     uint32_t texture_height, uint32_t count,
                     const uint8_t* records, uint32_t bytes);

// The resolved geometry for this frame, drawn under the sprites so the HUD
// composites over the scene instead of one replacing the other. Nothing is
// copied: the sprite pass samples the command processor's own resolved texture
// on the same queue, so ordering follows from submission order.
void XnaSetBackgroundFromSwapTexture();

// Called from D3D_Device_Present. Refreshes the presenter's guest output, so
// the window shows what the title asked for rather than nothing.
void XnaPresentFrame();

// Releases the D3D12 objects this holds. Safe to call without a frame.
void XnaShutdownPresent();

uint32_t XnaTextureSwapUnit(uint32_t surface_format);
void XnaCopyTextureRow(uint8_t* destination, const uint8_t* source,
                       size_t bytes, uint32_t unit);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_PRESENT_H_
