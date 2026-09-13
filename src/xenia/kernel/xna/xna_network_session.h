/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_NETWORK_SESSION_H_
#define XENIA_KERNEL_XNA_XNA_NETWORK_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xe {
namespace kernel {
namespace xna {

uint32_t XnaReserveAsyncOperation();
void XnaCompleteAsyncOperation(uint32_t id);

struct XnaSessionRequest {
  uint32_t type = 0;
  uint32_t first_local = 0xFFFFFFFFu;
  uint32_t local_mask = 0;
  uint32_t max_local = 0;
  uint32_t max_gamers = 0;
  uint32_t private_slots = 0;
  std::vector<uint32_t> properties;
};

struct XnaSessionSummary {
  uint32_t handle = 0;
  uint32_t type = 0;
  uint32_t max_gamers = 0;
  uint32_t private_slots = 0;
  std::vector<uint32_t> properties;
};

uint32_t XnaSessionBeginCreate(const XnaSessionRequest& request);
uint32_t XnaSessionEndCreate(uint32_t operation, XnaSessionSummary* out);

uint32_t XnaSessionBeginFind(const XnaSessionRequest& request);
uint32_t XnaSessionEndFind(uint32_t operation, std::vector<uint8_t>* reply);
void XnaSessionDestroyFinder(uint32_t finder);

uint32_t XnaSessionBeginJoin(uint32_t finder, uint32_t index);
uint32_t XnaSessionEndJoin(uint32_t operation, XnaSessionSummary* out);

bool XnaSessionPrepareUpdate(uint32_t handle, uint32_t current_size);
uint32_t XnaSessionUpdate(uint32_t handle, const uint8_t* records,
                          size_t size, uint32_t buffer_total,
                          std::vector<uint8_t>* events, uint32_t* needed);
void XnaSessionDestroy(uint32_t handle);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_NETWORK_SESSION_H_
