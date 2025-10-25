/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/kernel/util/object_table.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <typeinfo>

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace util {

// Fixed-size pools sized for common titles; tune if needed.
static constexpr uint32_t kInitGuestCap = 65536;  // guest-visible handles
static constexpr uint32_t kInitHostCap  = 4096;   // host-only objects
static constexpr uint32_t kHostReservedSlot0 = 0; // never use host slot 0

// Recycle FIFO hysteresis (enable using recycled slots at >= HIGH,
// keep using until size drops to <= LOW)
static constexpr size_t kRecycleHighWatermark = 16;
static constexpr size_t kRecycleLowWatermark  = 8;

ObjectTable::ObjectTable() {
  auto global_lock = global_critical_region_.Acquire();

  table_capacity_ = kInitGuestCap;
  host_table_capacity_ = kInitHostCap;

  table_ = reinterpret_cast<ObjectTableEntry*>(
      std::calloc(table_capacity_, sizeof(ObjectTableEntry)));
  host_table_ = reinterpret_cast<ObjectTableEntry*>(
      std::calloc(host_table_capacity_, sizeof(ObjectTableEntry)));

  pinned_guest_.assign(table_capacity_, 0);
  pinned_host_.assign(host_table_capacity_, 0);

  last_free_entry_ = 0;
  last_free_host_entry_ = 1;  // skip host slot 0

  recycle_enabled_guest_ = false;
  recycle_enabled_host_ = false;

  recycle_fifo_guest_.clear();
  recycle_fifo_host_.clear();

  XELOGI("ObjectTable: preallocated guest={} host={}",
         table_capacity_, host_table_capacity_);
}

ObjectTable::~ObjectTable() { Reset(); }

void ObjectTable::Reset() {
  auto global_lock = global_critical_region_.Acquire();

  for (uint32_t n = 0; n < table_capacity_; ++n) {
    auto& e = table_[n];
    if (e.object) {
      e.object->Release();
      e.object = nullptr;
    }
    e.handle_ref_count = 0;
    e.in_use = false;
  }
  for (uint32_t n = 0; n < host_table_capacity_; ++n) {
    auto& e = host_table_[n];
    if (e.object) {
      e.object->Release();
      e.object = nullptr;
    }
    e.handle_ref_count = 0;
    e.in_use = false;
  }

  table_capacity_ = 0;
  host_table_capacity_ = 0;
  last_free_entry_ = 0;
  last_free_host_entry_ = 0;

  std::free(table_);
  table_ = nullptr;
  std::free(host_table_);
  host_table_ = nullptr;

  recycle_fifo_guest_.clear();
  recycle_fifo_host_.clear();
  recycle_enabled_guest_ = false;
  recycle_enabled_host_ = false;

  pinned_guest_.clear();
  pinned_host_.clear();
}

// Fixed-size scan: find or fail (no dynamic growth from normal path).
X_STATUS ObjectTable::FindFreeSlot(uint32_t* out_slot, bool host) {
  uint32_t capacity = host ? host_table_capacity_ : table_capacity_;
  if (capacity == 0) {
    return X_STATUS_NO_MEMORY;
  }

  uint32_t start = host ? last_free_host_entry_ : last_free_entry_;
  for (uint32_t scanned = 0; scanned < capacity; ++scanned) {
    uint32_t slot = (start + scanned) % capacity;
    if (host && slot == kHostReservedSlot0) {
      continue;  // never use host slot 0
    }

    // Skip pinned slots (reserved for pooled handles / FIFO).
    if (host) {
      if (slot < pinned_host_.size() && pinned_host_[slot]) continue;
    } else {
      if (slot < pinned_guest_.size() && pinned_guest_[slot]) continue;
    }

    ObjectTableEntry& entry = host ? host_table_[slot] : table_[slot];
    if (!entry.in_use) {
      entry.in_use = true;
      *out_slot = slot;

      if (host) {
        last_free_host_entry_ = (slot + 1) % capacity;
        if (last_free_host_entry_ == kHostReservedSlot0) last_free_host_entry_ = 1;
      } else {
        last_free_entry_ = (slot + 1) % capacity;
      }
      return X_STATUS_SUCCESS;
    }
  }

  return X_STATUS_NO_MEMORY;
}

// Reserved for save/restore paths that may want to reallocate the arrays.
bool ObjectTable::Resize(uint32_t new_capacity, bool host) {
  uint32_t capacity = host ? host_table_capacity_ : table_capacity_;
  uint32_t new_size = new_capacity * sizeof(ObjectTableEntry);
  uint32_t old_size = capacity * sizeof(ObjectTableEntry);

  auto new_table = reinterpret_cast<ObjectTableEntry*>(
      std::realloc(host ? host_table_ : table_, new_size));
  if (!new_table) return false;

  if (new_size > old_size) {
    std::memset(reinterpret_cast<uint8_t*>(new_table) + old_size, 0,
                new_size - old_size);
  }

  if (host) {
    host_table_capacity_ = new_capacity;
    host_table_ = new_table;
    last_free_host_entry_ = (new_capacity > 1) ? 1u : 0u;
    pinned_host_.resize(new_capacity, 0);
  } else {
    table_capacity_ = new_capacity;
    table_ = new_table;
    last_free_entry_ = 0;
    pinned_guest_.resize(new_capacity, 0);
  }

  return true;
}

bool ObjectTable::TryPopRecycledSlot(bool host, uint32_t& out_slot) {
  auto& fifo = host ? recycle_fifo_host_ : recycle_fifo_guest_;
  auto& enabled = host ? recycle_enabled_host_ : recycle_enabled_guest_;
  const auto& pinned = host ? pinned_host_ : pinned_guest_;
  const uint32_t capacity = host ? host_table_capacity_ : table_capacity_;

  // Hysteresis: enable only once we have enough to matter.
  if (!enabled) {
    if (fifo.size() >= kRecycleHighWatermark) {
      enabled = true;
    } else {
      return false;  // not allowed to pop yet
    }
  }

  // Pop from the front until a valid slot is found or the FIFO is empty.
  while (!fifo.empty()) {
    uint32_t slot = fifo.front();
    fifo.pop_front();

    if (host && slot == kHostReservedSlot0) {
      continue;
    }
    if (slot >= capacity) {
      continue;
    }

    if (slot < pinned.size() && pinned[slot]) {
      continue;  // never reuse pinned
    }

    ObjectTableEntry& entry = host ? host_table_[slot] : table_[slot];
    if (!entry.in_use && entry.object == nullptr && entry.handle_ref_count == 0) {
      entry.in_use = true;
      out_slot = slot;

      // If we dropped below the low watermark, disable popping until refilled.
      if (fifo.size() <= kRecycleLowWatermark) {
        enabled = false;
      }
      return true;
    }
    // Skip stale / invalid entries; keep scanning.
  }

  // FIFO drained — disable until we refill to HIGH watermark again.
  enabled = false;
  return false;
}

void ObjectTable::PushRecycledSlot(bool host, uint32_t slot) {
  if (host && slot == kHostReservedSlot0) return;

  auto& fifo = host ? recycle_fifo_host_ : recycle_fifo_guest_;
  fifo.push_back(slot);

  // Do not immediately enable; TryPop will enable when size >= HIGH.
}

X_STATUS ObjectTable::AddHandle(XObject* object, X_HANDLE* out_handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  uint32_t handle = 0;
  {
    auto global_lock = global_critical_region_.Acquire();

    const bool host_object = object->is_host_object();
    uint32_t slot = 0;

    // Try a recycled slot first (subject to FIFO hysteresis).
    if (!TryPopRecycledSlot(host_object, slot)) {
      result = FindFreeSlot(&slot, host_object);
      if (!XSUCCEEDED(result)) {
        return result;
      }
    }

    XELOGI("Adding handle to slot:{:08X}", slot);

    ObjectTableEntry& entry = host_object ? host_table_[slot] : table_[slot];
    entry.object = object;
    entry.handle_ref_count = 1;

    // Build handle: slot<<2 plus base (guest or host).
    handle = (slot << 2);
    if (!host_object) {
      if (object->type() != XObject::Type::Socket) {
        handle += XObject::kHandleBase;       // 0xF8000000
      }
    } else {
      handle += XObject::kHandleHostBase;     // 0x01000000
    }
    object->handles().push_back(handle);

    // Retain while in the table.
    object->Retain();

    XELOGI("Added handle:{:08X} for {}", handle, typeid(*object).name());
  }

  if (out_handle) {
    *out_handle = handle;
  }
  return result;
}

X_STATUS ObjectTable::DuplicateHandle(X_HANDLE handle, X_HANDLE* out_handle) {
  X_STATUS result = X_STATUS_SUCCESS;
  handle = TranslateHandle(handle);

  XObject* object = LookupObject(handle, false);
  if (object) {
    result = AddHandle(object, out_handle);
    object->Release();  // drop ref from LookupObject
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

X_STATUS ObjectTable::RetainHandle(X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();

  ObjectTableEntry* entry = LookupTableInLock(handle);
  if (!entry) return X_STATUS_INVALID_HANDLE;

  entry->handle_ref_count++;
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::ReleaseHandle(X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();
  return ReleaseHandleInLock(handle);
}

X_STATUS ObjectTable::ReleaseHandleInLock(X_HANDLE handle) {
  ObjectTableEntry* entry = LookupTableInLock(handle);
  if (!entry) return X_STATUS_INVALID_HANDLE;

  if (--entry->handle_ref_count == 0) {
    // No more references. Remove it from the table.
    return RemoveHandle(handle);
  }

  return X_STATUS_SUCCESS;
}

void ObjectTable::SetPinned(X_HANDLE handle, bool pinned) {
  auto global_lock = global_critical_region_.Acquire();

  handle = TranslateHandle(handle);
  if (!handle) return;

  const bool is_host = XObject::is_handle_host_object(handle);
  uint32_t slot = GetHandleSlot(handle, is_host);
  if (is_host) {
    if (slot >= host_table_capacity_) return;
    if (slot >= pinned_host_.size()) pinned_host_.resize(slot + 1, 0);
    pinned_host_[slot] = pinned ? 1 : 0;
    // Keep slot reserved while pinned.
    host_table_[slot].in_use = pinned ? true : host_table_[slot].in_use;
  } else {
    if (slot >= table_capacity_) return;
    if (slot >= pinned_guest_.size()) pinned_guest_.resize(slot + 1, 0);
    pinned_guest_[slot] = pinned ? 1 : 0;
    table_[slot].in_use = pinned ? true : table_[slot].in_use;
  }
}

bool ObjectTable::IsPinned(X_HANDLE handle) const {
  X_HANDLE h = TranslateHandle(handle);
  if (!h) return false;
  const bool is_host = XObject::is_handle_host_object(h);
  uint32_t slot = GetHandleSlot(h, is_host);
  if (is_host) {
    return slot < pinned_host_.size() && pinned_host_[slot];
  } else {
    return slot < pinned_guest_.size() && pinned_guest_[slot];
  }
}

X_STATUS ObjectTable::RebindPinned(X_HANDLE handle, XObject* object) {
  auto global_lock = global_critical_region_.Acquire();

  handle = TranslateHandle(handle);
  if (!handle) return X_STATUS_INVALID_HANDLE;

  const bool is_host = XObject::is_handle_host_object(handle);
  uint32_t slot = GetHandleSlot(handle, is_host);
  if (is_host) {
    if (slot >= host_table_capacity_ || !pinned_host_[slot]) {
      return X_STATUS_INVALID_HANDLE;
    }
  } else {
    if (slot >= table_capacity_ || !pinned_guest_[slot]) {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  ObjectTableEntry& entry = is_host ? host_table_[slot] : table_[slot];

  // Drop previous occupant if any (detach without freeing slot).
  if (entry.object) {
    auto it = std::find(entry.object->handles().begin(),
                        entry.object->handles().end(), handle);
    if (it != entry.object->handles().end()) {
      entry.object->handles().erase(it);
    }
    if (!entry.object->name().empty()) {
      RemoveNameMapping(entry.object->name());
    }
    entry.object->Release();
  }

  // Bind new object into the reserved slot.
  entry.object = object;
  entry.handle_ref_count = 1;
  entry.in_use = true;

  object->handles().clear();
  object->handles().push_back(handle);
  object->Retain();

  XELOGI("Rebound pinned handle:{:08X} to {}", handle, typeid(*object).name());
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::RemoveHandle(X_HANDLE handle) {
  handle = TranslateHandle(handle);
  if (!handle) return X_STATUS_INVALID_HANDLE;

  auto global_lock = global_critical_region_.Acquire();

  ObjectTableEntry* entry = LookupTableInLock(handle);
  if (!entry || !entry->in_use) return X_STATUS_INVALID_HANDLE;

  const bool is_host = XObject::is_handle_host_object(handle);
  const uint32_t slot = GetHandleSlot(handle, is_host);

  // If pinned: detach object but keep slot reserved.
  bool pinned = is_host
                    ? (slot < pinned_host_.size() && pinned_host_[slot])
                    : (slot < pinned_guest_.size() && pinned_guest_[slot]);

  XObject* object = entry->object;

  if (pinned) {
    entry->object = nullptr;
    entry->handle_ref_count = 0;
    entry->in_use = true;  // reserved

    if (object) {
      auto it = std::find(object->handles().begin(), object->handles().end(), handle);
      if (it != object->handles().end()) object->handles().erase(it);
      XELOGI("Removed (detached, pinned) handle:{:08X} for {}", handle, typeid(*object).name());
      if (!object->name().empty()) {
        RemoveNameMapping(object->name());
      }
      object->Release();
    }
    return X_STATUS_SUCCESS;
  }

  // Normal removal — free slot.
  entry->object = nullptr;
  entry->handle_ref_count = 0;
  entry->in_use = false;

  if (object) {
    auto it = std::find(object->handles().begin(), object->handles().end(), handle);
    if (it != object->handles().end()) object->handles().erase(it);

    XELOGI("Removed handle:{:08X} for {}", handle, typeid(*object).name());

    if (!object->name().empty()) {
      RemoveNameMapping(object->name());
    }
    object->Release();
  }

  // Recycle slot for fast reuse (avoid pushing anything suspicious).
  if (entry->object == nullptr && entry->handle_ref_count == 0) {
    PushRecycledSlot(is_host, slot);
  }

  return X_STATUS_SUCCESS;
}

std::vector<object_ref<XObject>> ObjectTable::GetAllObjects() {
  auto lock = global_critical_region_.Acquire();
  std::vector<object_ref<XObject>> results;

  for (uint32_t slot = 0; slot < host_table_capacity_; slot++) {
    auto& entry = host_table_[slot];
    if (entry.object && std::find(results.begin(), results.end(),
                                  entry.object) == results.end()) {
      entry.object->Retain();
      results.push_back(object_ref<XObject>(entry.object));
    }
  }
  for (uint32_t slot = 0; slot < table_capacity_; slot++) {
    auto& entry = table_[slot];
    if (entry.object && std::find(results.begin(), results.end(),
                                  entry.object) == results.end()) {
      entry.object->Retain();
      results.push_back(object_ref<XObject>(entry.object));
    }
  }

  return results;
}

void ObjectTable::PurgeAllObjects() {
  auto lock = global_critical_region_.Acquire();

  for (uint32_t slot = 0; slot < table_capacity_; ++slot) {
    auto& e = table_[slot];
    if (e.object) {
      e.handle_ref_count = 0;
      e.object->Release();
      e.object = nullptr;
    }
    e.in_use = (slot < pinned_guest_.size() && pinned_guest_[slot]) ? true : false;
  }
  for (uint32_t slot = 0; slot < host_table_capacity_; ++slot) {
    auto& e = host_table_[slot];
    if (e.object) {
      e.handle_ref_count = 0;
      e.object->Release();
      e.object = nullptr;
    }
    e.in_use = (slot < pinned_host_.size() && pinned_host_[slot]) ? true : false;
  }

  recycle_fifo_guest_.clear();
  recycle_fifo_host_.clear();
  recycle_enabled_guest_ = false;
  recycle_enabled_host_ = false;
}

ObjectTable::ObjectTableEntry* ObjectTable::LookupTable(X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();
  return LookupTableInLock(handle);
}

ObjectTable::ObjectTableEntry* ObjectTable::LookupTableInLock(X_HANDLE handle) {
  handle = TranslateHandle(handle);
  if (!handle) return nullptr;

  const bool is_host_object = XObject::is_handle_host_object(handle);
  uint32_t slot = GetHandleSlot(handle, is_host_object);
  if (is_host_object) {
    if (slot < host_table_capacity_) return &host_table_[slot];
  } else {
    if (slot < table_capacity_) return &table_[slot];
  }
  return nullptr;
}

// Generic lookup specialization
template <>
object_ref<XObject> ObjectTable::LookupObject<XObject>(X_HANDLE handle,
                                                       bool already_locked) {
  auto object = ObjectTable::LookupObject(handle, already_locked);
  return object_ref<XObject>(reinterpret_cast<XObject*>(object));
}

XObject* ObjectTable::LookupObject(X_HANDLE handle, bool already_locked) {
  handle = TranslateHandle(handle);
  if (!handle) return nullptr;

  XObject* object = nullptr;
  if (!already_locked) {
    global_critical_region_.mutex().lock();
  }

  const bool is_host_object = XObject::is_handle_host_object(handle);
  uint32_t slot = GetHandleSlot(handle, is_host_object);

  if (is_host_object) {
    if (slot < host_table_capacity_) {
      ObjectTableEntry& entry = host_table_[slot];
      if (entry.object) object = entry.object;
    }
  } else {
    if (slot < table_capacity_) {
      ObjectTableEntry& entry = table_[slot];
      if (entry.object) object = entry.object;
    }
  }

  if (object) object->Retain();

  if (!already_locked) {
    global_critical_region_.mutex().unlock();
  }

  return object;
}

void ObjectTable::GetObjectsByType(XObject::Type type,
                                   std::vector<object_ref<XObject>>* results) {
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t slot = 0; slot < host_table_capacity_; ++slot) {
    auto& entry = host_table_[slot];
    if (entry.object && entry.object->type() == type) {
      entry.object->Retain();
      results->push_back(object_ref<XObject>(entry.object));
    }
  }
  for (uint32_t slot = 0; slot < table_capacity_; ++slot) {
    auto& entry = table_[slot];
    if (entry.object && entry.object->type() == type) {
      entry.object->Retain();
      results->push_back(object_ref<XObject>(entry.object));
    }
  }
}

X_HANDLE ObjectTable::TranslateHandle(X_HANDLE handle) const {
  // Likely case: not a special handle.
  XE_LIKELY_IF(handle < 0xFFFFFFFE) {
    return handle;
  } else if (handle == 0xFFFFFFFF) {
    return 0;
  } else {
    return XThread::GetCurrentThreadHandle();
  }
}

X_STATUS ObjectTable::AddNameMapping(const std::string_view name,
                                     X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();
  if (name_table_.count(string_key_case(name))) {
    return X_STATUS_OBJECT_NAME_COLLISION;
  }
  name_table_.insert({string_key_case::create(name), handle});
  return X_STATUS_SUCCESS;
}

void ObjectTable::RemoveNameMapping(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = name_table_.find(string_key_case(name));
  if (it != name_table_.end()) {
    name_table_.erase(it);
  }
}

X_STATUS ObjectTable::GetObjectByName(const std::string_view name,
                                      X_HANDLE* out_handle) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = name_table_.find(string_key_case(name));
  if (it == name_table_.end()) {
    *out_handle = X_INVALID_HANDLE_VALUE;
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  *out_handle = it->second;

  // Bump the handle refcount.
  auto obj = LookupObject(it->second, true);
  if (obj) {
    obj->RetainHandle();
    obj->Release();
  }

  return X_STATUS_SUCCESS;
}

bool ObjectTable::Save(ByteStream* stream) {
  stream->Write<uint32_t>(host_table_capacity_);
  for (uint32_t i = 0; i < host_table_capacity_; i++) {
    auto& entry = host_table_[i];
    stream->Write<int32_t>(entry.handle_ref_count);
  }

  stream->Write<uint32_t>(table_capacity_);
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& entry = table_[i];
    stream->Write<int32_t>(entry.handle_ref_count);
  }

  return true;
}

bool ObjectTable::Restore(ByteStream* stream) {
  Resize(stream->Read<uint32_t>(), true);
  for (uint32_t i = 0; i < host_table_capacity_; i++) {
    auto& e = host_table_[i];
    e.handle_ref_count = stream->Read<int32_t>();
    e.object = nullptr;
    e.in_use = (e.handle_ref_count > 0) ||
               (i < pinned_host_.size() && pinned_host_[i]);
  }

  Resize(stream->Read<uint32_t>(), false);
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& e = table_[i];
    e.handle_ref_count = stream->Read<int32_t>();
    e.object = nullptr;
    e.in_use = (e.handle_ref_count > 0) ||
               (i < pinned_guest_.size() && pinned_guest_[i]);
  }

  recycle_fifo_guest_.clear();
  recycle_fifo_host_.clear();
  recycle_enabled_guest_ = false;
  recycle_enabled_host_ = false;

  return true;
}

X_STATUS ObjectTable::RestoreHandle(X_HANDLE handle, XObject* object) {
  const bool is_host_object = XObject::is_handle_host_object(handle);
  uint32_t slot = GetHandleSlot(handle, is_host_object);
  uint32_t capacity = is_host_object ? host_table_capacity_ : table_capacity_;
  assert_true(capacity > slot);

  if (capacity > slot) {
    auto& entry = is_host_object ? host_table_[slot] : table_[slot];
    entry.object = object;
    entry.in_use = true;
    if (entry.handle_ref_count == 0) {
      entry.handle_ref_count = 1;
    }
    object->Retain();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
