/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>
#include <filesystem>
#include <string>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xna/xna_avatar.h"
#include "xenia/xbox.h"

// Exports the flash system titles call that a game never does. AvatarEditor.xex
// runs as a title here (it statically links XUI, so it needs no Xui* from xam),
// and these are what it reaches for beyond the game surface.
//
// Two deliberate policies:
//  - Telemetry, Kinect, SmartGlass and upload queues answer "nothing here" and
//    succeed. The title is meant to carry on without them.
//  - Anything that would have to do REAL work - content metadata, the cache,
//    profile sync, tile writes - fails instead of pretending. A lie here sends
//    the title down a path with data it never received; a clean failure sends
//    it down the one it has for a console that cannot do it either.

namespace xe {
namespace kernel {
namespace xam {

// A failure is still a RESULT, and it has to reach the caller through the
// overlapped when one was supplied - a title waiting on its event never wakes
// otherwise. That is the XamAvatarEnumAssets hang, so these do not repeat it.
static dword_result_t CompleteStub(uint32_t code,
                                   pointer_t<XAM_OVERLAPPED> overlapped) {
  if (overlapped) {
    kernel_state()->CompleteOverlappedImmediate(overlapped, code);
    return X_ERROR_IO_PENDING;
  }
  return code;
}

// -- Web instrumentation (telemetry). Inert. ---------------------------------

dword_result_t XamWebInstrumentationCreateReport_entry(dword_t a, dword_t b,
                                                       dword_t c, dword_t d,
                                                       lpdword_t out_handle) {
  if (out_handle) {
    *out_handle = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationCreateReport, kNone, kStub);

dword_result_t XamWebInstrumentationCreateSampledReport_entry(
    dword_t a, dword_t b, dword_t c, dword_t d, lpdword_t out_handle) {
  if (out_handle) {
    *out_handle = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationCreateSampledReport, kNone, kStub);

dword_result_t XamWebInstrumentationDestroyReport_entry(dword_t handle) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationDestroyReport, kNone, kStub);

dword_result_t XamWebInstrumentationSendReport_entry(dword_t handle,
                                                     dword_t flags) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationSendReport, kNone, kStub);

dword_result_t XamWebInstrumentationSetUserVar_entry(dword_t handle,
                                                     dword_t index,
                                                     lpvoid_t value) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationSetUserVar, kNone, kStub);

dword_result_t XamWebInstrumentationSetUserVarNoEscape_entry(dword_t handle,
                                                             dword_t index,
                                                             lpvoid_t value) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationSetUserVarNoEscape, kNone, kStub);

dword_result_t XamWebInstrumentationGetURL_entry(dword_t handle,
                                                 lpvoid_t buffer,
                                                 lpdword_t length) {
  if (length) {
    *length = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationGetURL, kNone, kStub);

dword_result_t XamWebInstrumentationGetURLEx_entry(dword_t handle,
                                                   lpvoid_t buffer,
                                                   lpdword_t length,
                                                   dword_t flags) {
  if (length) {
    *length = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationGetURLEx, kNone, kStub);

// -- Upload queues and studio hooks. Inert. ----------------------------------

dword_result_t XamXlfsInitializeUploadQueue_entry(dword_t a, dword_t b) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamXlfsInitializeUploadQueue, kNone, kStub);

dword_result_t XamXlfsUninitializeUploadQueue_entry() {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamXlfsUninitializeUploadQueue, kNone, kStub);

dword_result_t XamXlfsMountUploadQueueInstance_entry(dword_t a, dword_t b) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamXlfsMountUploadQueueInstance, kNone, kStub);

dword_result_t XamXlfsUnmountUploadQueueInstance_entry(dword_t a) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamXlfsUnmountUploadQueueInstance, kNone, kStub);

dword_result_t XamXlfsNotifyContentDeletion_entry(dword_t a, dword_t b) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamXlfsNotifyContentDeletion, kNone, kStub);

dword_result_t XamXStudioRequest_entry(dword_t a, dword_t b, dword_t c,
                                       dword_t d) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamXStudioRequest, kNone, kStub);

// -- SmartGlass ("LRC") keyboard. No remote is attached. ---------------------

dword_result_t XamLrcKeyboardRegister_entry(dword_t a, dword_t b, dword_t c) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardRegister, kNone, kStub);

dword_result_t XamLrcKeyboardUnregister_entry(dword_t a) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardUnregister, kNone, kStub);

dword_result_t XamLrcKeyboardGetInput_entry(dword_t a, lpvoid_t buffer,
                                            dword_t size) {
  return X_ERROR_NO_MORE_FILES;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardGetInput, kNone, kStub);

