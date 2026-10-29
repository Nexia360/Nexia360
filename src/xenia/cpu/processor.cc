/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/processor.h"

#include "xenia/base/assert.h"
#include "xenia/base/atomic.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_decode_data.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/thread.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"

// TODO(benvanik): based on compiler support
#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif

#if 0 && DEBUG
#define DEFAULT_DEBUG_FLAG true
#else
#define DEFAULT_DEBUG_FLAG false
#endif

DEFINE_bool(debug, DEFAULT_DEBUG_FLAG,
            "Allow debugging and retain debug information.", "General");
DEFINE_path(trace_function_data_path, "", "File to write trace data to.",
            "CPU");
DEFINE_bool(break_on_start, false, "Break into the debugger on startup.",
            "CPU");
DEFINE_int32(jit_background_threads, 2,
             "Threads that translate code a title writes at runtime (the XNA "
             "runtime's JIT output) as soon as it is swept, instead of on the "
             "first call into it. 0 translates on first call only.",
             "CPU");

namespace xe {
namespace kernel {
class XThread;
}  // namespace kernel

namespace cpu {

using xe::cpu::ppc::PPCOpcode;
using xe::kernel::XThread;

using namespace xe::literals;

class BuiltinModule : public Module {
 public:
  explicit BuiltinModule(Processor* processor)
      : Module(processor), name_("builtin") {}

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }

  bool ContainsAddress(uint32_t address) override {
    return (address & 0xFFFFFFF0) == 0xFFFFFFF0;
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return std::unique_ptr<Function>(new BuiltinFunction(this, address));
  }

 private:
  std::string name_;
};

class DynamicCodeModule : public Module {
 public:
  explicit DynamicCodeModule(Processor* processor)
      : Module(processor), name_("dynamic") {}

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }

  // User mode addresses alias physical memory, so ask about the kernel address.
  bool ContainsAddress(uint32_t address) override {
    const uint32_t kernel_address = memory_->UserModeKernelAddress(address);
    auto heap = memory_->LookupHeap(kernel_address);
    uint32_t protect = 0;
    return heap && heap->QueryProtect(kernel_address, &protect) &&
           (protect & kMemoryProtectRead);
  }

  // A physical window's view keeps the protection last set through that
  // window, but the physical view keeps the latest set through any of them,
  // which is what ContainsAddress checks. So read code through that.
  const uint8_t* TranslateCode(uint32_t address) const override {
    return memory_->TranslateForRead(memory_->UserModeKernelAddress(address));
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    auto function = processor_->backend()->CreateGuestFunction(this, address);
    // Dynamic code has no .pdata so only the swept range bounds the scan. It's
    // used only where the range ends on an unconditional branch. A range that
    // covers part of a function, like a patch or an old sweep of reused
    // memory, rarely does and doesn't cut the function off. A branch that
    // links counts too: XeFu ends its blocks with a bl that never returns.
    const uint32_t end = function ? processor_->SweptCodeEnd(address) : 0;
    const uint32_t last = ((end + 3) & ~uint32_t(3)) - 4;
    if (end && last > address && ContainsAddress(last) &&
        IsUnconditionalBranch(
            xe::load_and_swap<uint32_t>(TranslateCode(last)))) {
      function->set_end_address(last);
    }
    return function;
  }

 private:
  // b, blr or bctr, with or without link.
  static bool IsUnconditionalBranch(uint32_t code) {
    switch (code >> 26) {
      case 18:
        return true;
      case 19: {
        const uint32_t extended = (code >> 1) & 0x3FF;
        const uint32_t bo = (code >> 21) & 0x1F;
        return (extended == 16 || extended == 528) && (bo & 0x14) == 0x14;
      }
      default:
        return false;
    }
  }

  std::string name_;
};

Processor::Processor(xe::Memory* memory, ExportResolver* export_resolver)
    : memory_(memory), export_resolver_(export_resolver) {}

Processor::~Processor() {
  StopBackgroundTranslation();
  {
    auto global_lock = global_critical_region_.Acquire();
    modules_.clear();
    dynamic_code_module_.reset();
  }

  frontend_.reset();
  backend_.reset();

  if (functions_trace_file_) {
    functions_trace_file_->Flush();
    functions_trace_file_.reset();
  }
}

bool Processor::Setup(std::unique_ptr<backend::Backend> backend) {
  // TODO(benvanik): query mode from debugger?
  debug_info_flags_ = 0;

  auto frontend = std::make_unique<ppc::PPCFrontend>(this);
  // TODO(benvanik): set options/etc.

  // Must be initialized by subclass before calling into this.
  assert_not_null(memory_);

  std::unique_ptr<Module> builtin_module(new BuiltinModule(this));
  builtin_module_ = builtin_module.get();
  modules_.push_back(std::move(builtin_module));

  if (frontend_ || backend_) {
    return false;
  }

  if (!backend) {
    return false;
  }
  if (!backend->Initialize(this)) {
    return false;
  }
  if (!frontend->Initialize()) {
    return false;
  }

  backend_ = std::move(backend);
  frontend_ = std::move(frontend);

  // Stack walker is used when profiling, debugging, and dumping.
  // Note that creation may fail, in which case we'll have to disable those
  // features.
  // The code cache may be unavailable in case of a "null" backend.
  cpu::backend::CodeCache* code_cache = backend_->code_cache();
  if (code_cache) {
    stack_walker_ = StackWalker::Create(code_cache);
  }
  if (!stack_walker_) {
    // TODO(benvanik): disable features.
    if (cvars::debug) {
      XELOGW("Disabling --debug due to lack of stack walker");
      cvars::debug = false;
    }
  }

  // Open the trace data path, if requested.
  functions_trace_path_ = cvars::trace_function_data_path;
  if (!functions_trace_path_.empty()) {
    functions_trace_file_ =
        ChunkedMappedMemoryWriter::Open(functions_trace_path_, 32_MiB, true);
  }

  return true;
}

void Processor::PreLaunch() {
  if (cvars::break_on_start) {
    // Start paused.
    XELOGI("Breaking into debugger because of --break_on_start...");
    execution_state_ = ExecutionState::kRunning;
    Pause();
  } else {
    // Start running.
    execution_state_ = ExecutionState::kRunning;
  }
}

bool Processor::AddModule(std::unique_ptr<Module> module) {
  auto global_lock = global_critical_region_.Acquire();
  // LookupModule answers with the FIRST module whose range contains an
  // address, so a module left behind by a previous title shadows the one
  // being added at the same base and every function resolves against the
  // dead image.
  bool superseded = false;
  for (const auto& existing : modules_) {
    if (existing->name() == module->name()) {
      superseded = true;
      break;
    }
  }
  XELOGI("Processor::AddModule('{}'), {} module(s) loaded", module->name(),
         modules_.size() + 1);
  if (superseded) {
    // A module of this name is still registered from a previous title. It is
    // NOT erased: a retained XModule keeps a raw processor_module_ pointer at
    // it, and destroying it here leaves that dangling. Putting the new one in
    // front is enough, because both lookups take the first match - the live
    // image wins every name and address resolution, and the dead one is only
    // reachable through the pointer that already had it.
    XELOGD(
        "Processor::AddModule('{}'): superseding a module of that name left "
        "by a previous title",
        module->name());
    modules_.insert(modules_.begin(), std::move(module));
  } else {
    modules_.push_back(std::move(module));
  }
  return true;
}

