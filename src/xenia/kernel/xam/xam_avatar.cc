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
#include <cstddef>
#include <cstring>
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
    // The editor inserts each component into its collection keyed by the type
    // at +0x10 of the 32-byte record, and a failed insert aborts the whole
    // loop - so a zero or bogus type there leaves the collection empty even
    // though the manifest looks populated.
    for (size_t i = 0; i < parsed.components.size(); ++i) {
      const uint8_t* raw = parsed.components[i].data();
      XELOGW(
          "[avatar]   component {}: id {:02X}{:02X}{:02X}{:02X}... type "
          "{:02X}{:02X}{:02X}{:02X}",
          i, raw[0], raw[1], raw[2], raw[3], raw[0x10], raw[0x11], raw[0x12],
          raw[0x13]);
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
    dword_t buffer_size, lpdword_t asset_data_ptr, dword_t custom_color_count,
    lpdword_t custom_colors_ptr, dword_t unknown,
    pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr) {
  TraceAvatar("XamAvatarSetCustomAsset", 0);
  return X_STATUS_SUCCESS;
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

dword_result_t XamAvatarManifestGetBodyType_entry(
    pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr) {
  TraceAvatar("XamAvatarManifestGetBodyType", 0);
  const uint32_t address = avatar_metadata_ptr.guest_address();
  const uint8_t* bytes =
      address ? kernel_memory()->TranslateVirtual<const uint8_t*>(address)
              : nullptr;
  if (!bytes) {
    return static_cast<uint8_t>(X_AVATAR_BODY_TYPE::Male);
  }
  return xe::kernel::xna::XnaAvatarBodyType(bytes, sizeof(X_AVATAR_METADATA))
             ? static_cast<uint8_t>(X_AVATAR_BODY_TYPE::Male)
             : static_cast<uint8_t>(X_AVATAR_BODY_TYPE::Female);
}
DECLARE_XAM_EXPORT1(XamAvatarManifestGetBodyType, kAvatars, kStub);

dword_result_t XamAvatarGetInstrumentation_entry(qword_t unk1, lpdword_t unk2) {
  /* Notes:
     - unk1 not used?
     - unk1 recieves values of 1, 2, and 6
     - mark implemented once confirmed first param not used and params named
  */
  TraceAvatar("XamAvatarGetInstrumentation", 0);
  if (unk2) {
    *unk2 = 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(XamAvatarGetInstrumentation, kAvatars, kStub);

dword_result_t XamAvatarGetAssetIcon_entry(
    lpqword_t unk1, dword_t unk2, lpqword_t unk3, lpqword_t unk4,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarGetAssetIcon", overlapped_ptr.guest_address());
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssetIcon, kAvatars, kStub);

dword_result_t XamAvatarGetAssetBinary_entry(
    lpvoid_t asset_metadata, dword_t unk2, dword_t unk3, dword_t unk4,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarGetAssetBinary", overlapped_ptr.guest_address());
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
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
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGenerateMipMaps, kAvatars, kStub);

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
  // +0x01D. Three bytes per colour group, `record + 0x1D + group * 3`, which
  // the enumerator packs into one dword per group. Nine groups fit before the
  // next field, and a group is skipped entirely if colours_per_group > 3.
  uint8_t colour_groups[9][3];  // +0x01D .. +0x037
  // +0x038 onwards. The tile builder's copies from "+0x28" and "+0x38" that
  // this struct used to describe belong to the editor's own 0x198-byte record,
  // not to this buffer - it builds that from this one and the tile reads that.
  // The old component_type at +0x28 sat inside the colour groups above, so it
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
static_assert(offsetof(X_AVATAR_ASSET_RECORD, colour_groups) == 0x1D, "");
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
  // Bit 0 is not decoration: the editor drops every record that lacks it.
  out->flags = 1;

  // Colour layout. The enumerator reads `>> 4` as the number of colour groups
  // and `& 0xF` as the colours in each, and the tile builder turns that second
  // number into the button class it uses - 1, 2 or 3 give
  // Grid1x1{One,Two,Three}ColourButton. Leaving this zero, which is what we did
  // before, gives a tile with no class.
  //
  // The catalogue carries no colour data for an asset, so there is nothing
  // truthful to put in the groups themselves: one group of one colour is the
  // least we can say that still produces a usable tile. That is OUR choice, not
  // the console's data, and it is the thing to revisit when the asset pack's
  // per-item colour table is parsed.
  out->colour_layout = (1 << 4) | 1;
  std::memset(out->colour_groups, 0, sizeof(out->colour_groups));

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
dword_result_t XamAvatarWearNow_entry(
    qword_t unk1, lpdword_t unk2, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  TraceAvatar("XamAvatarWearNow", overlapped_ptr.guest_address());
  X_RESULT result = X_ERROR_SUCCESS;
  if (kernel_state()->title_id() == kAvatarEditorID) {
    /*
      - ops
    XamSendMessageToLoadedApps(0xffffffff8000000e,0xffffffff80050018,lVar5);
    XNotifyBroadcast(0xffffffff80050018,lVar5);
    if (overlapped_ptr) {
      XMsgCompleteIORequest(overlapped_ptr,0,0,0);
    }
    */
    // The console completes it here too (see XMsgCompleteIORequest above);
    // dropping it left a caller that polls this overlapped pending forever.
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
      return X_ERROR_IO_PENDING;
    }
  } else {
    // buffer_ptr = concat(unk1, *unk2);
    //  XMsgStartIORequestEx(0xf3,0x600018,overlapped_ptr,&buffer_ptr,0x14,0);
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
      return X_ERROR_IO_PENDING;
    }
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamAvatarWearNow, kAvatars, kStub);

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