dword_result_t XamLrcKeyboardGetRequiredBufferSize_entry(dword_t a,
                                                         lpdword_t out_size) {
  if (out_size) {
    *out_size = 0;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardGetRequiredBufferSize, kNone, kStub);

dword_result_t XamLrcKeyboardUpdateText_entry(dword_t a, lpvoid_t text) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardUpdateText, kNone, kStub);

dword_result_t XamLrcKeyboardValidateTextChange_entry(dword_t a, lpvoid_t text,
                                                      dword_t length) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardValidateTextChange, kNone, kStub);

dword_result_t XamLrcKeyboardApplyTextChange_entry(dword_t a, lpvoid_t text,
                                                   dword_t length) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardApplyTextChange, kNone, kStub);

// -- Kinect. Report no camera rather than a broken one. ----------------------

dword_result_t XamNuiCameraElevationSetAngle_entry(dword_t angle) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationSetAngle, kNone, kStub);

dword_result_t XamNuiCameraElevationStopMovement_entry() {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationStopMovement, kNone, kStub);

dword_result_t XamNuiCameraRememberFloor_entry(dword_t a) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraRememberFloor, kNone, kStub);

dword_result_t XamNuiCameraTiltReportStatus_entry(dword_t a, dword_t b) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltReportStatus, kNone, kStub);

dword_result_t XamNuiCameraTiltSetCallback_entry(dword_t a, dword_t b) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltSetCallback, kNone, kStub);

dword_result_t XamNuiSkeletonScoreUpdate_entry(dword_t a, dword_t b) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiSkeletonScoreUpdate, kNone, kStub);

dword_result_t XamUserNuiGetEnrollmentIndex_entry(dword_t user_index,
                                                  lpdword_t out_index) {
  if (out_index) {
    *out_index = 0xFFFFFFFF;
  }
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetEnrollmentIndex, kNone, kStub);

dword_result_t XamShowNuiDeviceSelectorUI_entry(dword_t a, dword_t b, dword_t c,
                                                dword_t d) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamShowNuiDeviceSelectorUI, kUI, kStub);

dword_result_t XamShowNuiMarketplaceUI_entry(dword_t a, dword_t b, dword_t c,
                                             dword_t d) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamShowNuiMarketplaceUI, kUI, kStub);

dword_result_t XamReadBiometricData_entry(dword_t a, lpvoid_t buffer,
                                          dword_t size, lpdword_t out_size) {
  if (out_size) {
    *out_size = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamReadBiometricData, kNone, kStub);

dword_result_t XamWriteBiometricData_entry(dword_t a, lpvoid_t buffer,
                                           dword_t size) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamWriteBiometricData, kNone, kStub);

