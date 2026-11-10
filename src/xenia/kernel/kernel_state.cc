/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <ranges>

#include "xenia/kernel/kernel_state.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/vfs/devices/host_path_device.h"

#include "third_party/crypto/TinySHA1.hpp"

DEFINE_bool(apply_title_update, true, "Apply title updates.", "Kernel");
DEFINE_bool(allow_incompatible_title_update, true,
            "Allow title updates with mismatched signatures to be applied.",
            "Kernel");

DEFINE_uint32(kernel_build_version, 1888, "Define current kernel version",
              "Kernel");

DECLARE_string(cl);

DECLARE_int32(network_mode);

namespace xe {
namespace kernel {

constexpr std::chrono::milliseconds kDeferredOverlappedDelayMillis(25);

// This is a global object initialized with the XboxkrnlModule.
// It references the current kernel state object that all kernel methods should
// be using to stash their variables.
KernelState* shared_kernel_state_ = nullptr;

KernelState* kernel_state() { return shared_kernel_state_; }

KernelState::KernelState(Emulator* emulator)
    : emulator_(emulator),
      memory_(emulator->memory()),
      dispatch_thread_running_(false),
      dpc_list_(emulator->memory()),
      kernel_trampoline_group_(emulator->processor()->backend()) {
  assert_null(shared_kernel_state_);
  shared_kernel_state_ = this;
  processor_ = emulator->processor();
  file_system_ = emulator->file_system();
  xam_state_ = std::make_unique<xam::XamState>(emulator, this);
  smc_ = std::make_unique<SystemManagementController>();
  xconfig_ =
      std::make_unique<XConfig>(emulator->storage_root() / "xconfig.settings");
  nui_ = std::make_unique<NUIController>();

  InitializeKernelGuestGlobals();
  kernel_version_ = KernelVersion(cvars::kernel_build_version);

  auto hc_loc_heap = memory_->LookupHeap(strange_hardcoded_page_);
  bool fixed_alloc_worked = hc_loc_heap->AllocFixed(
      strange_hardcoded_page_, 65536, 0,
      kMemoryAllocationCommit | kMemoryAllocationReserve,
      kMemoryProtectRead | kMemoryProtectWrite);

  xenia_assert(fixed_alloc_worked);

  StartHangWatchdog();
}

KernelState::KernelState(Memory* memory, cpu::Processor* processor,
                         vfs::VirtualFileSystem* file_system)
    : emulator_(nullptr),
      memory_(memory),
      processor_(processor),
      file_system_(file_system),
      dispatch_thread_running_(false),
      dpc_list_(memory),
      kernel_trampoline_group_(processor->backend()) {
  assert_null(shared_kernel_state_);
  shared_kernel_state_ = this;
  InitializeKernelGuestGlobals();
  kernel_version_ = KernelVersion(cvars::kernel_build_version);
}

KernelState::~KernelState() {
  StopHangWatchdog();

  SetExecutableModule(nullptr);

  if (dispatch_thread_running_) {
    dispatch_thread_running_ = false;
    dispatch_cond_.notify_all();
    dispatch_thread_->Wait(0, 0, 0, nullptr);
  }

  executable_module_.reset();
  user_modules_.clear();
  kernel_modules_.clear();

  // Delete all objects.
  object_table_.Reset();

  xam_state_.reset();

  assert_true(shared_kernel_state_ == this);
  shared_kernel_state_ = nullptr;
}

KernelState* KernelState::shared() { return shared_kernel_state_; }

xex2_opt_execution_info* KernelState::GetExecutionInfo() const {
  if (!executable_module_) {
    return 0;
  }

  xex2_opt_execution_info* exec_info = 0;
  executable_module_->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &exec_info);
  return exec_info;
}

uint32_t KernelState::title_id() const {
  // A HOSTED TITLE HAS NO EXECUTABLE MODULE. An XNA title runs its own IL on
  // the host CLR and never loads a XEX, so there is nothing here to read a
  // title id out of - and everything keyed on the title, content and profile
  // storage included, would look up title 0 instead of the real one. The
  // launcher knows it from the package's content header and sets it here.
  if (hosted_title_id_) {
    return hosted_title_id_;
  }
  if (!executable_module_) {
    return 0;
  }

  assert_not_null(executable_module_);

  xex2_opt_execution_info* exec_info = GetExecutionInfo();

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

bool KernelState::is_title_open() const { return emulator_->is_title_open(); }

XLiveAPI* KernelState::GetXboxLiveAPI() const {
  return emulator()->GetXboxLiveAPI();
}

bool KernelState::is_title_system_type(uint32_t title_id) {
  if (!title_id) {
    return true;
  }

  if ((title_id & 0xFF000000) == 0x58000000u) {
    return (title_id & 0xFF0000) != 0x410000;  // if 'X' but not 'XA' (XBLA)
  }

  return (title_id >> 16) == 0xFFFE;
}

XNKEY* KernelState::title_lan_key() const {
  if (!executable_module_) {
    return nullptr;
  }

  xex2_opt_lan_key* opt_lan_key_ptr = 0;
  executable_module_->GetOptHeader(XEX_HEADER_LAN_KEY, &opt_lan_key_ptr);

  return reinterpret_cast<XNKEY*>(opt_lan_key_ptr->key);
}

const std::unique_ptr<xam::SpaInfo> KernelState::title_xdbf() const {
  return module_xdbf(executable_module_);
}

const std::unique_ptr<xam::SpaInfo> KernelState::module_xdbf(
    object_ref<UserModule> exec_module) const {
  assert_not_null(exec_module);

  uint32_t resource_data = 0;
  uint32_t resource_size = 0;
  if (XSUCCEEDED(exec_module->GetSection(
          fmt::format("{:08X}", exec_module->title_id()).c_str(),
          &resource_data, &resource_size))) {
    return std::make_unique<xam::SpaInfo>(std::span<uint8_t>(
        memory()->TranslateVirtual(resource_data), resource_size));
  }

  return nullptr;
}

uint32_t KernelState::AllocateTLS(cpu::ppc::PPCContext* context) {
  auto globals =
      memory()->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = xboxkrnl::xeKeKfAcquireSpinLock(context, tls_lock);

  int result = -1;

  auto current_thread = XThread::GetCurrentThread();
  if (!current_thread) {
    XELOGE("AllocateTLS: No current thread");
    xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
    return X_TLS_OUT_OF_INDEXES;
  }

  auto process_ptr = memory()->TranslateVirtual(
      current_thread->guest_object<X_KTHREAD>()->process);
  if (!process_ptr) {
    XELOGE("AllocateTLS: Failed to translate process pointer");
    xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
    return X_TLS_OUT_OF_INDEXES;
  }

  // Search for a free TLS slot in the process bitmap
  // Bitmap format: 1 = free, 0 = allocated
  // 8 x 32-bit words = 256 total TLS slots
  for (xe::be<uint32_t>* i = &process_ptr->tls_slot_bitmap[0];
       i < &process_ptr->tls_slot_bitmap[8]; ++i) {
    // Read bitmap value (handles big-endian conversion)
    uint32_t bitmap_value = static_cast<uint32_t>(*i);

    // Find highest free slot using lzcnt (leading zero count)
    // Returns 0-31 if a bit is set, 32 if no bits are set
    uint32_t leading_zeros = xe::lzcnt(bitmap_value);

    if (leading_zeros != 32) {
      // Calculate absolute slot index from bitmap position and bit offset
      // Each bitmap word represents 32 slots
      size_t bitmap_index = i - &process_ptr->tls_slot_bitmap[0];
      uint32_t base_slot = static_cast<uint32_t>(bitmap_index) * 32;
      int calculated_slot = base_slot + leading_zeros;

      // Validate slot is within Xbox 360 TLS range
      if (calculated_slot >= 0 && calculated_slot < 256) {
        result = calculated_slot;

        // Clear the bit to mark as allocated
        // lzcnt returns 0 for bit 31, 31 for bit 0
        uint32_t bit_index = 31 - leading_zeros;
        *i = bitmap_value & ~(1U << bit_index);
        break;
      } else {
        XELOGE("AllocateTLS: Invalid slot calculation: {}", calculated_slot);
      }
    }
  }

  if (result == -1) {
    XELOGW("AllocateTLS: All TLS slots exhausted for current process");
  }

  xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
  return static_cast<uint32_t>(result);
}

void KernelState::FreeTLS(cpu::ppc::PPCContext* context, uint32_t slot) {
  if (slot >= 256) {
    XELOGE("FreeTLS: Invalid slot index {}", slot);
    return;
  }

  auto current_thread = XThread::GetCurrentThread();
  if (!current_thread) {
    XELOGE("FreeTLS: No current thread");
    return;
  }

  auto current_kthread = current_thread->guest_object<X_KTHREAD>();
  if (!current_kthread) {
    XELOGE("FreeTLS: Failed to get guest thread object");
    return;
  }

  auto process_ptr = memory()->TranslateVirtual(current_kthread->process);
  if (!process_ptr) {
    XELOGE("FreeTLS: Failed to translate process pointer");
    return;
  }

  auto globals =
      memory()->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = xboxkrnl::xeKeKfAcquireSpinLock(context, tls_lock);

  uint32_t bitmap_index = slot / 32;
  uint32_t bit_mask = 1U << (31 - (slot % 32));
  uint32_t bitmap_value =
      static_cast<uint32_t>(process_ptr->tls_slot_bitmap[bitmap_index]);

  if (bitmap_value & bit_mask) {
    XELOGW("FreeTLS: Slot {} is already free", slot);
    xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
    return;
  }

  // Clear TLS values in all threads of this process
  const std::vector<object_ref<XThread>> threads =
      object_table()->GetObjectsByType<XThread>();

  uint32_t current_process_ptr = current_kthread->process.m_ptr;
  for (const object_ref<XThread>& thread : threads) {
    if (!thread || !thread->is_guest_thread()) {
      continue;
    }

    auto thread_kthread = thread->guest_object<X_KTHREAD>();
    if (thread_kthread &&
        thread_kthread->process.m_ptr == current_process_ptr) {
      thread->SetTLSValue(slot, 0);
    }
  }

  // Mark slot as free in bitmap
  process_ptr->tls_slot_bitmap[bitmap_index] = bitmap_value | bit_mask;

  xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
}

void KernelState::RegisterTitleTerminateNotification(uint32_t routine,
                                                     uint32_t priority) {
  TerminateNotification notify;
  notify.guest_routine = routine;
  notify.priority = priority;

  terminate_notifications_.push_back(notify);
}

void KernelState::RemoveTitleTerminateNotification(uint32_t routine) {
  for (auto it = terminate_notifications_.begin();
       it != terminate_notifications_.end(); it++) {
    if (it->guest_routine == routine) {
      terminate_notifications_.erase(it);
      break;
    }
  }
}

void KernelState::RegisterModule(XModule* module) {}

void KernelState::UnregisterModule(XModule* module) {}

bool KernelState::RegisterUserModule(object_ref<UserModule> module) {
  auto lock = global_critical_region_.Acquire();

  for (auto user_module : user_modules_) {
    if (user_module->path() == module->path()) {
      // Already loaded.
      return false;
    }
  }

  user_modules_.push_back(module);
  return true;
}

void KernelState::UnregisterUserModule(UserModule* module) {
  auto lock = global_critical_region_.Acquire();

  for (auto it = user_modules_.begin(); it != user_modules_.end(); it++) {
    if ((*it)->path() == module->path()) {
      user_modules_.erase(it);
      return;
    }
  }
}

bool KernelState::IsKernelModule(const std::string_view name) {
  if (name.empty()) {
    // Executing module isn't a kernel module.
    return false;
  }
  // NOTE: no global lock required as the kernel module list is static.
  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return true;
    }
  }
  return false;
}

