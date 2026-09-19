/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_avatar.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_avatar_assets.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xna/xna_avatar.h"
#include "xenia/memory.h"

DEFINE_bool(allow_avatar_initialization, false,
            "Enable Avatar Initialization\n"
            "Only set true when testing Avatar games. Certain games may crash "
            "due to requirement of full avatar implementation.",
            "Kernel");

namespace xe {
namespace kernel {
namespace xam {

static_assert(sizeof(X_AVATAR_METADATA) ==
              xe::kernel::xna::avatar::kManifestBytes);

// The guest return address of whoever called the shim we are inside. Feed it
// to XEXMagic (AvatarEditor.xex loads at 0x92000000) to name the call site.
static uint32_t GuestCaller() {
  auto* state = cpu::ThreadState::Get();
  return state ? static_cast<uint32_t>(state->context()->lr) : 0;
}

// Every avatar shim announces itself, its caller and its overlapped. The
// A-press hang leaves NO export trace at all, so the last few lines before the
// silence are the only evidence of what the title is spinning inside - and a
// call with an overlapped that never gets a matching "completed" line is the
// bug. Warning level on purpose: log_level 2 drops XELOGD.
static void TraceAvatar(const char* name, uint32_t overlapped) {
  XELOGW("[avatar] {} caller {:08X} overlapped {:08X}", name, GuestCaller(),
         overlapped);
}

static void TraceAvatarDone(const char* name, uint32_t overlapped,
                            uint32_t result) {
  XELOGW("[avatar] {} completed overlapped {:08X} result {:08X}", name,
         overlapped, result);
}

static void WriteAvatarMetadata(
    uint32_t guest, const xe::kernel::xna::XnaAvatarManifestBytes& bytes) {
  uint8_t* out =
      guest ? kernel_memory()->TranslateVirtual<uint8_t*>(guest) : nullptr;
  if (out) {
    std::memcpy(out, bytes.data(), sizeof(X_AVATAR_METADATA));
  }
}

// Start/End
dword_result_t XamAvatarInitialize_entry(dword_t version,
                                         dword_t coordinate_system,
                                         dword_t processor_number,
                                         lpdword_t function_ptrs, dword_t heap,
                                         dword_t flags) {
  TraceAvatar("XamAvatarInitialize", 0);
  SetAvatarCoordinateSystem(coordinate_system);
  if (kernel_state()->title_id() == kAvatarEditorID) {
    return X_STATUS_SUCCESS;
  }
  if (cvars::allow_avatar_initialization ||
      xe::kernel::xna::XnaAvatarCatalog()) {
    return X_STATUS_SUCCESS;
  }
  XELOGW(
      "XamAvatarInitialize: the Avatar update (AvatarAssetPack.toc) is not "
      "installed - avatars stay disabled");
  return ~0u;
}
DECLARE_XAM_EXPORT1(XamAvatarInitialize, kAvatars, kStub);

void XamAvatarShutdown_entry() {
  TraceAvatar("XamAvatarShutdown", 0);
  // Calls XMsgStartIORequestEx(0xf3,0x600002,0,0,0,0).
  // in 12611 its XamUnloadSysApp(0xf2,1)
}
DECLARE_XAM_EXPORT1(XamAvatarShutdown, kAvatars, kStub);

// Get & Set
// Defined below, beside the enumeration it filters.
static bool AvatarAssetMatches(const xe::kernel::xna::avatar::Entry& entry,
                               uint32_t kind_mask, uint32_t body);

dword_result_t XamAvatarGetManifestLocalUser_entry(
    dword_t user_index, pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarGetManifestLocalUser", overlapped_ptr.guest_address());
  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;

    if (user_index >= XUserMaxUserCount) {
      extended_error = X_E_INVALIDARG;
      return X_ERROR_INVALID_PARAMETER;
    }

    if (!avatar_metadata_ptr) {
      extended_error = X_E_INVALIDARG;
      return X_ERROR_INVALID_PARAMETER;
    }

    const auto user_profile =
        kernel_state()->xam_state()->GetUserProfile(user_index);

    if (!user_profile) {
      extended_error = X_E_NO_SUCH_USER;
      return X_ERROR_FUNCTION_FAILED;
    }

    const auto manifest =
        xe::kernel::xna::XnaAvatarManifestForXuid(user_profile->xuid());
    WriteAvatarMetadata(avatar_metadata_ptr.guest_address(), manifest);
    // The editor builds its component collection (0x92331D3C) purely from this
    // manifest's component list -
    // Avatars::ManifestReader::GetComponentInfoCount decides it. With zero
    // components the feature scenes report no items and the navigation refuses
    // to install them, so this count is load bearing.
    xe::kernel::xna::avatar::Description parsed;
    const bool ok = xe::kernel::xna::avatar::ParseAnyDescription(
        xe::kernel::xna::XnaAvatarCatalog(), manifest.data(), manifest.size(),
        &parsed);
    XELOGW(
        "[avatar] manifest for {:016X}: parsed={} components={} body={} "
        "items set={}",
        user_profile->xuid(), ok, parsed.components.size(), parsed.body,
        std::count_if(parsed.items.begin(), parsed.items.end(),
                      [](uint16_t item) {
                        return item != xe::kernel::xna::avatar::kNoItem;
                      }));
    // The editor has no catalogue of its own. Every id we put in the manifest
    // it resolves against the records XamAvatarEnumAssets handed it, so an id
    // that is not in that enumeration names an asset the editor does not
    // believe exists - and a category it cannot find an asset for is a
    // category it will not let you colour. This prints, for every id in the
    // manifest, whether it is in the catalogue at all and whether our own
    // enumeration filter would have shown it to the editor.
    auto* catalog = xe::kernel::xna::XnaAvatarCatalog();
    const auto check = [&](const char* what, size_t index, const uint8_t* raw) {
      const auto* entry = catalog ? catalog->FindAsset(raw) : nullptr;
      XELOGW(
          "[avatar]   {} {}: id {:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}"
          "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X} mask "
          "{:02X}{:02X} -> {}",
          what, index, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6],
          raw[7], raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14],
          raw[15], raw[0x10], raw[0x11],
          entry ? fmt::format("catalogue entry {} kind {:08X} enumerated={}",
                              entry->index, entry->kind,
                              AvatarAssetMatches(*entry, 0x01FFFFFF, 1))
                : std::string("NOT IN THE CATALOGUE"));
    };
    const uint8_t* manifest_bytes = manifest.data();
    static constexpr const char* kFaceNames[6] = {
        "face mouth", "face eyes",   "face eyebrows",
        "face fhair", "face shadow", "face paint"};
    for (size_t i = 0; i < 6; ++i) {
      const uint8_t* raw = manifest_bytes + 0x3C + i * 0x20;
      bool empty = true;
      for (size_t k = 0; k < 16; ++k) {
        empty = empty && raw[k] == 0;
      }
      if (empty) {
        XELOGW("[avatar]   {} {}: empty", kFaceNames[i], i);
        continue;
      }
      check(kFaceNames[i], i, raw);
    }
    check("body", 0, manifest_bytes + 0x120);
    check("head", 0, manifest_bytes + 0x140);
    for (size_t i = 0; i < parsed.components.size(); ++i) {
      check("component", i, parsed.components[i].data());
    }
    TraceAvatarDone("XamAvatarGetManifestLocalUser",
                    overlapped_ptr.guest_address(), X_ERROR_SUCCESS);
    return X_ERROR_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    X_RESULT result = run(extended_error, length);