void Processor::RemoveModule(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();

  auto itr =
      std::find_if(modules_.cbegin(), modules_.cend(),
                   [name](std::unique_ptr<xe::cpu::Module> const& module) {
                     return module->name() == name;
                   });

  if (itr != modules_.cend()) {
    const std::vector<uint32_t> addressed_functions =
        (*itr)->GetAddressedFunctions();

    XELOGI("Processor::RemoveModule('{}'), {} function(s), {} module(s) left",
           name, addressed_functions.size(), modules_.size() - 1);
    modules_.erase(itr);

    for (const uint32_t entry : addressed_functions) {
      RemoveFunctionByAddress(entry);
    }
  }
}

Module* Processor::GetModule(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& module : modules_) {
    if (module->name() == name) {
      return module.get();
    }
  }
  return nullptr;
}

std::vector<Module*> Processor::GetModules() {
  auto global_lock = global_critical_region_.Acquire();
  // reserve, not resize: sizing it first left that many null entries in front
  // of the real ones, and every caller iterates the whole vector.
  std::vector<Module*> clone;
  clone.reserve(modules_.size());
  for (const auto& module : modules_) {
    clone.push_back(module.get());
  }
  return clone;
}

Function* Processor::DefineBuiltin(const std::string_view name,
                                   BuiltinFunction::Handler handler, void* arg0,
                                   void* arg1) {
  uint32_t address = next_builtin_address_;
  next_builtin_address_ += 4;

  Function* function;
  builtin_module_->DeclareFunction(address, &function);
  function->set_end_address(address + 4);
  function->set_name(name);

  auto builtin_function = static_cast<BuiltinFunction*>(function);
  builtin_function->SetupBuiltin(handler, arg0, arg1);

  function->set_status(Symbol::Status::kDeclared);
  return function;
}

Function* Processor::QueryFunction(uint32_t address) {
  auto entry = entry_table_.Get(address);
  if (!entry) {
    return nullptr;
  }
  return entry->function;
}

std::vector<Function*> Processor::FindFunctionsWithAddress(uint32_t address) {
  return entry_table_.FindWithAddress(address);
}

void Processor::RemoveFunctionByAddress(uint32_t address) {
  entry_table_.Delete(address);
}

void Processor::FlushCode() {
  auto global_lock = global_critical_region_.Acquire();
  entry_table_.Clear();
  for (const auto& module : modules_) {
    module->ForEachFunction([](Function* function) {
      if (function->is_guest() &&
          function->status() == Symbol::Status::kDefined) {
        function->set_status(Symbol::Status::kDeclared);
      }
    });
  }
  // Dynamic code belongs to the title that compiled it, and its functions
  // point into the code freed below - a relaunched runtime called straight
  // into the old cache. The next title that enters user mode enables it again
  // when its imports bind.
  if (dynamic_code_module_) {
    XELOGI(
        "Processor::FlushCode: dynamic code translated {} time(s) in the "
        "background, {} on guest threads, {} again after a sweep mid-way",
        background_translated_.exchange(0), guest_translated_.exchange(0),
        stale_retranslated_.exchange(0));
  }
  dynamic_calls_user_mode_.store(-1, std::memory_order_relaxed);
  dynamic_code_module_.reset();
  dynamic_code_enabled_.store(false, std::memory_order_relaxed);
  keep_dynamic_context_stores_.store(false, std::memory_order_relaxed);
  swept_code_ranges_.clear();
  dynamic_code_pieces_.clear();
  if (backend_) {
    // Every thread's cache of dynamic call targets points into it too.
    backend_->InvalidateDynamicCalls(0, UINT32_MAX);
  }
  auto* code_cache = backend_ ? backend_->code_cache() : nullptr;
  const size_t flushed = code_cache ? code_cache->Flush() : 0;
  XELOGI(
      "Processor::FlushCode released {} bytes of generated code, {} "
      "module(s) still loaded",
      flushed, modules_.size());
  for (const auto& module : modules_) {
    XELOGI("   surviving module '{}'", module->name());
  }
  // Paused since teardown began; nothing is queued until the next title
  // enables dynamic code again.
  ResumeBackgroundTranslation();
}

void Processor::InvalidateCodeRange(uint32_t address, uint32_t length) {
  if (!length) {
    return;
  }
  const uint32_t end = address + length - 1;
  auto global_lock = global_critical_region_.Acquire();
  RecordSweptCode(address, length);
  {
    // Published after the record, for a translation to find on install.
    const uint64_t generation = sweep_generation_.load() + 1;
    SweepRecord& record = sweep_records_[generation % kSweepRecordCount];
    record.generation = generation;
    record.start_key = CodeRangeKey(address);
    record.end_key = CodeRangeKey(end) == record.start_key + (length - 1)
                         ? record.start_key + length
                         : 0;
    sweep_generation_.store(generation);
  }
  auto forget = [this, address, end](Function* function) {
    if (!function) {
      return;
    }
    // The entry is what a call looks up, but the module would hand back the
    // same already defined symbol and never compile the new code.
    if (function->module()) {
      function->module()->ForgetSymbol(function->address());
    }
    // Live dynamic code was patched, most often a call site the runtime bound,
    // and is about to be called again: translate the new code ahead of it.
    if (function->module() &&
        function->module() == dynamic_code_module_.get()) {
      QueueBackgroundTranslation(function->address());
    }
    // The dynamic call cache keys a function by its entry, which the range
    // misses for a patch inside it or code compiled under another name.
    if (function->address() < address || function->address() > end) {
      backend_->InvalidateDynamicCalls(function->address(),
                                       function->address());
    }
  };
  for (Function* function : entry_table_.DeleteRange(address, end)) {
    forget(function);
  }
  // Dynamic code compiled under another name for the same memory.
  if (!dynamic_code_pieces_.empty()) {
    for (uint64_t page = address & ~uint64_t(0xFFF); page <= end;
         page += 0x1000) {
      const uint32_t piece = uint32_t(std::max<uint64_t>(page, address));
      const uint64_t key = CodeRangeKey(piece);
      const uint64_t end_key =
          key + (std::min<uint64_t>(page + 0x1000, uint64_t(end) + 1) - piece);
      // No piece crosses a page, which makes the page's first key the lowest
      // that can overlap.
      auto it = dynamic_code_pieces_.lower_bound(key & ~uint64_t(0xFFF));
      while (it != dynamic_code_pieces_.end() && it->first < end_key) {
        if (it->second.end_key <= key) {
          ++it;
          continue;
        }
        const uint32_t function_address = it->second.function_address;
        it = dynamic_code_pieces_.erase(it);
        for (Function* function :
             entry_table_.DeleteRange(function_address, function_address)) {
          forget(function);
        }
      }
    }
  }
  backend_->InvalidateDynamicCalls(address, end);
}

void Processor::RecordDynamicCode(const Function* function) {
  const uint64_t start = function->address();
  // The end address is the last instruction's.
  const uint64_t end = std::max<uint64_t>(function->end_address(), start) + 4;
  auto global_lock = global_critical_region_.Acquire();
  for (uint64_t piece = start; piece < end;) {
    const uint64_t piece_end =
        std::min<uint64_t>((piece & ~uint64_t(0xFFF)) + 0x1000, end);
    const uint64_t key = CodeRangeKey(uint32_t(piece));
    const uint64_t end_key = key + (piece_end - piece);
    // A recompile of the same function reuses its piece rather than piling up.
    auto [first, last] = dynamic_code_pieces_.equal_range(key);
    auto same = std::find_if(first, last, [function](const auto& existing) {
      return existing.second.function_address == function->address();
    });
    if (same != last) {
      same->second.end_key = std::max(same->second.end_key, end_key);
    } else {
      dynamic_code_pieces_.emplace(
          key, DynamicCodePiece{end_key, function->address()});
    }
    piece = piece_end;
  }
}