bool KernelState::IsModuleLoaded(const std::string_view name) {
  if (name.empty()) {
    return true;
  }

  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return true;
    }
  }

  auto global_lock = global_critical_region_.Acquire();

  for (auto user_module : user_modules_) {
    if (user_module->Matches(name)) {
      return true;
    }
  }

  return false;
}

object_ref<KernelModule> KernelState::GetKernelModule(
    const std::string_view name) {
  assert_true(IsKernelModule(name));

  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return retain_object(kernel_module.get());
    }
  }

  return nullptr;
}

object_ref<XModule> KernelState::GetModule(const std::string_view name,
                                           bool user_only) {
  if (name.empty()) {
    // NULL name = self.
    // TODO(benvanik): lookup module from caller address.
    return GetExecutableModule();
  } else if (xe::utf8::equal_case(name, "kernel32.dll")) {
    // Some games request this, for some reason. wtf.
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  if (!user_only) {
    for (auto kernel_module : kernel_modules_) {
      if (kernel_module->Matches(name)) {
        return retain_object(kernel_module.get());
      }
    }
  }

  auto path(name);

  // Resolve the path to an absolute path.
  if (name.find_first_of("\\:") != std::string_view::npos) {
    auto entry = file_system_->ResolvePath(name);
    if (entry) {
      path = entry->absolute_path();
    }
  }

  for (auto user_module : user_modules_) {
    if (user_module->Matches(path)) {
      return retain_object(user_module.get());
    }
  }
  return nullptr;
}

object_ref<XThread> KernelState::LaunchModule(object_ref<UserModule> module) {
  if (!module->is_executable()) {
    return nullptr;
  }

  SetExecutableModule(module);
  XELOGI("KernelState: Launching module...");

  // The startup notifications belong to each title, not to the process. When
  // the dashboard launched first it used them up, so a game started from it
  // never got kXNotificationLiveConnectionChanged - MW3 asks for its NAT type
  // only from that handler and reported Strict. A title launched from the dash
  // now gets exactly what it gets when launched directly.
  has_notified_startup_ = false;
  has_notified_live_startup_ = false;
  has_notified_xmp_startup_ = false;

  // Create a thread to run in.
  // We start suspended so we can run the debugger prep.
  auto thread = object_ref<XThread>(
      new XThread(kernel_state(), module->stack_size(), 0,
                  module->entry_point(), 0, X_CREATE_SUSPENDED, true, true));

  // We know this is the 'main thread'.
  thread->set_name("Main XThread");

  X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    XELOGE("Could not create launch thread: {:08X}", result);
    return nullptr;
  }

  // Waits for a debugger client, if desired.
  emulator()->processor()->PreLaunch();

  return thread;
}

object_ref<UserModule> KernelState::GetExecutableModule() {
  if (!executable_module_) {
    return nullptr;
  }
  return executable_module_;
}

void KernelState::SetExecutableModule(object_ref<UserModule> module,
                                      bool start_dispatch_thread) {
  if (module.get() == executable_module_.get()) {
    return;
  }
  executable_module_ = std::move(module);
  if (!executable_module_) {
    return;
  }

  auto title_process =
      memory_->TranslateVirtual<X_KPROCESS*>(GetTitleProcess());

  InitializeProcess(title_process, X_PROCTYPE_TITLE, 10, 13, 17);

  xex2_opt_tls_info* tls_header = nullptr;
  executable_module_->GetOptHeader(XEX_HEADER_TLS_INFO, &tls_header);
  if (tls_header) {
    title_process->tls_static_data_address = tls_header->raw_data_address;
    title_process->tls_data_size = tls_header->data_size;
    title_process->tls_raw_data_size = tls_header->raw_data_size;
    title_process->tls_slot_size = tls_header->slot_count * 4;
    SetProcessTLSVars(title_process, tls_header->slot_count,
                      tls_header->data_size, tls_header->raw_data_address);
  }

  uint32_t kernel_stacksize = 0;

  executable_module_->GetOptHeader(XEX_HEADER_DEFAULT_STACK_SIZE,
                                   &kernel_stacksize);
  if (kernel_stacksize) {
    kernel_stacksize = (kernel_stacksize + 4095) & 0xFFFFF000;
    if (kernel_stacksize < 0x4000) {
      kernel_stacksize = 0x4000;
    }
    title_process->kernel_stack_size = kernel_stacksize;
  }

  // Setup the kernel's XexExecutableModuleHandle field.
  auto export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::XexExecutableModuleHandle);
  if (export_entry) {
    assert_not_zero(export_entry->variable_ptr);
    auto variable_ptr = memory()->TranslateVirtual<xe::be<uint32_t>*>(
        export_entry->variable_ptr);
    *variable_ptr = executable_module_->hmodule_ptr();
  }

  // Setup the kernel's ExLoadedImageName field
  export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::ExLoadedImageName);

  if (export_entry) {
    char* variable_ptr =
        memory()->TranslateVirtual<char*>(export_entry->variable_ptr);
    xe::string_util::copy_truncating(
        variable_ptr, executable_module_->path(),
        xboxkrnl::XboxkrnlModule::kExLoadedImageNameSize);
  }

  // Setup the kernel's ExLoadedCommandLine field
  export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::ExLoadedCommandLine);
  if (export_entry) {
    char* variable_ptr =
        memory()->TranslateVirtual<char*>(export_entry->variable_ptr);

    std::string module_name =
        fmt::format("\"{}.xex\"", executable_module_->name());
    if (!cvars::cl.empty()) {
      module_name += " " + cvars::cl;
    }
    auto xam = GetKernelModule<xam::XamModule>("xam.xex");
    if (xam && !xam->loader_data().command_line.empty()) {
      module_name += " " + xam->loader_data().command_line;
      xam->loader_data().command_line.clear();
    }

    xe::string_util::copy_truncating(
        variable_ptr, module_name,
        xboxkrnl::XboxkrnlModule::kExLoadedCommandLineSize);
  }

  // Initialize file I/O hooks for XMP volume title-specific patches.
  InitXmpVolumePatch();

  // Spin up deferred dispatch worker.
  // TODO(benvanik): move someplace more appropriate (out of ctor, but around
  // here).
  if (start_dispatch_thread && !dispatch_thread_running_) {
    dispatch_thread_running_ = true;
    dispatch_thread_ = object_ref<XHostThread>(new XHostThread(
        this, 128 * 1024, 0,
        [this]() {
          // As we run guest callbacks the debugger must be able to suspend us.
          dispatch_thread_->set_can_debugger_suspend(true);

          auto global_lock = global_critical_region_.AcquireDeferred();
          while (dispatch_thread_running_) {
            global_lock.lock();
            if (dispatch_queue_.empty()) {
              dispatch_cond_.wait(global_lock);
              if (!dispatch_thread_running_) {
                global_lock.unlock();
                break;
              }
            }
            auto fn = std::move(dispatch_queue_.front());
            dispatch_queue_.pop_front();
            global_lock.unlock();

            fn();
          }
          return 0;
        },
        GetSystemProcess()));  // don't think an equivalent exists on real hw
    dispatch_thread_->set_name("Kernel Dispatch");
    dispatch_thread_->Create();
  }
}

void KernelState::LoadKernelModule(object_ref<KernelModule> kernel_module) {
  auto global_lock = global_critical_region_.Acquire();
  kernel_modules_.push_back(std::move(kernel_module));
}

