/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstring>
#include <vector>

#include "xenia/apu/sdl/voice_chat.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

// The voice path works by signature-scanning the loaded title for Microsoft's
// g726adpcm codec and patching a hook over it (see FindG726Adpcm /
// InstallExternHook below). That rewrites guest code at runtime, so when a
// title misbehaves this is worth ruling out first. Set to false to leave the
// title's own codec completely untouched; voice chat then does nothing, but
// nothing is injected either.
DEFINE_bool(voice_xbadpcm_inject, true,
            "Inject the XBADPCM (g726adpcm) codec hook into the running title "
            "to enable voice chat. Disable to leave guest code unpatched.",
            "Kernel");

namespace xe {
namespace kernel {
namespace xam {

// Opaque voice object handed to the title. Layout is internal to us; titles
// only pass the pointer back to the other XamVoice* calls. Sized to cover the
// real xam object so stray reads land in zeroed memory.
constexpr uint32_t kVoiceObjectSize = 0x2000;

dword_result_t XamVoiceIsActiveProcess_entry() {
  // Returning non-zero keeps the title's voice path alive (0 short-circuits
  // it).
  return 1;
}
DECLARE_XAM_EXPORT1(XamVoiceIsActiveProcess, kNone, kImplemented);

dword_result_t XamVoiceCreate_entry(dword_t user_index,
                                    dword_t max_attached_packets,
                                    lpdword_t out_voice_ptr, dword_t a4,
                                    dword_t a5, dword_t a6) {
  if (!out_voice_ptr) {
    return X_E_INVALIDARG;
  }
  uint32_t voice_ptr =
      kernel_state()->memory()->SystemHeapAlloc(kVoiceObjectSize);
  if (!voice_ptr) {
    *out_voice_ptr = 0;
    return X_E_INVALIDARG;
  }
  std::memset(kernel_state()->memory()->TranslateVirtual<uint8_t*>(voice_ptr),
              0, kVoiceObjectSize);
  *out_voice_ptr = voice_ptr;
  apu::sdl::VoiceChat::Get().AddRef();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVoiceCreate, kNone, kImplemented);

dword_result_t XamVoiceClose_entry(lpunknown_t voice_ptr) {
  if (voice_ptr) {
    kernel_state()->memory()->SystemHeapFree(voice_ptr.guest_address());
    apu::sdl::VoiceChat::Get().Release();
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVoiceClose, kNone, kImplemented);

dword_result_t XamVoiceHeadsetPresent_entry(lpunknown_t voice_ptr) {
  // Report a headset only while Voice Chat is enabled (Sound menu). When it's
  // off, titles see no headset and route voice to the speakers instead.
  return apu::sdl::VoiceChat::Get().enabled() ? 1 : 0;
}
DECLARE_XAM_EXPORT1(XamVoiceHeadsetPresent, kNone, kImplemented);

// Locate Microsoft's real g726adpcm (the XBADPCM<->PCM codec) in the loaded
// title by signature. It's the same xhv2 static-lib code in every voice title;
// only the load address varies, so one signature works for all of them with no
// per-game patching. We then run it in place via the JIT for a bit-exact decode
// (the loader has already resolved its tables and save/restore stubs). Returns
// the entry VA or 0.
static uint32_t FindG726Adpcm() {
  // 7 fixed (non-relocated) instructions at entry+0x08, anchored by mflr r12 at
  // entry: li r14,0; stw r4,0x1C(r1); mr r8,r4; stw r5,0x24(r1); stw
  // r6,0x2C(r1); cmpwi cr6,r6,0; stb r14,0xFF60(r1).
  static const uint8_t kSig[28] = {0x39, 0xC0, 0x00, 0x00, 0x90, 0x81, 0x00,
                                   0x1C, 0x7C, 0x88, 0x23, 0x78, 0x90, 0xA1,
                                   0x00, 0x24, 0x90, 0xC1, 0x00, 0x2C, 0x2F,
                                   0x06, 0x00, 0x00, 0x99, 0xC1, 0xFF, 0x60};
  static const uint8_t kMflr[4] = {0x7D, 0x88, 0x02, 0xA6};
  auto module = kernel_state()->GetExecutableModule();
  if (!module) {
    return 0;
  }
  auto* xex = module->xex_module();
  if (!xex) {
    return 0;
  }
  const uint32_t base = xex->base_address();
  uint32_t size = xex->image_size();
  if (!base || !size) {
    return 0;
  }
  if (size > 0x4000000) {
    size = 0x4000000;
  }
  auto* memory = kernel_state()->memory();
  const uint32_t end = base + size;
  XELOGD("XBADPCM: scanning {:08X}..{:08X}", base, end);
  // Walk page by page; only touch pages that are actually mapped-readable,
  // since the image has uncommitted gaps between sections and reading one
  // faults.
  for (uint32_t page = base & ~0xFFFu; page < end; page += 0x1000) {
    auto* heap = memory->LookupHeap(page);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(page, &protect) ||
        !(protect & kMemoryProtectRead)) {
      continue;
    }
    // Keep the whole 36-byte window (mflr at a-8 .. sig end at a+28) inside
    // this readable page so we never touch a possibly-unmapped neighbor.
    uint32_t a = page + 8;
    if (a < base + 8) {
      a = base + 8;
    }
    for (; a + sizeof(kSig) <= page + 0x1000 && a + sizeof(kSig) < end;
         a += 4) {
      auto* p = memory->TranslateVirtual<uint8_t*>(a);
      if (p[0] == 0x39 && std::memcmp(p, kSig, sizeof(kSig)) == 0 &&
          std::memcmp(p - 8, kMflr, 4) == 0) {
        return a - 8;  // entry = match - 8
      }
    }
  }
  return 0;
}

// ---- codec hook -------------------------------------------------------------
// We intercept xhv2's controller-headset ADPCM codec (g726adpcm) and do the
// audio I/O on its uncompressed (PCM) side, keeping the whole voice path on PCM
// with no lossy controller-codec conversion:
//   mode 1 (mic decode):     hand XHV live host-mic PCM in `out`.
//   mode 0 (speaker encode):  play the PCM XHV handed us in `in`.
// The tiny ADPCM buffers are throwaway. Signature g726adpcm(mode, in, out,
// count, state); count is the sample count.
static void G726AdpcmHook(cpu::ppc::PPCContext* ctx, KernelState* ks) {
  const uint32_t mode = static_cast<uint32_t>(ctx->r[3]);
  const uint32_t in = static_cast<uint32_t>(ctx->r[4]);
  const uint32_t out = static_cast<uint32_t>(ctx->r[5]);
  const uint32_t count = static_cast<uint32_t>(ctx->r[6]);
  XELOGD("XBADPCM: g726 HIT mode={} in={:08X} out={:08X} count={}", mode, in,
         out,
         count);  // every call, ungated -- so we can see mode 0 (speaker) vs 1
  ctx->r[3] = mode;  // g726adpcm returns the mode

  // Defensive: bail on anything that doesn't look like a real codec call so a
  // bad/uninitialized arg can never fault the host.
  auto ok = [](uint32_t a) { return a >= 0x1000u && a < 0xC0000000u; };
  if (count == 0 || count > 8192 || !ok(in) || !ok(out)) {
    return;
  }

  auto* memory = ks->memory();
  auto& voice = apu::sdl::VoiceChat::Get();
  // Reused per-thread scratch so the hook never allocates on XHV's thread.
  thread_local std::vector<int16_t> scratch;
  if (mode == 1) {
    // Mic decode: write live mic PCM (16-bit big-endian) to out.
    auto* o = memory->TranslateVirtual<uint8_t*>(out);
    scratch.assign(count, 0);
    voice.ReadCapturePcm(scratch.data(), count);
    for (uint32_t i = 0; i < count; ++i) {
      o[i * 2] = static_cast<uint8_t>(scratch[i] >> 8);
      o[i * 2 + 1] = static_cast<uint8_t>(scratch[i] & 0xFF);
    }
  } else {
    // Speaker encode: play the PCM XHV produced, upsampled 2x (linear interp).
    auto* ip = memory->TranslateVirtual<uint8_t*>(in);
    auto sample = [&](uint32_t i) {
      return static_cast<int16_t>((ip[i * 2] << 8) | ip[i * 2 + 1]);
    };
    scratch.resize(count * 2);
    for (uint32_t i = 0; i < count; ++i) {
      int16_t cur = sample(i);
      int16_t nxt = (i + 1 < count) ? sample(i + 1) : cur;
      scratch[i * 2] = cur;
      scratch[i * 2 + 1] = static_cast<int16_t>((cur + nxt) / 2);
    }
    voice.PlayPcm(scratch.data(), count * 2);
    std::memset(memory->TranslateVirtual<uint8_t*>(out), 0, count / 2);
  }
}

// Install the import-style guest->host thunk (sc 2; blr) at a guest function
// and route it to a host handler -- the same mechanism Xenia uses for kernel
// imports. Works for any title (the codec code is identical across them).
static bool InstallExternHook(uint32_t addr,
                              cpu::GuestFunction::ExternHandler handler) {
  if (!addr) {
    return false;
  }
  auto* processor = kernel_state()->processor();
  auto* module = processor->LookupModule(addr);
  if (!module) {
    XELOGD("XBADPCM: hook {:08X} no module", addr);
    return false;
  }
  cpu::Function* fn = nullptr;
  module->DeclareFunction(addr, &fn);
  if (!fn) {
    XELOGD("XBADPCM: hook {:08X} declare failed", addr);
    return false;
  }
  fn->set_end_address(addr + 12);
  static_cast<cpu::GuestFunction*>(fn)->SetupExtern(handler);
  fn->set_status(cpu::Symbol::Status::kDeclared);
  XELOGD("XBADPCM: hook {:08X} declared extern", addr);
  // Write the import-style thunk into guest memory, then drop any cached
  // compile so it re-JITs into a host call on next invocation. Ensure the entry
  // page is writable first -- .text is often mapped read+execute only.
  auto* memory = kernel_state()->memory();
  if (auto* heap = memory->LookupHeap(addr)) {
    uint32_t old_protect = 0;
    heap->Protect(addr & ~0xFFFu, 0x2000,
                  kMemoryProtectRead | kMemoryProtectWrite, &old_protect);
  }
  auto* p = memory->TranslateVirtual<uint8_t*>(addr);
  xe::store_and_swap<uint32_t>(p + 0, 0x44000042);  // sc 2 (LEV=2 -> host call)
  xe::store_and_swap<uint32_t>(p + 4, 0x4E800020);  // blr
  xe::store_and_swap<uint32_t>(p + 8, 0x60000000);  // nop
  xe::store_and_swap<uint32_t>(p + 12, 0x60000000);  // nop
  processor->RemoveFunctionByAddress(addr);
  XELOGD("XBADPCM: hook {:08X} armed", addr);
  return true;
}

dword_result_t XamVoiceSubmitPacket_entry(lpunknown_t voice_ptr,
                                          dword_t packet_count,
                                          lpdword_t buffer_ptr) {
  // Install our codec hooks once, the first time the title touches the voice
  // path. The audio rides the hooks (we intercept xhv2's ADPCM codec and do the
  // PCM I/O there), so this kernel entry itself just succeeds.
  static bool installed = false;
  if (!installed) {
    installed = true;
    if (!cvars::voice_xbadpcm_inject) {
      // Leave the title's codec alone entirely -- no scan, no patch.
      XELOGD("XBADPCM: injection disabled by cvar, guest codec untouched");
    } else {
      uint32_t g726 = FindG726Adpcm();
      bool ok = InstallExternHook(g726, &G726AdpcmHook);
      XELOGD("XBADPCM: g726adpcm hook {} ({:08X})", ok ? "installed" : "FAILED",
             g726);
    }
  }
  return uint32_t(packet_count);
}
DECLARE_XAM_EXPORT1(XamVoiceSubmitPacket, kNone, kImplemented);

dword_result_t XamVoiceGetMicArrayStatus_entry() {
  // Returning 0 here tells caller mic is not connected
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayStatus, kNone, kStub);

dword_result_t XamVoiceGetBatteryStatus_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceGetBatteryStatus, kNone, kStub);

dword_result_t XamVoiceSetAudioCaptureRoutine_entry(lpunknown_t voice_ptr,
                                                    lpdword_t desc_ptr) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVoiceSetAudioCaptureRoutine, kNone, kStub);

dword_result_t XamVoiceGetDirectionalData_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceGetDirectionalData, kNone, kStub);

dword_result_t XamVoiceSetMicArrayIdleUsers_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceSetMicArrayIdleUsers, kNone, kStub);

dword_result_t XamVoiceMuteMicArray_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceMuteMicArray, kNone, kStub);

dword_result_t XamVoiceGetMicArrayUnderrunStatus_entry() {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayUnderrunStatus, kNone, kStub);

dword_result_t XamVoiceDisableMicArray_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceDisableMicArray, kNone, kStub);

dword_result_t XamVoiceSetMicArrayBeamAngle_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceSetMicArrayBeamAngle, kNone, kStub);

dword_result_t XamVoiceRecordUserPrivileges_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamVoiceRecordUserPrivileges, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Voice);
