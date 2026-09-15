/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_avatar.h"

#include <array>
#include <cstring>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_avatar_assets.h"
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
  // Calls XMsgStartIORequestEx(0xf3,0x600002,0,0,0,0).
  // in 12611 its XamUnloadSysApp(0xf2,1)
}
DECLARE_XAM_EXPORT1(XamAvatarShutdown, kAvatars, kStub);

// Get & Set
dword_result_t XamAvatarGetManifestLocalUser_entry(
    dword_t user_index, pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
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

    WriteAvatarMetadata(
        avatar_metadata_ptr.guest_address(),
        xe::kernel::xna::XnaAvatarManifestForXuid(user_profile->xuid()));
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

dword_result_t XamAvatarSetCustomAsset_entry(
    dword_t buffer_size, lpdword_t asset_data_ptr, dword_t custom_color_count,
    lpdword_t custom_colors_ptr,
    pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr) {
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarSetCustomAsset, kAvatars, kStub)

dword_result_t XamAvatarSetManifest_entry(
    dword_t user_index, dword_t avatar_info_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!avatar_info_ptr) {
    return X_E_INVALIDARG;
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
  if (unk2) {
    *unk2 = 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(XamAvatarGetInstrumentation, kAvatars, kStub);

dword_result_t XamAvatarGetAssetIcon_entry(
    lpqword_t unk1, dword_t unk2, lpqword_t unk3, lpqword_t unk4,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
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
  XCONTENT_AGGREGATE_DATA content_data;
  content_data.content_type = XContentType::kAvatarItem;
  content_data.xuid = 0;
  content_data.title_id = asset_id_ptr->title_id;
  std::string file_name =
      fmt::format("{:016X}{:08X}{:08X}", asset_id_ptr->data,
                  asset_id_ptr->data2, asset_id_ptr->title_id);
  content_data.set_file_name(file_name);
  *content_data_ptr = content_data;
  XELOGD("Looking for avatar asset: {:X}", file_name);
}
DECLARE_XAM_EXPORT1(XamAvatarGetInstalledAssetPackageDescription, kAvatars,
                    kSketchy);

void XamAvatarSetMocks_entry() {
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
  const uint32_t object_address = output.guest_address();
  if (!asset_id_ptr || !object_address) {
    return X_E_INVALIDARG;
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
  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    length = 0;
    extended_error = LoadAvatarAnimation(asset_id, object_address, label);
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
  const std::string item = item_ptr ? xe::to_utf8(item_ptr.value()) : "";
  XELOGW("XamLaunchAvatarEditor: user {}, flags {:08X}, item '{}'",
         uint32_t(user_index), uint32_t(flags), item);
  kernel_state()->emulator()->on_avatar_editor(user_index);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLaunchAvatarEditor, kAvatars, kImplemented);

// Enum
dword_result_t XamAvatarBeginEnumAssets_entry(
    dword_t unk1, dword_t unk2, dword_t unk3, word_t unk4, dword_t unk5,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
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
    lpvoid_t unk1, lpqword_t unk2, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // unk1 pointer to a struct of size 0x8a48
  // buffer_ptr = concat(unk1, unk2)
  // unknown = 0x20080002
  // XMsgStartIORequestEx(0xf3, 0x60000d, overlapped_ptr, &buffer_ptr, 8,
  // &unknown) 0xf2 12611

  return X_E_NO_MORE_FILES;  // Stop it from calling endlessly
}
DECLARE_XAM_EXPORT1(XamAvatarEnumAssets, kAvatars, kStub);

dword_result_t XamAvatarEndEnumAssets_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
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
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarReinstallAwardedAsset, kAvatars, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Avatar);