object_ref<UserModule> KernelState::LoadUserModule(
    const std::string_view raw_name, bool call_entry) {
  // Some games try to load relative to launch module, others specify full path.
  auto name = xe::utf8::find_name_from_guest_path(raw_name);
  std::string path(raw_name);
  if (name == raw_name) {
    if (!executable_module_) {
      path = xe::utf8::join_guest_paths(
          xe::utf8::find_base_guest_path((*user_modules_.cbegin())->path()),
          name);
    } else {
      path = xe::utf8::join_guest_paths(
          xe::utf8::find_base_guest_path(executable_module_->path()), name);
    }
  }

  object_ref<UserModule> module;
  {
    auto global_lock = global_critical_region_.Acquire();

    // See if we've already loaded it
    for (auto& existing_module : user_modules_) {
      if (existing_module->Matches(path)) {
        return existing_module;
      }
    }

    global_lock.unlock();

    // Module wasn't loaded, so load it.
    module = object_ref<UserModule>(new UserModule(this));
    X_STATUS status = module->LoadFromFile(path);
    if (XFAILED(status)) {
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    global_lock.lock();

    // Putting into the listing automatically retains.
    user_modules_.push_back(module);
  }
  return module;
}

object_ref<UserModule> KernelState::LoadUserModuleFromMemory(
    const std::string_view raw_name, const void* addr, const size_t length) {
  auto name = xe::utf8::find_base_name_from_guest_path(raw_name);

  object_ref<UserModule> module;
  {
    auto global_lock = global_critical_region_.Acquire();

    // See if we've already loaded it
    for (auto& existing_module : user_modules_) {
      if (existing_module->Matches(name)) {
        return existing_module;
      }
    }

    global_lock.unlock();

    // Module wasn't loaded, so load it.
    module = object_ref<UserModule>(new UserModule(this));
    X_STATUS status = module->LoadFromMemoryNamed(name, addr, length);
    if (XFAILED(status)) {
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    global_lock.lock();

    // Putting into the listing automatically retains.
    user_modules_.push_back(module);
  }
  return module;
}

namespace {

// The end of one function in a Lua 5.1 dump as the console writes it: big
// endian, 4-byte int, size_t and instruction, 8-byte number. Zero if the bytes
// do not parse, so a caller never patches something it did not understand.
size_t SkipLuaFunction(const uint8_t* data, size_t size, size_t at,
                       int depth = 0) {
  auto u32 = [&](size_t& pos, uint32_t& out) {
    if (pos + 4 > size) {
      return false;
    }
    out = xe::load_and_swap<uint32_t>(data + pos);
    pos += 4;
    return true;
  };
  auto skip_string = [&](size_t& pos) {
    uint32_t length = 0;
    if (!u32(pos, length) || pos + length > size) {
      return false;
    }
    pos += length;
    return true;
  };
  uint32_t count = 0;
  if (depth > 64 || !skip_string(at) || at + 12 > size) {
    return 0;
  }
  at += 12;  // linedefined, lastlinedefined, nups, numparams, vararg, stack
  if (!u32(at, count) || at + size_t(count) * 4 > size) {
    return 0;
  }
  at += size_t(count) * 4;  // code
  if (!u32(at, count)) {
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i) {  // constants
    if (at >= size) {
      return 0;
    }
    switch (data[at++]) {
      case 0:  // nil
        break;
      case 1:  // boolean
        at += 1;
        break;
      case 3:  // number
        at += 8;
        break;
      case 4:  // string
        if (!skip_string(at)) {
          return 0;
        }
        break;
      default:
        return 0;
    }
  }
  if (!u32(at, count)) {
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i) {  // nested functions
    at = SkipLuaFunction(data, size, at, depth + 1);
    if (!at) {
      return 0;
    }
  }
  if (!u32(at, count) || at + size_t(count) * 4 > size) {
    return 0;
  }
  at += size_t(count) * 4;  // line info
  if (!u32(at, count)) {
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i) {  // locals
    if (!skip_string(at) || at + 8 > size) {
      return 0;
    }
    at += 8;
  }
  if (!u32(at, count)) {
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i) {  // upvalue names
    if (!skip_string(at)) {
      return 0;
    }
  }
  return at;
}

// My Games draws a game's tile from dataItem.boxart - the scene binds
// MarketplaceSlotImage {img_0=boxart;img_1=thumbnailLargeMediaType} - but the
// 17559 scripts only ever fill boxart from a Quick Play URL, so every other
// game showed the controller icon.
//
// Marketplace.GameLibrary.ConvertToLibraryItem already reads the title's own
// picture: `item.thumbnail = titleInfo.ImagePath`, a path the image control
// opens for itself, which DashboardTitleImageRedirect turns into the box art.
// That one SETTABLE is pointed at "boxart" instead, so the tile holds a path
// that resolves again on every build.
//
// The loader is deliberately left alone. Writing
// titleInfo:GetImageInfo().Path over boxart worked once and then wedged:
// .Path is "memory://<buffer>,<size>", the buffer dies with the scene, and
// GetImageInfo keeps answering from titleinfo+0x42D, so coming back from Game
// Details left every tile blank until My Games was entered afresh.
void PatchDashboardLibraryArt(Memory* memory, cpu::XexModule* xex) {
  const uint32_t base = xex->base_address();
  const uint32_t size = xex->image_size();
  if (!base || !size) {
    return;
  }
  uint8_t* image = memory->TranslateVirtual<uint8_t*>(base);
  const std::string_view source = "@Marketplace.GameLibrary.lua";
  const std::string_view haystack(reinterpret_cast<const char*>(image), size);
  const size_t source_at = haystack.find(source);
  // header (12) and the source string's size_t (4) come before its characters
  if (source_at == std::string_view::npos || source_at < 16) {
    XELOGW("Dashboard: no GameLibrary chunk; My Games art unpatched");
    return;
  }
  const size_t start = source_at - 16;
  static constexpr uint8_t kHeader[] = {0x1B, 'L', 'u', 'a', 0x51, 0,
                                        0,    4,   4,   4,   8,    0};
  if (std::memcmp(image + start, kHeader, sizeof(kHeader)) != 0) {
    XELOGW("Dashboard: GameLibrary chunk has an unexpected header");
    return;
  }
  const size_t end = SkipLuaFunction(image, size, start + sizeof(kHeader));
  if (!end) {
    XELOGW("Dashboard: GameLibrary chunk does not parse; unpatched");
    return;
  }

  // SETTABLE A B C: op 9, A at 6, C at 14, B at 23. RK(B) 290 and 287 are
  // constants 34 ("thumbnail") and 31 ("boxart") of ConvertToLibraryItem.
  const auto settable = [](uint32_t a, uint32_t b, uint32_t c) {
    return 9u | (a << 6) | (c << 14) | (b << 23);
  };
  const uint32_t old_ins = settable(17, 290, 18);
  const uint32_t new_ins = settable(17, 287, 18);
  uint8_t old_be[4] = {uint8_t(old_ins >> 24), uint8_t(old_ins >> 16),
                       uint8_t(old_ins >> 8), uint8_t(old_ins)};
  uint8_t new_be[4] = {uint8_t(new_ins >> 24), uint8_t(new_ins >> 16),
                       uint8_t(new_ins >> 8), uint8_t(new_ins)};

  size_t hit = 0;
  uint32_t hits = 0;
  for (size_t at = start; at + sizeof(old_be) <= end; ++at) {
    if (std::memcmp(image + at, old_be, sizeof(old_be)) == 0) {
      hit = at;
      ++hits;
    }
  }
  if (hits != 1) {
    XELOGW(
        "Dashboard: GameLibrary has {} thumbnail=ImagePath store(s), expected "
        "1; unpatched",
        hits);
    return;
  }

  // The key stays ImagePath. SlotImage (titleinfo+0x218) has no 42D gate, so
  // it would also answer on the first build, but it is marketplace slot
  // artwork and is empty for an offline library - which is what the
  // dashboard's own LocalContentInfo guards with IsStringNilOrEmpty, and what
  // left every tile blank when it was tried.
  //
  // The loader leaves the image's data pages read-only; open the stretch being
  // rewritten for the length of the write and put the protection back.
  const uint32_t write_start = base + uint32_t(hit);
  auto* heap = memory->LookupHeap(write_start);
  uint32_t old_protect = 0;
  if (!heap || !heap->Protect(write_start, sizeof(new_be),
                              kMemoryProtectRead | kMemoryProtectWrite,
                              &old_protect)) {
    XELOGW("Dashboard: could not make the GameLibrary chunk writable");
    return;
  }
  std::memcpy(image + hit, new_be, sizeof(new_be));
  heap->Protect(write_start, sizeof(new_be), old_protect);
  XELOGW(
      "Dashboard: My Games tiles take titleInfo.ImagePath (dataItem.boxart at "
      "{:08X}, chunk {:08X}..{:08X})",
      write_start, base + uint32_t(start), base + uint32_t(end));
}

// ConvertToLibraryItem reads ImagePath before anything has loaded the picture,
// and ImagePath is gated on titleinfo+0x42D - which GetImageInfo, called from
// Marketplace.AsyncMetadataLoader afterwards, is what sets. So the very first
// build of My Games put nil on every tile and only later ones were right.
//
// The loader is therefore pointed at the same place, where the gate is open by
// the time it runs: `dataItem.boxart = titleInfo.ImagePath`, in place of
// `dataItem.thumbnail = info.Path`.
//
// The two constants it needs are two bytes longer than the two it replaces, so
// the chunk's own source name gives those two bytes back and the span between
// it and the constant table shifts down to meet them. The chunk's total size
// is unchanged and nothing past the constants moves at all.
void PatchDashboardLibraryLoader(Memory* memory, cpu::XexModule* xex) {
  const uint32_t base = xex->base_address();
  const uint32_t size = xex->image_size();
  if (!base || !size) {
    return;
  }
  uint8_t* image = memory->TranslateVirtual<uint8_t*>(base);
  const std::string_view source = "@Marketplace.AsyncMetadataLoader.lua";
  const std::string_view haystack(reinterpret_cast<const char*>(image), size);
  const size_t source_at = haystack.find(source);
  if (source_at == std::string_view::npos || source_at < 16) {
    XELOGW("Dashboard: no AsyncMetadataLoader chunk; first-build art unpatched");
    return;
  }
  const size_t start = source_at - 16;
  static constexpr uint8_t kHeader[] = {0x1B, 'L', 'u', 'a', 0x51, 0,
                                        0,    4,   4,   4,   8,    0};
  if (std::memcmp(image + start, kHeader, sizeof(kHeader)) != 0) {
    XELOGW("Dashboard: AsyncMetadataLoader chunk has an unexpected header");
    return;
  }
  const size_t end = SkipLuaFunction(image, size, start + sizeof(kHeader));
  if (!end) {
    XELOGW("Dashboard: AsyncMetadataLoader chunk does not parse; unpatched");
    return;
  }

  // The source string's own length field, which has to say what we expect
  // before anything is moved on the strength of it.
  const size_t source_size_at = start + sizeof(kHeader);
  const size_t stored = (size_t(image[source_size_at]) << 24) |
                        (size_t(image[source_size_at + 1]) << 16) |
                        (size_t(image[source_size_at + 2]) << 8) |
                        size_t(image[source_size_at + 3]);
  if (stored != source.size() + 1) {
    XELOGW("Dashboard: AsyncMetadataLoader source is {} bytes, expected {}",
           stored, source.size() + 1);
    return;
  }
  static constexpr char kShortSource[] = "@Marketplace.AsyncMetadataLoad.lua";
  constexpr size_t kShrink = 2;
  static_assert(sizeof(kShortSource) + kShrink ==
                    sizeof("@Marketplace.AsyncMetadataLoader.lua"),
                "the shortened source must give back exactly two bytes");
  const size_t source_end = source_at + source.size() + 1;

  // GETTABLE A B C: op 6. R6 = R5['Path'] becomes R6 = R4['ImagePath'], where
  // R4 is the entry's TitleInfo and K[8] is about to hold that name.
  const auto gettable = [](uint32_t a, uint32_t b, uint32_t c) {
    return 6u | (a << 6) | (c << 14) | (b << 23);
  };
  const auto settable = [](uint32_t a, uint32_t b, uint32_t c) {
    return 9u | (a << 6) | (c << 14) | (b << 23);
  };
  const uint32_t words[4] = {gettable(6, 5, 265), gettable(6, 4, 264),
                             settable(3, 264, 6), settable(3, 265, 6)};
  uint8_t be[4][4];
  for (int i = 0; i < 4; ++i) {
    for (int b = 0; b < 4; ++b) {
      be[i][b] = uint8_t(words[i] >> (24 - 8 * b));
    }
  }
  static constexpr uint8_t kKeyOld[] = {4,   0,   0,   0,   10,  't', 'h', 'u',
                                        'm', 'b', 'n', 'a', 'i', 'l', 0};
  static constexpr uint8_t kKeyNew[] = {4,   0,   0,   0,   10,  'I', 'm',
                                        'a', 'g', 'e', 'P', 'a', 't', 'h', 0};
  static constexpr uint8_t kValOld[] = {4, 0, 0, 0, 5, 'P', 'a', 't', 'h', 0};
  static constexpr uint8_t kValNew[] = {4,   0,   0,   0,   7, 'b',
                                        'o', 'x', 'a', 'r', 't', 0};
  static_assert(sizeof(kValNew) - sizeof(kValOld) == kShrink,
                "the new constants must cost exactly what the source gives");

  const auto find_one = [&](const uint8_t* pattern, size_t len,
                            const char* what, size_t& out) {
    uint32_t hits = 0;
    for (size_t at = start; at + len <= end; ++at) {
      if (std::memcmp(image + at, pattern, len) == 0) {
        out = at;
        ++hits;
      }
    }
    if (hits != 1) {
      XELOGW("Dashboard: AsyncMetadataLoader has {} {}, expected 1; unpatched",
             hits, what);
    }
    return hits == 1;
  };

  size_t read_at = 0, write_at = 0, key_at = 0, val_at = 0;
  if (!find_one(be[0], 4, "info.Path reads", read_at) ||
      !find_one(be[2], 4, "thumbnail stores", write_at) ||
      !find_one(kKeyOld, sizeof(kKeyOld), "'thumbnail' constants", key_at) ||
      !find_one(kValOld, sizeof(kValOld), "'Path' constants", val_at)) {
    return;
  }
  // Everything shifts down into the source string, so the constants must come
  // after it and the value constant must come last.
  if (!(source_end < read_at && read_at < write_at && write_at < key_at &&
        key_at < val_at && val_at + sizeof(kValOld) <= end)) {
    XELOGW("Dashboard: AsyncMetadataLoader is not laid out as expected");
    return;
  }

  const uint32_t write_start = base + uint32_t(source_size_at);
  const uint32_t write_size =
      uint32_t(val_at + sizeof(kValOld) - source_size_at);
  auto* heap = memory->LookupHeap(write_start);
  uint32_t old_protect = 0;
  if (!heap ||
      !heap->Protect(write_start, write_size,
                     kMemoryProtectRead | kMemoryProtectWrite, &old_protect)) {
    XELOGW("Dashboard: could not make the AsyncMetadataLoader chunk writable");
    return;
  }
  std::memmove(image + source_end - kShrink, image + source_end,
               val_at - source_end);
  const size_t shorter = sizeof(kShortSource);  // chars + nul
  image[source_size_at] = uint8_t(shorter >> 24);
  image[source_size_at + 1] = uint8_t(shorter >> 16);
  image[source_size_at + 2] = uint8_t(shorter >> 8);
  image[source_size_at + 3] = uint8_t(shorter);
  std::memcpy(image + source_at, kShortSource, shorter);
  std::memcpy(image + read_at - kShrink, be[1], 4);
  std::memcpy(image + write_at - kShrink, be[3], 4);
  std::memcpy(image + key_at - kShrink, kKeyNew, sizeof(kKeyNew));
  std::memcpy(image + val_at - kShrink, kValNew, sizeof(kValNew));
  heap->Protect(write_start, write_size, old_protect);
  XELOGW(
      "Dashboard: AsyncMetadataLoader fills dataItem.boxart from "
      "titleInfo.ImagePath (chunk {:08X}..{:08X})",
      base + uint32_t(start), base + uint32_t(end));
}

}  // namespace

X_RESULT KernelState::FinishLoadingUserModule(
    const object_ref<UserModule> module, bool call_entry) {
  // TODO(Gliniak): Apply custom patches here
  X_RESULT result = module->LoadContinue();
  if (XFAILED(result)) {
    return result;
  }
  module->Dump();
  emulator_->patcher()->ApplyPatchesForTitle(memory_, module->title_id(),
                                             module->hash());
  if (module->title_id() == kDashboardID && module->xex_module()) {
    PatchDashboardLibraryArt(memory_, module->xex_module());
    PatchDashboardLibraryLoader(memory_, module->xex_module());
  }
  emulator_->on_patch_apply();
  if (module->xex_module()) {
    module->xex_module()->Precompile();
  }

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // Call DllMain(DLL_PROCESS_ATTACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    uint64_t args[] = {
        module->handle(),
        1,  // DLL_PROCESS_ATTACH
        0,  // 0 because always dynamic
    };

    module->is_attached_ = true;

    auto thread_state = XThread::GetCurrentThread()->thread_state();
    processor()->Execute(thread_state, module->entry_point(), args,
                         xe::countof(args));
  }
  return result;
}

