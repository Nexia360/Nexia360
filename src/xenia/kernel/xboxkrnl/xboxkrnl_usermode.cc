/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

// THE KERNEL'S USER MODE, ON HOST FIBERS.
//
// Behaviour follows xenia-edge's xboxkrnl_usermode.cc, which runs XNA titles;
// the fibers are raw Win32 ones rather than edge's boost.context fibers.
//
// The .NET Compact Framework in XNA titles runs managed code in user mode: a
// second address space (Memory::EnableUserModeViews), entered with
// KeEnterUserMode and left with KeLeaveUserMode, where sc traps back to a
// handler the kernel names. RuntimeHost drives it like this:
//
//   for (;;) {
//     r = KeEnterUserMode(&context, handler, 0, 0);
//     if (r != 1) break;                  // the handler left with 0: done
//     call the service the trap asked for, result into the kframes' r3;
//     KeContextFromKframes(&kframes, &context);   // iar = after the sc
//   }
//
// The handler either returns - user code resumes from the kframes, maybe
// redirected - or calls KeLeaveUserMode(1) to have the kernel service the
// trap, which then enters again at the instruction after the sc with the
// stack pointer it trapped with. Guest frames are host frames here, so that
// second entry must CONTINUE the trapped code where it stopped. So user code
// and the handler each run on a host fiber of their own, and the kernel's
// thread is a fiber too. Exactly one runs at a time. A fiber left with frames
// on it starts over at its procedure before it runs anything else (see
// FiberStart). Each fiber keeps its own stackpoint records, swapped into the
// context while it runs.

namespace xe {
namespace kernel {
namespace xboxkrnl {

using cpu::ppc::PPCContext;

namespace {

// CONTEXT fields.
constexpr uint32_t kContextFlags = 0x0;
constexpr uint32_t kContextMsr = 0x4;
constexpr uint32_t kContextIar = 0x8;
constexpr uint32_t kContextLr = 0xC;
constexpr uint32_t kContextCtr = 0x10;
constexpr uint32_t kContextGpr = 0x18;
constexpr uint32_t kContextCr = 0x118;
constexpr uint32_t kContextXer = 0x11C;
// After the FPSCR and the 32 FPRs: whether the context is user mode's.
constexpr uint32_t kContextUserModeControl = 0x228;
constexpr uint32_t kContextControl = 0x1;
constexpr uint32_t kContextInteger = 0x4;

// Trap frame layout. RuntimeHost uses the GPRs, lr and iar. XeFu also uses
// ctr, cr and xer and sets ctr before resuming. Where msr goes is unknown.
constexpr uint32_t kKframesSize = 0x1C0;
constexpr uint32_t kKframesGpr = 0x50;
constexpr uint32_t kKframesMsr = 0x160;
constexpr uint32_t kKframesCtr = 0x1A0;
constexpr uint32_t kKframesLr = 0x1A8;
constexpr uint32_t kKframesIar = 0x1AC;
constexpr uint32_t kKframesCr = 0x1B0;
constexpr uint32_t kKframesXer = 0x1B8;

// MSR[PR], which a trap frame from user mode carries.
constexpr uint32_t kMsrUserMode = 0x4000;

}  // namespace

#if XE_PLATFORM_WIN32

namespace {

// The exception record a fault hands the trap handler.
constexpr uint32_t kRecordCode = 0x0;
constexpr uint32_t kRecordAddress = 0xC;
constexpr uint32_t kRecordParameterCount = 0x10;
constexpr uint32_t kRecordInformation = 0x14;
constexpr uint32_t kRecordSize = 0x1C;

constexpr uint32_t kReturnSentinel = 0xBCBCBCBC;
// Keeps the handler clear of the frame that called KeEnterUserMode.
constexpr uint64_t kHandlerStackGap = 0x100;

constexpr SIZE_T kFiberCommitBytes = 256 * 1024;
constexpr SIZE_T kFiberReserveBytes = 16 * 1024 * 1024;

// Where a fiber's procedure began, captured as it first ran. Restoring it
// starts the fiber over on the stack it already has: the frames above are
// dropped without unwinding, as DeleteFiber drops them, but no stack is freed
// or made. Every sc the runtime services leaves the handler's fiber with
// frames, and remaking it - a 16 MB stack freed and reserved again - was most
// of what each one cost.
struct FiberStart {
  CONTEXT context;
  bool captured = false;
  // Restore |context| the next time the fiber is switched to.
  bool restart = false;
};

struct UserFiber {
  void* fiber = nullptr;
  // Left with frames on it, so it starts over before it runs anything.
  bool abandoned = false;
  FiberStart start;
  // Where a fresh entry starts.
  uint32_t entry_address = 0;
  // Where the parked trap resumes, and the guest stack pointer it had.
  uint32_t resume_address = 0;
  uint32_t stack_pointer = 0;
  // Set when the handler returned instead of leaving user mode.
  bool handler_returned = false;
  // Backend stackpoint records for this stack.
  void* stackpoint_state = nullptr;
};

struct UserModeState {
  XThread* thread = nullptr;
  cpu::Processor* processor = nullptr;