void Processor::RecordSweptCode(uint32_t address, uint32_t length) {
  if (!dynamic_code_enabled_.load()) {
    // Only dynamic code is bounded this way, and nothing prunes the map.
    return;
  }
  // Spans are keyed by physical address, so a range whose pages aren't
  // physically contiguous would claim someone else's memory.
  const uint64_t span_start = CodeRangeKey(address);
  const uint64_t span_end = span_start + length;
  if (CodeRangeKey(address + length - 1) != span_end - 1) {
    return;
  }
  // Spans stay exact rather than merging, since back-to-back blocks must not
  // bound each other. A sweep replaces any span it overlaps, except a single
  // instruction patched past a span's start, which keeps the block's end.
  auto range = swept_code_ranges_.lower_bound(span_start);
  if (range != swept_code_ranges_.begin()) {
    auto previous = range;
    --previous;
    if (previous->second > span_start) {
      if (length == 4 && previous->second >= span_end) {
        return;
      }
      range = previous;
    }
  }
  while (range != swept_code_ranges_.end() && range->first < span_end) {
    range = swept_code_ranges_.erase(range);
  }
  swept_code_ranges_.emplace(span_start, span_end);
}

uint64_t Processor::CodeRangeKey(uint32_t address) {
  const uint32_t kernel_address = memory_->UserModeKernelAddress(address);
  auto heap = memory_->LookupHeap(kernel_address);
  if (heap && heap->heap_type() == HeapType::kGuestPhysical) {
    const uint32_t physical_address =
        memory_->GetPhysicalAddress(kernel_address);
    if (physical_address != UINT32_MAX) {
      return kPhysicalCodeKey | physical_address;
    }
  }
  return kernel_address;
}

uint32_t Processor::SweptCodeEnd(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  const uint64_t key = CodeRangeKey(address);
  auto range = swept_code_ranges_.upper_bound(key);
  if (range == swept_code_ranges_.begin()) {
    return 0;
  }
  --range;
  if (key >= range->second) {
    return 0;
  }
  // The range may be keyed by another address for the same memory, so convert
  // back to this one.
  return address + uint32_t(range->second - key);
}

Function* Processor::ResolveFunction(uint32_t address) {
  Entry* entry;
  Entry::Status status = entry_table_.GetOrCreate(address, &entry);
  if (status == Entry::STATUS_NEW) {
    // Needs to be generated. We have the 'lock' on it and must do so now.
    status = CompileEntry(entry, address, false);
  }
  if (status == Entry::STATUS_READY) {
    // Ready to use.
    return entry->function;
  } else {
    // Failed or bad state.
    return nullptr;
  }
}

Entry::Status Processor::CompileEntry(Entry* entry, uint32_t address,
                                      bool background) {
  // Past this many sweeps of the same code mid-translation the guest is
  // rewriting it as fast as it can be read; install what was read, as before.
  constexpr int kMaxAttempts = 4;
  for (int attempt = 1;; ++attempt) {
    const uint64_t generation = sweep_generation_.load();

    // Grab symbol declaration.
    auto function = LookupFunction(address);

    if (!function) {
      entry->status = Entry::STATUS_FAILED;
      return Entry::STATUS_FAILED;
    }

    if (!DemandFunction(function)) {
      entry->status = Entry::STATUS_FAILED;
      return Entry::STATUS_FAILED;
    }
    // only add it to the list of resolved functions if resolving succeeded
    auto module_for = function->module();

    auto xexmod = dynamic_cast<XexModule*>(module_for);
    if (xexmod) {
      auto addr_flags = xexmod->GetInstructionAddressFlags(address);
      if (addr_flags) {
        addr_flags->was_resolved = 1;
      }
    }

    // A sweep must not find dynamic code ready before its pieces are recorded.
    auto global_lock = global_critical_region_.Acquire();
    const bool dynamic = module_for == dynamic_code_module_.get();
    // DeleteRange passes over an entry that is still compiling, so a sweep
    // while this read the code didn't drop it. Only dynamic code is checked:
    // it has no indirection slot, so nothing could have called this yet.
    if (dynamic && attempt < kMaxAttempts && SweptSince(generation, function)) {
      module_for->ForgetSymbol(address);
      ++stale_retranslated_;
      continue;
    }
    entry->function = function;
    entry->end_address = function->end_address();
    entry->status = Entry::STATUS_READY;
    if (dynamic) {
      RecordDynamicCode(function);
      ++(background ? background_translated_ : guest_translated_);
      if (!background) {
        dynamic_calls_user_mode_.store(
            memory_->UserModeKernelAddress(address) != address ? 1 : 0,
            std::memory_order_relaxed);
      }
    }
    return Entry::STATUS_READY;
  }
}

bool Processor::SweptSince(uint64_t generation, const Function* function) {
  const uint64_t current = sweep_generation_.load();
  if (current == generation) {
    return false;
  }
  if (current - generation > kSweepRecordCount) {
    // The records no longer reach back that far.
    return true;
  }
  const uint64_t start = function->address();
  const uint64_t end = std::max<uint64_t>(function->end_address(), start) + 4;
  for (uint64_t g = generation + 1; g <= current; ++g) {
    const SweepRecord& record = sweep_records_[g % kSweepRecordCount];
    if (record.generation != g || !record.end_key) {
      return true;
    }
    // Compared by key, page by page, as RecordDynamicCode records it.
    for (uint64_t piece = start; piece < end;) {
      const uint64_t piece_end =
          std::min<uint64_t>((piece & ~uint64_t(0xFFF)) + 0x1000, end);
      const uint64_t key = CodeRangeKey(uint32_t(piece));
      if (key < record.end_key &&
          key + (piece_end - piece) > record.start_key) {
        return true;
      }
      piece = piece_end;
    }
  }
  return false;
}

void Processor::OnCodeSwept(uint32_t address, uint32_t length) {
  // A single instruction is a patch inside code that already exists, whose
  // function InvalidateCodeRange has queued again.
  if (length <= 4 || !dynamic_code_enabled_.load()) {
    return;
  }
  // Translate the code under the name the guest calls it by. A sweep through
  // a user mode address can be turned into the kernel one, and the 0xA0000000
  // window back into its user alias; a user page table mapping can't be
  // inverted, so then there is nothing to translate ahead.
  const int user_mode =
      dynamic_calls_user_mode_.load(std::memory_order_relaxed);
  const uint32_t kernel_address = memory_->UserModeKernelAddress(address);
  uint32_t target = address;
  if (user_mode == 0) {
    target = kernel_address;
  } else if (user_mode == 1 && kernel_address == address) {
    target = xe::Memory::KernelModeUserAddress(address);
    if (target == address) {
      return;
    }
  }
  QueueBackgroundTranslation(target);
}