namespace {
// The path of a module relative to the mount it was loaded from. The
// offset cannot be guessed: a content package mounts with a trailing
// separator (\Device\Content\3\) and a host folder without one
// (\Device\Harddisk0\Partition1), so the same '+ 1' that is right for
// one eats a character of the filename for the other - which reads as a
// missing title update rather than a bad path. Take everything past the
// prefix and drop whichever separator happens to be there.
std::string RelativeToMount(const std::string& path,
                            const std::string& mount_path) {
  std::string relative = path.substr(mount_path.size());
  const size_t start = relative.find_first_not_of("/\\");
  return start == std::string::npos ? std::string() : relative.substr(start);
}
}  // namespace

X_RESULT KernelState::ApplyTitleUpdate(
    const object_ref<UserModule> title_module) {
  object_ref<UserModule> patch_module = {};

  // Prefer the library-managed active update, loaded directly from disk so
  // large updates don't need to be copied or linked into the content tree.
  if (cvars::apply_title_update && emulator_ &&
      emulator_->title_update_manager()) {
    std::filesystem::path lib_path =
        emulator_->title_update_manager()->GetActiveLibraryPath(
            title_module->title_id());
    if (!lib_path.empty() && title_module->is_multi_disc_title()) {
      const std::filesystem::path disc_path =
          lib_path / fmt::format("disc{:03}", title_module->disc_number());
      std::error_code disc_ec;
      if (std::filesystem::is_directory(disc_path, disc_ec)) {
        lib_path = disc_path;
      }
    }
    std::string mount_path;
    if (!lib_path.empty() &&
        file_system()->FindSymbolicLink("game:", mount_path) &&
        title_module->path().starts_with(mount_path)) {
      const std::string relative =
          RelativeToMount(title_module->path(), mount_path) + 'p';

      // The library owns the update. Mount the selected folder straight into
      // the VFS as UPDATE: - no copy, no link, and nothing routed through the
      // content tree.
      //
      // Every user module the title loads comes through here and only the
      // executables have a matching .xexp, so mount once per active update:
      // remounting per module would tear the device out from under a title
      // reading its updated resources, and dropping it on "no patch for this
      // module" would unmount it for good.
      const std::string device_path = kTitleUpdateMountPath;
      const bool already_mounted = mounted_title_update_path_ == lib_path;
      bool mounted_here = false;

      if (!already_mounted) {
        file_system()->UnregisterSymbolicLink(kDefaultUpdateSymbolicLink);
        file_system()->UnregisterDevice(device_path);
        file_system()->UnregisterOverlayDevices();
        mounted_title_update_path_.clear();

        auto device =
            std::make_unique<vfs::HostPathDevice>(device_path, lib_path, false);
        if (device->Initialize() &&
            file_system()->RegisterDevice(std::move(device))) {
          file_system()->RegisterSymbolicLink(kDefaultUpdateSymbolicLink,
                                              device_path);
          mounted_title_update_path_ = lib_path;
          mounted_here = true;

          // A title update is not just the executable patch. Its fastfiles,
          // packs and loose resources have to replace the disc copies for
          // titles that open them by their normal game: path and never touch
          // update: - so layer the same folder over the game mount. Lookups
          // miss straight through to the disc for anything the update does
          // not carry.
          std::error_code layout_ec;
          const bool engine_merges_update = std::filesystem::is_regular_file(
              lib_path / "Data" / "layout.toc", layout_ec);
          if (engine_merges_update) {
            XELOGI(
                "Title update {} carries Data/layout.toc; mounted as update: "
                "only",
                xe::path_to_utf8(lib_path));
          } else {
            auto overlay = std::make_unique<vfs::HostPathDevice>(
                mount_path, lib_path, true);
            if (overlay->Initialize()) {
              file_system()->RegisterOverlayDevice(mount_path,
                                                   std::move(overlay));
            }
          }
        }
      }

      if (already_mounted || mounted_here) {
        std::string update_target;
        if (file_system()->FindSymbolicLink(kDefaultUpdateSymbolicLink,
                                            update_target)) {
          xe::vfs::Entry* patch_entry = file_system()->ResolvePath(
              xe::utf8::join_guest_paths(update_target, relative));
          if (patch_entry) {
            auto candidate = object_ref<UserModule>(new UserModule(this));
            if (candidate->LoadFromFile(patch_entry->absolute_path()) ==
                X_STATUS_SUCCESS) {
              patch_module = candidate;
            }
          }
        }
        if (!patch_module && mounted_here) {
          // Nothing loaded off a mount nothing else is using yet - drop it so
          // the legacy fallback can mount UPDATE: itself.
          file_system()->UnregisterSymbolicLink(kDefaultUpdateSymbolicLink);
          file_system()->UnregisterDevice(device_path);
          file_system()->UnregisterOverlayDevices();
          mounted_title_update_path_.clear();
        }
      }
    }
  }

  // Fall back to the legacy content-tree lookup.
  if (!patch_module) {
    const auto title_updates = FindTitleUpdate(title_module->title_id());
    if (title_updates.empty()) {
      return X_STATUS_SUCCESS;
    }
    patch_module = LoadTitleUpdate(&title_updates.front(), title_module);
  }

  if (!patch_module) {
    return X_STATUS_SUCCESS;
  }

  if (!patch_module->xex_module()->is_patch()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (!IsPatchSignatureProper(title_module, patch_module)) {
    if (!cvars::allow_incompatible_title_update) {
      XELOGW(
          "Skipping incompatible title update for {} due to signature mismatch",
          title_module->name());
      if (!GetExecutableModule()) {
        emulator_->display_window()->app_context().CallInUIThread([&]() {
          new xe::ui::HostNotificationWindow(
              emulator_->imgui_drawer(), "Warning!",
              "Title Update signature doesn't match. Skipping its application!",
              0);
        });
      }
      return X_STATUS_SUCCESS;
    }

    // First module that is loaded is always main executable. That way we can
    // prevent random message spam in case of loading/unloading.
    XELOGW(
        "Applying incompatible title update for {} due to enabled "
        "allow_incompatible_title_update config option!",
        title_module->name());
    if (!GetExecutableModule()) {
      emulator_->display_window()->app_context().CallInUIThread([&]() {
        new xe::ui::HostNotificationWindow(
            emulator_->imgui_drawer(), "Warning!",
            "Title Update signature doesn't match. This can cause unexpected "
            "issues or crashes!",
            0);
      });
    }
  }

  return ApplyTitleUpdate(title_module, patch_module);
}

std::vector<xam::XCONTENT_DATA_INTERNAL> KernelState::FindTitleUpdate(
    const uint32_t title_id) const {
  if (!cvars::apply_title_update) {
    return {};
  }

  return xam_state_->content_manager()->ListContent(
      1, 0, title_id, xe::XContentType::kInstaller,
      xe::kernel::xam::XContentFlag::kNone);
}

const object_ref<UserModule> KernelState::LoadTitleUpdate(
    const xam::XCONTENT_DATA_INTERNAL* title_update,
    const object_ref<UserModule> module) {
  uint32_t content_license = 0;
  X_RESULT open_status = content_manager()->OpenContent(
      "UPDATE", 0, *title_update, content_license,
      module->is_multi_disc_title() ? module->disc_number() : -1);

  std::string mount_path = "";
  if (!file_system()->FindSymbolicLink(kDefaultGameSymbolicLink, mount_path)) {
    return nullptr;
  }

  if (!module->path().starts_with(mount_path)) {
    return nullptr;
  }

  std::string resolved_path = "";
  if (!file_system()->FindSymbolicLink(kDefaultUpdateSymbolicLink,
                                       resolved_path)) {
    return nullptr;
  }

  const std::string relative_path =
      RelativeToMount(module->path(), mount_path) + 'p';

  xe::vfs::Entry* patch_entry = kernel_state()->file_system()->ResolvePath(
      xe::utf8::join_guest_paths(resolved_path, relative_path));

  if (!patch_entry) {
    XELOGI("Loading XEX patch failed. Path doesn't exist {}",
           xe::utf8::join_guest_paths(resolved_path, relative_path));
    return nullptr;
  }

  const std::string patch_path = patch_entry->absolute_path();
  XELOGI("Loading XEX patch from {}", patch_path);
  auto patch_module = object_ref<UserModule>(new UserModule(this));

  X_RESULT result = patch_module->LoadFromFile(patch_path);
  if (result != X_STATUS_SUCCESS) {
    XELOGE("Failed to load XEX patch, code: {}", result);
    return nullptr;
  }

  return patch_module;
}

bool KernelState::IsPatchSignatureProper(
    const object_ref<UserModule> title_module,
    const object_ref<UserModule> patch_module) const {
  xex2_opt_delta_patch_descriptor* patch_header = nullptr;
  patch_module->GetOptHeader(XEX_HEADER_DELTA_PATCH_DESCRIPTOR,
                             reinterpret_cast<void**>(&patch_header));

  if (!patch_header) {
    XELOGE("XEX patch has no delta patch descriptor");
    return false;
  }

  // Compare hash inside delta descriptor to base XEX signature
  uint8_t digest[0x14];
  sha1::SHA1 s;
  s.processBytes(title_module->xex_module()->xex_security_info()->rsa_signature,
                 0x100);
  s.finalize(digest);

  if (memcmp(digest, patch_header->digest_source, 0x14) != 0) {
    XELOGW(
        "XEX patch signature hash doesn't match base XEX signature hash, patch "
        "will likely fail!");

    return false;
  }
  return true;
}

X_RESULT KernelState::ApplyTitleUpdate(
    const object_ref<UserModule> title_module,
    const object_ref<UserModule> patch_module) {
  if (!title_module) {
    XELOGE("{}: No title_module provided!", __FUNCTION__);
    return X_STATUS_UNSUCCESSFUL;
  }

  if (!patch_module) {
    XELOGE("{}: No patch_module provided!", __FUNCTION__);
    return X_STATUS_UNSUCCESSFUL;
  }

  X_STATUS result =
      patch_module->xex_module()->ApplyPatch(title_module->xex_module());
  if (result != X_STATUS_SUCCESS) {
    XELOGE("Failed to apply XEX patch, code: {}", result);
  }
  return result;
}

void KernelState::DumpGuestThreads(const std::string_view reason) {
  auto* stack_walker = processor() ? processor()->stack_walker() : nullptr;
  if (!stack_walker) {
    XELOGE("DumpGuestThreads({}): no stack walker", reason);
    return;
  }

  std::vector<object_ref<XThread>> threads;
  {
    auto global_lock = global_critical_region_.Acquire();
    for (const auto& [id, thread] : threads_by_id_) {
      if (thread && !thread->is_host_object()) {
        threads.push_back(retain_object(thread));
      }
    }
  }

  XELOGE("==== GUEST THREAD DUMP ({}) - {} thread(s) ====", reason,
         threads.size());
  for (const auto& thread : threads) {
    auto* host_thread = thread->thread();
    if (!host_thread) {
      continue;
    }
    XELOGE(" thread {:08X} '{}' cpu {}", thread->handle(), thread->name(),
           thread->active_cpu());

    // The walker needs the thread held still, and the frames are only
    // meaningful while it is.
    host_thread->Suspend();
    uint64_t host_pcs[64] = {};
    cpu::StackFrame frames[64] = {};
    const size_t count = stack_walker->CaptureStackTrace(
        host_thread->native_handle(), host_pcs, 0, xe::countof(host_pcs),
        nullptr, nullptr, nullptr);
    const bool resolved =
        count && stack_walker->ResolveStack(host_pcs, frames, count);
    host_thread->Resume();

    if (!resolved) {
      XELOGE("   <no frames>");
      continue;
    }
    for (size_t i = 0; i < count; ++i) {
      const auto& frame = frames[i];
      if (frame.type == cpu::StackFrame::Type::kGuest) {
        auto* function = frame.guest_symbol.function;
        XELOGE("   [{:2}] guest {:08X} {}", i, frame.guest_pc,
               function ? function->name() : "");
      } else {
        XELOGE("   [{:2}] host  {:016X} {}", i, frame.host_pc,
               frame.host_symbol.name);
      }
    }
  }
  XELOGE("==== END GUEST THREAD DUMP ====");
}

void KernelState::PinLiveThreadBlocks() {
  // A thread that outlives the title keeps its PCR and KTHREAD, and the sweep
  // has no idea who owns what. Handing a live thread's blocks to a new one
  // gives two threads the same KTHREAD address - and since a guest critical
  // section decides ownership by comparing that address, both of them then
  // believe they hold every lock the other takes.
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& [id, thread] : threads_by_id_) {
    if (thread) {
      thread->PinGuestBlocks();
    }
  }
}