    return result == X_ERROR_SUCCESS ? result : extended_error;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamAvatarGetManifestLocalUser, kAvatars, kStub);

dword_result_t XamAvatarGetManifestsByXuid_entry(
    dword_t user_index, dword_t xuid_count, lpqword_t xuid, dword_t unk,
    dword_t avatar_info_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarGetManifestsByXuid", overlapped_ptr.guest_address());
  if (xuid && avatar_info_ptr) {
    const xe::be<uint64_t>* xuids = xuid;
    for (uint32_t i = 0; i < xuid_count; ++i) {
      const uint64_t id = xuids[i];
      WriteAvatarMetadata(
          avatar_info_ptr + i * uint32_t(sizeof(X_AVATAR_METADATA)),
          xe::kernel::xna::XnaAvatarManifestForXuid(id));
    }
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetManifestsByXuid, kAvatars, kStub)

dword_result_t XamAvatarGetAssetsResultSize_entry(
    dword_t avatar_component_mask, lpdword_t result_buffer_size_ptr,
    lpdword_t gpu_resource_buffer_size_ptr) {
  TraceAvatar("XamAvatarGetAssetsResultSize", 0);
  if (result_buffer_size_ptr) {
    *result_buffer_size_ptr = kAvatarResultBufferSize;
  }
  if (gpu_resource_buffer_size_ptr) {
    *gpu_resource_buffer_size_ptr = kAvatarGpuBufferSize;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssetsResultSize, kAvatars, kStub);

dword_result_t XamAvatarGetAssets_entry(
    pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr,
    dword_t avatar_component_mask, dword_t flags, dword_t result_buffer_ptr,
    dword_t gpu_resource_buffer_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarGetAssets", overlapped_ptr.guest_address());
  const uint32_t overlapped_address = overlapped_ptr.guest_address();
  const uint32_t metadata_address = avatar_metadata_ptr.guest_address();
  const uint32_t mask = avatar_component_mask;
  const uint32_t result_address = result_buffer_ptr;
  const uint32_t gpu_address = gpu_resource_buffer_ptr;
  std::vector<uint8_t> metadata;
  if (metadata_address) {
    const uint8_t* source =
        kernel_memory()->TranslateVirtual<const uint8_t*>(metadata_address);
    metadata.assign(source, source + sizeof(X_AVATAR_METADATA));
  }
  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    length = 0;
    const X_RESULT result =
        BuildAvatarAssets(metadata.empty() ? nullptr : metadata.data(),
                          metadata.size(), mask, result_address, gpu_address);
    extended_error = result;
    TraceAvatarDone("XamAvatarGetAssets", overlapped_address, result);
    return result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS
                                     : X_ERROR_FUNCTION_FAILED;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    const X_RESULT result = run(extended_error, length);
    return result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : extended_error;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssets, kAvatars, kStub);

// SIX arguments, not five. The real export packs a1..a6 into a 0x18 byte
// message and hands it to the avatar sysapp with XMsgInProcessCall(0xF2,
// 0x60000A) - there is no overlapped, it is synchronous. Declaring five here
// shifted every parameter after the missing one onto the wrong register.
dword_result_t XamAvatarSetCustomAsset_entry(
    dword_t buffer_size, lpvoid_t asset_data_ptr, dword_t custom_color_count,
    lpdword_t custom_colors_ptr, dword_t unknown,
    pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr) {
  TraceAvatar("XamAvatarSetCustomAsset", 0);
  // The SDK wrapper (XAvatarSetCustomAsset, 0x92143058 in the editor) checks
  // all of this before it ever reaches the export, so a call that fails here
  // came from something that did not go through the wrapper.
  if (!buffer_size || buffer_size > 0xFFFFFF || !asset_data_ptr ||
      custom_color_count > 3 || (custom_color_count && !custom_colors_ptr) ||
      !avatar_metadata_ptr) {
    return X_E_INVALIDARG;
  }
  const uint8_t* blob = asset_data_ptr.as<const uint8_t*>();
  const bool is_strb = buffer_size >= 4 && std::memcmp(blob, "STRB", 4) == 0;
  XELOGW(
      "[avatar] XamAvatarSetCustomAsset: {} bytes at {:08X} ({}), {} custom "
      "colour(s), metadata {:08X} - accepted and ignored",
      uint32_t(buffer_size), asset_data_ptr.guest_address(),
      is_strb ? "STRB" : "not an asset blob", uint32_t(custom_color_count),
      avatar_metadata_ptr.guest_address());
  // NOT applied, and not faked either.
  //
  // The export packs its six arguments into a 0x18 block and makes a
  // synchronous in-process call to app 0xF2 message 0x60000A; the handler is
  // not in xam's exported code, and what it writes into the metadata - the
  // SDK says the metadata ends up holding a POINTER to this very buffer - is
  // a layout nothing on this image states. The one thing the blob cannot tell
  // us is which component it replaces: the catalogue takes an asset's kind
  // from the first dword of its asset id, and a title-supplied buffer has no
  // id. The sample item on disk has no tag 8 record either, so the body/type
  // metadata is not always there to fall back on.
  //
  // Succeeding without applying it means the avatar draws with its own items
  // instead of the title's custom one, which is what happens today anyway;
  // failing would send callers that check the result down an error path for a
  // feature that was never there. The log line above is what a run needs to
  // take this further.
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarSetCustomAsset, kAvatars, kStub)

dword_result_t XamAvatarSetManifest_entry(
    dword_t user_index, dword_t avatar_info_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarSetManifest", overlapped_ptr.guest_address());
  const uint32_t overlapped_address = overlapped_ptr.guest_address();
  if (!avatar_info_ptr) {
    // Same shape as XamAvatarLoadAnimation: deliver the error through the
    // overlapped when one was given, or a caller tracking this as pending
    // never learns it failed.
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                  X_ERROR_INVALID_PARAMETER);
      return X_ERROR_IO_PENDING;
    }
    // Synchronously these return an extended error (a WIN32 code), not an
    // HRESULT - see XamAvatarEnumAssets for the pattern in the real xam.xex.
    return X_ERROR_INVALID_PARAMETER;
  }
  const uint8_t* source = kernel_memory()->TranslateVirtual<const uint8_t*>(
      static_cast<uint32_t>(avatar_info_ptr));
  const std::vector<uint8_t> manifest(source, source + kMaxUserDataSize);
  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;
    const auto& user_profile =
        kernel_state()->xam_state()->GetUserProfile(user_index);

    if (!user_profile) {
      extended_error = X_E_NO_SUCH_USER;
      return X_ERROR_FUNCTION_FAILED;
    }

    const UserSetting setting(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1,
                              manifest);

    kernel_state()->xam_state()->user_tracker()->UpsertSetting(
        user_profile->xuid(), kDashboardID, &setting);

    xe::kernel::xna::avatar::Description description;
    if (xe::kernel::xna::avatar::ParseAnyDescription(
            xe::kernel::xna::XnaAvatarCatalog(), manifest.data(),
            manifest.size(), &description)) {
      xe::kernel::xna::XnaAvatarWriteProfileFile(user_profile->xuid(),
                                                 description);
    }

    TraceAvatarDone("XamAvatarSetManifest", overlapped_address,
                    X_ERROR_SUCCESS);
    return X_STATUS_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    X_RESULT result = run(extended_error, length);

    return result == X_ERROR_SUCCESS ? result : extended_error;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamAvatarSetManifest, kAvatars, kStub);

dword_result_t XamAvatarGetMetadataRandom_entry(
    dword_t body_type, dword_t avatars_count,
    pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarGetMetadataRandom", overlapped_ptr.guest_address());
  const int32_t wire_body =
      body_type == static_cast<uint32_t>(X_AVATAR_BODY_TYPE::Male)     ? 1
      : body_type == static_cast<uint32_t>(X_AVATAR_BODY_TYPE::Female) ? 0
                                                                       : -1;
  const uint32_t base = avatar_metadata_ptr.guest_address();
  for (uint32_t i = 0; base && i < avatars_count; ++i) {
    WriteAvatarMetadata(base + i * uint32_t(sizeof(X_AVATAR_METADATA)),
                        xe::kernel::xna::XnaAvatarRandomManifest(wire_body));
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetMetadataRandom, kAvatars, kStub);

dword_result_t XamAvatarGetMetadataSignedOutProfileCount_entry(
    lpdword_t profile_count_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarGetMetadataSignedOutProfileCount",
              overlapped_ptr.guest_address());
  if (profile_count_ptr) {
    *profile_count_ptr = 0;
  }
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetMetadataSignedOutProfileCount, kAvatars, kStub);

dword_result_t XamAvatarGetMetadataSignedOutProfile_entry(
    dword_t profile_index, pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarGetMetadataSignedOutProfile",
              overlapped_ptr.guest_address());
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetMetadataSignedOutProfile, kAvatars, kStub);

