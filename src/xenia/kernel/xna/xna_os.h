/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_OS_H_
#define XENIA_KERNEL_XNA_XNA_OS_H_

#include <stddef.h>
#include <stdint.h>

// The console-services boundary for XNA titles whose IL runs on the HOST CLR.
//
// Those titles are managed code all the way down - Microsoft.Xna.Framework and
// friends contain no InternalCalls, only named P/Invokes - so their IL needs no
// emulation and is JITted once by the host. Drawing and audio are the host's
// job too; routing them back through an emulated Xenos would translate the same
// work twice. What the host cannot supply is the CONSOLE: who is signed in,
// what their gamertag is, which save devices exist, whether the guide is up.
//
// That is what this is. Nexia is the operating system underneath a managed XNA
// title, and this table is the syscall surface.
//
// It is a plain C ABI passed to managed code as a pointer, deliberately not an
// exported symbol: a static library's exports only survive if something
// references them, and the host already has to call in here to start a title.
// Everything is blittable, so the managed side can hold it as a struct of
// function pointers and call through with no marshalling.

namespace xe {
namespace kernel {
namespace xna {

// Bump when a field is added. Fields are only ever APPENDED, and the managed
// side checks `size` before touching anything it was not compiled against.
constexpr uint32_t kXnaOsAbiVersion = 4;

// A guide request is the one thing here that is not instantaneous: the dialog
// stays up while a person reads it, on the emulator's UI thread, while the
// title's game loop runs on another. So the boundary is begin-then-poll rather
// than a blocking call - which is also the shape XNA titles already expect,
// since they poll IAsyncResult.IsCompleted from Update.
enum XnaOsGuideStatus : int32_t {
  kXnaOsGuideUnknownRequest = -1,
  kXnaOsGuidePending = 0,
  kXnaOsGuideCompleted = 1,
  kXnaOsGuideCancelled = 2,
};

// Mirrors X_USER_SIGNIN_STATE.
enum XnaOsSigninState : uint32_t {
  kXnaOsNotSignedIn = 0,
  kXnaOsSignedInLocally = 1,
  kXnaOsSignedInToLive = 2,
};

#pragma pack(push, 4)

struct XnaOsUser {
  uint64_t xuid;
  uint64_t online_xuid;  // 0 when the profile is not LIVE-enabled.
  uint32_t signin_state;
  uint32_t is_live_enabled;
  uint32_t is_guest;
  uint32_t country;
  uint32_t language;
  // UTF-8, NUL-terminated. The console caps a gamertag at 15 characters.
  char gamertag[16];
};

struct XnaOsTable {
  uint32_t size;     // sizeof(XnaOsTable) as the host built it.
  uint32_t version;  // kXnaOsAbiVersion.

  // How many controller slots exist. Always 4 - a slot with nobody in it
  // reports kXnaOsNotSignedIn rather than being absent, because that is the
  // shape XNA's SignedInGamer collection expects.
  uint32_t (*GetUserSlotCount)();

  // Fills `out` for a slot. Returns 0 on success, non-zero if the slot index is
  // out of range. An empty slot succeeds with signin_state == NotSignedIn.
  int32_t (*GetUser)(uint32_t slot, XnaOsUser* out);

  uint32_t (*GetTitleId)();

  // Writes a NUL-terminated UTF-8 title name. Returns the number of bytes
  // needed including the NUL, so a caller can size a buffer by passing 0.
  uint32_t (*GetTitleName)(char* buffer, uint32_t capacity);

  // --- Storage (ABI version 2) ---------------------------------------------
  //
  // XNA's StorageContainer is a directory of ordinary files that the title
  // opens, reads and writes with normal stream calls, so the honest thing to
  // hand back is a real host path and let managed code use System.IO on it.
  // The alternative - proxying every file operation across this boundary, or
  // mounting an STFS package into the guest VFS that only host code will ever
  // read - buys nothing a title can observe. Nexia still decides WHERE the
  // save lives, which is the part that belongs to the OS.
  //
  // Resolves (creating it if needed) the container directory for a slot's
  // profile, writing a NUL-terminated UTF-8 host path. `display_name` is the
  // title's own container name and is sanitised before it reaches the
  // filesystem. Returns bytes needed including the NUL, or 0 on failure.
  uint32_t (*ResolveStorageContainer)(uint32_t slot, const char* display_name,
                                      char* buffer, uint32_t capacity);

  // Writes the name to show in a device selector, e.g. "Hard Drive".
  uint32_t (*GetStorageDeviceName)(char* buffer, uint32_t capacity);

  uint64_t (*GetStorageTotalSpace)();
  uint64_t (*GetStorageFreeSpace)();

  // --- Guide (ABI version 3) -----------------------------------------------
  //
  // These are Nexia's existing XAM dialogs - the same MessageBoxDialog and
  // KeyboardInputDialog a real 360 title gets - reached without going through
  // guest memory.

  // Whether any system dialog is up. A title uses this to pause itself.
  uint32_t (*GuideIsVisible)();

  // Both return a request id, or 0 if the dialog could not be shown. Every id
  // handed out must be released, whatever the outcome.
  uint32_t (*GuideBeginMessageBox)(uint32_t slot, const char* title,
                                   const char* text, const char* const* buttons,
                                   uint32_t button_count,
                                   uint32_t focus_button);
  uint32_t (*GuideBeginKeyboard)(uint32_t slot, const char* title,
                                 const char* description,
                                 const char* default_text, uint32_t max_length);

  // Returns an XnaOsGuideStatus. `out_button` is the chosen button, or -1;
  // `out_text_bytes` is what GuideGetText would need, including the NUL.
  int32_t (*GuidePoll)(uint32_t request, int32_t* out_button,
                       uint32_t* out_text_bytes);

  // Valid until the request is released. Returns bytes needed including NUL.
  uint32_t (*GuideGetText)(uint32_t request, char* buffer, uint32_t capacity);

  void (*GuideRelease)(uint32_t request);

  // --- Logging (ABI version 4) ---------------------------------------------
  //
  // Managed code has nowhere to print. The emulator is a windowed process with
  // no console attached, so Console.WriteLine from the host or from a title
  // goes nowhere at all - which makes every managed failure silent. Levels
  // match XELOG*: 0 debug, 1 info, 2 warning, 3 error.
  void (*Log)(uint32_t level, const char* message);
};

#pragma pack(pop)

// The managed side declares this same layout by hand (managed/XnaOs.cs). A
// silent disagreement here would not fail to compile on either side - it would
// corrupt memory at the first call and look like a bug somewhere else - so the
// shape is pinned down here and checked against Marshal.SizeOf in the tests.
static_assert(sizeof(XnaOsUser) == 52, "XnaOsUser layout changed");
static_assert(offsetof(XnaOsUser, online_xuid) == 8,
              "XnaOsUser layout changed");
static_assert(offsetof(XnaOsUser, signin_state) == 16,
              "XnaOsUser layout changed");
static_assert(offsetof(XnaOsUser, gamertag) == 36, "XnaOsUser layout changed");
static_assert(sizeof(void*) != 8 || sizeof(XnaOsTable) == 128,
              "XnaOsTable layout changed - bump kXnaOsAbiVersion and update "
              "managed/XnaOs.cs");

// Never null. The table is a function-local static, so it outlives any title.
const XnaOsTable* GetXnaOsTable();

uint32_t XnaGuideBeginKeyboard(uint32_t slot, const char* title,
                               const char* description,
                               const char* default_text, uint32_t max_length,
                               bool password);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_OS_H_