static std::filesystem::path CacheEntryPath(uint32_t cache_id, uint32_t hash,
                                            const char* name) {
  auto* emulator = kernel_state()->emulator();
  if (!emulator || emulator->cache_root().empty()) {
    return {};
  }

  std::string key = fmt::format("{:08X}_{:08X}", cache_id, hash);
  if (name && *name) {
    // Keep it recognisable on disk, but never let a key escape the folder.
    std::string suffix;
    for (const char* c = name; *c && suffix.size() < 48; ++c) {
      suffix.push_back((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') ||
                               (*c >= 'a' && *c <= 'z') || *c == '.' ||
                               *c == '-' || *c == '_'
                           ? *c
                           : '_');
    }
    key += "_" + suffix;
  }
  return emulator->cache_root() / "xam" / (key + ".bin");
}

dword_result_t XamCacheFetchFile_entry(dword_t cache_id, dword_t flags,
                                       dword_t hash, lpstring_t name,
                                       lpvoid_t buffer_ptr,
                                       lpdword_t size_ptr) {
  const auto path =
      CacheEntryPath(cache_id, hash, name ? name.value().c_str() : nullptr);
  std::error_code ec;
  if (path.empty() || !std::filesystem::exists(path, ec)) {
    if (size_ptr) {
      *size_ptr = 0;
    }
    return X_ERROR_FILE_NOT_FOUND;
  }

  const uint64_t file_size = std::filesystem::file_size(path, ec);
  if (ec) {
    if (size_ptr) {
      *size_ptr = 0;
    }
    return X_ERROR_FILE_NOT_FOUND;
  }

  // Report the size the caller needs rather than truncating into its buffer.
  if (!buffer_ptr || !size_ptr || *size_ptr < file_size) {
    if (size_ptr) {
      *size_ptr = static_cast<uint32_t>(file_size);
    }
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    *size_ptr = 0;
    return X_ERROR_FILE_NOT_FOUND;
  }
  const size_t read =
      fread(buffer_ptr.as<uint8_t*>(), 1, static_cast<size_t>(file_size), file);
  fclose(file);

  *size_ptr = static_cast<uint32_t>(read);
  return read == file_size ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamCacheFetchFile, kFileSystem, kImplemented);

dword_result_t XamCacheStoreFile_entry(dword_t cache_id, dword_t flags,
                                       dword_t hash, lpstring_t name,
                                       lpvoid_t buffer_ptr, dword_t size) {
  const auto path =
      CacheEntryPath(cache_id, hash, name ? name.value().c_str() : nullptr);
  if (path.empty() || !buffer_ptr || !size) {
    return X_ERROR_INVALID_PARAMETER;
  }

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  FILE* file = xe::filesystem::OpenFile(path, "wb");
  if (!file) {
    return X_ERROR_ACCESS_DENIED;
  }
  const size_t written = fwrite(buffer_ptr.as<uint8_t*>(), 1, size, file);
  fclose(file);

  if (written != size) {
    std::filesystem::remove(path, ec);
    return X_ERROR_FUNCTION_FAILED;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamCacheStoreFile, kFileSystem, kImplemented);

dword_result_t XamCacheDeleteFile_entry(dword_t cache_id, dword_t flags,
                                        dword_t hash, lpstring_t name) {
  const auto path =
      CacheEntryPath(cache_id, hash, name ? name.value().c_str() : nullptr);
  if (path.empty()) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  std::error_code ec;
  return std::filesystem::remove(path, ec) ? X_ERROR_SUCCESS
                                           : X_ERROR_FILE_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamCacheDeleteFile, kFileSystem, kImplemented);

dword_result_t XamCacheOpenFile_entry(lpvoid_t name, dword_t flags,
                                      lpdword_t out_handle) {
  // The handle-based half of the API. Nothing in the dashboard's tile path
  // uses it; the fetch/store pair above is what it calls.
  if (out_handle) {
    *out_handle = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamCacheOpenFile, kFileSystem, kStub);

dword_result_t XamCacheCloseFile_entry(dword_t handle) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamCacheCloseFile, kFileSystem, kStub);

dword_result_t XamCacheReset_entry(dword_t cache_id) {
  auto* emulator = kernel_state()->emulator();
  if (!emulator || emulator->cache_root().empty()) {
    return X_ERROR_SUCCESS;
  }
  std::error_code ec;
  std::filesystem::remove_all(emulator->cache_root() / "xam", ec);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamCacheReset, kFileSystem, kImplemented);

// -- Content, profile and tiles. Fail rather than lie. -----------------------

dword_result_t XamContentGetHeaderInternal_entry(dword_t user_index,
                                                 lpvoid_t data, lpvoid_t buffer,
                                                 dword_t size,
                                                 lpdword_t out_size) {
  if (out_size) {
    *out_size = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamContentGetHeaderInternal, kContent, kStub);

dword_result_t XamContentGetMetaDataInternal_entry(dword_t user_index,
                                                   lpvoid_t data,
                                                   lpvoid_t buffer,
                                                   dword_t size,
                                                   lpdword_t out_size) {
  if (out_size) {
    *out_size = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamContentGetMetaDataInternal, kContent, kStub);

dword_result_t XamContentQueryLicenseInternal_entry(dword_t user_index,
                                                    lpvoid_t data,
                                                    lpdword_t out_license) {
  if (out_license) {
    *out_license = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamContentQueryLicenseInternal, kContent, kStub);

dword_result_t XamCopyFile_entry(lpvoid_t source, lpvoid_t target,
                                 dword_t flags,
                                 pointer_t<XAM_OVERLAPPED> overlapped) {
  return CompleteStub(X_ERROR_FUNCTION_FAILED, overlapped);
}
DECLARE_XAM_EXPORT1(XamCopyFile, kFileSystem, kStub);

dword_result_t XamPackageManagerGetFilePathW_entry(dword_t a, lpvoid_t name,
                                                   lpvoid_t buffer,
                                                   dword_t size) {
  return X_ERROR_FILE_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamPackageManagerGetFilePathW, kFileSystem, kStub);

dword_result_t XamUserProfileSync_entry(dword_t user_index, dword_t flags,
                                        pointer_t<XAM_OVERLAPPED> overlapped) {
  return CompleteStub(X_ERROR_FUNCTION_FAILED, overlapped);
}
DECLARE_XAM_EXPORT1(XamUserProfileSync, kUserProfiles, kStub);

dword_result_t XamUserGetDeviceId_entry(dword_t user_index,
                                        lpdword_t out_device) {
  if (out_device) {
    *out_device = 0;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetDeviceId, kUserProfiles, kStub);

dword_result_t XamVerifyXSignerSignature_entry(lpvoid_t a, dword_t b,
                                               lpvoid_t c) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVerifyXSignerSignature, kNone, kStub);

dword_result_t XamWriteTile_entry(dword_t a, dword_t b, qword_t c, dword_t d,
                                  dword_t e,
                                  pointer_t<XAM_OVERLAPPED> overlapped) {
  return CompleteStub(X_ERROR_FUNCTION_FAILED, overlapped);
}
DECLARE_XAM_EXPORT1(XamWriteTile, kNone, kStub);

// XamWriteGamerTileEx and XamPngEncodeEx live in xam_user.cc, beside
// XamWriteGamerTile and the rest of the tile exports they share their storage
// with. Both used to fail here, which is what put "Can't save your gamer
// picture" on screen, and both were declared with the wrong arity.

dword_result_t XamFormatMessage_entry(dword_t flags, lpvoid_t source,
                                      dword_t message_id, dword_t language_id,
                                      lpvoid_t buffer, dword_t size) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamFormatMessage, kLocale, kStub);

dword_result_t XamFormatCurrency_entry(qword_t amount, dword_t currency,
                                       lpvoid_t buffer, dword_t size) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamFormatCurrency, kLocale, kStub);

dword_result_t XamShowPasscodeVerifyUIEx_entry(
    dword_t user_index, dword_t flags, dword_t a, dword_t b, dword_t c,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  return CompleteStub(X_ERROR_FUNCTION_FAILED, overlapped);
}
DECLARE_XAM_EXPORT1(XamShowPasscodeVerifyUIEx, kUI, kStub);

// The awarded-asset enumerator: which avatar items this user OWNS, not which
// ones the asset pack can draw. That distinction is the whole point of the
// export, and returning a failure here is not neutral.
//
// The one caller that matters is the Avatar Editor's sub_920C3D50, reached
// from sub_920C3E90 whenever a worn component's source byte says "awarded".
// It creates this enumerator for the id's own title, enumerates once, looks
// for the id, and if it does not find it - or if the create fails at all -
// answers "not owned", and the caller then STRIPS that item off the avatar.
// So the old empty-and-fail stub silently threw every DLC item out of the
// manifest the moment the editor opened.
//
// Record shape, read off that caller (0x34 bytes):
//   +0x00  dword, unread
//   +0x04  XAVATAR_ASSET_ID, the 16 bytes it matches on
//   +0x14  0x18 bytes, unread
//   +0x2C  flags; bit 0x20000 is the one it tests, and an item that does not
//          set it is treated as not owned
//   +0x30  dword, unread
// The console's own fields come out of the XONLINE_AVATAR_ASSET record that
// service 0x714 returns (_XProfileEnumAvatarAssets), which is not something
// an offline console ever sees, so everything no caller reads stays zero
// rather than being invented.
//
// The 0x204 stride the real export switches to for (flags & 7) carries more
// per item - a name, most likely - but the only known consumer asks for the
// short form, so the long one is sized correctly and left blank.
constexpr uint32_t kAvatarAssetRecordSize = 0x34;
constexpr uint32_t kAvatarAssetRecordSizeExtended = 0x204;
constexpr uint32_t kAvatarAssetOwnedFlag = 0x20000;

dword_result_t XamUserCreateAvatarAssetEnumerator_entry(
    dword_t user_index, dword_t title_id, dword_t a3, dword_t flags, dword_t a5,
    dword_t item_count, lpdword_t out_buffer_size, lpdword_t out_handle) {
  if (user_index >= XUserMaxUserCount || !item_count || !out_buffer_size ||
      !out_handle) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const uint32_t record_size =
      (flags & 7) ? kAvatarAssetRecordSizeExtended : kAvatarAssetRecordSize;

  auto e = object_ref<XStaticUntypedEnumerator>(
      new XStaticUntypedEnumerator(kernel_state(), item_count, record_size));
  X_STATUS result = e->Initialize(user_index, 0xFB, 0xB0070, 0xB000B, 0);
  if (XFAILED(result)) {
    return X_ERROR_FUNCTION_FAILED;
  }

  for (const auto& asset : xna::XnaInstalledAvatarAssets()) {
    // Title 0 means "everything this user owns"; the editor always names one.
    if (title_id && asset.title_id != title_id) {
      continue;
    }
    uint8_t* record = e->AppendItem();
    if (!record) {
      break;
    }
    std::memset(record, 0, record_size);
    std::memcpy(record + 0x04, asset.asset_id.data(), asset.asset_id.size());
    xe::store_and_swap<uint32_t>(record + 0x2C, kAvatarAssetOwnedFlag);
  }

  *out_buffer_size = record_size * item_count;
  *out_handle = e->handle();
  XELOGD("XamUserCreateAvatarAssetEnumerator({:08X}): {} owned asset(s)",
         uint32_t(title_id), e->item_count());
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserCreateAvatarAssetEnumerator, kAvatars, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(SystemTitle);
