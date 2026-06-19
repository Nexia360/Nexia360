/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/d3d12_zpd_query_pool.h"

#include <algorithm>
#include <utility>

#include "xenia/base/logging.h"
#include "xenia/gpu/d3d12/deferred_command_list.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/d3d12/d3d12_util.h"

namespace xe {
namespace gpu {
namespace d3d12 {

bool D3D12ZPDQueryPool::EnsureInitialized(
    const ui::d3d12::D3D12Provider& provider, uint32_t capacity) {
  if (is_initialized()) {
    return true;
  }
  if (!capacity) {
    return false;
  }
  ID3D12Device* device = provider.GetDevice();

  D3D12_QUERY_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  heap_desc.Count = capacity;
  heap_desc.NodeMask = 0;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> query_heap;
  if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&query_heap)))) {
    XELOGE("D3D12ZPDQueryPool: failed to create the occlusion query heap");
    return false;
  }

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      buffer_desc, uint64_t(capacity) * sizeof(uint64_t),
      D3D12_RESOURCE_FLAG_NONE);
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_buffer;
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&readback_buffer)))) {
    XELOGE("D3D12ZPDQueryPool: failed to create the query readback buffer");
    return false;
  }
  void* mapping = nullptr;
  D3D12_RANGE read_range = {0, size_t(capacity) * sizeof(uint64_t)};
  if (FAILED(readback_buffer->Map(0, &read_range, &mapping))) {
    XELOGE("D3D12ZPDQueryPool: failed to map the query readback buffer");
    return false;
  }

  query_heap_ = std::move(query_heap);
  readback_buffer_ = std::move(readback_buffer);
  readback_mapping_ = static_cast<uint64_t*>(mapping);
  capacity_ = capacity;

  free_indices_.resize(capacity);
  for (uint32_t i = 0; i < capacity; ++i) {
    free_indices_[i] = capacity - 1 - i;
  }
  index_generations_.assign(capacity, 0);
  resolve_batch_pending_.assign(capacity, 0);
  resolve_batch_indices_.clear();
  return true;
}

void D3D12ZPDQueryPool::Shutdown() {
  if (readback_buffer_ && readback_mapping_) {
    D3D12_RANGE write_range = {0, 0};
    readback_buffer_->Unmap(0, &write_range);
  }
  readback_mapping_ = nullptr;
  readback_buffer_.Reset();
  query_heap_.Reset();
  capacity_ = 0;
  free_indices_.clear();
  index_generations_.clear();
  resolve_batch_pending_.clear();
  resolve_batch_indices_.clear();
  resolve_batch_ranges_.clear();
}

bool D3D12ZPDQueryPool::AcquireQueryIndex(uint32_t& query_index,
                                          uint32_t& query_generation) {
  if (free_indices_.empty()) {
    return false;
  }
  uint32_t index = free_indices_.back();
  free_indices_.pop_back();
  query_index = index;
  query_generation = ++index_generations_[index];
  return true;
}

void D3D12ZPDQueryPool::ReleaseQueryIndex(uint32_t query_index,
                                          uint32_t query_generation) {
  if (query_index >= capacity_ ||
      index_generations_[query_index] != query_generation) {
    return;
  }
  free_indices_.push_back(query_index);
}

bool D3D12ZPDQueryPool::GenerationMatches(uint32_t query_index,
                                          uint32_t query_generation) const {
  return query_index < capacity_ &&
         index_generations_[query_index] == query_generation;
}

void D3D12ZPDQueryPool::BeginQuery(DeferredCommandList& command_list,
                                   uint32_t query_index) const {
  command_list.D3DBeginQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                             query_index);
}

void D3D12ZPDQueryPool::EndQuery(DeferredCommandList& command_list,
                                 uint32_t query_index) const {
  command_list.D3DEndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                           query_index);
}

void D3D12ZPDQueryPool::QueueQueryResolve(uint32_t query_index) {
  if (query_index >= capacity_ || resolve_batch_pending_[query_index]) {
    return;
  }
  resolve_batch_pending_[query_index] = 1;
  resolve_batch_indices_.push_back(query_index);
}

void D3D12ZPDQueryPool::FlushResolveBatch(DeferredCommandList& command_list) {
  if (resolve_batch_indices_.empty()) {
    return;
  }
  std::sort(resolve_batch_indices_.begin(), resolve_batch_indices_.end());

  resolve_batch_ranges_.clear();
  uint32_t run_start = resolve_batch_indices_.front();
  uint32_t run_length = 1;
  for (size_t i = 1; i < resolve_batch_indices_.size(); ++i) {
    uint32_t index = resolve_batch_indices_[i];
    if (index == run_start + run_length) {
      ++run_length;
      continue;
    }
    resolve_batch_ranges_.push_back({run_start, run_length});
    run_start = index;
    run_length = 1;
  }
  resolve_batch_ranges_.push_back({run_start, run_length});

  for (const ResolveRange& range : resolve_batch_ranges_) {
    command_list.D3DResolveQueryData(
        query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, range.start, range.count,
        readback_buffer_.Get(), uint64_t(range.start) * sizeof(uint64_t));
  }

  for (uint32_t index : resolve_batch_indices_) {
    resolve_batch_pending_[index] = 0;
  }
  resolve_batch_indices_.clear();
}

uint64_t D3D12ZPDQueryPool::GetQueryReadbackValue(uint32_t query_index) const {
  if (query_index >= capacity_ || !readback_mapping_) {
    return 0;
  }
  return readback_mapping_[query_index];
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