void Processor::QueueBackgroundTranslation(uint32_t address) {
  std::lock_guard<std::mutex> lock(background_mutex_);
  if (background_threads_.empty() || background_paused_ ||
      background_stopping_ || !background_queued_.insert(address).second) {
    return;
  }
  background_queue_.push_back(address);
  background_work_.notify_one();
}

void Processor::StartBackgroundTranslation() {
  std::lock_guard<std::mutex> lock(background_mutex_);
  if (!background_threads_.empty() || cvars::jit_background_threads <= 0) {
    return;
  }
  const int count = std::min(cvars::jit_background_threads, 8);
  for (int i = 0; i < count; ++i) {
    xe::threading::Thread::CreationParameters params;
    // Below the guest threads: this only gets ahead of them.
    params.initial_priority = -1;
    auto thread = xe::threading::Thread::Create(
        params, [this]() { BackgroundTranslationMain(); });
    if (!thread) {
      break;
    }
    thread->set_name(fmt::format("JIT Translator {}", i));
    background_threads_.push_back(std::move(thread));
  }
  XELOGI("Processor: {} background translation thread(s)",
         background_threads_.size());
}

void Processor::StopBackgroundTranslation() {
  {
    std::lock_guard<std::mutex> lock(background_mutex_);
    background_stopping_ = true;
    background_queue_.clear();
    background_queued_.clear();
  }
  background_work_.notify_all();
  for (auto& thread : background_threads_) {
    xe::threading::Wait(thread.get(), false);
  }
  background_threads_.clear();
}

void Processor::PauseBackgroundTranslation() {
  std::unique_lock<std::mutex> lock(background_mutex_);
  background_paused_ = true;
  background_queue_.clear();
  background_queued_.clear();
  // A translation in flight finishes against code that is still there.
  background_idle_.wait(lock, [this]() { return !background_busy_; });
}

void Processor::ResumeBackgroundTranslation() {
  std::lock_guard<std::mutex> lock(background_mutex_);
  background_paused_ = false;
}

void Processor::BackgroundTranslationMain() {
  while (true) {
    uint32_t address;
    {
      std::unique_lock<std::mutex> lock(background_mutex_);
      background_work_.wait(lock, [this]() {
        return background_stopping_ ||
               (!background_paused_ && !background_queue_.empty());
      });
      if (background_stopping_) {
        return;
      }
      address = background_queue_.front();
      background_queue_.pop_front();
      background_queued_.erase(address);
      ++background_busy_;
    }
    // Only code no module claims: a loaded module's functions are found
    // before the dynamic module, and modules can be unloaded under us.
    Module* module = LookupModule(address);
    if (module && module == dynamic_code_module_.get()) {
      Entry* entry;
      if (entry_table_.GetOrCreate(address, &entry) == Entry::STATUS_NEW) {
        CompileEntry(entry, address, true);
      }
    }
    {
      std::lock_guard<std::mutex> lock(background_mutex_);
      if (!--background_busy_) {
        background_idle_.notify_all();
      }
    }
  }
}
Module* Processor::LookupModule(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  // TODO(benvanik): sort by code address (if contiguous) so can bsearch.
  // TODO(benvanik): cache last module low/high, as likely to be in there.
  for (const auto& module : modules_) {
    if (module->ContainsAddress(address)) {
      return module.get();
    }
  }
  if (dynamic_code_module_ && dynamic_code_module_->ContainsAddress(address)) {
    return dynamic_code_module_.get();
  }
  return nullptr;
}

void Processor::EnableDynamicCode() {
  auto global_lock = global_critical_region_.Acquire();
  if (!dynamic_code_module_) {
    dynamic_code_module_ = std::make_unique<DynamicCodeModule>(this);
  }
  dynamic_code_enabled_.store(true, std::memory_order_relaxed);
  // Only a title that writes its own code gets translator threads.
  StartBackgroundTranslation();
}

Function* Processor::LookupFunction(uint32_t address) {
  // TODO(benvanik): fast reject invalid addresses/log errors.

  // Find the module that contains the address.
  Module* code_module = LookupModule(address);

  if (!code_module) {
    // No module found that could contain the address.
    return nullptr;
  }

  return LookupFunction(code_module, address);
}

Function* Processor::LookupFunction(Module* module, uint32_t address) {
  // Atomic create/lookup symbol in module.
  // If we get back the NEW flag we must declare it now.
  Function* function = nullptr;
  auto symbol_status = module->DeclareFunction(address, &function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undeclared, so declare now.
    assert_true(function->is_guest());
    if (!frontend_->DeclareFunction(static_cast<GuestFunction*>(function))) {
      function->set_status(Symbol::Status::kFailed);
      return nullptr;
    }
    function->set_status(Symbol::Status::kDeclared);
  }
  return function;
}

bool Processor::DemandFunction(Function* function) {
  // Lock function for generation. If it's already being generated
  // by another thread this will block and return DECLARED.
  auto module = function->module();
  auto symbol_status = module->DefineFunction(function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undefined, so define now.
    assert_true(function->is_guest());
    if (!frontend_->DefineFunction(static_cast<GuestFunction*>(function),
                                   debug_info_flags_)) {
      function->set_status(Symbol::Status::kFailed);
      return false;
    }

    // Before we give the symbol back to the rest, let the debugger know.
    OnFunctionDefined(function);

    function->set_status(Symbol::Status::kDefined);
    symbol_status = function->status();
  }

  if (symbol_status == Symbol::Status::kFailed) {
    // Symbol likely failed.
    return false;
  }

  return true;
}

bool Processor::Execute(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  auto function = ResolveFunction(address);
  if (!function) {
    // Symbol not found in any module.
    XELOGCPU("Execute({:08X}): failed to find function", address);
    return false;
  }

  auto context = thread_state->context();

  // Pad out stack a bit, as some games seem to overwrite the caller by about
  // 16 to 32b.
  context->r[1] -= 64 + 112;

  // This could be set to anything to give us a unique identifier to track
  // re-entrancy/etc.
  uint64_t previous_lr = context->lr;
  context->lr = 0xBCBCBCBC;

  // Execute the function.
  auto result = function->Call(thread_state, uint32_t(context->lr));

  context->lr = previous_lr;
  context->r[1] += 64 + 112;

  return result;
}

bool Processor::ExecuteRaw(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  auto function = ResolveFunction(address);
  if (!function) {
    // Symbol not found in any module.
    XELOGCPU("Execute({:08X}): failed to find function", address);
    return false;
  }

  return function->Call(thread_state, 0xBCBCBCBC);
}

uint64_t Processor::Execute(ThreadState* thread_state, uint32_t address,
                            uint64_t args[], size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");

  auto context = thread_state->context();
  for (size_t i = 0; i < std::min(arg_count, static_cast<size_t>(8)); ++i) {
    context->r[3 + i] = args[i];
  }

  if (arg_count > 7) {
    // Rest of the arguments go on the stack.
    // FIXME: This assumes arguments are 32 bits!
    auto stack_arg_base =
        memory()->TranslateVirtual((uint32_t)context->r[1] + 0x54 - (64 + 112));
    for (size_t i = 0; i < arg_count - 8; i++) {
      xe::store_and_swap<uint32_t>(stack_arg_base + (i * 8),
                                   (uint32_t)args[i + 8]);
    }
  }

  if (!Execute(thread_state, address)) {
    return 0xDEADBABE;
  }
  return context->r[3];
}

bool Processor::Save(ByteStream* stream) {
  stream->Write(kProcessorSaveSignature);
  return true;
}