// XAM does not parse anything here. 0x816753C8 wraps the manifest in a reader
// and tail-calls 0x8196CA80, which is two sixteen-byte memcmps of the BODY
// ENTRY at manifest+0x120 against two constants in its own .rdata:
//
//   0x81632E68  00 00 00 02 00 00 00 01 C1 C8 F1 09 A1 9C B2 E0  -> 1 male
//   0x81632E78  00 00 00 02 00 01 00 02 C1 C8 F1 09 A1 9C B2 E0  -> 2 female
//
// and ZERO for anything else. Deriving the answer from our own description
// parser instead could never return that zero, and it answered Male for a
// manifest whose body entry says nothing of the kind. Our own writer emits
// exactly these two ids, so the value does not change for a manifest we built
// - but one we did not now gets the console's answer instead of a guess.
dword_result_t XamAvatarManifestGetBodyType_entry(
    pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr) {
  TraceAvatar("XamAvatarManifestGetBodyType", 0);
  static constexpr uint8_t kMaleBodyId[16] = {
      0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
      0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0};
  static constexpr uint8_t kFemaleBodyId[16] = {
      0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02,
      0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0};
  constexpr uint32_t kManifestBodyOffset = 0x120;
  const uint32_t address = avatar_metadata_ptr.guest_address();
  const uint8_t* bytes =
      address ? kernel_memory()->TranslateVirtual<const uint8_t*>(address)
              : nullptr;
  if (!bytes) {
    return static_cast<uint8_t>(X_AVATAR_BODY_TYPE::Unknown);
  }
  const uint8_t* body = bytes + kManifestBodyOffset;
  if (std::memcmp(body, kMaleBodyId, sizeof(kMaleBodyId)) == 0) {
    return static_cast<uint8_t>(X_AVATAR_BODY_TYPE::Male);
  }
  if (std::memcmp(body, kFemaleBodyId, sizeof(kFemaleBodyId)) == 0) {
    return static_cast<uint8_t>(X_AVATAR_BODY_TYPE::Female);
  }
  XELOGW(
      "[avatar] ManifestGetBodyType: body entry {:02X}{:02X}{:02X}{:02X} "
      "{:02X}{:02X} {:02X}{:02X} {:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}"
      "{:02X} matches neither stock body - XAM answers UNKNOWN",
      body[0], body[1], body[2], body[3], body[4], body[5], body[6], body[7],
      body[8], body[9], body[10], body[11], body[12], body[13], body[14],
      body[15]);
  return static_cast<uint8_t>(X_AVATAR_BODY_TYPE::Unknown);
}
DECLARE_XAM_EXPORT1(XamAvatarManifestGetBodyType, kAvatars, kImplemented);

