/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xobject.h"

#include <algorithm>
#include <utility>

#include "xenia/base/byte_stream.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xsymboliclink.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

// Internal storage for an XObject's parent/child relationships. See the
// XChildren commentary in xobject.h for semantics.
//
// Two parallel containers:
//   * owned      - strong refs. Released in ~XObject; cascade-destruction
//                  happens naturally as each object_ref's Release() runs.
//   * dependents - weak (raw) pointers. Notified via OnOwnerDeath() in
//                  ~XObject; the parent does not control their lifetime.
struct XChildren {
  std::vector<object_ref<XObject>> owned;
  std::vector<XObject*> dependents;
};

XObject::XObject(Type type)
    : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type, bool host_object)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false),
      host_object_(host_object) {
  handles_.reserve(10);
  // Handle allocation is now lazy - done in handle() on first access.
}

XObject::~XObject() {
  assert_true(handles_.empty());
  assert_zero(pointer_ref_count_);

  // Detach this node from the parent/child tree before the rest of the
  // destructor runs. Two responsibilities here:
  //   1. Notify our dependents that their owner is dying. Each dependent's
  //      back-pointer is cleared and OnOwnerDeath() is invoked. We must
  //      clear back-pointers BEFORE invoking the hook so that any code in
  //      the hook that calls back into this XObject (e.g., a Disown) sees
  //      a consistent "no longer linked" view.
  //   2. Drop our owned children. Each released object_ref may itself be
  //      the last ref, triggering a recursive ~XObject that repeats this
  //      dance — that's the self-collapsing tree.
  //
  // Both passes are done while temporarily holding the global critical
  // region so that concurrent Adopt / Disown calls (e.g., from another
  // thread reparenting one of our children just as we die) see consistent
  // state. The lock is released before owned children are released so the
  // cascade does not happen with the lock held — that would serialize
  // potentially deep destruction trees against the rest of the kernel.
  std::vector<object_ref<XObject>> owned_to_release;
  std::vector<XObject*> dependents_to_notify;
  if (children_) {
    auto lock = global_critical_region::Acquire();
    for (auto& ch : children_->owned) {
      ch->parent_ = nullptr;
      ch->parent_link_ = ParentLink::kNone;
    }
    for (auto* dep : children_->dependents) {
      dep->parent_ = nullptr;
      dep->parent_link_ = ParentLink::kNone;
    }
    owned_to_release = std::move(children_->owned);
    dependents_to_notify = std::move(children_->dependents);
    children_.reset();
  }
  for (auto* dep : dependents_to_notify) {
    dep->OnOwnerDeath();
  }
  // Cascade: each Release() here may transitively destroy a subtree.
  owned_to_release.clear();

  if (allocated_guest_object_) {
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

void XObject::Adopt(object_ref<XObject> child) {
  assert_not_null(child.get());
  assert_true(child.get() != this);
  auto lock = global_critical_region::Acquire();
  assert_true(child->parent_ == nullptr);
  child->parent_ = this;
  child->parent_link_ = ParentLink::kOwned;
  if (!children_) {
    children_ = std::make_unique<XChildren>();
  }
  children_->owned.emplace_back(std::move(child));
}

void XObject::AddDependent(XObject* child) {
  assert_not_null(child);
  assert_true(child != this);
  auto lock = global_critical_region::Acquire();
  assert_true(child->parent_ == nullptr);
  child->parent_ = this;
  child->parent_link_ = ParentLink::kDependent;
  if (!children_) {
    children_ = std::make_unique<XChildren>();
  }
  children_->dependents.emplace_back(child);
}

void XObject::Disown() {
  // Capture and clear linkage under the lock; if we're an owned child the
  // parent's strong ref is moved out so its destruction (which may delete
  // `this`) happens AFTER the lock is released, avoiding reentrant work
  // while the global critical region is held.
  object_ref<XObject> released_owned_ref;
  {
    auto lock = global_critical_region::Acquire();
    if (!parent_) {
      return;
    }
    XObject* p = parent_;
    ParentLink link = parent_link_;
    parent_ = nullptr;
    parent_link_ = ParentLink::kNone;
    if (!p->children_) {
      // Inconsistent state — parent claims no children but we pointed at
      // it. Treat as already detached.
      return;
    }
    if (link == ParentLink::kOwned) {
      auto& v = p->children_->owned;
      auto it = std::find_if(
          v.begin(), v.end(),
          [this](const object_ref<XObject>& r) { return r.get() == this; });
      if (it != v.end()) {
        released_owned_ref = std::move(*it);
        v.erase(it);
      }
    } else if (link == ParentLink::kDependent) {
      auto& v = p->children_->dependents;
      auto it = std::find(v.begin(), v.end(), this);
      if (it != v.end()) {
        v.erase(it);
      }
    }
  }
  // released_owned_ref destructs here, outside the lock. If `this` was
  // kept alive only by the parent, this is the call that deletes it —
  // which means we must not touch any member of `this` past this point.
}

Emulator* XObject::emulator() const { return kernel_state_->emulator_; }
KernelState* XObject::kernel_state() const { return kernel_state_; }
Memory* XObject::memory() const { return kernel_state_->memory(); }

XObject::Type XObject::type() const { return type_; }

X_HANDLE XObject::handle() {
  if (handles_.empty()) {
    // Lazy allocation - add handle to table on first access.
    if (kernel_state_) {
      kernel_state_->object_table()->AddHandle(this, nullptr);
    }
  }
  return handles_.empty() ? X_INVALID_HANDLE_VALUE : handles_[0];
}

void XObject::RetainHandle() {
  if (!handles_.empty()) {
    kernel_state_->object_table()->RetainHandle(handles_[0]);
  }
}

bool XObject::ReleaseHandle() {
  if (handles_.empty()) {
    return false;
  }
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) ==
         X_STATUS_SUCCESS;
}

void XObject::Retain() { ++pointer_ref_count_; }

void XObject::Release() {
  if (--pointer_ref_count_ == 0) {
    delete this;
  }
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    if (handles_.empty()) {
      // No handle was ever allocated - nothing to remove.
      return X_STATUS_SUCCESS;
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  if (!handles_.empty()) {
    stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));
  }

  return true;
}