bool Processor::Restore(ByteStream* stream) {
  if (stream->Read<uint32_t>() != kProcessorSaveSignature) {
    XELOGE("Processor::Restore - Invalid magic value!");
    return false;
  }

  // Clear cached thread data for zombie threads.
  std::vector<uint32_t> to_delete;
  for (auto& it : thread_debug_infos_) {
    if (it.second->state == ThreadDebugInfo::State::kZombie) {
      it.second->thread_handle = 0;
      to_delete.push_back(it.first);
    }
  }
  for (uint32_t thread_id : to_delete) {
    thread_debug_infos_.erase(thread_id);
  }

  return true;
}

uint8_t* Processor::AllocateFunctionTraceData(size_t size) {
  if (!functions_trace_file_) {
    return nullptr;
  }
  return functions_trace_file_->Allocate(size);
}

void Processor::OnFunctionDefined(Function* function) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    if (breakpoint->address_type() == Breakpoint::AddressType::kGuest) {
      if (function->ContainsAddress(breakpoint->guest_address())) {
        if (breakpoint->is_installed()) {
          backend_->InstallBreakpoint(breakpoint, function);
        }
      }
    }
  }
}

void Processor::OnThreadCreated(uint32_t thread_handle,
                                ThreadState* thread_state, Thread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto thread_info = std::make_unique<ThreadDebugInfo>();
  thread_info->thread_id = thread_state->thread_id();
  thread_info->thread = thread;
  thread_info->state = ThreadDebugInfo::State::kAlive;
  thread_info->suspended = false;
  thread_info->thread_handle = thread_handle;
  thread_debug_infos_.emplace(thread_info->thread_id, std::move(thread_info));
}

void Processor::OnThreadExit(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  thread_info->state = ThreadDebugInfo::State::kExited;
}

void Processor::OnThreadDestroyed(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  it->second->thread_handle = 0;
  thread_debug_infos_.erase(it);
}

void Processor::OnThreadEnteringWait(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  thread_info->state = ThreadDebugInfo::State::kWaiting;
}

void Processor::OnThreadLeavingWait(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  if (thread_info->state == ThreadDebugInfo::State::kWaiting) {
    thread_info->state = ThreadDebugInfo::State::kAlive;
  }
}

std::vector<ThreadDebugInfo*> Processor::QueryThreadDebugInfos() {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<ThreadDebugInfo*> result;
  for (auto& it : thread_debug_infos_) {
    result.push_back(it.second.get());
  }
  return result;
}

ThreadDebugInfo* Processor::QueryThreadDebugInfo(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  const auto& it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    return nullptr;
  }
  return it->second.get();
}

void Processor::AddBreakpoint(Breakpoint* breakpoint) {
  auto global_lock = global_critical_region_.Acquire();

  // Add to breakpoints map.
  breakpoints_.push_back(breakpoint);

  if (execution_state_ == ExecutionState::kRunning) {
    breakpoint->Resume();
  }
}

void Processor::RemoveBreakpoint(Breakpoint* breakpoint) {
  auto global_lock = global_critical_region_.Acquire();

  // Uninstall (if needed).
  if (execution_state_ == ExecutionState::kRunning) {
    breakpoint->Suspend();
  }

  // Remove from breakpoint map.
  auto it = std::find(breakpoints_.begin(), breakpoints_.end(), breakpoint);
  breakpoints_.erase(it);
}

Breakpoint* Processor::FindBreakpoint(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    if (breakpoint->address() == address) {
      return breakpoint;
    }
  }
  return nullptr;
}

void Processor::set_debug_listener(DebugListener* debug_listener) {
  if (debug_listener == debug_listener_) {
    return;
  }
  if (debug_listener_) {
    // Detach old debug listener.
    debug_listener_->OnDetached();
    debug_listener_ = nullptr;
  }
  if (debug_listener) {
    debug_listener_ = debug_listener;
  } else {
    if (execution_state_ == ExecutionState::kPaused) {
      XELOGI("Debugger detaching while execution is paused; continuing...");
      Continue();
    }
  }
}

void Processor::DemandDebugListener() {
  if (debug_listener_) {
    // Already present.
    debug_listener_->OnFocus();
    return;
  }
  if (!debug_listener_handler_) {
    XELOGE("Debugger demanded a listener but no handler was registered.");
    xe::debugging::Break();
    return;
  }
  set_debug_listener(debug_listener_handler_(this));
}

bool Processor::OnThreadBreakpointHit(Exception* ex) {
  auto global_lock = global_critical_region_.Acquire();

  // Suspend all threads (but ourselves).
  SuspendAllThreads();

  // Lookup thread info block.
  auto it = thread_debug_infos_.find(ThreadState::GetThreadID());
  if (it == thread_debug_infos_.end()) {
    // Not found - exception on a thread we don't know about?
    assert_always("UD2 on a thread we don't track");
    return false;
  }
  auto thread_info = it->second.get();

  // Run through and uninstall all breakpoint UD2s to get us back to a clean
  // state.
  if (execution_state_ != ExecutionState::kStepping) {
    SuspendAllBreakpoints();
  }

  // Update all thread states with their latest values, using the context we
  // got from the exception instead of a sampled value (as it would just show
  // the exception handler).
  UpdateThreadExecutionStates(thread_info->thread_id, ex->thread_context());

  // Walk the captured thread stack and look for breakpoints at any address in
  // the stack. We just look for the first one.
  Breakpoint* breakpoint = nullptr;
  for (size_t i = 0; i < thread_info->frames.size(); ++i) {
    auto& frame = thread_info->frames[i];
    for (auto scan_breakpoint : breakpoints_) {
      if ((scan_breakpoint->address_type() == Breakpoint::AddressType::kGuest &&
           scan_breakpoint->guest_address() == frame.guest_pc) ||
          (scan_breakpoint->address_type() == Breakpoint::AddressType::kHost &&
           scan_breakpoint->host_address() == frame.host_pc)) {
        breakpoint = scan_breakpoint;
        break;
      }
    }
    if (breakpoint) {
      breakpoint->OnHit(thread_info, frame.host_pc);
      break;
    }
  }

  // We are waiting on the debugger now. Either wait for it to continue, add a
  // new step, or direct us somewhere else.
  // The debugger will ResumeAllThreads or just resume us (depending on what
  // it wants to do).
  execution_state_ = ExecutionState::kPaused;
  thread_info->suspended = true;

  // Must unlock, or we will deadlock.
  global_lock.unlock();

  if (debug_listener_) {
    debug_listener_->OnExecutionPaused();
  }

  ResumeAllThreads();
  thread_info->thread->thread()->Suspend();

  // Apply thread context changes.
  // TODO(benvanik): apply to all threads?
#if XE_ARCH_AMD64
  ex->set_resume_pc(thread_info->host_context.rip + 2);
#elif XE_ARCH_ARM64
  ex->set_resume_pc(thread_info->host_context.pc + 2);
#else
#error Instruction pointer not specified for the target CPU architecture.
#endif  // XE_ARCH

  // Resume execution.
  return true;
}

void Processor::OnStepCompleted(ThreadDebugInfo* thread_info) {
  auto global_lock = global_critical_region_.Acquire();
  execution_state_ = ExecutionState::kPaused;
  if (debug_listener_) {
    debug_listener_->OnExecutionPaused();
  }

  // Note that we stay suspended.
}