void KernelState::StartHangWatchdog() {
  if (hang_watchdog_running_.exchange(true)) {
    return;
  }
  hang_watchdog_ = xe::threading::Thread::Create({}, [this]() {
    xe::threading::set_name("Hang Watchdog");
    XELOGD("Hang watchdog: started");
    uint64_t last_calls = 0;
    uint32_t still = 0;
    bool reported = false;
    while (hang_watchdog_running_.load(std::memory_order_relaxed)) {
      xe::threading::Sleep(std::chrono::seconds(2));
      const uint64_t calls = XThread::kernel_call_count();
      // Zero means no guest code has ever entered the kernel, i.e. the
      // emulator is sitting idle with nothing loaded. That is not a hang.
      if (calls == 0) {
        still = 0;
        reported = false;
        continue;
      }
      if (calls != last_calls) {
        last_calls = calls;
        still = 0;
        reported = false;
        continue;
      }
      // NO title_open GATE. A title that wedges during its own teardown or
      // before the launch completes is exactly when this is wanted, and the
      // gate made the watchdog silent for those.
      ++still;
      if (still <= 6) {
        XELOGD("Hang watchdog: still at {} call(s) for {} second(s)", calls,
               still * 2);
      }
      // Ten seconds without the title asking the kernel for anything. The one
      // line that says WHY the dump below appeared stays at error level; the
      // per-tick chatter above is only useful when you are already watching.
      if (still >= 5 && !reported) {
        reported = true;
        XELOGE("Hang watchdog: no kernel call for {} second(s)", still * 2);
        DumpGuestThreads("hang watchdog");
      }
    }
    XELOGD("Hang watchdog: stopped");
  });
}

void KernelState::StopHangWatchdog() {
  if (!hang_watchdog_running_.exchange(false)) {
    return;
  }
  if (hang_watchdog_) {
    // Thread derives from WaitHandle, so this is the free Wait, not XThread's.
    xe::threading::Wait(hang_watchdog_.get(), false,
                        std::chrono::milliseconds(3000));
    hang_watchdog_.reset();
  }
}

void KernelState::RetainLoaderModule(const object_ref<UserModule>& module) {
  auto global_lock = global_critical_region_.Acquire();
  loader_modules_.push_back(module);
}

void KernelState::ReleaseLoaderModule(UserModule* module) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = std::find_if(loader_modules_.begin(), loader_modules_.end(),
                         [module](const object_ref<UserModule>& held) {
                           return held.get() == module;
                         });
  if (it != loader_modules_.end()) {
    loader_modules_.erase(it);
  }
}

void KernelState::UnloadUserModule(const object_ref<UserModule>& module,
                                   bool call_entry) {
  auto global_lock = global_critical_region_.Acquire();

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // Call DllMain(DLL_PROCESS_DETACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    uint64_t args[] = {
        module->handle(),
        0,  // DLL_PROCESS_DETACH
        0,  // 0 for now, assume XexUnloadImage is like FreeLibrary
    };
    auto thread_state = XThread::GetCurrentThread()->thread_state();
    processor()->Execute(thread_state, module->entry_point(), args,
                         xe::countof(args));
  }

  auto iter = std::ranges::find_if(user_modules_, [&module](const auto& e) {
    return e->path() == module->path();
  });
  assert_true(iter != user_modules_.end());  // Unloading an unregistered module
                                             // is probably really bad
  user_modules_.erase(iter);

  // Ensure this module was not somehow registered twice
  assert_true(std::find_if(user_modules_.begin(), user_modules_.end(),
                           [&module](const auto& e) {
                             return e->path() == module->path();
                           }) == user_modules_.end());

  object_table()->ReleaseHandleInLock(module->handle());
}

void KernelState::InitXmpVolumePatch() {
  xmp_volume_patch_ = XmpVolumePatch::CreateForTitle(title_id(), this);
}

void KernelState::TerminateTitle(bool clear_handles) {
  XELOGD("KernelState::TerminateTitle");
  xmp_volume_patch_.reset();
  // Emulator::TerminateTitle drops the UPDATE: mount right after this.
  mounted_title_update_path_.clear();

  // Cooperative exit first: ask the title's guest threads to leave on their
  // own and give them a moment. They unwind normally, so their destructors run
  // and nothing host-side is left holding a lock. Done WITHOUT the global lock
  // held - an exiting thread needs it. Whatever is still running after the
  // deadline (a thread spinning in guest code never enters the kernel) is
  // parked by the loop below, as before.
  {
    std::vector<object_ref<XThread>> leaving;
    {
      auto lock = global_critical_region_.Acquire();
      for (const auto& [thread_id, thread] : threads_by_id_) {
        if (thread && !XThread::IsInThread(thread) &&
            thread->is_guest_thread() && !IsSystemThread(thread_id)) {
          leaving.push_back(retain_object(thread));
        }
      }
    }
    for (auto& thread : leaving) {
      thread->RequestExit();
      if (thread->thread() && !emulator_->is_paused()) {
        thread->thread()->Resume();
      }
    }
    const uint64_t deadline = Clock::QueryHostUptimeMillis() + 250;
    uint32_t left = 0;
    for (auto& thread : leaving) {
      const uint64_t now = Clock::QueryHostUptimeMillis();
      if (now >= deadline || !thread->thread()) {
        continue;
      }
      if (xe::threading::Wait(thread->thread(), false,
                              std::chrono::milliseconds(deadline - now)) ==
          xe::threading::WaitResult::kSuccess) {
        ++left;
      }
    }
    XELOGI("TerminateTitle: {} of {} guest thread(s) exited cooperatively",
           left, leaving.size());
  }

  auto global_lock = global_critical_region_.Acquire();

  // Call terminate routines.
  // TODO(benvanik): these might take arguments.
  // FIXME: Calling these will send some threads into kernel code and they'll
  // hold the lock when terminated! Do we need to wait for all threads to exit?
  /*
  if (from_guest_thread) {
    for (auto routine : terminate_notifications_) {
      auto thread_state = XThread::GetCurrentThread()->thread_state();
      processor()->Execute(thread_state, routine.guest_routine);
    }
  }
  terminate_notifications_.clear();
  */

  // Kill all guest threads.
  for (auto it = threads_by_id_.begin(); it != threads_by_id_.end();) {
    if (!XThread::IsInThread(it->second) && it->second->is_guest_thread() &&
        !IsSystemThread(it->first)) {
      auto thread = retain_object(it->second);
      it = threads_by_id_.erase(it);

      if (thread->is_running()) {
        if (!emulator_->is_paused()) {
          thread->thread()->Suspend();
        }
        XELOGI("TerminateTitle: parking thread {:08X} (handle {:08X})",
               thread->thread_id(), thread->handle());
        // Not Terminate(): killing a host thread that holds the loader or CRT
        // heap lock faults inside ntdll. Abandon frees the guest blocks, which
        // is what the memory reclaim needs.
        thread->Abandon();
      }
    } else {
      ++it;
    }
  }

  XELOGI("TerminateTitle: unloading {} user modules ({} held by the loader)",
         user_modules_.size(), loader_modules_.size());
  // Third: Unload all user modules (including the executable).
  //
  // The loader's references go first. A title killed mid-run never calls the
  // XexUnloadImage that would balance its XexLoadImage, so without this every
  // DLL it loaded outlives it - and because ~XModule is what calls
  // Processor::RemoveModule, the dead module stays registered and shadows the
  // next title's module of the same name at the same base address.
  loader_modules_.clear();
  for (size_t i = 0; i < user_modules_.size(); i++) {
    user_modules_[i]->ReleaseHandle();
  }
  user_modules_.clear();

  XELOGI("TerminateTitle: content roots");
  xam::CloseOpenedContentFiles();
  if (auto* manager = content_manager()) {
    manager->CloseAllContent();
  }

  XELOGI("TerminateTitle: handles (clear {})", clear_handles);
  // Release all objects in the object table.
  if (clear_handles) {
    object_table_.PurgeAllObjects();
  }
  XELOGI("TerminateTitle: kernel done");

  // Unregister all notify listeners.
  notify_listeners_.clear();

  // Unset the executable module.
  executable_module_ = nullptr;

  if (XThread::IsInThread()) {
    threads_by_id_.erase(XThread::GetCurrentThread()->thread_id());

    // Now commit suicide (using Terminate, because we can't call into guest
    // code anymore).
    global_lock.unlock();
    XThread::GetCurrentThread()->Terminate(0);
  }
}

