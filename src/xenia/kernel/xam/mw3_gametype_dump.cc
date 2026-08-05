/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// One-shot dump of MW3's runtime gametype registry.
//
// MW3 (title 0x415608CB) parses its gametype definitions with Playlist_Parse
// (guest 0x822F0668); the `gametype` keyword handler interns each code string
// into a global registry that Gametype_Find (0x822F0490) reads back:
//
//   count   : u32 big-endian at 0x8371E504
//   entries : base 0x8371E508, stride 0x78 (120) bytes
//   code    : NUL-terminated ASCII at entry+0x10
//
// Because Gametype_Find reads that fixed global, the populating parse must
// target it, so the global IS the authoritative table. Rather than fight the
// extern-thunk hook mechanism (which truncates the hooked function, making a
// run-original return-hook fragile), we observe the global: once it becomes
// non-empty we write it verbatim to STORE.BIN next to the emulator exe. The
// tick is driven from XamInputGetState (polled every frame by the title), so
// the dump lands the first frame after the registry is populated.

#include <cstdint>
#include <fstream>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {
namespace xam {

static constexpr uint32_t kMw3TitleId = 0x415608CB;
static constexpr uint32_t kGtCountAddr = 0x8371E504;    // u32 BE count
static constexpr uint32_t kGtEntriesAddr = 0x8371E508;  // entries base
static constexpr uint32_t kGtStride = 0x78;             // 120 bytes/entry
static constexpr uint32_t kGtMaxCount = 8192;           // sanity bound

void Mw3GametypeDumpTick() {
  static bool dumped = false;
  if (dumped) {
    return;
  }
  auto* ks = kernel_state();
  if (!ks || ks->title_id() != kMw3TitleId) {
    return;
  }
  auto* memory = ks->memory();
  if (!memory) {
    return;
  }

  auto* count_ptr = memory->TranslateVirtual<uint8_t*>(kGtCountAddr);
  if (!count_ptr) {
    return;
  }
  const uint32_t count = xe::load_and_swap<uint32_t>(count_ptr);
  // Diagnostic: report the registry state at each trigger so we can see whether
  // it ever populates before the title tears down.
  XELOGD("MW3 gametype dump: registry count = {} @ {:08X}", count,
         kGtCountAddr);
  if (count == 0 || count >= kGtMaxCount) {
    return;  // not populated yet (or garbage) -- keep watching
  }

  // Dump the count header followed by every entry, verbatim (big-endian guest
  // bytes). Layout is documented above; consumer can walk entry+0x10 for codes.
  const uint32_t total = 4u + count * kGtStride;
  const auto* base = memory->TranslateVirtual<const uint8_t*>(kGtCountAddr);
  if (!base) {
    return;
  }

  const auto path = xe::filesystem::GetExecutableFolder() / "STORE.BIN";
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    XELOGE("MW3 gametype dump: failed to open {}", path.string());
    dumped = true;  // don't spin on an unwritable path
    return;
  }
  f.write(reinterpret_cast<const char*>(base), total);
  f.close();
  dumped = true;
  XELOGD("MW3 gametype dump: wrote {} bytes ({} entries) to {}", total, count,
         path.string());
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