  // Suspended in KeEnterUserMode while user code runs.
  void* kernel_fiber = nullptr;
  // The fiber running now, so a call knows whose frames it is leaving.
  void* current_fiber = nullptr;

  void* handler_fiber = nullptr;
  bool handler_abandoned = false;
  FiberStart handler_start;
  void* handler_stackpoint_state = nullptr;
  uint32_t handler = 0;
  cpu::Function* handler_function = nullptr;

  // Integer and control registers from KeEnterUserMode, restored on leave.
  struct {
    uint64_t r[32];
    uint64_t ctr;
    uint64_t lr;
    uint32_t cr;
    uint32_t xer;
  } kernel_registers = {};

  std::vector<std::unique_ptr<UserFiber>> fibers;
  // Fibers parked in a trap, most recent last.
  std::vector<UserFiber*> parked;
  // The fiber user code last ran on.
  UserFiber* running = nullptr;
  // The fiber whose trap the handler is running for.
  UserFiber* trapped = nullptr;
  // Stackpoint states exchanged into the context, exchanged back in reverse.
  std::vector<void*> swapped_stackpoint_states;

  // Guest buffer the handler receives as the trapped register frame.
  uint32_t kframes = 0;
  // Guest buffer for the exception record a fault hands the handler.
  uint32_t exception_record = 0;
  // The trap frame its KTHREAD names, where the console saves interrupted
  // user code's CR; changes made there apply when it resumes.
  uint32_t interrupt_frame = 0;
  uint32_t interrupted_cr = 0;

  // Where KeEnterUserMode restarts the fiber after a dispatched fault.
  bool restart_pending = false;
  uint32_t restart_address = 0;

  bool in_user_code = false;
  uint32_t leave_value = 0;