void KernelState::ReleaseTitleMemory() {
  std::vector<std::pair<uint32_t, uint32_t>> keep = {{0x80000000, 0x80400000}};
  {
    auto global_lock = global_critical_region_.Acquire();
    for (const auto& [thread_id, thread] : threads_by_id_) {
      const uint32_t base = thread->stack_alloc_base();
      if (base) {
        keep.emplace_back(base, base + thread->stack_alloc_size());
      }
    }
  }
  const uint32_t released = memory()->ReleaseTitleAllocations(keep);
  XELOGI("KernelState::ReleaseTitleMemory released {} allocations", released);
  const uint64_t zeroed = memory()->ZeroTitleFreedMemory(keep);
  XELOGW("TerminateTitle: zeroed {} KB the title had freed", zeroed / 1024);
}

void KernelState::MarkSystemThreads() {
  auto global_lock = global_critical_region_.Acquire();
  system_thread_ids_.clear();
  for (const auto& [thread_id, thread] : threads_by_id_) {
    system_thread_ids_.push_back(thread_id);
  }
  XELOGI("KernelState: {} threads predate the title",
         system_thread_ids_.size());
}

bool KernelState::IsSystemThread(uint32_t thread_id) const {
  for (uint32_t id : system_thread_ids_) {
    if (id == thread_id) {
      return true;
    }
  }
  return false;
}

void KernelState::WaitForInterruptDispatch() {
  for (uint32_t i = 0; i < 1000 && interrupt_dispatch_count_.load(); ++i) {
    xe::threading::Sleep(std::chrono::milliseconds(1));
  }
}

void KernelState::RegisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  threads_by_id_[thread->thread_id()] = thread;
}

void KernelState::UnregisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = threads_by_id_.find(thread->thread_id());
  if (it != threads_by_id_.end()) {
    threads_by_id_.erase(it);
  }
}

namespace {
// The loader lock: DLL entry points run one at a time, as on the console.
// Not the global lock - an entry point is guest code, and guest code that
// resolves a function another thread is compiling waits for it giving back
// only one level of the global lock. Held across the call as well, the
// compiling thread could never take it to place the code.
std::recursive_mutex loader_lock;
}  // namespace

void KernelState::OnThreadExecute(XThread* thread) {
  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // Call DllMain(DLL_THREAD_ATTACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  std::lock_guard<std::recursive_mutex> loader(loader_lock);
  std::vector<std::pair<object_ref<UserModule>, uint64_t>> attach;
  {
    auto global_lock = global_critical_region_.Acquire();
    for (auto user_module : user_modules_) {
      if (user_module->is_dll_module() && user_module->entry_point()) {
        attach.emplace_back(
            user_module,
            user_module->is_attached()
                ? static_cast<uint64_t>(2)    // DLL_THREAD_ATTACH - Used to
                                              // call DLL for each thread.
                : static_cast<uint64_t>(1));  // DLL_PROCESS_ATTACH - Used
                                              // only once for initialization.
        user_module->is_attached_ = true;
      }
    }
  }
  auto thread_state = thread->thread_state();
  for (auto& [user_module, reason] : attach) {
    // The last argument is 0 because loading is always dynamic.
    uint64_t args[] = {user_module->handle(), reason, 0};
    processor()->Execute(thread_state, user_module->entry_point(), args,
                         xe::countof(args));
  }
}

void KernelState::OnThreadExit(XThread* thread) {
  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // Call DllMain(DLL_THREAD_DETACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  // Under the loader lock, not the global one, as in OnThreadExecute.
  {
    std::lock_guard<std::recursive_mutex> loader(loader_lock);
    std::vector<object_ref<UserModule>> detach;
    {
      auto global_lock = global_critical_region_.Acquire();
      for (auto user_module : user_modules_) {
        if (user_module->is_dll_module() && user_module->entry_point()) {
          detach.push_back(user_module);
        }
      }
    }
    auto thread_state = thread->thread_state();
    for (auto& user_module : detach) {
      uint64_t args[] = {
          user_module->handle(),
          3,  // DLL_THREAD_DETACH
          0,  // 0 because always dynamic
      };
      processor()->Execute(thread_state, user_module->entry_point(), args,
                           xe::countof(args));
    }
  }

  auto global_lock = global_critical_region_.Acquire();
  emulator()->processor()->OnThreadExit(thread->thread_id());
}

object_ref<XThread> KernelState::GetThreadByID(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  XThread* thread = nullptr;
  auto it = threads_by_id_.find(thread_id);
  if (it != threads_by_id_.end()) {
    thread = it->second;
  }
  return retain_object(thread);
}

std::vector<uint32_t> KernelState::GetAllThreadIDs() {
  auto global_lock = global_critical_region_.Acquire();

  auto thread_ids_view =
      threads_by_id_ |
      std::views::transform([](const auto& pair) { return pair.first; });

  std::vector<std::uint32_t> thread_ids(thread_ids_view.begin(),
                                        thread_ids_view.end());

  return thread_ids;
}

void KernelState::RegisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  notify_listeners_.push_back(retain_object(listener));

  // Games seem to expect a few notifications on startup, only for the first
  // listener.
  // https://cs.rin.ru/forum/viewtopic.php?f=38&t=60668&hilit=resident+evil+5&start=375
  if (!has_notified_startup_ && listener->mask() & kXNotifySystem) {
    has_notified_startup_ = true;
    listener->EnqueueNotification(kXNotificationSystemUI,
                                  xam_state()->IsUIActive());

    const auto signed_in_players =
        xam_state()->profile_manager()->GetUsedUserSlots().to_ulong();

    listener->EnqueueNotification(kXNotificationSystemSignInChanged,
                                  signed_in_players);
  }

  // The storage devices are there before anything listens, so the dashboard
  // is told about them on EVERY system listener it creates, not only the
  // first. It keeps several: the Storage view polls its own (sub_92334770 in
  // the 17559 dash.xex) and rebuilds its device list only on 0x8000000D (tray
  // changed), 0xB (this) or a change of signed-in user - so with the first
  // listener alone getting 0xB, Storage stayed empty until a disc went in.
  // 415607FA: Doesn't like receiving this notification on boot. Causes game
  // to crash. Dashboard only, not SYSTEM|LIVE: the theme picker closes on it.
  if (title_id() == kDashboardID && (listener->mask() & kXNotifySystem) &&
      listener->mask() != (kXNotifySystem | kXNotifyLive)) {
    listener->EnqueueNotification(kXNotificationSystemStorageDevicesChanged, 0);
  }

  if (!has_notified_live_startup_ && listener->mask() & kXNotifyLive) {
    has_notified_live_startup_ = true;

    listener->EnqueueNotification(kXNotificationLiveVoicechatAway, 0);

    // Tell the title the Live connection state. Sent here, to the listener that
    // just registered, because that is the only moment delivery is guaranteed:
    // broadcasting earlier finds no listener and is dropped, and broadcasting
    // repeatedly re-enters the title's handler mid-session (which crashes MW3).
    //
    // MW3 queries XOnlineGetNatType ONLY from this notification's handler
    // (0x02000001 -> 8235E6C8 -> XLiveBase msg 0x58006 -> global 0x825AFA5C),
    // so without it the console reports NAT Strict and refuses to host.
    // 415707D1 fails to join sessions and 4E4D07D3 gets stuck in online menus
    // without it as well.
    listener->EnqueueNotification(
        kXNotificationLiveConnectionChanged,
        static_cast<uint32_t>(xam_state()->user_tracker()->GetLogonState()));
  }

  // Join Game from the hub apps' Friends launched this title to join a friend.
  // Accepted here, on the first Live listener, for the same reason as the
  // connection state above: earlier there is nobody to tell. The invite is
  // the one the Join Session buttons set - the guest reads it through
  // XInviteGetAcceptedInfo.
  if (listener->mask() & kXNotifyLive) {
    uint64_t inviter = 0;
    if (xam::TakePendingHubJoin(title_id(), &inviter)) {
      if (auto* profile = xam_state()->GetUserProfile(uint32_t(0))) {
        X_INVITE_INFO invite = {};
        invite.xuid_invitee = profile->GetOnlineXUID();
        invite.xuid_inviter = inviter;
        invite.title_id = title_id();
        invite.from_game_invite = true;
        profile->SetSelfInvite(invite);
        const uint32_t user_index =
            xam_state()->GetUserIndexAssignedToProfileFromXUID(profile->xuid());
        listener->EnqueueNotification(kXNotificationLiveInviteAccepted,
                                      user_index);
        XELOGI("HubJoin: invite from {:016X} accepted into {:08X}", inviter,
               title_id());
      }
    }
  }

  // 4E4D07ED, 58410869. Fixes creating Xbox Live sessions.
  // 4D5307D4 expects multiple notifications to access Xbox Live menus.
  // Sign in related. Not the dashboard: its theme picker closes on 0xA.
  if (listener->mask() == (kXNotifySystem | kXNotifyLive) &&
      title_id() != kDashboardID) {
    const auto signed_in_players =
        xam_state()->profile_manager()->GetUsedUserSlots().to_ulong();

    listener->EnqueueNotification(kXNotificationSystemSignInChanged,
                                  signed_in_players);
  }

  if (!has_notified_xmp_startup_ && listener->mask() & kXNotifyXmp) {
    has_notified_xmp_startup_ = true;
    // Playback state is idle until the media player broadcasts a transition
    // of its own, so only the controller is worth priming.
    listener->EnqueueNotification(
        kXNotificationXmpPlaybackControllerChanged,
        emulator()->audio_media_player()->IsTitleInPlaybackControl());
  }
}

void KernelState::UnregisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto it = notify_listeners_.begin(); it != notify_listeners_.end();
       ++it) {
    if ((*it).get() == listener) {
      notify_listeners_.erase(it);
      break;
    }
  }
}

void KernelState::BroadcastNotification(XNotificationID id, uint32_t data) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& notify_listener : notify_listeners_) {
    notify_listener->EnqueueNotification(id, data);
  }
}

void KernelState::CompleteOverlapped(uint32_t overlapped_ptr, X_RESULT result) {
  CompleteOverlappedEx(overlapped_ptr, result, result, 0);
}

void KernelState::CompleteOverlappedEx(uint32_t overlapped_ptr, X_RESULT result,
                                       uint32_t extended_error,
                                       uint32_t length) {
  // If function failed then overwrite return error.
  // What if a function expects a different return error?
  if (result != X_ERROR_SUCCESS) {
    result = X_ERROR_FUNCTION_FAILED;

    // Function failed without setting extended_error.
    if (extended_error == X_ERROR_SUCCESS) {
      extended_error = X_E_FUNCTION_FAILED;
    }
  }

  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, result);
  XOverlappedSetExtendedError(ptr, extended_error);
  XOverlappedSetLength(ptr, length);
  // The event is left unsignaled when there is a completion routine.
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle && !XOverlappedGetCompletionRoutine(ptr)) {
    auto ev = object_table()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    if (ev) {
      ev->Set(0, false);
    }
  }
  if (XOverlappedGetCompletionRoutine(ptr)) {
    X_HANDLE thread_handle = XOverlappedGetContext(ptr);
    auto thread = object_table()->LookupObject<XThread>(thread_handle);
    if (thread) {
      // Queue APC on the thread that requested the overlapped operation.
      uint32_t routine = XOverlappedGetCompletionRoutine(ptr);
      thread->EnqueueApc(routine, result, length, overlapped_ptr);
    }
  }
}