bool Processor::OnUnhandledException(Exception* ex) {
  // If we have no listener return right away.
  // TODO(benvanik): DemandDebugListener()?
  if (!debug_listener_) {
    return false;
  }

  // If this isn't a managed thread, fail - let VS handle it for now.
  if (!Thread::IsInThread()) {
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();

  // Suspend all guest threads (but this one).
  SuspendAllThreads();

  UpdateThreadExecutionStates(Thread::GetCurrentThreadId(),
                              ex->thread_context());

  // Stop and notify the listener.
  // This will take control.
  assert_true(execution_state_ == ExecutionState::kRunning);
  execution_state_ = ExecutionState::kPaused;

  // Notify debugger that exceution stopped.
  // debug_listener_->OnException(info);
  debug_listener_->OnExecutionPaused();

  // Suspend self.
  Thread::GetCurrentThread()->thread()->Suspend();

  return true;
}

void Processor::ShowDebugger() {
  if (debug_listener_) {
    debug_listener_->OnFocus();
  } else {
    DemandDebugListener();
  }
}

bool Processor::SuspendAllThreads() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    if (thread_info->suspended) {
      // Already suspended - ignore.
      continue;
    } else if (thread_info->state == ThreadDebugInfo::State::kZombie ||
               thread_info->state == ThreadDebugInfo::State::kExited) {
      // Thread is dead and cannot be suspended - ignore.
      continue;
    } else if (Thread::IsInThread() &&
               thread_info->thread_id == Thread::GetCurrentThreadId()) {
      // Can't suspend ourselves.
      continue;
    }
    auto thread = thread_info->thread;
    if (!thread->can_debugger_suspend()) {
      // Thread is a host thread, and we aren't suspending those (for now).
      continue;
    }
    bool did_suspend = thread->thread()->Suspend(nullptr);
    assert_true(did_suspend);
    thread_info->suspended = true;
  }
  return true;
}

bool Processor::ResumeThread(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    return false;
  }
  auto thread_info = it->second.get();
  assert_true(thread_info->suspended);
  assert_false(thread_info->state == ThreadDebugInfo::State::kExited ||
               thread_info->state == ThreadDebugInfo::State::kZombie);
  thread_info->suspended = false;
  auto thread = thread_info->thread;
  return thread->thread()->Resume();
}

bool Processor::ResumeAllThreads() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    if (!thread_info->suspended) {
      // Not suspended by us - ignore.
      continue;
    } else if (thread_info->state == ThreadDebugInfo::State::kZombie ||
               thread_info->state == ThreadDebugInfo::State::kExited) {
      // Thread is dead and cannot be resumed - ignore.
      continue;
    } else if (Thread::IsInThread() &&
               thread_info->thread_id == Thread::GetCurrentThreadId()) {
      // Can't resume ourselves.
      continue;
    }
    auto thread = thread_info->thread;
    if (!thread->can_debugger_suspend()) {
      // Thread is a host thread, and we aren't suspending those (for now).
      continue;
    }
    thread_info->suspended = false;
    bool did_resume = thread->thread()->Resume();
    assert_true(did_resume);
  }
  return true;
}

void Processor::UpdateThreadExecutionStates(
    uint32_t override_thread_id, HostThreadContext* override_context) {
  auto global_lock = global_critical_region_.Acquire();
  uint64_t frame_host_pcs[64];
  xe::cpu::StackFrame cpu_frames[64];
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    auto thread = thread_info->thread;
    if (!thread) {
      continue;
    }

    // Grab PPC context.
    // Note that this is only up to date if --store_all_context_values is
    // enabled (or --debug).
    if (thread->can_debugger_suspend()) {
      std::memcpy(&thread_info->guest_context,
                  thread->thread_state()->context(),
                  sizeof(thread_info->guest_context));
    }

    // Grab stack trace and X64 context then resolve all symbols.
    uint64_t hash;
    HostThreadContext* in_host_context = nullptr;
    if (override_thread_id == thread_info->thread_id) {
      // If we were passed an override context we use that. Otherwise, ask the
      // stack walker for a new context.
      in_host_context = override_context;
    }
    size_t count = stack_walker_->CaptureStackTrace(
        thread->thread()->native_handle(), frame_host_pcs, 0,
        xe::countof(frame_host_pcs), in_host_context,
        &thread_info->host_context, &hash);
    stack_walker_->ResolveStack(frame_host_pcs, cpu_frames, count);
    thread_info->frames.resize(count);
    for (size_t i = 0; i < count; ++i) {
      auto& cpu_frame = cpu_frames[i];
      auto& frame = thread_info->frames[i];
      frame.host_pc = cpu_frame.host_pc;
      frame.host_function_address = cpu_frame.host_symbol.address;
      frame.guest_pc = cpu_frame.guest_pc;
      frame.guest_function_address = 0;
      frame.guest_function = nullptr;
      auto function = cpu_frame.guest_symbol.function;
      if (cpu_frame.type == cpu::StackFrame::Type::kGuest && function) {
        frame.guest_function_address = function->address();
        frame.guest_function = function;
      } else {
        std::strncpy(frame.name, cpu_frame.host_symbol.name,
                     xe::countof(frame.name));
        frame.name[xe::countof(frame.name) - 1] = 0;
      }
    }
  }
}

void Processor::SuspendAllBreakpoints() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    breakpoint->Suspend();
  }
}

void Processor::ResumeAllBreakpoints() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    breakpoint->Resume();
  }
}

void Processor::Pause() {
  {
    auto global_lock = global_critical_region_.Acquire();
    assert_true(execution_state_ == ExecutionState::kRunning);
    SuspendAllThreads();
    SuspendAllBreakpoints();
    UpdateThreadExecutionStates();
    execution_state_ = ExecutionState::kPaused;
    if (debug_listener_) {
      debug_listener_->OnExecutionPaused();
    }
  }
  DemandDebugListener();
}

void Processor::Continue() {
  auto global_lock = global_critical_region_.Acquire();
  if (execution_state_ == ExecutionState::kRunning) {
    return;
  } else if (execution_state_ == ExecutionState::kStepping) {
    assert_always("cancel stepping not done yet");
  }
  execution_state_ = ExecutionState::kRunning;
  ResumeAllBreakpoints();
  ResumeAllThreads();

  if (debug_listener_) {
    debug_listener_->OnExecutionContinued();
  }
}

