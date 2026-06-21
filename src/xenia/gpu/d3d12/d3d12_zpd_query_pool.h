/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_D3D12_D3D12_ZPD_QUERY_POOL_H_
#define XENIA_GPU_D3D12_D3D12_ZPD_QUERY_POOL_H_

#include <cstdint>
#include <vector>

#include "xenia/gpu/command_processor.h"
#include "xenia/ui/d3d12/d3d12_api.h"

namespace xe {
namespace ui {
namespace d3d12 {
class D3D12Provider;
}
}  // namespace ui

namespace gpu {
namespace d3d12 {

class DeferredCommandList;

// Occlusion query heap plus a persistently mapped readback buffer. BeginQuery
// and EndQuery must land in the same command list, so segments are closed at
// submission boundaries; ResolveQueryData copies into the readback buffer and
// the result is read once the owning submission's fence has signaled.
class D3D12ZPDQueryPool {
 public:
  D3D12ZPDQueryPool() = default;
  D3D12ZPDQueryPool(const D3D12ZPDQueryPool&) = delete;
  D3D12ZPDQueryPool& operator=(const D3D12ZPDQueryPool&) = delete;
  ~D3D12ZPDQueryPool() { Shutdown(); }

  bool EnsureInitialized(const ui::d3d12::D3D12Provider& provider,
                         uint32_t capacity);
  void Shutdown();

  bool is_initialized() const {
    return query_heap_ && readback_buffer_ && readback_mapping_ && capacity_;
  }
  uint32_t capacity() const { return capacity_; }
  bool has_free_indices() const { return !free_indices_.empty(); }
  bool has_pending_resolve_batch() const {
    return !resolve_batch_indices_.empty();
  }

  bool AcquireQueryIndex(uint32_t& query_index, uint32_t& query_generation);
  void ReleaseQueryIndex(uint32_t query_index, uint32_t query_generation);
  bool GenerationMatches(uint32_t query_index, uint32_t query_generation) const;

  void BeginQuery(DeferredCommandList& command_list,
                  uint32_t query_index) const;
  void EndQuery(DeferredCommandList& command_list, uint32_t query_index) const;
  void QueueQueryResolve(uint32_t query_index);
  void FlushResolveBatch(DeferredCommandList& command_list);

  uint64_t GetQueryReadbackValue(uint32_t query_index) const;

 private:
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> query_heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_buffer_;
  uint64_t* readback_mapping_ = nullptr;
  uint32_t capacity_ = 0;

  std::vector<uint32_t> free_indices_;
  // Bumped on each acquire so a stale readback from a recycled slot is dropped.
  std::vector<uint32_t> index_generations_;

  std::vector<uint8_t> resolve_batch_pending_;
  std::vector<uint32_t> resolve_batch_indices_;
  std::vector<ResolveRange> resolve_batch_ranges_;
};

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_D3D12_D3D12_ZPD_QUERY_POOL_H_