void KernelState::CompleteOverlappedImmediate(uint32_t overlapped_ptr,
                                              X_RESULT result) {
  // TODO(gibbed): there are games that check 'length' of overlapped as
  // an indication of success. WTF?
  // Setting length to -1 when not success seems to be helping.
  uint32_t length = !result ? 0 : 0xFFFFFFFF;
  CompleteOverlappedImmediateEx(overlapped_ptr, result, result, length);
}

void KernelState::CompleteOverlappedImmediateEx(uint32_t overlapped_ptr,
                                                X_RESULT result,
                                                uint32_t extended_error,
                                                uint32_t length) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
}

void KernelState::CompleteOverlappedDeferred(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(std::move(completion_callback), overlapped_ptr,
                               result, result, 0, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, uint32_t extended_error, uint32_t length,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback, result, extended_error, length](
          uint32_t& cb_extended_error, uint32_t& cb_length) -> X_RESULT {
        completion_callback();
        cb_extended_error = extended_error;
        cb_length = length;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferred(
    std::function<X_RESULT()> completion_callback, uint32_t overlapped_ptr,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback](uint32_t& extended_error,
                            uint32_t& length) -> X_RESULT {
        auto result = completion_callback();
        extended_error = static_cast<uint32_t>(result);
        length = 0;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<X_RESULT(uint32_t&, uint32_t&)> completion_callback,
    uint32_t overlapped_ptr, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle) {
    auto ev = object_table()->LookupObject<XObject>(event_handle);

    assert_not_null(ev);
    if (ev && ev->type() == XObject::Type::Event) {
      ev.get<XEvent>()->Reset();
    }
  }
  auto global_lock = global_critical_region_.Acquire();
  dispatch_queue_.push_back([this, completion_callback, overlapped_ptr,
                             pre_callback, post_callback]() {
    if (pre_callback) {
      pre_callback();
    }
    /*
     5454082B infinitely loads free roam in netplay without sleep.
     Small delay fixes it e.g. 25ms.

     53450814 black screens in netplay before main menu with high delay e.g.
     100ms.
     Small delay fixes it e.g. 25ms.

     55530848 and 55530816 fail to create Xbox Live session with high delay e.g.
     100ms.
     Small delay fixes it e.g. 25ms.

     555307EE quickly disconnects from session with high delay e.g.
     100ms.
     Small delay fixes it e.g. 25ms.

     4C4107ED internal log says "timed out connecting" and crashes attempting to
     join session via custom search with a delay of 25ms.
     Smaller delay fixes it e.g. 5ms.
    */
    xe::threading::Sleep(kDeferredOverlappedDelayMillis);
    uint32_t extended_error = 0;
    uint32_t length = 0;
    uint32_t result = completion_callback(extended_error, length);

    CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
    if (post_callback) {
      post_callback();
    }
  });
  dispatch_cond_.notify_all();
}

bool KernelState::Save(ByteStream* stream) {
  XELOGD("Serializing the kernel...");
  stream->Write(kKernelSaveSignature);

  // Save the object table
  object_table_.Save(stream);

  // Write the TLS allocation bitmap
  // We save XThreads absolutely first, as they will execute code upon save
  // (which could modify the kernel state)
  auto threads = object_table_.GetObjectsByType<XThread>();
  uint32_t* num_threads_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(threads.size()));

  size_t num_threads = threads.size();
  XELOGD("Serializing {} threads...", threads.size());
  for (auto thread : threads) {
    if (!thread->is_guest_thread()) {
      // Don't save host threads. They can be reconstructed on startup.
      num_threads--;
      continue;
    }

    if (!thread->Save(stream)) {
      XELOGD("Failed to save thread \"{}\"", thread->name());
      num_threads--;
    }
  }

  *num_threads_ptr = static_cast<uint32_t>(num_threads);

  // Save all other objects
  auto objects = object_table_.GetAllObjects();
  uint32_t* num_objects_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(objects.size()));

  size_t num_objects = objects.size();
  XELOGD("Serializing {} objects...", num_objects);
  for (auto object : objects) {
    auto prev_offset = stream->offset();

    if (object->is_host_object() || object->type() == XObject::Type::Thread) {
      // Don't save host objects or save XThreads again
      num_objects--;
      continue;
    }

    stream->Write<uint32_t>(static_cast<uint32_t>(object->type()));
    if (!object->Save(stream)) {
      XELOGD("Did not save object of type {}",
             static_cast<uint32_t>(object->type()));
      assert_always();

      // Revert backwards and overwrite if a save failed.
      stream->set_offset(prev_offset);
      num_objects--;
    }
  }

  *num_objects_ptr = static_cast<uint32_t>(num_objects);
  return true;
}

// this only gets triggered once per ms at most, so fields other than tick count
// will probably not be updated in a timely manner for guest code that uses them
void KernelState::UpdateKeTimestampBundle() {
  X_TIME_STAMP_BUNDLE* lpKeTimeStampBundle =
      memory_->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ke_timestamp_bundle_ptr_);
  uint32_t uptime_ms = Clock::QueryGuestUptimeMillis();
  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->interrupt_time,
                               Clock::QueryGuestInterruptTime());
  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->system_time,
                               Clock::QueryGuestSystemTime());
  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->tick_count, uptime_ms);

  // Every 20 ticks (~20ms), decay priority on running guest threads.
  // This simulates the Xenon decrementer-driven quantum expiration.
  if (++quantum_timer_counter_ >= 20) {
    quantum_timer_counter_ = 0;
    auto global_lock = global_critical_region_.Acquire();
    for (auto& [id, thread] : threads_by_id_) {
      if (thread->is_running()) {
        thread->CheckQuantumAndDecay();
      }
    }
  }
}

uint32_t KernelState::GetKeTimestampBundle() {
  XE_LIKELY_IF(ke_timestamp_bundle_ptr_) { return ke_timestamp_bundle_ptr_; }
  else {
    global_critical_region::PrepareToAcquire();
    return CreateKeTimestampBundle();
  }
}

XE_NOINLINE
XE_COLD
uint32_t KernelState::CreateKeTimestampBundle() {
  auto crit = global_critical_region::Acquire();

  const uint32_t pKeTimeStampBundle = 0x80240EE0;
  X_TIME_STAMP_BUNDLE* lpKeTimeStampBundle =
      memory_->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(pKeTimeStampBundle);

  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->interrupt_time,
                               Clock::QueryGuestInterruptTime());

  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->system_time,
                               Clock::QueryGuestSystemTime());

  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->tick_count,
                               Clock::QueryGuestUptimeMillis());

  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->padding, 0);

  ke_timestamp_bundle_ptr_ = pKeTimeStampBundle;
  timestamp_timer_ = xe::threading::HighResolutionTimer::CreateRepeating(
      std::chrono::milliseconds(1),
      [this]() { this->UpdateKeTimestampBundle(); });
  return pKeTimeStampBundle;
}

