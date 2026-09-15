/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_HOST_PAYLOAD_H_
#define XENIA_KERNEL_XNA_XNA_HOST_PAYLOAD_H_

#include <cstddef>
#include <cstdint>

namespace xe {
namespace kernel {
namespace xna {

struct XnaHostPayloadFile {
  const char* name;
  const uint8_t* data;
  size_t size;
  uint32_t resource_id;
};

extern const char* const kXnaHostPayloadVersion;
extern const XnaHostPayloadFile kXnaHostPayload[];
extern const size_t kXnaHostPayloadCount;

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_HOST_PAYLOAD_H_