// Complete, not a stub: the real export at 0x816755D0 is six instructions -
// it writes zero through its second argument when that is non-null, ignores
// the first entirely, and returns 1. Instrumentation is off on a retail
// console and this is what "off" looks like.
dword_result_t XamAvatarGetInstrumentation_entry(dword_t counter,
                                                 lpdword_t out_value) {
  TraceAvatar("XamAvatarGetInstrumentation", 0);
  if (out_value) {
    *out_value = 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(XamAvatarGetInstrumentation, kAvatars, kImplemented);

// These two are the same shim twice over: r3 is a pointer to the sixteen-byte
// XAVATAR_ASSET_ID, r7 is the overlapped, and r4/r5/r6 go into a 0x1C message
// for app 0xF2 - 0x60000B for the icon, 0x600008 for the binary.
//
// What the three middle arguments MEAN is not decided here, and deliberately:
// no module on this console image calls either export (AvatarEditor.xex
// imports both and never reaches them; dash.xex imports neither), xam itself
// never references its own "icon.png" string, and the public XAvatar SDK has
// no wrapper for them. So the ordering of buffer, size and out-size would be
// an invention, and a wrong one writes over the caller's memory.
//
// They therefore report "nothing here" and complete cleanly, and print the
// three dwords so the first real caller settles it in one line of log rather
// than another read of the disassembly.
static dword_result_t AvatarAssetFileStub(
    const char* name, uint32_t message, lpvoid_t asset_id_ptr, dword_t a2,
    dword_t a3, dword_t a4, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar(name, overlapped_ptr.guest_address());
  const uint8_t* asset_id =
      asset_id_ptr ? asset_id_ptr.as<const uint8_t*>() : nullptr;
  const auto* installed =
      asset_id ? xna::XnaFindInstalledAvatarAsset(asset_id) : nullptr;
  XELOGW(
      "[avatar] {} msg {:06X} id {:08X} args {:08X} {:08X} {:08X} - {}; the "
      "argument roles are unknown, so nothing is written",
      name, message, asset_id_ptr.guest_address(), uint32_t(a2), uint32_t(a3),
      uint32_t(a4),
      installed ? "the asset IS installed" : "no such installed asset");
  const X_RESULT result = X_ERROR_FUNCTION_FAILED;
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}

dword_result_t XamAvatarGetAssetIcon_entry(
    lpvoid_t asset_id_ptr, dword_t a2, dword_t a3, dword_t a4,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return AvatarAssetFileStub("XamAvatarGetAssetIcon", 0x60000B, asset_id_ptr,
                             a2, a3, a4, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssetIcon, kAvatars, kStub);

dword_result_t XamAvatarGetAssetBinary_entry(
    lpvoid_t asset_id_ptr, dword_t a2, dword_t a3, dword_t a4,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return AvatarAssetFileStub("XamAvatarGetAssetBinary", 0x600008, asset_id_ptr,
                             a2, a3, a4, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssetBinary, kAvatars, kStub);

void XamAvatarGetInstalledAssetPackageDescription_entry(
    pointer_t<X_ASSET_ID> asset_id_ptr,
    pointer_t<XCONTENT_AGGREGATE_DATA>
        content_data_ptr  // pointer_t<XCONTENT_DATA_INTERNAL>
) {
  TraceAvatar("XamAvatarGetInstalledAssetPackageDescription", 0);
  XCONTENT_AGGREGATE_DATA content_data;
  content_data.content_type = XContentType::kAvatarItem;
  content_data.xuid = 0;
  content_data.title_id = asset_id_ptr->title_id;
  std::string file_name =
      fmt::format("{:016X}{:08X}{:08X}", asset_id_ptr->data,
                  asset_id_ptr->data2, asset_id_ptr->title_id);
  content_data.set_file_name(file_name);
  // The real export memsets 0x200 bytes before filling anything, which is more
  // than this struct - anything the caller reads past it would otherwise be
  // whatever was in the buffer.
  if (content_data_ptr) {
    std::memset(content_data_ptr, 0, 0x200);
  }
  *content_data_ptr = content_data;
  XELOGD("Looking for avatar asset: {:X}", file_name);
}
DECLARE_XAM_EXPORT1(XamAvatarGetInstalledAssetPackageDescription, kAvatars,
                    kSketchy);

void XamAvatarSetMocks_entry() {
  TraceAvatar("XamAvatarSetMocks", 0);
  // No-op.
}
DECLARE_XAM_EXPORT1(XamAvatarSetMocks, kAvatars, kStub);

// Animation
const static std::map<uint64_t, std::string> XAnimationTypeMap = {
    // Animation Generic Stand
    {0x0040000000030003, "Animation Generic Stand 0"},
    {0x0040000000040003, "Animation Generic Stand 1"},
    {0x0040000000050003, "Animation Generic Stand 2"},
    {0x0040000000270003, "Animation Generic Stand 3"},
    {0x0040000000280003, "Animation Generic Stand 4"},
    {0x0040000000290003, "Animation Generic Stand 5"},
    {0x00400000002A0003, "Animation Generic Stand 6"},
    {0x00400000002B0003, "Animation Generic Stand 7"},
    // Animation Idle
    {0x0040000000130001, "Animation Male Idle Looks Around"},
    {0x0040000000140001, "Animation Male Idle Stretch"},
    {0x0040000000150001, "Animation Male Idle Shifts Weight"},
    {0x0040000000260001, "Animation Male Idle Checks Hand"},
    {0x0040000000090002, "Animation Female Idle Check Nails"},
    {0x00400000000A0002, "Animation Female Idle Looks Around"},
    {0x00400000000B0002, "Animation Female Idle Shifts Weight"},
    {0x00400000000C0002, "Animation Female Idle Fixes Shoe"},
};

// https://github.com/xenia-canary/xenia-canary/commit/212c99eee2724de15f471148d10197d89794ff32
dword_result_t XamAvatarLoadAnimation_entry(
    lpqword_t asset_id_ptr, dword_t flags, lpvoid_t output,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarLoadAnimation", overlapped_ptr.guest_address());
  const uint32_t overlapped_address = overlapped_ptr.guest_address();
  const uint32_t object_address = output.guest_address();
  if (!asset_id_ptr || !object_address) {
    // The caller has already put this animation in its loading list, and
    // Avatars::Renderer_c::WaitForLoadingIdles SPINS until every entry leaves
    // the pending state - it never blocks, so there is no wait for us to
    // notice. Returning the error synchronously leaves the entry pending and
    // hangs the title with no log line at all.
    // The caller's LR says which guest code asked for this, so a null is
    // traceable to a real call site instead of guessed at. AvatarEditor.xex
    // loads at 0x92000000, so feed the address to XEXMagic.
    XELOGW(
        "XamAvatarLoadAnimation: null {} from caller {:08X} - failing through "
        "the overlapped",
        !asset_id_ptr ? "asset id" : "output object", GuestCaller());
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                  X_ERROR_INVALID_PARAMETER);
      return X_ERROR_IO_PENDING;
    }
    // Synchronously these return an extended error (a WIN32 code), not an
    // HRESULT - see XamAvatarEnumAssets for the pattern in the real xam.xex.
    return X_ERROR_INVALID_PARAMETER;
  }
  std::array<uint8_t, 16> asset_id;
  std::memcpy(asset_id.data(),
              kernel_memory()->TranslateVirtual<const uint8_t*>(
                  asset_id_ptr.guest_address()),
              asset_id.size());
  const uint64_t id = *asset_id_ptr;
  const auto found = XAnimationTypeMap.find(id);
  const std::string label = found != XAnimationTypeMap.cend()
                                ? found->second
                                : fmt::format("0x{:016X}", id);
  XELOGD("XamAvatarLoadAnimation: {} from caller {:08X}", label, GuestCaller());
  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    length = 0;
    extended_error = LoadAvatarAnimation(asset_id, object_address, label);
    TraceAvatarDone("XamAvatarLoadAnimation", overlapped_address,
                    extended_error);
    return extended_error == X_ERROR_SUCCESS ? X_ERROR_SUCCESS
                                             : X_ERROR_FUNCTION_FAILED;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    const X_RESULT result = run(extended_error, length);
    return result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : extended_error;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamAvatarLoadAnimation, kAvatars, kStub);

dword_result_t XamAvatarGenerateMipMaps_entry(
    lpdword_t avatar_assets_ptr, dword_t flags, dword_t buffer_size,
    lpdword_t mip_map_buffer_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // Renderable_c::UpdateLoading polls THIS overlapped (at renderable+0x50) and
  // stores the result into its own status field, so a miss here stalls the
  // avatar's load state machine.
  TraceAvatar("XamAvatarGenerateMipMaps", overlapped_ptr.guest_address());
  const X_RESULT result =
      GenerateAvatarMipMaps(avatar_assets_ptr.guest_address(),
                            mip_map_buffer_ptr.guest_address(), buffer_size);
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  }

  return result;
}
DECLARE_XAM_EXPORT1(XamAvatarGenerateMipMaps, kAvatars, kImplemented);

dword_result_t XamLaunchAvatarEditor_entry(dword_t user_index, dword_t flags,
                                           lpu16string_t item_ptr) {
  const std::u16string item = item_ptr ? item_ptr.value() : std::u16string();
  XELOGW("XamLaunchAvatarEditor: user {}, flags {:08X}, item '{}'",
         uint32_t(user_index), uint32_t(flags), xe::to_utf8(item));
  // The real XamLaunchAvatarEditor packs this string into the launch blob at
  // +0x0C and launches AvatarEditor.xex with it - that string is the only
  // meaningful payload the editor gets, so hand it over the same way instead of
  // dropping it on the floor.
  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  if (xam) {
    xam->loader_data().launch_data = BuildAvatarEditorLaunchData(item);
  }
  kernel_state()->emulator()->on_avatar_editor(user_index);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLaunchAvatarEditor, kAvatars, kImplemented);

// Enum
//
// The Avatar Editor's item enumerator (sub_92217420 in AvatarEditor.xex) is the
// only consumer we can actually read, and it pins the contract exactly:
//
//   *count = 0x32;                     // capacity in, returned count out
//   memset(buffer, 0, 0x8A48);         // 0x8A48 == 0x32 * 0x2C4
//   r = XamAvatarEnumAssets(buffer, count, overlapped);
//   if (r < 0) stop;
//   while (*count) { walk *count records, stride 0x2C4; call again }
//
// It SKIPS any record whose flags bit 0 is clear, takes the display name from
// +0xE4 unless flags bit 0x800 is set, and reads the package name from +0x238.
// Fields it never touches are left zero here rather than invented.
#pragma pack(push, 1)
struct X_AVATAR_ASSET_RECORD {
  uint8_t asset_id[16];  // +0x000
  // +0x010. The asset's TYPE MASK - slot bits, `4 << slot`. The editor hands
  // this field to its category mapper to decide which bucket the asset goes in,
  // and masks it to 13 bits for the component type. It is not an index.
  xe::be<uint32_t> type_mask;
  // +0x014 and +0x015 are TWO BODY-TYPE CANDIDATES, not a slot and a mask. The
  // editor's record constructor (sub_922182E8) takes the first of the two that
  // is 1 or 2 and maps 1 -> male, 2 -> female, neither -> both.
  //
  // We used to write the slot number at +0x14, which collides: a shirt is slot
  // 1 and trousers are slot 2, so both were being read as a body restriction
  // and their real body mask was never looked at.
  uint8_t body_type_primary;
  uint8_t body_type_fallback;
  uint8_t unknown_016[2];  // +0x016
  xe::be<uint32_t> flags;  // +0x018, bit 0 REQUIRED; bit 0x800 swaps the
                           //         display name for a generic one
  // +0x01C. Packed as `(colour_groups << 4) | colours_per_group`, and read by
  // the enumerator as exactly that: it takes `>> 4` as the group count and
  // `& 0xF` as the colours per group, then walks the groups below. The colour
  // count is what the tile builder turns into a button class, so zero here
  // leaves a tile with no class at all.
  uint8_t colour_layout;
  // +0x01D. The colour table: `colour_layout >> 4` groups of
  // `colour_layout & 0xF` RGB triples, packed back to back from here. The
  // editor reads triple `k` for k in [0, 3 * groups) as the three bytes at
  // +0x1D + k*3 and packs each into a dword (0x92217638..0x92217664), so three
  // groups of three colours is 27 bytes and fills this field EXACTLY. Three
  // groups is therefore the most the record can carry, whatever the pack holds.
  uint8_t colour_table[27];  // +0x01D .. +0x037
  // +0x038 onwards. The tile builder's copies from "+0x28" and "+0x38" that
  // this struct used to describe belong to the editor's own 0x198-byte record,
  // not to this buffer - it builds that from this one and the tile reads that.
  // The old component_type at +0x28 sat inside the colour table above, so it
  // was never a field here at all.
  //
  // What this region really is has not been read. It is left unnamed rather
  // than carrying the wrong name forward.
  uint8_t unknown_038[0x84];     // +0x038 .. +0x0BB
  uint8_t unknown_0bc[4];        // +0x0BC
  xe::be<uint32_t> unknown_0c0;  // +0x0C0 - these four go to a helper
  xe::be<uint32_t> unknown_0c4;  // +0x0C4   together with the asset id;
  xe::be<uint32_t> unknown_0c8;  // +0x0C8   the editor tolerates them
  xe::be<uint32_t> unknown_0cc;  // +0x0CC   zeroed
  uint8_t unknown_0d0[0x14];     // +0x0D0
  // +0x0E4 .. +0x163. wchar[0x40], and xam.xex's own writer for this record
  // (sub_8197C278 -> sub_8197BE30) confirms the width by nulling +0x162.
  //
  // This used to be declared as wchar[0x16] with a component count at +0x110
  // and another field at +0x114 - offsets that are INSIDE the name. Those two
  // belong to the editor's own 0x198-byte asset record, which its enumerator
  // builds FROM this one; the tile builder reads that record, never this.
  // Mixing the two structures up is why they ended up here.
  xe::be<uint16_t> name[0x40];
  uint8_t unknown_164[0x2C];            // +0x164 .. +0x18F
  uint8_t unknown_190[0xA4];            // +0x190 .. +0x233
  xe::be<uint32_t> package_id;          // +0x234
  xe::be<uint16_t> package_name[0x40];  // +0x238 .. +0x2B7
  xe::be<uint32_t> unknown_2b8;         // +0x2B8
  xe::be<uint32_t> unknown_2bc;         // +0x2BC
  uint8_t unknown_2c0;                  // +0x2C0
  uint8_t padding_2c1[3];               // +0x2C1
};
#pragma pack(pop)
static_assert(sizeof(X_AVATAR_ASSET_RECORD) == 0x2C4,
              "the editor steps its enumeration buffer by 0x2C4");
static_assert(offsetof(X_AVATAR_ASSET_RECORD, flags) == 0x18, "");
static_assert(offsetof(X_AVATAR_ASSET_RECORD, colour_layout) == 0x1C, "");
static_assert(offsetof(X_AVATAR_ASSET_RECORD, colour_table) == 0x1D, "");
static_assert(offsetof(X_AVATAR_ASSET_RECORD, unknown_038) == 0x38, "");
static_assert(offsetof(X_AVATAR_ASSET_RECORD, name) == 0xE4, "");
static_assert(offsetof(X_AVATAR_ASSET_RECORD, type_mask) == 0x10, "");
static_assert(offsetof(X_AVATAR_ASSET_RECORD, package_id) == 0x234, "");
static_assert(offsetof(X_AVATAR_ASSET_RECORD, package_name) == 0x238, "");

struct AvatarAssetEnumerator {
  std::mutex mutex;
  bool active = false;
  uint32_t batch = 0;
  uint32_t kind_mask = 0;
  uint32_t body = 0;
  size_t cursor = 0;
};
static AvatarAssetEnumerator avatar_asset_enum;

static bool AvatarAssetMatches(const xe::kernel::xna::avatar::Entry& entry,
                               uint32_t kind_mask, uint32_t body) {
  if (kind_mask && !(entry.kind & kind_mask)) {
    return false;
  }
  // The editor asks with an all-bits mask, which also covers animations
  // (0x400000) and the hiding templates at 0x01000000. Those are not items and
  // must not be listed.
  //
  // This used to reject anything PrimarySlot could not place, which is only the
  // eleven WORN slots - so the 18 noses, the 9 chins and the 9 pairs of ears,
  // whose kinds are 0x80000, 0x100000 and 0x200000, were never enumerated at
  // all. Their screens came up with nothing in them, and the real editor
  // dereferences a null when it opens one.
  constexpr uint32_t kOutfitBit = 0x00800000u;
  constexpr uint32_t kItemKinds = 0x003FFFFFu;  // head, body, the worn slots,
                                                // the face textures and the
                                                // three blend shapes
  if (!(entry.kind & kItemKinds) ||
      (entry.kind & ~(kItemKinds | kOutfitBit)) != 0) {
    return false;
  }
  // The 88 "(Hat)" hairstyles are worn by substitution when a hat goes on, not
  // chosen - the pack marks each one as another hairstyle's stand-in. Listing
  // them put a second copy of every haircut in the grid.
  if (entry.is_substitute()) {
    return false;
  }
  // BodyMask is 1 (male), 2 (female) or 3 (both); 0 means the pack did not say,
  // so it stays in rather than disappearing from every list.
  const uint32_t body_mask = entry.BodyMask();
  if (body && body_mask && !(body_mask & body)) {
    return false;
  }
  return true;
}

static void WriteAvatarAssetRecord(
    const xe::kernel::xna::avatar::Entry& entry,
    const xe::kernel::xna::avatar::Description& description,
    X_AVATAR_ASSET_RECORD* out) {
  std::memset(out, 0, sizeof(*out));
  // Two thirds of the pack's entries carry no id of their own, and copying
  // those handed the editor sixteen zero bytes - which the manifest writer
  // reads as "take it off", so picking one of those items removed whatever was
  // in that slot instead of replacing it.
  const std::array<uint8_t, 16> asset_id =
      xe::kernel::xna::avatar::ManifestAssetId(entry);
  std::memcpy(out->asset_id, asset_id.data(), sizeof(out->asset_id));
  // +0x10 IS THE TYPE MASK, not an index. The editor's enumerator passes this
  // field whole to its category mapper (sub_922169E0), which walks a 22-entry
  // table of slot bits and answers with the bucket the asset belongs in - and
  // the same field, masked to 13 bits, becomes the type of the
  // _XAVATAR_COMPONENT_INFO it builds.
  //
  // We used to write a sequential catalogue index here, which maps to no
  // category at all: every asset would be filed into bucket 0 or dropped. The
  // slot bit is what belongs in it.
  // The KIND BITS, not a slot bit. The editor's category mapper knows the whole
  // range - 0x1, 0x2, 0x4..0x1000 for worn slots, 0x2000..0x40000 for the face
  // textures and 0x80000/0x100000/0x200000 for nose, chin and ears - and a slot
  // bit only ever covers the worn ones. Deriving this from PrimarySlot gave 0
  // for every shape and every multi-slot outfit, so chin, nose, ears and Dress
  // up arrived with no category and their screens came up empty.
  // 0x800000 marks an outfit; it is not a category bit.
  constexpr uint32_t kOutfitBit = 0x800000u;
  out->type_mask = entry.kind & ~kOutfitBit;
  // Leave the first candidate clear so the editor falls through to the real
  // body mask. Writing anything that happens to be 1 or 2 here would override
  // it, which is exactly what the old slot number did.
  out->body_type_primary = 0;
  out->body_type_fallback = static_cast<uint8_t>(entry.BodyMask());
  // The PACK's flags byte, zero extended, exactly as XAM's record filler does
  // it (0x8197E300: `record[0x18] = *(u8*)(entry + 0x06)`). Bit 0 is the one
  // the editor's enumerator tests before it keeps a record, and bit 3 is half
  // of what decides whether a Colour tile lights up - see the colour layout
  // below. Forcing this to 1 cleared bit 3 on every asset in the pack.
  out->flags = entry.flags_byte();

  // The colour layout and the table behind it are the PACK's, copied whole.
  // XAM does not compute them: `sub_8197E300` is
  // `memcpy(record + 0x1C, entry + 0x07, 0x91)`, so byte +0x07 of the pack
  // entry lands on colour_layout and the rest follows it verbatim.
  //
  // This decides the Colour menu. `sub_920E9F60` in the editor sets a colour
  // tile's enable byte (+0x20C) from +0x30 of the asset record for whatever
  // the avatar is wearing in that category, and +0x30 is computed at
  // 0x922175B4 as:
  //
  //     groups = record[0x1C] >> 4
  //     colourable = groups > 1 || (groups == 0 && (record[0x18] & 8))
  //
  // The pack answers that per asset: every hairstyle, eyebrow, eye, facial
  // hair and eye shadow carries groups 0 with flags bit 3 set, and the 36
  // shirts with colourways carry groups 1. The old placeholder wrote
  // `(1 << 4) | 1` for everything the pack gave no table, which is groups == 1
  // - neither branch - so every Colour tile but Skin and Eye Shadow, the two
  // the editor never gates, greyed out.
  static_assert(offsetof(X_AVATAR_ASSET_RECORD, colour_layout) + 0x91 <=
                    sizeof(X_AVATAR_ASSET_RECORD),
                "the pack block has to fit from colour_layout on");
  std::memcpy(&out->colour_layout, entry.record_block.data(),
              entry.record_block.size());

  // The manifest component that matches this asset is deliberately NOT copied
  // in here any more. The 12-byte payload and the count this used to write went
  // to +0x38 and +0x110, and both of those belong to the editor's own
  // 0x198-byte record rather than to this buffer - +0x110 is inside the name,
  // and +0x38 is a region whose purpose has not been read.
  (void)description;
  const std::u16string name = xe::to_utf16(entry.name);
  const size_t count =
      std::min(name.size(), size_t(xe::countof(out->name) - 1));
  for (size_t i = 0; i < count; ++i) {
    out->name[i] = name[i];
  }
}

// The editor's caller (sub_92217420) passes the value it keeps at app+0x110,
// and sub_920C6F00 hands that same value to XamUserGetSigninState - so the
// first argument is a user index.
dword_result_t XamAvatarBeginEnumAssets_entry(
    dword_t user_index, dword_t batch_size, dword_t kind_mask,
    dword_t body_type, dword_t unk5, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarBeginEnumAssets", overlapped_ptr.guest_address());
  XELOGW("[avatar] BeginEnumAssets user {} batch {} kinds {:08X} body {}",
         uint32_t(user_index), uint32_t(batch_size), uint32_t(kind_mask),
         uint32_t(body_type));
  {
    std::lock_guard<std::mutex> lock(avatar_asset_enum.mutex);
    avatar_asset_enum.active = true;
    avatar_asset_enum.batch = batch_size;
    avatar_asset_enum.kind_mask = kind_mask;
    avatar_asset_enum.body = body_type;
    avatar_asset_enum.cursor = 0;
  }
  // unknown[4] & unknown[0] = 0x20080002
  // buffer_ptr[8]
  // XMsgStartIORequestEx(0xf3, 0x60000c, overlapped_ptr, buffer_ptr, 0x14,
  // unknown) 0xf2 12611

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarBeginEnumAssets, kAvatars, kStub);

dword_result_t XamAvatarEnumAssets_entry(
    lpvoid_t buffer_ptr, lpdword_t count_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarEnumAssets", overlapped_ptr.guest_address());
  // buffer_ptr is the 0x8A48 record array; count_ptr is IN capacity / OUT
  // returned count. It is a DWORD - the editor does `*(u32*)count = 0x32`
  // before every call - not the QWORD the old XMsgStartIORequestEx note
  // suggested (that note describes how the message packs the two pointers).
  const uint32_t buffer_address = buffer_ptr.guest_address();
  const uint32_t capacity = count_ptr ? uint32_t(*count_ptr) : 0;

  std::vector<const xe::kernel::xna::avatar::Entry*> batch;
  {
    std::lock_guard<std::mutex> lock(avatar_asset_enum.mutex);
    // A caller that skipped Begin still gets a full enumeration rather than
    // silence.
    uint32_t limit = capacity;
    if (avatar_asset_enum.batch &&
        (!limit || avatar_asset_enum.batch < limit)) {
      limit = avatar_asset_enum.batch;
    }
    if (!limit) {
      limit = 1;
    }
    auto* catalog = xe::kernel::xna::XnaAvatarCatalog();
    if (catalog && buffer_address) {
      const auto& entries = catalog->entries();
      while (avatar_asset_enum.cursor < entries.size() &&
             batch.size() < limit) {
        const auto& entry = entries[avatar_asset_enum.cursor++];
        if (AvatarAssetMatches(entry, avatar_asset_enum.kind_mask,
                               avatar_asset_enum.body)) {
          batch.push_back(&entry);
        }
      }
    }
  }

  if (!batch.empty()) {
    // The component colours an item advertises are the wearer's, so load the
    // profile once per batch rather than per record.
    xe::kernel::xna::avatar::Description description;
    const auto* profile =
        kernel_state()->xam_state()->GetUserProfile(static_cast<uint32_t>(0));
    if (profile) {
      xe::kernel::xna::XnaAvatarLoadProfile(profile->xuid(), &description);
    }
    auto* records = kernel_memory()->TranslateVirtual<X_AVATAR_ASSET_RECORD*>(
        buffer_address);
    for (size_t i = 0; i < batch.size(); ++i) {
      WriteAvatarAssetRecord(*batch[i], description, &records[i]);
    }
    if (count_ptr) {
      *count_ptr = static_cast<uint32_t>(batch.size());
    }
    XELOGW("[avatar] EnumAssets returned {} of {} requested", batch.size(),
           capacity);
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                  X_ERROR_SUCCESS);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_SUCCESS;
  }

  if (count_ptr) {
    *count_ptr = 0;
  }
  XELOGW("[avatar] EnumAssets exhausted");

  // "No more files" ends the enumeration, but it still has to be delivered
  // THROUGH the overlapped when the caller supplied one - every other export
  // here does. Returning it synchronously and leaving the overlapped
  // unsignalled leaves a caller that waits on the event waiting forever.
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_NO_MORE_FILES);
    return X_ERROR_IO_PENDING;
  }

  // Synchronously the real export returns XGetOverlappedExtendedError(), i.e. a
  // WIN32 code - ERROR_NO_MORE_FILES (0x12), not the HRESULT 0x80070012. The
  // difference is its sign, and the editor's enumerator bails on `r < 0`.
  return X_ERROR_NO_MORE_FILES;
}
DECLARE_XAM_EXPORT1(XamAvatarEnumAssets, kAvatars, kStub);

