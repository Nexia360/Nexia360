/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2020 Ben Van...
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_OBJECT_TABLE_H_
#define XENIA_KERNEL_UTIL_OBJECT_TABLE_H_

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/base/string_key.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
class ByteStream;
}  // namespace xe

namespace xe {
namespace kernel {
namespace util {

class ObjectTable {
 public:
  ObjectTable();
  ~ObjectTable();

  void Reset();

  X_STATUS AddHandle(XObject* object, X_HANDLE* out_handle);
  X_STATUS DuplicateHandle(X_HANDLE orig, X_HANDLE* out_handle);
  X_STATUS RetainHandle(X_HANDLE handle);
  X_STATUS ReleaseHandle(X_HANDLE handle);
  X_STATUS ReleaseHandleInLock(X_HANDLE handle);
  X_STATUS RemoveHandle(X_HANDLE handle);

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  // Restore a specific handle->object binding (used for save/restore).
  X_STATUS RestoreHandle(X_HANDLE handle, XObject* object);

  // ---------- Lookup / enumeration ----------
  template <typename T>
  object_ref<T> LookupObject(X_HANDLE handle, bool already_locked = false) {
    auto object = LookupObject(handle, already_locked);
    if (object) {
      assert_true(object->type() == T::kObjectType);
    }
    return object_ref<T>(reinterpret_cast<T*>(object));
  }

  template <typename T>
  std::vector<object_ref<T>> GetObjectsByType(XObject::Type type) {
    std::vector<object_ref<T>> results;
    GetObjectsByType(
        type, reinterpret_cast<std::vector<object_ref<XObject>>*>(&results));
    return results;
  }

  template <typename T>
  std::vector<object_ref<T>> GetObjectsByType() {
    std::vector<object_ref<T>> results;
    GetObjectsByType(
        T::kObjectType,
        reinterpret_cast<std::vector<object_ref<XObject>>*>(&results));
    return results;
  }

  std::vector<object_ref<XObject>> GetAllObjects();
  void PurgeAllObjects();

  // ---------- Name table ----------
  X_STATUS AddNameMapping(const std::string_view name, X_HANDLE handle);
  void RemoveNameMapping(const std::string_view name);
  X_STATUS GetObjectByName(const std::string_view name, X_HANDLE* out_handle);

  // ---------- Pinned / pooled handle helpers ----------
  // Prevents Remove/Release from freeing the slot; allows rebind into same
  // logical handle without racing with the recycler.
  void SetPinned(X_HANDLE handle, bool pinned);
  bool IsPinned(X_HANDLE handle) const;

  // Atomically rebind a *pinned* slot to a new object, keeping the same handle.
  // Returns X_STATUS_INVALID_HANDLE if the slot isn't pinned / out of range.
  X_STATUS RebindPinned(X_HANDLE handle, XObject* object);

 private:
  struct ObjectTableEntry {
    uint16_t handle_ref_count = 0;
    XObject* object = nullptr;
    bool in_use = false;
  };

  // Lookup helpers
  ObjectTableEntry* LookupTable(X_HANDLE handle);
  ObjectTableEntry* LookupTableInLock(X_HANDLE handle);
  XObject* LookupObject(X_HANDLE handle, bool already_locked);
  void GetObjectsByType(XObject::Type type,
                        std::vector<object_ref<XObject>>* results);

  // Handle helpers
  X_HANDLE TranslateHandle(X_HANDLE handle) const;

  // NOTE: Handles encode slot<<2 plus a base. (Slots are 4-byte aligned.)
  static constexpr uint32_t GetHandleSlot(X_HANDLE handle, bool host) {
    uint32_t h = static_cast<uint32_t>(handle);
    h &= host ? ~XObject::kHandleHostBase : ~XObject::kHandleBase;
    return h >> 2;
  }

  X_STATUS FindFreeSlot(uint32_t* out_slot, bool host);
  bool Resize(uint32_t new_capacity, bool host);

  // Recycle FIFO (with hysteresis).
  bool TryPopRecycledSlot(bool host, uint32_t& out_slot);
  void PushRecycledSlot(bool host, uint32_t slot);

  // State
  xe::global_critical_region global_critical_region_;

  uint32_t table_capacity_ = 0;       // guest-visible table
  uint32_t host_table_capacity_ = 0;  // host-only table

  ObjectTableEntry* table_ = nullptr;
  ObjectTableEntry* host_table_ = nullptr;

  // Circular scan cursors
  uint32_t last_free_entry_ = 0;
  uint32_t last_free_host_entry_ = 0;

  // Recycle FIFOs (guest/host) and enable flags.
  std::deque<uint32_t> recycle_fifo_guest_;
  std::deque<uint32_t> recycle_fifo_host_;
  bool recycle_enabled_guest_ = false;
  bool recycle_enabled_host_ = false;

  // Pinned slot bitmaps (0/1). Size tracks capacity.
  std::vector<uint8_t> pinned_guest_;
  std::vector<uint8_t> pinned_host_;

  // Name map
  std::unordered_map<string_key_case, X_HANDLE> name_table_;
};

// Generic lookup specialization (non-template overload)
template <>
object_ref<XObject> ObjectTable::LookupObject<XObject>(X_HANDLE handle,
                                                       bool already_locked);

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_OBJECT_TABLE_H_
