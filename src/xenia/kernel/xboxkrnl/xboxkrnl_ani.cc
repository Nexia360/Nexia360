/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_ani.h"

#include <mutex>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// The boot animation, as xboxkrnl plays it.
//
// Read out of the real 17559 kernel (lle/kernel/xboxkrnl_17559.bin, base
// 0x80040000) rather than guessed. AniStartBootAnimation @ 0x80061730:
//
//   if (!ShouldAnimate())                     return 0xC0000001;
//   XexLoadImage("\Device\Flash\bootanim.xex", 0x40000009, 0x20449700, &h);
//   *(u32*)0x8017CDC4 = h;
//   ExCreateThread(&0x8017CDC0, 0x8000, NULL, NULL,
//                  0x800614F0, *(u32*)0x80170000, 0x020000A0);
//   if (failed) XexUnloadImage(h); else *(u32*)0x8017CDC8 = 1;
//
// and the player thread it spawns, @0x800614F0, is only:
//
//   XexGetProcedureAddress(h, 1, &proc);
//   proc(h, context);
//
// So the animation IS bootanim.xex's ordinal 1, called with the module handle
// and the kernel's start context. Nothing here executes the xex as a title -
// that is a different thing and would be wrong.
//
// AniBlockOnAnimation @0x80061810 and AniTerminateAnimation @0x80061818 are
// both tail calls into one routine at 0x80061550 with r3 = 0 and r3 = 1: wait
// for it to end, or end it. NEITHER TAKES ARGUMENTS.

namespace {

struct BootAnimation {
  std::mutex mutex;
  object_ref<XThread> thread;
  uint32_t hmodule = 0;
  // bootanim's ordinal 2 - its own stop entry, taking the same flag
  // AniBlockOnAnimation and AniTerminateAnimation differ by.
  uint32_t stop = 0;
  uint32_t logo = 0;
  bool running = false;
};

BootAnimation& Animation() {
  static BootAnimation animation;
  return animation;
}

// The kernel names the animation by its flash path. We run out of
// $EXEFOLDER\Dashboard, so that folder is mounted under the console's own
// device name and the path below is the real one - bootanim.xex reaches for
// \Device\Flash itself, so calling the mount anything else would strand it.
constexpr const char* kBootAnimationPath = "\\Device\\Flash\\bootanim.xex";

std::filesystem::path DashboardDirectory() {
  return xe::filesystem::GetExecutableFolder() / "Dashboard";
}

// Returns false when there is no animation to play. The host file is checked
// FIRST: asking the VFS before anything is mounted only produces a confusing
// "device not found" for a file that may not exist either way.
bool EnsureFlashMounted() {
  const std::filesystem::path dashboard = DashboardDirectory();
  std::error_code ec;
  if (!std::filesystem::exists(dashboard / "bootanim.xex", ec)) {
    // A warning, not info: at the default log level this is the only line
    // that says why the boot animation did not play. bootanim.xex lives in
    // NAND flash, so a system update does not bring it - it has to be copied
    // in beside dash.xex.
    XELOGW("AniStartBootAnimation: no bootanim.xex in {}",
           xe::path_to_utf8(dashboard));
    return false;
  }

  auto* file_system = kernel_state()->file_system();
  if (!file_system) {
    return false;
  }
  if (file_system->ResolvePath(kBootAnimationPath)) {
    return true;
  }
  auto device = std::make_unique<vfs::HostPathDevice>(
      "\\Device\\Flash", dashboard, /*read_only=*/true);
  if (!device->Initialize() ||
      !file_system->RegisterDevice(std::move(device))) {
    XELOGW("AniStartBootAnimation: could not mount {} as \\Device\\Flash",
           xe::path_to_utf8(dashboard));
    return false;
  }
  if (!file_system->ResolvePath(kBootAnimationPath)) {
    // The one path that used to fail without saying so.
    XELOGW("AniStartBootAnimation: {} is mounted but {} still does not resolve",
           xe::path_to_utf8(dashboard), kBootAnimationPath);
    return false;
  }
  return true;
}

}  // namespace