  ~UserModeState();
};

thread_local std::unique_ptr<UserModeState> tl_user_mode;

uint32_t GetXer(const PPCContext* context) {
  return (uint32_t(context->xer_so) << 31) | (uint32_t(context->xer_ov) << 30) |
         (uint32_t(context->xer_ca) << 29);
}

void SetXer(PPCContext* context, uint32_t xer) {
  context->xer_so = (xer >> 31) & 1;
  context->xer_ov = (xer >> 30) & 1;
  context->xer_ca = (xer >> 29) & 1;
}

// Points the JIT's guest addressing at the user mode or kernel address space.
void SetUserAddressing(PPCContext* context, bool user) {
  auto memory = kernel_memory();
  context->virtual_membase =
      user ? memory->user_virtual_membase() : memory->virtual_membase();
}

void SaveKframes(PPCContext* context, uint8_t* kframes, uint32_t iar) {
  std::memset(kframes, 0, kKframesSize);
  for (size_t i = 0; i < 32; ++i) {
    xe::store_and_swap<uint64_t>(kframes + kKframesGpr + i * 8, context->r[i]);
  }
  xe::store_and_swap<uint64_t>(kframes + kKframesCtr, context->ctr);
  xe::store_and_swap<uint32_t>(kframes + kKframesCr, uint32_t(context->cr()));
  xe::store_and_swap<uint32_t>(kframes + kKframesXer, GetXer(context));
  xe::store_and_swap<uint32_t>(kframes + kKframesMsr,
                               uint32_t(context->msr) | kMsrUserMode);
  xe::store_and_swap<uint32_t>(kframes + kKframesLr, uint32_t(context->lr));
  xe::store_and_swap<uint32_t>(kframes + kKframesIar, iar);
}

uint32_t LoadKframes(PPCContext* context, const uint8_t* kframes) {
  for (size_t i = 0; i < 32; ++i) {
    context->r[i] = xe::load_and_swap<uint64_t>(kframes + kKframesGpr + i * 8);
  }
  context->ctr = xe::load_and_swap<uint64_t>(kframes + kKframesCtr);
  context->set_cr(xe::load_and_swap<uint32_t>(kframes + kKframesCr));
  SetXer(context, xe::load_and_swap<uint32_t>(kframes + kKframesXer));
  context->lr = xe::load_and_swap<uint32_t>(kframes + kKframesLr);
  return xe::load_and_swap<uint32_t>(kframes + kKframesIar);
}

PPCContext* GuestContext(UserModeState* state) {
  return state->thread->thread_state()->context();
}

// Kernel memory by guest address, whichever membase the context holds now.
uint8_t* KernelTranslate(uint32_t address) {
  return kernel_memory()->TranslateVirtual<uint8_t*>(address);
}

// Saves the CR of the user code being interrupted in its trap frame.
void SaveInterruptedUserCr(UserModeState* state) {
  if (!state->interrupt_frame) {
    return;
  }
  const uint32_t cr = uint32_t(GuestContext(state)->cr());
  state->interrupted_cr = cr;
  xe::store_and_swap<uint32_t>(
      KernelTranslate(state->interrupt_frame + kKframesCr), cr);
}

// Applies the CR bits changed in the trap frame since SaveInterruptedUserCr,
// as returning to user code reloads CR from the frame.
void RestoreInterruptedUserCr(UserModeState* state) {
  if (!state->interrupt_frame) {
    return;
  }
  const uint32_t frame_cr = xe::load_and_swap<uint32_t>(
      KernelTranslate(state->interrupt_frame + kKframesCr));
  const uint32_t changed = frame_cr ^ state->interrupted_cr;
  if (changed) {
    auto context = GuestContext(state);
    context->set_cr((uint32_t(context->cr()) & ~changed) |
                    (frame_cr & changed));
  }
}

// Runs guest code from a host frame that nothing returns to.
void RunGuest(UserModeState* state, cpu::Function* function,
              uint32_t address) {
  if (!function) {
    XELOGE("User mode: no guest code at {:08X}", address);
    return;
  }
  function->Call(state->thread->thread_state(), kReturnSentinel);
}

void RunGuest(UserModeState* state, uint32_t address) {
  RunGuest(state, state->processor->ResolveFunction(address), address);
}

void PushStackpointState(UserModeState* state, void* stackpoint_state) {
  if (!stackpoint_state) {
    return;
  }
  state->processor->backend()->SwapStackpointState(GuestContext(state),
                                                   stackpoint_state);
  state->swapped_stackpoint_states.push_back(stackpoint_state);
}

void PopStackpointState(UserModeState* state) {
  if (state->swapped_stackpoint_states.empty()) {
    return;
  }
  state->processor->backend()->SwapStackpointState(
      GuestContext(state), state->swapped_stackpoint_states.back());
  state->swapped_stackpoint_states.pop_back();
}

void* NewFiber(LPFIBER_START_ROUTINE proc, void* param) {
  void* fiber = CreateFiberEx(kFiberCommitBytes, kFiberReserveBytes,
                              FIBER_FLAG_FLOAT_SWITCH, proc, param);
  if (!fiber) {
    XELOGE("User mode: CreateFiberEx failed ({})", GetLastError());
  }
  return fiber;
}

// Back on a fiber that switched away: one renewed since starts over. Every
// switch goes through SwitchTo, so this is where an abandoned fiber resumes.
void RestartIfRenewed(UserModeState* state) {
  void* fiber = GetCurrentFiber();
  FiberStart* start = nullptr;
  if (fiber == state->handler_fiber) {
    start = &state->handler_start;
  } else {
    for (auto& user_fiber : state->fibers) {
      if (user_fiber->fiber == fiber) {
        start = &user_fiber->start;
        break;
      }
    }
  }
  if (start && start->restart) {
    start->restart = false;
    // No exception record: a plain jump, nothing unwound.
    RtlRestoreContext(&start->context, nullptr);
  }
}

void SwitchTo(UserModeState* state, void* fiber) {
  state->current_fiber = fiber;
  SwitchToFiber(fiber);
  RestartIfRenewed(state);
}

// Marks the fiber running now as holding frames that must never run again.
void AbandonCurrentFiber(UserModeState* state) {
  if (state->current_fiber == state->handler_fiber) {
    state->handler_abandoned = true;
    return;
  }
  for (auto& user_fiber : state->fibers) {
    if (user_fiber->fiber == state->current_fiber) {
      user_fiber->abandoned = true;
      return;
    }
  }
}

// Puts back what KeEnterUserMode found: the kernel's registers, address space
// and stackpoint records.
void RestoreKernel(UserModeState* state, uint32_t value) {
  auto context = GuestContext(state);
  state->in_user_code = false;
  state->leave_value = value;
  SetUserAddressing(context, false);
  auto& registers = state->kernel_registers;
  std::memcpy(context->r, registers.r, sizeof(registers.r));
  context->ctr = registers.ctr;
  context->lr = registers.lr;
  context->set_cr(registers.cr);
  SetXer(context, registers.xer);
  while (!state->swapped_stackpoint_states.empty()) {
    PopStackpointState(state);
  }
}

// Resumes KeEnterUserMode on the kernel fiber. |abandon| when the fiber this
// runs on still holds frames that must never run again.
void LeaveUserMode(UserModeState* state, uint32_t value, bool abandon) {
  if (abandon) {
    AbandonCurrentFiber(state);
  }
  RestoreKernel(state, value);
  SwitchTo(state, state->kernel_fiber);
  // Only a fiber left without frames is ever switched to again, and it goes
  // straight back to its loop.
}

VOID WINAPI UserFiberProc(void* param) {
  auto user_fiber = static_cast<UserFiber*>(param);
  // A restart resumes here. Captured in this frame and not a helper's: a
  // helper's return address lies below, where later frames overwrite it.
  RtlCaptureContext(&user_fiber->start.context);
  user_fiber->start.captured = true;
  for (;;) {
    UserModeState* state = tl_user_mode.get();
    RunGuest(state, user_fiber->entry_address);
    XELOGE("User mode: code at {:08X} returned without leaving user mode",
           user_fiber->entry_address);
    LeaveUserMode(state, 0, false);
  }
}

VOID WINAPI HandlerFiberProc(void* param) {
  auto state = static_cast<UserModeState*>(param);
  // A restart resumes here, as in UserFiberProc.
  RtlCaptureContext(&state->handler_start.context);
  state->handler_start.captured = true;
  for (;;) {
    RunGuest(state, state->handler_function, state->handler);
    // The handler returned rather than leaving user mode: back to the trap.
    auto trapped = state->trapped;
    trapped->handler_returned = true;
    SwitchTo(state, trapped->fiber);
  }
}

// Makes |user_fiber| ready to run from its start.
bool RenewUserFiber(UserFiber* user_fiber) {
  if (user_fiber->fiber && !user_fiber->abandoned) {
    return true;
  }
  user_fiber->abandoned = false;
  if (user_fiber->fiber && user_fiber->start.captured) {
    user_fiber->start.restart = true;
    return true;
  }
  if (user_fiber->fiber) {
    DeleteFiber(user_fiber->fiber);
  }
  user_fiber->start.captured = false;
  user_fiber->start.restart = false;
  user_fiber->fiber = NewFiber(&UserFiberProc, user_fiber);
  return user_fiber->fiber != nullptr;
}

bool RenewHandlerFiber(UserModeState* state) {
  if (state->handler_fiber && !state->handler_abandoned) {
    return true;
  }
  state->handler_abandoned = false;
  if (state->handler_fiber && state->handler_start.captured) {
    state->handler_start.restart = true;
    return true;
  }
  if (state->handler_fiber) {
    DeleteFiber(state->handler_fiber);
  }
  state->handler_start.captured = false;
  state->handler_start.restart = false;
  state->handler_fiber = NewFiber(&HandlerFiberProc, state);
  return state->handler_fiber != nullptr;
}

UserModeState::~UserModeState() {
  for (auto& user_fiber : fibers) {
    if (user_fiber->fiber && user_fiber->fiber != current_fiber) {
      DeleteFiber(user_fiber->fiber);
    }
  }
  if (handler_fiber && handler_fiber != current_fiber) {
    DeleteFiber(handler_fiber);
  }
  // A state still swapped into a context belongs to it now; the backend frees
  // whichever records the context holds.
  if (swapped_stackpoint_states.empty() && processor) {
    auto backend = processor->backend();
    for (auto& user_fiber : fibers) {
      backend->DestroyStackpointState(user_fiber->stackpoint_state);
    }
    backend->DestroyStackpointState(handler_stackpoint_state);
  }
  if (kernel_state()) {
    for (uint32_t block : {kframes, exception_record, interrupt_frame}) {
      if (block) {
        kernel_memory()->SystemHeapFree(block);
      }
    }
  }
}

UserModeState* CreateUserMode(XThread* thread) {
  auto state = std::make_unique<UserModeState>();
  state->thread = thread;
  state->processor = kernel_state()->processor();
  state->handler_stackpoint_state =
      state->processor->backend()->CreateStackpointState();
  auto memory = kernel_memory();
  state->kframes = memory->SystemHeapAlloc(kKframesSize);
  state->exception_record = memory->SystemHeapAlloc(kRecordSize);
  state->interrupt_frame = memory->SystemHeapAlloc(kKframesSize);
  thread->guest_object<X_KTHREAD>()->unk_128 = state->interrupt_frame;
  tl_user_mode = std::move(state);
  return tl_user_mode.get();
}

// Runs the trap handler for user code that trapped at |resume_address|.
// |record| is the fault's exception record, or 0 for a system call. Returns
// true if the handler returned; |out_address| is where it resumes, possibly
// moved. Returns false if the handler left user mode and the kernel later
// resumed the trap, having loaded the registers itself.
bool TrapIntoHandler(PPCContext* context, UserModeState* state,
                     uint32_t record, uint32_t resume_address,
                     uint32_t* out_address) {
  auto trapped = state->running;
  state->in_user_code = false;
  SaveInterruptedUserCr(state);
  SetUserAddressing(context, false);
  uint8_t* kframes = KernelTranslate(state->kframes);
  SaveKframes(context, kframes, resume_address);
  trapped->resume_address = resume_address;
  trapped->stack_pointer = uint32_t(context->r[1]);
  trapped->handler_returned = false;
  state->parked.push_back(trapped);
  state->trapped = trapped;

  if (!RenewHandlerFiber(state)) {
    LeaveUserMode(state, 0, true);
    return false;
  }

  // The handler runs in kernel mode, below the frame that entered.
  auto& registers = state->kernel_registers;
  context->r[1] = (registers.r[1] - kHandlerStackGap) & ~uint64_t(0xF);
  context->r[2] = registers.r[2];
  context->r[13] = registers.r[13];
  context->r[3] = record;
  context->r[4] = state->kframes;
  context->lr = kReturnSentinel;
  PushStackpointState(state, state->handler_stackpoint_state);
  state->processor->backend()->PrepareForReentry(context);
  SwitchTo(state, state->handler_fiber);

  // Back on the trapped fiber, because the handler returned or because the
  // kernel entered user mode again to resume this trap.
  state->running = trapped;
  if (!trapped->handler_returned) {
    // KeEnterUserMode loaded the resumed registers and stackpoints.
    return false;
  }
  PopStackpointState(state);
  auto parked_trap =
      std::find(state->parked.begin(), state->parked.end(), trapped);
  if (parked_trap != state->parked.end()) {
    state->parked.erase(parked_trap);
  }

  *out_address = LoadKframes(context, kframes);
  RestoreInterruptedUserCr(state);
  SetUserAddressing(context, true);
  state->in_user_code = true;
  return true;
}

// A guest sc. Claims it only while user code runs on this thread.
bool UserModeSyscall(PPCContext* context) {
  UserModeState* state = tl_user_mode.get();
  if (!state || !state->in_user_code) {
    return false;
  }
  const uint32_t resume_address = uint32_t(context->scratch);
  uint32_t address;
  if (!TrapIntoHandler(context, state, 0, resume_address, &address)) {
    return true;
  }
  if (address != resume_address) {
    // The handler redirected the trap rather than resuming it.
    RunGuest(state, address);
    XELOGE("User mode: code at {:08X} returned without leaving user mode",
           address);
    LeaveUserMode(state, 0, true);
  }
  return true;
}

// Hands the fiber back to KeEnterUserMode, which restarts it at
// restart_address. The faulting access's host frames are abandoned.
void RestartUserModeThunk() {
  UserModeState* state = tl_user_mode.get();
  state->in_user_code = false;
  AbandonCurrentFiber(state);
  SwitchTo(state, state->kernel_fiber);
}

// Fills the exception record for an access violation at |fault_address| by
// the instruction at |address|.
void FillAccessViolationRecord(UserModeState* state, uint32_t address,
                               bool is_write, uint32_t fault_address) {
  uint8_t* record = KernelTranslate(state->exception_record);
  std::memset(record, 0, kRecordSize);
  xe::store_and_swap<uint32_t>(record + kRecordCode, X_STATUS_ACCESS_VIOLATION);
  xe::store_and_swap<uint32_t>(record + kRecordAddress, address);
  xe::store_and_swap<uint32_t>(record + kRecordParameterCount, 2);
  xe::store_and_swap<uint32_t>(record + kRecordInformation, is_write ? 1 : 0);
  xe::store_and_swap<uint32_t>(record + kRecordInformation + 4, fault_address);
}

// A user mode call to an address with no code faults on the instruction fetch.
// The guest's handler can move the call elsewhere and resume at the new code.
uint32_t UserModeCodeFault(PPCContext* context, uint32_t address) {
  UserModeState* state = tl_user_mode.get();
  if (!state || !state->in_user_code || !state->running ||
      state->current_fiber != state->running->fiber) {
    return 0;
  }
  FillAccessViolationRecord(state, address, false, address);
  uint32_t resume_address;
  if (!TrapIntoHandler(context, state, state->exception_record, address,
                       &resume_address)) {
    // The kernel resumed the trap at the target, which still has no code.
    XELOGE("User mode: the fetch fault at {:08X} was retried by the kernel",
           address);
    return 0;
  }
  return resume_address != address ? resume_address : 0;
}

// Runs the trap handler for a fault on the guest instruction at |address|.
// Returns whether the interrupted access runs again, false if the fiber has to
// restart at the address the handler left instead.
bool RunFaultHandler(PPCContext* context, UserModeState* state,
                     uint32_t address) {
  uint32_t resume_address;
  const bool handler_returned = TrapIntoHandler(
      context, state, state->exception_record, address, &resume_address);
  if (handler_returned && resume_address == address) {
    return true;
  }
  state->restart_pending = true;
  state->restart_address = handler_returned ? resume_address : address;
  return false;
}

// A user mode access with no page table entry, or one its entry's protection
// forbids. The guest's handler can fix the entry and return, and the access
// runs again.
Memory::UserFaultResult UserModeFault(uint32_t fault_address, bool is_write,
                                      Exception* ex) {
  UserModeState* state = tl_user_mode.get();
  if (!state || !state->in_user_code || !state->running ||
      state->current_fiber != state->running->fiber) {
    return Memory::UserFaultResult::kNotTaken;
  }
  PPCContext* context = GuestContext(state);
  auto function =
      state->processor->backend()->code_cache()->LookupFunction(ex->pc());
  if (!function) {
    // Host code faulted on the guest's behalf, so there is no guest
    // instruction to resume.
    return Memory::UserFaultResult::kNotTaken;
  }
  const uint32_t address = function->MapMachineCodeToGuestAddress(ex->pc());
  FillAccessViolationRecord(state, address, is_write, fault_address);
  if (RunFaultHandler(context, state, address)) {
    // The retry runs from the host registers, so changes the handler made to
    // the frame are lost.
    return Memory::UserFaultResult::kTaken;
  }
  // The handler did not fix the page and retry: the runtime turns these into
  // managed exceptions (NullReferenceException), so this is the trail of an
  // exception that may end the title.
  XELOGW(
      "User mode: {} of {:08X} by {:08X} (lr {:08X}) not retried, restarts at "
      "{:08X}",
      is_write ? "write" : "read", fault_address, address,
      uint32_t(context->lr), state->restart_address);
  DivertToThunk(ex, &RestartUserModeThunk);
  return Memory::UserFaultResult::kDiverted;
}

}  // namespace

dword_result_t KeCreateUserMode_entry(dword_t unknown, lpvoid_t descriptor,
                                      const ppc_context_t& context) {
  // User code runs on its own host fibers, so the backend has to keep their
  // stackpoint records apart. One that keeps none would unwind a fiber onto
  // another's host stack.
  auto backend = context->processor->backend();
  void* stackpoint_probe = backend->CreateStackpointState();
  if (!stackpoint_probe) {
    XELOGE("KeCreateUserMode: the backend keeps no per-stack records");
    return X_STATUS_NOT_SUPPORTED;
  }
  backend->DestroyStackpointState(stackpoint_probe);
  kernel_memory()->SetUserPageTable(descriptor.guest_address());
  if (!kernel_memory()->EnableUserModeViews()) {
    return X_STATUS_NO_MEMORY;
  }
  context->processor->EnableDynamicCode();
  context->processor->set_syscall_hook(&UserModeSyscall);
  // The handler reads the registers of a fault from the context, which the JIT
  // otherwise leaves stale mid-block.
  context->processor->KeepDynamicCodeContextStores();
  kernel_memory()->set_user_fault_hook(&UserModeFault);
  context->processor->set_code_fault_hook(&UserModeCodeFault);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(KeCreateUserMode, kThreading, kImplemented, kSketchy);

dword_result_t KeDeleteUserMode_entry(lpvoid_t descriptor) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeDeleteUserMode, kThreading, kImplemented);

// Returns the value KeLeaveUserMode is called with.
dword_result_t KeEnterUserMode_entry(lpvoid_t user_context, dword_t handler,
                                     lpvoid_t nonvolatile_save, dword_t unknown,
                                     const ppc_context_t& context) {
  XThread* thread = XThread::GetCurrentThread();
  PPCContext* guest_context = context.value();
  if (!kernel_memory()->user_virtual_membase()) {
    XELOGE("KeEnterUserMode without a user mode address space");
    return 0;
  }
  UserModeState* state = tl_user_mode.get();
  if (state && state->current_fiber &&
      state->current_fiber != state->kernel_fiber) {
    // Entering from a fiber this already runs would restart it under itself.
    XELOGE("KeEnterUserMode from user mode is not supported");
    return 0;
  }
  if (!state) {
    state = CreateUserMode(thread);
  }

  if (!IsThreadAFiber()) {
    state->kernel_fiber =
        ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    if (!state->kernel_fiber) {
      XELOGE("KeEnterUserMode: ConvertThreadToFiberEx failed ({})",
             GetLastError());
      return 0;
    }
  } else {
    state->kernel_fiber = GetCurrentFiber();
  }
  state->current_fiber = state->kernel_fiber;

  auto& registers = state->kernel_registers;
  std::memcpy(registers.r, guest_context->r, sizeof(registers.r));
  registers.ctr = guest_context->ctr;
  registers.lr = guest_context->lr;
  registers.cr = uint32_t(guest_context->cr());
  registers.xer = GetXer(guest_context);
  if (!state->handler_function || state->handler != handler) {
    state->handler = handler;
    state->handler_function = state->processor->ResolveFunction(handler);
  }

  const uint8_t* p = KernelTranslate(user_context.guest_address());
  for (size_t i = 0; i < 32; ++i) {
    guest_context->r[i] = xe::load_and_swap<uint64_t>(p + kContextGpr + i * 8);
  }
  guest_context->ctr = xe::load_and_swap<uint64_t>(p + kContextCtr);
  guest_context->lr = xe::load_and_swap<uint32_t>(p + kContextLr);
  guest_context->set_cr(xe::load_and_swap<uint32_t>(p + kContextCr));
  SetXer(guest_context, xe::load_and_swap<uint32_t>(p + kContextXer));
  const uint32_t entry_address = xe::load_and_swap<uint32_t>(p + kContextIar);
  if (!guest_context->r[1]) {
    // A context with no stack pointer borrows the kernel's. User code gets the
    // lower half of the stack left so the trap handler and the kernel's
    // servicing of a trap run above it without overwriting its frames.
    const uint32_t kernel_sp = uint32_t(registers.r[1]);
    const uint32_t limit = thread->stack_limit();
    const uint32_t user_sp =
        limit && limit < kernel_sp
            ? (limit + (kernel_sp - limit) / 2) & ~uint32_t(0xF)
            : kernel_sp;
    guest_context->r[1] = user_sp;
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
      XELOGW(
          "KeEnterUserMode: the context at {:08X} names no stack. User mode "
          "runs at {:08X} on the kernel stack",
          user_context.guest_address(), user_sp);
    }
  }