dword_result_t XamAvatarEndEnumAssets_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarEndEnumAssets", overlapped_ptr.guest_address());
  {
    std::lock_guard<std::mutex> lock(avatar_asset_enum.mutex);
    avatar_asset_enum.active = false;
    avatar_asset_enum.cursor = 0;
  }
  // unknown[4]
  // unknown[0] = 0x20080002
  // XMsgStartIORequestEx(0xf2,0x60000e,overlapped_ptr,0,0,unknown); 12611

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarEndEnumAssets, kAvatars, kStub);

// Other
// The eight-slot ring the notification data lives in.
//
// The console does NOT broadcast a copy: XamAvatarWearNow hands listeners a
// POINTER to a 0x14 block in xam's own data at 0x81A9B188, cycling a counter
// at 0x81A9B228 through eight slots so a listener that reads late still finds
// something valid. Broadcasting a host address instead would hand the guest a
// number it cannot dereference, so this allocates the ring out of guest memory
// once and keeps the console's shape - eight slots, 0x14 each, round robin.
static uint32_t WearNowMessageSlot() {
  static std::mutex mutex;
  static uint32_t ring = 0;
  static uint32_t counter = 0;
  constexpr uint32_t kSlots = 8;
  constexpr uint32_t kSlotSize = 0x14;
  std::lock_guard<std::mutex> lock(mutex);
  if (!ring) {
    ring = kernel_memory()->SystemHeapAlloc(kSlots * kSlotSize);
    if (!ring) {
      return 0;
    }
    std::memset(kernel_memory()->TranslateVirtual<uint8_t*>(ring), 0,
                kSlots * kSlotSize);
  }
  return ring + (++counter & (kSlots - 1)) * kSlotSize;
}