void Processor::StepHostInstruction(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  assert_true(execution_state_ == ExecutionState::kPaused);
  execution_state_ = ExecutionState::kStepping;

  auto thread_info = QueryThreadDebugInfo(thread_id);
  uint64_t new_host_pc = backend_->CalculateNextHostInstruction(
      thread_info, thread_info->frames[0].host_pc);

  assert_null(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint.reset(
      new Breakpoint(this, Breakpoint::AddressType::kHost, new_host_pc,
                     [this, thread_info](Breakpoint* breakpoint,
                                         ThreadDebugInfo* breaking_thread_info,
                                         uint64_t host_address) {
                       if (thread_info != breaking_thread_info) {
                         assert_always("Step in another thread?");
                       }
                       // Our step request has completed. Remove the breakpoint
                       // and fire event.
                       breakpoint->Suspend();
                       RemoveBreakpoint(breakpoint);
                       thread_info->step_breakpoint.reset();
                       OnStepCompleted(thread_info);
                     }));
  AddBreakpoint(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint->Resume();

  // ResumeAllBreakpoints();
  ResumeThread(thread_id);
}

void Processor::StepGuestInstruction(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  assert_true(execution_state_ == ExecutionState::kPaused);
  execution_state_ = ExecutionState::kStepping;

  auto thread_info = QueryThreadDebugInfo(thread_id);

  uint32_t next_pc = CalculateNextGuestInstruction(
      thread_info, thread_info->frames[0].guest_pc);

  assert_null(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint.reset(
      new Breakpoint(this, Breakpoint::AddressType::kGuest, next_pc,
                     [this, thread_info](Breakpoint* breakpoint,
                                         ThreadDebugInfo* breaking_thread_info,
                                         uint64_t host_address) {
                       if (thread_info != breaking_thread_info) {
                         assert_always("Step in another thread?");
                       }
                       // Our step request has completed. Remove the breakpoint
                       // and fire event.
                       breakpoint->Suspend();
                       RemoveBreakpoint(breakpoint);
                       thread_info->step_breakpoint.reset();
                       OnStepCompleted(thread_info);
                     }));
  AddBreakpoint(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint->Resume();

  // ResumeAllBreakpoints();
  ResumeThread(thread_id);
}

bool Processor::StepToGuestAddress(uint32_t thread_id, uint32_t pc) {
  auto functions = FindFunctionsWithAddress(pc);
  if (functions.empty()) {
    // Function hasn't been generated yet. Generate it.
    if (!ResolveFunction(pc)) {
      XELOGE(
          "Processor::StepToAddress({:08X}) - Function could not be resolved",
          pc);
      return false;
    }
  }

  // Instruct the thread to step forwards.
  threading::Fence fence;
  cpu::Breakpoint bp(
      this, Breakpoint::AddressType::kGuest, pc,
      [&fence](Breakpoint* breakpoint, ThreadDebugInfo* thread_info,
               uint64_t host_address) { fence.Signal(); });
  bp.Resume();

  // HACK
  auto thread_info = QueryThreadDebugInfo(thread_id);
  uint32_t suspend_count = 1;
  while (suspend_count) {
    thread_info->thread->thread()->Resume(&suspend_count);
  }

  fence.Wait();
  bp.Suspend();

  return true;
}

uint32_t Processor::StepIntoGuestBranchTarget(uint32_t thread_id, uint32_t pc) {
  xe::cpu::ppc::PPCDecodeData d;
  d.address = pc;
  d.code = xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(d.address));
  auto opcode = xe::cpu::ppc::LookupOpcode(d.code);

  // Must be on a branch.
  assert_true(xe::cpu::ppc::GetOpcodeInfo(opcode).group ==
              xe::cpu::ppc::PPCOpcodeGroup::kB);

  auto thread_info = QueryThreadDebugInfo(thread_id);
  auto thread = thread_info->thread;
  auto context = thread->thread_state()->context();

  if (d.code == 0x4E800020) {
    // blr
    uint32_t nia = uint32_t(context->lr);
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (d.code == 0x4E800420) {
    // bctr
    uint32_t nia = uint32_t(context->ctr);
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (opcode == PPCOpcode::bx) {
    // bx
    uint32_t nia = d.I.ADDR();
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (opcode == PPCOpcode::bcx || opcode == PPCOpcode::bcctrx ||
             opcode == PPCOpcode::bclrx) {
    threading::Fence fence;
    auto callback = [&fence, &pc](Breakpoint* breakpoint,
                                  ThreadDebugInfo* thread_info,
                                  uint64_t host_address) {
      pc = breakpoint->guest_address();
      fence.Signal();
    };

    cpu::Breakpoint bpf(this, Breakpoint::AddressType::kGuest, pc + 4,
                        callback);
    bpf.Resume();

    uint32_t nia = 0;
    if (opcode == PPCOpcode::bcx) {
      // bcx
      nia = d.B.ADDR();
    } else if (opcode == PPCOpcode::bcctrx) {
      // bcctrx
      nia = uint32_t(context->ctr);
    } else if (opcode == PPCOpcode::bclrx) {
      // bclrx
      nia = uint32_t(context->lr);
    }

    cpu::Breakpoint bpt(this, Breakpoint::AddressType::kGuest, nia, callback);
    bpt.Resume();

    // HACK
    uint32_t suspend_count = 1;
    while (suspend_count) {
      thread->thread()->Resume(&suspend_count);
    }

    fence.Wait();
    bpt.Suspend();
    bpf.Suspend();
  }

  return pc;
}

uint32_t Processor::StepToGuestSafePoint(uint32_t thread_id, bool ignore_host) {
  // This cannot be done if we're the calling thread!
  if (thread_id == ThreadState::GetThreadID()) {
    assert_always(
        "Processor::StepToSafePoint(): target thread is the calling thread!");
    return 0;
  }
  auto thread_info = QueryThreadDebugInfo(thread_id);
  auto thread = thread_info->thread;

  // Now the fun part begins: Registers are only guaranteed to be synchronized
  // with the PPC context at a basic block boundary. Unfortunately, we most
  // likely stopped the thread at some point other than a boundary. We need to
  // step forward until we reach a boundary, and then perform the save.
  uint64_t frame_host_pcs[64];
  cpu::StackFrame cpu_frames[64];
  size_t count = stack_walker_->CaptureStackTrace(
      thread->thread()->native_handle(), frame_host_pcs, 0,
      xe::countof(frame_host_pcs), nullptr, nullptr);
  stack_walker_->ResolveStack(frame_host_pcs, cpu_frames, count);
  if (count == 0) {
    return 0;
  }

  auto& first_frame = cpu_frames[0];
  if (ignore_host) {
    for (size_t i = 0; i < count; i++) {
      if (cpu_frames[i].type == cpu::StackFrame::Type::kGuest &&
          cpu_frames[i].guest_pc) {
        first_frame = cpu_frames[i];
        break;
      }
    }
  }

  // Check if we're in guest code or host code.
  uint32_t pc = 0;
  if (first_frame.type == cpu::StackFrame::Type::kGuest) {
    auto& frame = first_frame;
    if (!frame.guest_pc) {
      // Lame. The guest->host thunk is a "guest" function.
      frame = cpu_frames[1];
    }

    pc = frame.guest_pc;

    // We're in guest code.
    // First: Find a synchronizing instruction and go to it.
    xe::cpu::ppc::PPCDecodeData d;
    const xe::cpu::ppc::PPCOpcodeInfo* sync_info = nullptr;
    d.address = cpu_frames[0].guest_pc - 4;
    do {
      d.address += 4;
      d.code =
          xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(d.address));
      auto& opcode_info = xe::cpu::ppc::LookupOpcodeInfo(d.code);
      if (opcode_info.type == cpu::ppc::PPCOpcodeType::kSync) {
        sync_info = &opcode_info;
        break;
      }
    } while (true);

    if (d.address != pc) {
      StepToGuestAddress(thread_id, d.address);
      pc = d.address;
    }

    // Okay. Now we're on a synchronizing instruction but we need to step
    // past it in order to get a synchronized context.
    // If we're on a branching instruction, it's guaranteed only going to have
    // two possible targets. For non-branching instructions, we can just step
    // over them.
    if (sync_info->group == xe::cpu::ppc::PPCOpcodeGroup::kB) {
      pc = StepIntoGuestBranchTarget(thread_id, d.address);
    }
  } else {
    // We're in host code. Search backwards til we can get an idea of where
    // we are.
    cpu::GuestFunction* thunk_func = nullptr;
    cpu::Export* export_data = nullptr;
    uint32_t first_pc = 0;
    for (int i = 0; i < count; i++) {
      auto& frame = cpu_frames[i];
      if (frame.type == cpu::StackFrame::Type::kGuest && frame.guest_pc) {
        auto func = frame.guest_symbol.function;
        assert_true(func->is_guest());

        if (!first_pc) {
          first_pc = frame.guest_pc;
        }

        thunk_func = reinterpret_cast<cpu::GuestFunction*>(func);
        export_data = thunk_func->export_data();
        if (export_data) {
          break;
        }
      }
    }

    // If the export is blocking, we wrap up and save inside the export thunk.
    // When we're restored, we'll call the blocking export again.
    // Otherwise, we return from the thunk and save.
    if (export_data && export_data->tags & cpu::ExportTag::kBlocking) {
      pc = thunk_func->address();
    } else if (export_data) {
      // Non-blocking. Run until we return from the thunk.
      pc = static_cast<uint32_t>(thread->thread_state()->context()->lr);
      StepToGuestAddress(thread_id, pc);
    } else if (first_pc) {
      // We're in the MMIO handler/mfmsr/something calling out of the guest
      // that doesn't use an export. If the current instruction is
      // synchronizing, we can just save here. Otherwise, step forward
      // (and call ourselves again so we run the correct logic).
      uint32_t code =
          xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(first_pc));
      auto& opcode_info = xe::cpu::ppc::LookupOpcodeInfo(code);
      if (opcode_info.type == xe::cpu::ppc::PPCOpcodeType::kSync) {
        // Good to go.
        pc = first_pc;
      } else {
        // Step forward and run this logic again.
        StepToGuestAddress(thread_id, first_pc + 4);
        return StepToGuestSafePoint(thread_id, true);
      }
    } else {
      // We've managed to catch a thread before it called into the guest.
      // Set a breakpoint on its startup procedure and capture it there.
      // TODO(DrChat): Reimplement
      assert_always("Unimplemented");
      /*
      auto creation_params = thread->creation_params();
      pc = creation_params->xapi_thread_startup
               ? creation_params->xapi_thread_startup
               : creation_params->start_address;
      StepToGuestAddress(thread_id, pc);
      */
    }
  }

  return pc;
}

bool TestPpcCondition(const xe::cpu::ppc::PPCContext* context, uint32_t bo,
                      uint32_t bi, bool check_ctr, bool check_cond) {
  bool ctr_ok = true;
  if (check_ctr) {
    if (select_bits(bo, 2, 2)) {
      ctr_ok = true;
    } else {
      uint32_t new_ctr_value = static_cast<uint32_t>(context->ctr - 1);
      if (select_bits(bo, 1, 1)) {
        ctr_ok = new_ctr_value == 0;
      } else {
        ctr_ok = new_ctr_value != 0;
      }
    }
  }
  bool cond_ok = true;
  if (check_cond) {
    if (select_bits(bo, 4, 4)) {
      cond_ok = true;
    } else {
      uint8_t cr = *(reinterpret_cast<const uint8_t*>(&context->cr0) +
                     (4 * (bi >> 2)) + (bi & 3));
      if (select_bits(bo, 3, 3)) {
        cond_ok = cr != 0;
      } else {
        cond_ok = cr == 0;
      }
    }
  }
  return ctr_ok && cond_ok;
}

uint32_t Processor::CalculateNextGuestInstruction(ThreadDebugInfo* thread_info,
                                                  uint32_t current_pc) {
  xe::cpu::ppc::PPCDecodeData d;
  d.address = current_pc;
  d.code = xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(d.address));
  auto opcode = xe::cpu::ppc::LookupOpcode(d.code);
  if (d.code == 0x4E800020) {
    // blr -- unconditional branch to LR.
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.lr);
    return target_pc;
  } else if (d.code == 0x4E800420) {
    // bctr -- unconditional branch to CTR.
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.ctr);
    return target_pc;
  } else if (opcode == PPCOpcode::bx) {
    // b/ba/bl/bla
    uint32_t target_pc = d.I.ADDR();
    return target_pc;
  } else if (opcode == PPCOpcode::bcx) {
    // bc/bca/bcl/bcla
    uint32_t target_pc = d.B.ADDR();
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.B.BO(),
                                        d.B.BI(), true, true);
    return test_passed ? target_pc : current_pc + 4;
  } else if (opcode == PPCOpcode::bclrx) {
    // bclr/bclrl
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.lr);
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.XL.BO(),
                                        d.XL.BI(), true, true);
    return test_passed ? target_pc : current_pc + 4;
  } else if (opcode == PPCOpcode::bcctrx) {
    // bcctr/bcctrl
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.ctr);
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.XL.BO(),
                                        d.XL.BI(), false, true);
    return test_passed ? target_pc : current_pc + 4;
  } else {
    return current_pc + 4;
  }
}
uint32_t Processor::GuestAtomicIncrement32(ppc::PPCContext* context,
                                           uint32_t guest_address) {
  uint32_t* host_address = context->TranslateVirtual<uint32_t*>(guest_address);

  uint32_t result;
  while (true) {
    result = *host_address;
    // todo: should call a processor->backend function that acquires a
    // reservation instead of using host atomics
    if (xe::atomic_cas(result, xe::byte_swap(xe::byte_swap(result) + 1),
                       host_address)) {
      break;
    }
  }
  return xe::byte_swap(result);
}
uint32_t Processor::GuestAtomicDecrement32(ppc::PPCContext* context,
                                           uint32_t guest_address) {
  uint32_t* host_address = context->TranslateVirtual<uint32_t*>(guest_address);

  uint32_t result;
  while (true) {
    result = *host_address;
    // todo: should call a processor->backend function that acquires a
    // reservation instead of using host atomics
    if (xe::atomic_cas(result, xe::byte_swap(xe::byte_swap(result) - 1),
                       host_address)) {
      break;
    }
  }
  return xe::byte_swap(result);
}