X_STATUS StartBootAnimation() {
  BootAnimation& animation = Animation();
  std::lock_guard lock(animation.mutex);

  // The real predicate (0x80061498) ends by returning !already_running, and
  // checks console flags this build has no equivalent of.
  if (animation.running) {
    return X_STATUS_UNSUCCESSFUL;
  }
  if (!EnsureFlashMounted()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto module = kernel_state()->LoadUserModule(kBootAnimationPath);
  if (!module) {
    XELOGW("AniStartBootAnimation: {} would not load", kBootAnimationPath);
    return X_STATUS_UNSUCCESSFUL;
  }
  // LoadUserModule only reads the file in. The kernel reaches the animation
  // through XexLoadImage, which goes on to FinishLoadingUserModule - that is
  // what resolves the module's imports and precompiles it. Without this step
  // bootanim's 114 xboxkrnl imports are still unbound and its ordinal 1 has
  // nothing to call.
  kernel_state()->ApplyTitleUpdate(module);
  const X_RESULT loaded = kernel_state()->FinishLoadingUserModule(module);
  if (XFAILED(loaded)) {
    XELOGW("AniStartBootAnimation: bootanim.xex would not finish loading ({})",
           loaded);
    return X_STATUS_UNSUCCESSFUL;
  }

  const uint32_t entry = module->GetProcAddressByOrdinal(1);
  if (!entry) {
    XELOGW("AniStartBootAnimation: bootanim.xex has no ordinal 1");
    return X_STATUS_UNSUCCESSFUL;
  }

  animation.hmodule = module->hmodule_ptr();
  animation.stop = module->GetProcAddressByOrdinal(2);
  if (!animation.stop) {
    XELOGW(
        "AniStartBootAnimation: bootanim.xex has no ordinal 2 - it cannot "
        "be asked to stop");
  }

  // The player thread calls proc(handle, context) - two arguments. That is
  // exactly the shape XThread's xapi_thread_startup trampoline produces: it
  // enters `startup(start_address, start_context)`, so the procedure goes in
  // the trampoline slot and its two arguments in the other two.
  //
  // It runs under the SYSTEM process, not the title process, and that is not
  // cosmetic. The title X_KPROCESS is only ever filled in by
  // KernelState::SetExecutableModule - with no title started it is still all
  // zeroes, so XThread::Create's XeInsertTailList walks a null guest
  // thread_list.blink and writes through guest address 0. That is an access
  // violation before the animation draws a thing. The real kernel creates
  // this thread from ExCreateThread with the kernel's own process anyway.
  auto thread = object_ref<XThread>(
      new XThread(kernel_state(), 0x8000, /*xapi_thread_startup=*/entry,
                  /*start_address=*/animation.hmodule,
                  /*start_context=*/animation.logo, /*creation_flags=*/0,
                  /*guest_thread=*/true));
  const X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    XELOGW("AniStartBootAnimation: the player thread would not start ({:08X})",
           result);
    animation.hmodule = 0;
    return result;
  }

  animation.thread = std::move(thread);
  animation.running = true;
  // A warning on purpose: at the default log level this is the only line that
  // says the animation actually started.
  XELOGW("AniStartBootAnimation: playing bootanim.xex ordinal 1 @ {:08X}",
         entry);
  return X_STATUS_SUCCESS;
}

// 0x80061550, which both of the exports below tail call. `terminate` is their
// only difference: 0 waits for the animation to end on its own, 1 ends it.
X_STATUS AniStop(bool terminate) {
  BootAnimation& animation = Animation();
  std::unique_lock lock(animation.mutex);
  if (!animation.running) {
    return X_STATUS_SUCCESS;
  }
  object_ref<XThread> thread = animation.thread;
  const uint32_t stop = animation.stop;
  // Nothing else may hold the lock while this waits - stopping the animation
  // takes as long as bootanim takes to notice.
  lock.unlock();

  // The animation LOOPS. It does not end on its own, and waiting on its
  // thread alone waits forever - which is what this used to do. The kernel
  // asks bootanim to stop through its OWN ordinal 2, passing the same flag
  // these two exports differ by (0x800615C8: XexGetProcedureAddress(h, 2)
  // then proc(terminate)). Ordinal 2 has to run as guest code, so it gets a
  // thread of its own; start_context lands in r3, which is its only argument.
  if (stop) {
    // Same system process as the player - see the note there. Nothing has a
    // title process to belong to while the animation is up.
    auto stopper = object_ref<XThread>(
        new XThread(kernel_state(), 16 * 1024, /*xapi_thread_startup=*/0,
                    /*start_address=*/stop,
                    /*start_context=*/terminate ? 1u : 0u,
                    /*creation_flags=*/0, /*guest_thread=*/true));
    if (XSUCCEEDED(stopper->Create())) {
      uint64_t timeout = uint64_t(-int64_t(5000) * 10000);
      stopper->Wait(0, 0, 0, &timeout);
    } else {
      XELOGW("AniStop: could not run bootanim ordinal 2");
    }
  }

  if (thread) {
    // Now it has been told, so this ends. Bounded anyway: a guest thread is
    // never forcibly killed, and the title launch that follows tears the
    // whole context down regardless.
    uint64_t timeout = uint64_t(-int64_t(5000) * 10000);
    thread->Wait(0, 0, 0, &timeout);
  }
  lock.lock();
  animation.thread.reset();
  animation.hmodule = 0;
  animation.stop = 0;
  animation.running = false;
  return X_STATUS_SUCCESS;
}

X_STATUS TerminateBootAnimation() { return AniStop(true); }

X_STATUS BlockOnBootAnimation() {
  const X_STATUS result = AniStop(false);
  XELOGW("AniBlockOnAnimation: the boot animation has ended");
  return result;
}

dword_result_t AniStartBootAnimation_entry() { return StartBootAnimation(); }
DECLARE_XBOXKRNL_EXPORT1(AniStartBootAnimation, kNone, kImplemented);

dword_result_t AniBlockOnAnimation_entry() { return AniStop(false); }
DECLARE_XBOXKRNL_EXPORT1(AniBlockOnAnimation, kNone, kImplemented);

dword_result_t AniTerminateAnimation_entry() { return AniStop(true); }
DECLARE_XBOXKRNL_EXPORT1(AniTerminateAnimation, kNone, kImplemented);

dword_result_t AniSetLogo_entry(dword_t logo) {
  Animation().logo = logo;
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(AniSetLogo, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Ani);