// (user index, XAVATAR_ASSET_ID*, overlapped). Inside the Avatar Editor the
// console never leaves the process: it fills a slot with {user index, the
// sixteen id bytes}, tells the loaded apps and the notification listeners
// about it, and completes the overlapped itself. Every other title posts the
// same 0x14 block to app 0xF3 as message 0x600018 instead.
dword_result_t XamAvatarWearNow_entry(
    dword_t user_index, lpvoid_t asset_id_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarWearNow", overlapped_ptr.guest_address());
  const X_RESULT result = X_ERROR_SUCCESS;
  const uint32_t slot = WearNowMessageSlot();
  if (slot) {
    uint8_t* block = kernel_memory()->TranslateVirtual<uint8_t*>(slot);
    xe::store_and_swap<uint32_t>(block, user_index);
    if (asset_id_ptr) {
      std::memcpy(block + 4, asset_id_ptr.as<const uint8_t*>(), 0x10);
    } else {
      std::memset(block + 4, 0, 0x10);
    }
    kernel_state()->BroadcastNotification(kXNotificationSystemAvatarWearNow,
                                          slot);
  }
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamAvatarWearNow, kAvatars, kImplemented);

dword_result_t XamAvatarReinstallAwardedAsset_entry(lpstring_t string_out_ptr,
                                                    dword_t string_size,
                                                    lpdword_t unk_ptr) {
  TraceAvatar("XamAvatarReinstallAwardedAsset", 0);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarReinstallAwardedAsset, kAvatars, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Avatar);