  SetUserAddressing(guest_context, true);

  // The kernel services a trap by entering again at the instruction after it
  // with the stack pointer it trapped with, so that fiber continues in place.
  UserFiber* target = nullptr;
  for (size_t i = state->parked.size(); i-- > 0;) {
    auto parked = state->parked[i];
    if (parked->resume_address == entry_address &&
        parked->stack_pointer == uint32_t(guest_context->r[1])) {
      target = parked;
      break;
    }
  }
  if (target) {
    // Traps parked after it belong to calls that already finished.
    while (state->parked.back() != target) {
      state->parked.back()->abandoned = true;
      state->parked.pop_back();
    }
    state->parked.pop_back();
    PushStackpointState(state, target->stackpoint_state);
  } else {
    // Guest stacks grow down, so an entry at or above a parked trap's stack
    // pointer has overwritten its frames.
    while (!state->parked.empty() && state->parked.back()->stack_pointer <=
                                         uint32_t(guest_context->r[1])) {
      XELOGD(
          "User mode: entry at {:08X} with r1 {:08X} drops the trap parked "
          "at {:08X} with r1 {:08X}",
          entry_address, uint32_t(guest_context->r[1]),
          state->parked.back()->resume_address,
          state->parked.back()->stack_pointer);
      state->parked.back()->abandoned = true;
      state->parked.pop_back();
    }
    for (auto& user_fiber : state->fibers) {
      if (std::find(state->parked.begin(), state->parked.end(),
                    user_fiber.get()) == state->parked.end()) {
        target = user_fiber.get();
        break;
      }
    }
    if (!target) {
      auto user_fiber = std::make_unique<UserFiber>();
      user_fiber->stackpoint_state =
          state->processor->backend()->CreateStackpointState();
      state->fibers.push_back(std::move(user_fiber));
      target = state->fibers.back().get();
    }
    if (!RenewUserFiber(target)) {
      RestoreKernel(state, 0);
      return 0;
    }
    target->entry_address = entry_address;
    PushStackpointState(state, target->stackpoint_state);
    state->processor->backend()->PrepareForReentry(guest_context);
  }
  state->running = target;
  state->in_user_code = true;
  for (;;) {
    SwitchTo(state, target->fiber);
    // Back on the kernel fiber: user mode left, or a fault was dispatched.
    if (!state->restart_pending) {
      break;
    }
    // The handler sent a fault elsewhere; restart the fiber there.
    state->restart_pending = false;
    target->entry_address = state->restart_address;
    if (!RenewUserFiber(target)) {
      RestoreKernel(state, 0);
      return 0;
    }
    state->processor->backend()->PrepareForReentry(guest_context);
    state->in_user_code = true;
  }
  return state->leave_value;
}
DECLARE_XBOXKRNL_EXPORT3(KeEnterUserMode, kThreading, kImplemented,
                         kHighFrequency, kSketchy);

void KeLeaveUserMode_entry(dword_t value) {
  UserModeState* state = tl_user_mode.get();
  if (!state || !state->current_fiber ||
      state->current_fiber == state->kernel_fiber) {
    XELOGE("KeLeaveUserMode({}) outside user mode", uint32_t(value));
    return;
  }
  LeaveUserMode(state, value, true);
}
DECLARE_XBOXKRNL_EXPORT3(KeLeaveUserMode, kThreading, kImplemented,
                         kHighFrequency, kSketchy);

#endif  // XE_PLATFORM_WIN32

void KeContextFromKframes_entry(lpvoid_t kframes, lpvoid_t context) {
  auto k = kframes.as<const uint8_t*>();
  auto p = context.as<uint8_t*>();
  const uint32_t flags = xe::load_and_swap<uint32_t>(p + kContextFlags);
  // The kernel side only resumes user mode from a context that says so.
  xe::store_and_swap<uint32_t>(
      p + kContextUserModeControl,
      (xe::load_and_swap<uint32_t>(k + kKframesMsr) & kMsrUserMode) ? 1 : 0);
  if (flags & kContextControl) {
    xe::store_and_swap<uint32_t>(p + kContextMsr,
                                 xe::load_and_swap<uint32_t>(k + kKframesMsr));
    xe::store_and_swap<uint32_t>(p + kContextIar,
                                 xe::load_and_swap<uint32_t>(k + kKframesIar));
    xe::store_and_swap<uint32_t>(p + kContextLr,
                                 xe::load_and_swap<uint32_t>(k + kKframesLr));
    xe::store_and_swap<uint64_t>(p + kContextCtr,
                                 xe::load_and_swap<uint64_t>(k + kKframesCtr));
  }
  // RuntimeHost asks for control and integer state. The kframes carry no
  // floating point or vector registers, which user mode shares with the
  // kernel.
  if (flags & kContextInteger) {
    std::memcpy(p + kContextGpr, k + kKframesGpr, 32 * 8);
    xe::store_and_swap<uint32_t>(p + kContextCr,
                                 xe::load_and_swap<uint32_t>(k + kKframesCr));
    xe::store_and_swap<uint32_t>(p + kContextXer,
                                 xe::load_and_swap<uint32_t>(k + kKframesXer));
  }
}
DECLARE_XBOXKRNL_EXPORT2(KeContextFromKframes, kThreading, kImplemented,
                         kHighFrequency);

// The inverse, which a continue uses to load the context it resumes with.
void KeContextToKframes_entry(lpvoid_t kframes, lpvoid_t context,
                              dword_t flags) {
  auto k = kframes.as<uint8_t*>();
  auto p = context.as<const uint8_t*>();
  if (flags & kContextControl) {
    xe::store_and_swap<uint32_t>(k + kKframesIar,
                                 xe::load_and_swap<uint32_t>(p + kContextIar));
    xe::store_and_swap<uint32_t>(k + kKframesLr,
                                 xe::load_and_swap<uint32_t>(p + kContextLr));
    xe::store_and_swap<uint64_t>(k + kKframesCtr,
                                 xe::load_and_swap<uint64_t>(p + kContextCtr));
  }
  if (flags & kContextInteger) {
    std::memcpy(k + kKframesGpr, p + kContextGpr, 32 * 8);
    xe::store_and_swap<uint32_t>(k + kKframesCr,
                                 xe::load_and_swap<uint32_t>(p + kContextCr));
    xe::store_and_swap<uint32_t>(k + kKframesXer,
                                 xe::load_and_swap<uint32_t>(p + kContextXer));
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeContextToKframes, kThreading, kImplemented);

// Dropping more than was asked for only costs a fault, so none reads its args.
void KeFlushUserModeCurrentTb_entry() {
  kernel_memory()->FlushChangedUserPages();
}
DECLARE_XBOXKRNL_EXPORT1(KeFlushUserModeCurrentTb, kMemory, kImplemented);

void KeFlushUserModeTb_entry() { kernel_memory()->FlushChangedUserPages(); }
DECLARE_XBOXKRNL_EXPORT1(KeFlushUserModeTb, kMemory, kImplemented);

void KeFlushCurrentEntireTb_entry() { kernel_memory()->FlushUserPageTable(); }
DECLARE_XBOXKRNL_EXPORT1(KeFlushCurrentEntireTb, kMemory, kImplemented);

void KeFlushEntireTb_entry() { kernel_memory()->FlushUserPageTable(); }
DECLARE_XBOXKRNL_EXPORT1(KeFlushEntireTb, kMemory, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(UserMode);