uint32_t Processor::GuestAtomicOr32(ppc::PPCContext* context,
                                    uint32_t guest_address, uint32_t mask) {
  return xe::byte_swap(
      xe::atomic_or(context->TranslateVirtual<volatile int32_t*>(guest_address),
                    xe::byte_swap(mask)));
}
uint32_t Processor::GuestAtomicXor32(ppc::PPCContext* context,
                                     uint32_t guest_address, uint32_t mask) {
  return xe::byte_swap(xe::atomic_xor(
      context->TranslateVirtual<volatile int32_t*>(guest_address),
      xe::byte_swap(mask)));
}
uint32_t Processor::GuestAtomicAnd32(ppc::PPCContext* context,
                                     uint32_t guest_address, uint32_t mask) {
  return xe::byte_swap(xe::atomic_and(
      context->TranslateVirtual<volatile int32_t*>(guest_address),
      xe::byte_swap(mask)));
}

bool Processor::GuestAtomicCAS32(ppc::PPCContext* context, uint32_t old_value,
                                 uint32_t new_value, uint32_t guest_address) {
  return xe::atomic_cas(xe::byte_swap(old_value), xe::byte_swap(new_value),
                        context->TranslateVirtual<uint32_t*>(guest_address));
}
}  // namespace cpu
}  // namespace xe