bool XObject::RestoreObject(ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  auto handle_count = stream->Read<uint32_t>();
  if (handle_count > 0) {
    handles_.resize(handle_count);
    stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

    // Restore our pointer to our handles in the object table.
    for (size_t i = 0; i < handles_.size(); i++) {
      kernel_state_->object_table()->RestoreHandle(handles_[i], this);
    }
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(), xe::load_and_swap<uint32_t>(
                    memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    // Use handle() to ensure handle is allocated before adding name mapping.
    kernel_state_->object_table()->AddNameMapping(name_, handle());
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // NetDll_WSAWaitForMultipleEvents provides timeout in form of MS.
    return (uint32_t)timeout_ticks;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode,
                       uint32_t alertable, uint64_t* opt_timeout) {
  auto wait_handle = GetWaitHandle();
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  auto result =
      xe::threading::Wait(wait_handle, alertable ? true : false, timeout_ms);
  switch (result) {
    case xe::threading::WaitResult::kSuccess:
      WaitCallback();
      return X_STATUS_SUCCESS;
    case xe::threading::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case xe::threading::WaitResult::kTimeout:
      xe::threading::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case xe::threading::WaitResult::kAbandoned:
    case xe::threading::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object,
                                uint32_t wait_reason, uint32_t processor_mode,
                                uint32_t alertable, uint64_t* opt_timeout) {
  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  auto result = xe::threading::SignalAndWait(
      signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
      alertable ? true : false, timeout_ms);
  switch (result) {
    case xe::threading::WaitResult::kSuccess:
      wait_object->WaitCallback();
      return X_STATUS_SUCCESS;
    case xe::threading::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case xe::threading::WaitResult::kTimeout:
      xe::threading::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case xe::threading::WaitResult::kAbandoned:
    case xe::threading::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects,
                               uint32_t wait_type, uint32_t wait_reason,
                               uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  xe::threading::WaitHandle* wait_handles[64];

  for (size_t i = 0; i < count; ++i) {
    wait_handles[i] = objects[i]->GetWaitHandle();
    assert_not_null(wait_handles[i]);
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  if (wait_type) {
    auto result = xe::threading::WaitAny(wait_handles, count,
                                         alertable ? true : false, timeout_ms);
    switch (result.first) {
      case xe::threading::WaitResult::kSuccess:
        objects[result.second]->WaitCallback();

        return X_STATUS(result.second);
      case xe::threading::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case xe::threading::WaitResult::kTimeout:
        xe::threading::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case xe::threading::WaitResult::kAbandoned:
        return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
      case xe::threading::WaitResult::kFailed:
        return X_STATUS_UNSUCCESSFUL;
    }
  } else {
    auto result = xe::threading::WaitAll(wait_handles, count,
                                         alertable ? true : false, timeout_ms);
    switch (result) {
      case xe::threading::WaitResult::kSuccess:
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
        }

        return X_STATUS_SUCCESS;
      case xe::threading::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case xe::threading::WaitResult::kTimeout:
        xe::threading::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case xe::threading::WaitResult::kAbandoned:
      case xe::threading::WaitResult::kFailed:
        return X_STATUS_ABANDONED_WAIT_0;
    }
  }
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header =
      kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list.blink_ptr & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state,
                                             void* native_ptr, int32_t as_type,
                                             bool already_locked) {
  assert_not_null(native_ptr);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list.flink_ptr to a magic value. When set,
  // wait_list.blink_ptr will hold a handle to our object.
  if (!already_locked) {
    global_critical_region::mutex().lock();
  }

  XObject* result;

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);
  if (as_type == -1) {
    as_type = header->type;
  }

  if (header->wait_list.flink_ptr == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    uint32_t handle = header->wait_list.blink_ptr;
    result = kernel_state->object_table()
                 ->LookupObject<XObject>(handle, true)
                 .release();
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    XObject* object = nullptr;
    switch (as_type) {
      case 0:  // EventNotificationObject
      case 1:  // EventSynchronizationObject
      {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        object = ev;
      } break;
      case 2:  // MutantObject
      {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        object = mutant;
      } break;
      case 5:  // SemaphoreObject
      {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        object = sem;
      } break;
      case 3:   // ProcessObject
      case 4:   // QueueObject
      case 6:   // ThreadObject
      case 7:   // GateObject
      case 8:   // TimerNotificationObject
      case 9:   // TimerSynchronizationObject
      case 18:  // ApcObject
      case 19:  // DpcObject
      case 20:  // DeviceQueueObject
      case 21:  // EventPairObject
      case 22:  // InterruptObject
      case 23:  // ProfileObject
      case 24:  // ThreadedDpcObject
      default:
        assert_always();
        result = nullptr;
    }
    // Stash pointer in struct.
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    if (object) {
      StashHandle(header, object->handle());
    }
    result = object;
  }

  if (!already_locked) {
    global_critical_region::mutex().unlock();
  }
  return object_ref<XObject>(result);
}

}  // namespace kernel
}  // namespace xe
