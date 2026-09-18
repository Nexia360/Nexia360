/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
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

// -- Cache. There is no title cache partition here. --------------------------

dword_result_t XamCacheOpenFile_entry(lpvoid_t name, dword_t flags,
                                      lpdword_t out_handle) {
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

dword_result_t XamCacheReset_entry(dword_t a) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamCacheReset, kFileSystem, kStub);

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

dword_result_t XamWriteGamerTileEx_entry(dword_t a, dword_t b, dword_t c,
                                         dword_t d,
                                         pointer_t<XAM_OVERLAPPED> overlapped) {
  return CompleteStub(X_ERROR_FUNCTION_FAILED, overlapped);
}
DECLARE_XAM_EXPORT1(XamWriteGamerTileEx, kNone, kStub);

dword_result_t XamPngEncodeEx_entry(lpvoid_t source, dword_t width,
                                    dword_t height, dword_t pitch,
                                    lpvoid_t target, lpdword_t out_size) {
  if (out_size) {
    *out_size = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamPngEncodeEx, kNone, kStub);

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

// Avatar asset enumeration has a real implementation path through
// XamAvatarBeginEnumAssets; this entry point is the user-scoped wrapper and
// is not wired to it yet, so it reports an empty enumerator.
dword_result_t XamUserCreateAvatarAssetEnumerator_entry(dword_t user_index,
                                                        dword_t a, dword_t b,
                                                        dword_t c, dword_t d,
                                                        lpdword_t out_count,
                                                        lpdword_t out_handle) {
  if (out_count) {
    *out_count = 0;
  }
  if (out_handle) {
    *out_handle = 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamUserCreateAvatarAssetEnumerator, kAvatars, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(SystemTitle);