bool KernelState::Restore(ByteStream* stream) {
  // Check the magic value.
  if (stream->Read<uint32_t>() != kKernelSaveSignature) {
    return false;
  }

  // Restore the object table.
  if (!object_table_.Restore(stream)) {
    return false;
  }

  // TLS bitmap is now stored per-process in X_KPROCESS structures (in guest
  // memory) Skip reading old global TLS bitmap if present in old save files
  auto num_bitmap_entries = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_bitmap_entries; i++) {
    stream->Read<uint64_t>();  // Discard old data
  }

  uint32_t num_threads = stream->Read<uint32_t>();
  XELOGD("Loading {} threads...", num_threads);
  for (uint32_t i = 0; i < num_threads; i++) {
    auto thread = XObject::Restore(this, XObject::Type::Thread, stream);
    if (!thread) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  uint32_t num_objects = stream->Read<uint32_t>();
  XELOGD("Loading {} objects...", num_objects);
  for (uint32_t i = 0; i < num_objects; i++) {
    uint32_t type = stream->Read<uint32_t>();

    auto obj = XObject::Restore(this, XObject::Type(type), stream);
    if (!obj) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  return true;
}

std::bitset<4> KernelState::GetConnectedUsers() const {
  auto input_sys = emulator_->input_system();

  auto lock = input_sys->lock();

  return input_sys->GetConnectedSlots();
}
// todo: definitely need to do more to pretend to be in a dpc
void KernelState::BeginDPCImpersonation(cpu::ppc::PPCContext* context,
                                        DPCImpersonationScope& scope) {
  auto kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  xenia_assert(kpcr->prcb_data.dpc_active == 0);
  scope.previous_irql_ = kpcr->current_irql;

  kpcr->current_irql = 2;
  kpcr->prcb_data.dpc_active = 1;
}
void KernelState::EndDPCImpersonation(cpu::ppc::PPCContext* context,
                                      DPCImpersonationScope& end_scope) {
  auto kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  xenia_assert(kpcr->prcb_data.dpc_active == 1);
  kpcr->current_irql = end_scope.previous_irql_;
  kpcr->prcb_data.dpc_active = 0;
}
void KernelState::EmulateCPInterruptDPC(uint32_t interrupt_callback,
                                        uint32_t interrupt_callback_data,
                                        uint32_t source, uint32_t cpu) {
  if (!interrupt_callback) {
    return;
  }
  interrupt_dispatch_count_.fetch_add(1);

  auto thread = kernel::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  /*
    in reality, our interrupt is a callback that is called in a dpc which is
    scheduled by the actual interrupt

    we need to impersonate a dpc
  */
  auto current_context = thread->thread_state()->context();
  auto kthread = memory()->TranslateVirtual<X_KTHREAD*>(thread->guest_object());

  auto pcr = memory()->TranslateVirtual<X_KPCR*>(thread->pcr_ptr());

  DPCImpersonationScope dpc_scope{};
  BeginDPCImpersonation(current_context, dpc_scope);

  // todo: check VdGlobalXamDevice here. if VdGlobalXamDevice is nonzero, should
  // set X_PROCTYPE_SYSTEM
  xboxkrnl::xeKeSetCurrentProcessType(X_PROCTYPE_TITLE, current_context);

  uint64_t args[] = {source, interrupt_callback_data};
  processor_->Execute(thread->thread_state(), interrupt_callback, args,
                      xe::countof(args));
  xboxkrnl::xeKeSetCurrentProcessType(X_PROCTYPE_IDLE, current_context);

  EndDPCImpersonation(current_context, dpc_scope);
  interrupt_dispatch_count_.fetch_sub(1);
}

void KernelState::InitializeProcess(X_KPROCESS* process, uint32_t type,
                                    char priority_class, char default_priority,
                                    char max_dynamic_priority) {
  uint32_t guest_kprocess = memory()->HostToGuestVirtual(process);

  uint32_t thread_list_guest_ptr =
      guest_kprocess + offsetof(X_KPROCESS, thread_list);

  process->process_priority_class = priority_class;
  process->default_thread_priority = default_priority;
  process->max_dynamic_priority = max_dynamic_priority;
  util::XeInitializeListHead(&process->thread_list, thread_list_guest_ptr);
  process->quantum = 60;
  // doubt any guest code uses this ptr, which i think probably has something to
  // do with the page table
  process->clrdataa_masked_ptr = 0;
  // clrdataa_ & ~(1U << 31);
  process->thread_count = 0;
  process->disable_quantum_decay = 0x06;
  process->kernel_stack_size = 16 * 1024;
  process->tls_slot_size = 0x80;

  process->process_type = type;
  uint32_t unk_list_guest_ptr = guest_kprocess + offsetof(X_KPROCESS, unk_54);
  // TODO(benvanik): figure out what this list is.
  util::XeInitializeListHead(&process->unk_54, unk_list_guest_ptr);
}

void KernelState::SetTitleProcessTlsSlots(uint32_t slot_count) {
  auto title_process =
      memory_->TranslateVirtual<X_KPROCESS*>(GetTitleProcess());
  SetProcessTLSVars(title_process, int(slot_count), 0, 0);
}

void KernelState::SetProcessTLSVars(X_KPROCESS* process, int num_slots,
                                    int tls_data_size,
                                    int tls_static_data_address) {
  uint32_t slots_padded = (num_slots + 3) & 0xFFFFFFFC;
  process->tls_data_size = tls_data_size;
  process->tls_raw_data_size = tls_data_size;
  process->tls_static_data_address = tls_static_data_address;
  process->tls_slot_size = 4 * slots_padded;
  uint32_t count_div32 = slots_padded / 32;
  for (unsigned word_index = 0; word_index < count_div32; ++word_index) {
    process->tls_slot_bitmap[word_index] = -1;
  }

  // set remainder of bitset
  if (((num_slots + 3) & 0x1C) != 0) {
    process->tls_slot_bitmap[count_div32] = -1
                                            << (32 - ((num_slots + 3) & 0x1C));
  }
}
void AllocateThread(PPCContext* context) {
  uint32_t thread_mem_size = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  if (thread_mem_size <= 0xFD8) {
    thread_mem_size += 8;
  }
  uint32_t result =
      xboxkrnl::xeAllocatePoolTypeWithTag(context, thread_mem_size, a2, a3);
  if (((unsigned short)result & 0xFFF) != 0) {
    result += 2;
  }

  context->r[3] = static_cast<uint64_t>(result);
}
void FreeThread(PPCContext* context) {
  uint32_t thread_memory = static_cast<uint32_t>(context->r[3]);
  if ((thread_memory & 0xFFF) != 0) {
    thread_memory -= 8;
  }
  xboxkrnl::xeFreePool(context, thread_memory);
}

void SimpleForwardAllocatePoolTypeWithTag(PPCContext* context) {
  uint32_t a1 = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  context->r[3] = static_cast<uint64_t>(
      xboxkrnl::xeAllocatePoolTypeWithTag(context, a1, a2, a3));
}
void SimpleForwardFreePool(PPCContext* context) {
  xboxkrnl::xeFreePool(context, static_cast<uint32_t>(context->r[3]));
}

void DeleteMutant(PPCContext* context) {
  // todo: this should call kereleasemutant with some specific args

  xe::FatalError("DeleteMutant - need KeReleaseMutant(mutant, 1, 1, 0) ");
}
void DeleteTimer(PPCContext* context) {
  // todo: this should call KeCancelTimer
  xe::FatalError("DeleteTimer - need KeCancelTimer(mutant, 1, 1, 0) ");
}

void DeleteIoCompletion(PPCContext* context) {}

void UnknownProcIoDevice(PPCContext* context) {}

void CloseFileProc(PPCContext* context) {}

void DeleteFileProc(PPCContext* context) {}

void UnknownFileProc(PPCContext* context) {}

void DeleteSymlink(PPCContext* context) {
  X_KSYMLINK* lnk = context->TranslateVirtualGPR<X_KSYMLINK*>(context->r[3]);

  context->r[3] = lnk->refed_object_maybe;
  xboxkrnl::xeObDereferenceObject(context, lnk->refed_object_maybe);
}
void KernelState::InitializeKernelGuestGlobals() {
  kernel_guest_globals_ = memory_->SystemHeapAlloc(sizeof(KernelGuestGlobals));

  KernelGuestGlobals* block =
      memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  memset(block, 0, sizeof(KernelGuestGlobals));

  auto idle_process = memory()->TranslateVirtual<X_KPROCESS*>(GetIdleProcess());
  InitializeProcess(idle_process, X_PROCTYPE_IDLE, 0, 0, 0);
  idle_process->quantum = 0x7F;
  auto system_process =
      memory()->TranslateVirtual<X_KPROCESS*>(GetSystemProcess());
  InitializeProcess(system_process, X_PROCTYPE_SYSTEM, 2, 5, 9);
  SetProcessTLSVars(system_process, 32, 0, 0);

  // The title process too, even though no title is loaded yet.
  //
  // SetExecutableModule initialises this again when a title actually starts,
  // and until this line that was the ONLY place it ever happened - so with no
  // title the block stayed all zeroes. Any guest thread created before a
  // title (the boot animation, and anything it starts through ExCreateThread,
  // which passes no process and so lands here) then ran
  // XThread::InitializeGuestObject against it: XeInsertTailList follows
  // thread_list.blink_ptr, reads 0, and writes through guest address 0. An
  // access violation with "Title not started yet" on the dialog.
  //
  // An initialised empty list costs nothing and the later re-initialisation
  // from SetExecutableModule is unchanged.
  auto title_process =
      memory()->TranslateVirtual<X_KPROCESS*>(GetTitleProcess());
  InitializeProcess(title_process, X_PROCTYPE_TITLE, 10, 13, 17);

  uint32_t oddobject_offset =
      kernel_guest_globals_ +
      offsetof(KernelGuestGlobals, XboxKernelDefaultObject);

  // init unknown object

  block->XboxKernelDefaultObject.type = EventSynchronizationObject;
  block->XboxKernelDefaultObject.signal_state = 1;
  block->XboxKernelDefaultObject.wait_list.flink_ptr =
      oddobject_offset + offsetof(X_DISPATCH_HEADER, wait_list.flink_ptr);
  block->XboxKernelDefaultObject.wait_list.blink_ptr =
      block->XboxKernelDefaultObject.wait_list.flink_ptr;

  // init thread object
  block->ExThreadObjectType.pool_tag = 0x65726854;
  block->ExThreadObjectType.allocate_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(AllocateThread);

  block->ExThreadObjectType.free_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(FreeThread);

  // several object types just call freepool/allocatepool
  uint32_t trampoline_allocatepool =
      kernel_trampoline_group_.NewLongtermTrampoline(
          SimpleForwardAllocatePoolTypeWithTag);
  uint32_t trampoline_freepool =
      kernel_trampoline_group_.NewLongtermTrampoline(SimpleForwardFreePool);

  // init event object
  block->ExEventObjectType.pool_tag = 0x76657645;
  block->ExEventObjectType.allocate_proc = trampoline_allocatepool;
  block->ExEventObjectType.free_proc = trampoline_freepool;

  // init mutant object
  block->ExMutantObjectType.pool_tag = 0x6174754D;
  block->ExMutantObjectType.allocate_proc = trampoline_allocatepool;
  block->ExMutantObjectType.free_proc = trampoline_freepool;

  block->ExMutantObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteMutant);
  // init semaphore obj
  block->ExSemaphoreObjectType.pool_tag = 0x616D6553;
  block->ExSemaphoreObjectType.allocate_proc = trampoline_allocatepool;
  block->ExSemaphoreObjectType.free_proc = trampoline_freepool;
  // init timer obj
  block->ExTimerObjectType.pool_tag = 0x656D6954;
  block->ExTimerObjectType.allocate_proc = trampoline_allocatepool;
  block->ExTimerObjectType.free_proc = trampoline_freepool;
  block->ExTimerObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteTimer);
  // iocompletion object
  block->IoCompletionObjectType.pool_tag = 0x706D6F43;
  block->IoCompletionObjectType.allocate_proc = trampoline_allocatepool;
  block->IoCompletionObjectType.free_proc = trampoline_freepool;
  block->IoCompletionObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteIoCompletion);
  block->IoCompletionObjectType.unknown_size_or_object_ = oddobject_offset;

  // iodevice object
  block->IoDeviceObjectType.pool_tag = 0x69766544;
  block->IoDeviceObjectType.allocate_proc = trampoline_allocatepool;
  block->IoDeviceObjectType.free_proc = trampoline_freepool;
  block->IoDeviceObjectType.unknown_size_or_object_ = oddobject_offset;
  block->IoDeviceObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownProcIoDevice);

  // file object
  block->IoFileObjectType.pool_tag = 0x656C6946;
  block->IoFileObjectType.allocate_proc = trampoline_allocatepool;
  block->IoFileObjectType.free_proc = trampoline_freepool;
  block->IoFileObjectType.unknown_size_or_object_ =
      0x38;  // sizeof fileobject, i believe
  block->IoFileObjectType.close_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(CloseFileProc);
  block->IoFileObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteFileProc);
  block->IoFileObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownFileProc);

  // directory object
  block->ObDirectoryObjectType.pool_tag = 0x65726944;
  block->ObDirectoryObjectType.allocate_proc = trampoline_allocatepool;
  block->ObDirectoryObjectType.free_proc = trampoline_freepool;
  block->ObDirectoryObjectType.unknown_size_or_object_ = oddobject_offset;

  // symlink object
  block->ObSymbolicLinkObjectType.pool_tag = 0x626D7953;
  block->ObSymbolicLinkObjectType.allocate_proc = trampoline_allocatepool;
  block->ObSymbolicLinkObjectType.free_proc = trampoline_freepool;
  block->ObSymbolicLinkObjectType.unknown_size_or_object_ = oddobject_offset;
  block->ObSymbolicLinkObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteSymlink);

#define offsetof32(s, m) static_cast<uint32_t>(offsetof(s, m))

  host_object_type_enum_to_guest_object_type_ptr_ = {
      {XObject::Type::Event,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExEventObjectType)},
      {XObject::Type::Semaphore,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExSemaphoreObjectType)},
      {XObject::Type::Thread,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExThreadObjectType)},
      {XObject::Type::File,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoFileObjectType)},
      {XObject::Type::Mutant,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExMutantObjectType)},
      {XObject::Type::Device,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoDeviceObjectType)}};
  xboxkrnl::xeKeSetEvent(&block->UsbdBootEnumerationDoneEvent, 1, 0);
}

void KernelState::InitializeXbdmCpuCounters() {
  constexpr uint32_t counters_base_address = 0x91F00000;

  // These are not confirmed and there seems to be multiple types of counters,
  // but no idea how they're switched. For now this seems to be good enough.
  constexpr std::array<const char*, 0x11> xbdm_counters = {
      "load-hit-stores (S)",
      "instructions committed",
      "i-cache miss cycles",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "Bad counter number - must be 0-15."};

  auto xbdm_range = memory_->LookupHeap(counters_base_address);
  if (!xbdm_range->AllocFixed(
          counters_base_address, 0x1000, 0,
          kMemoryAllocationCommit | kMemoryAllocationReserve,
          kMemoryProtectRead | kMemoryProtectWrite)) {
    return;
  }

  uint32_t address = counters_base_address;

  for (size_t i = 0; i < xbdm_counters.size(); i++) {
    xbdm_counters_address[i] = address;
    const std::string entry = xbdm_counters[i];
    std::memcpy(memory_->TranslateVirtual<char*>(address), entry.c_str(),
                entry.size());
    address += static_cast<uint32_t>(entry.size()) + 1;
  }
}

}  // namespace kernel
}  // namespace xe
