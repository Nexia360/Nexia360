/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string_util.h"
#include "xenia/base/utf8.h"
#include "xenia/emulator.h"
#include "xenia/kernel/info/file.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/smc.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xna/xna_launcher.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <span>

#include "third_party/stb/stb_image.h"
#include "third_party/stb/stb_image_write.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/xbox.h"

DEFINE_int32(
    license_mask, 0,
    "Set license mask for activated content.\n"
    " 0 = No licenses enabled.\n"
    " 1 = First license enabled. Generally the full version license in\n"
    "     Xbox Live Arcade titles.\n"
    " -1 or 0xFFFFFFFF = All possible licenses enabled. Generally a\n"
    "                    bad idea, could lead to undefined behavior.",
    "Content");

namespace xe {
namespace kernel {
namespace xam {

static X_RESULT ReadContentData(lpvoid_t content_data_ptr,
                                uint32_t content_data_size,
                                XCONTENT_DATA_INTERNAL& result) {
  if (content_data_size == sizeof(XCONTENT_DATA)) {
    result = *content_data_ptr.as<XCONTENT_DATA*>();
  } else if (content_data_size == sizeof(XCONTENT_DATA_AGGREGATE)) {
    result = *content_data_ptr.as<XCONTENT_DATA_AGGREGATE*>();
  } else if (content_data_size == sizeof(XCONTENT_DATA_INTERNAL)) {
    result = *content_data_ptr.as<XCONTENT_DATA_INTERNAL*>();
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
  return X_ERROR_SUCCESS;
}

dword_result_t XamContentGetLicenseMask_entry(lpdword_t mask_ptr,
                                              lpvoid_t overlapped_ptr) {
  if (!mask_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto run = [mask_ptr](uint32_t& extended_error, uint32_t& length) {
    X_RESULT result = X_E_FAIL;

    // Remark: This cannot be reflected as on console. Xenia can boot games
    // directly and XBLA games can be repacked to ZAR. For these titles we must
    // provide some license. Normally it should fail for OpticalDisc type.
    if (kernel_state()->deployment_type_ != XDeploymentType::kOther) {
      // Each bit in the mask represents a granted license. Available licenses
      // seems to vary from game to game, but most appear to use bit 0 to
      // indicate if the game is purchased or not.
      *mask_ptr = static_cast<uint32_t>(cvars::license_mask);
      result = X_ERROR_SUCCESS;
    }

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = 0;
    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
}
DECLARE_XAM_EXPORT2(XamContentGetLicenseMask, kContent, kStub, kHighFrequency);

dword_result_t xeXamContentResolve(
    dword_t user_index, lpvoid_t content_data_ptr, dword_t content_data_size,
    lpstring_t path_ptr, dword_t path_size, dword_t create_directory,
    lpstring_t root_name_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  auto run = [user_index, content_data_ptr, content_data_size, path_ptr,
              path_size, create_directory, root_name_ptr](
                 uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    XCONTENT_DATA_INTERNAL content_data{};
    X_RESULT result =
        ReadContentData(content_data_ptr, content_data_size, content_data);
    if (result != X_ERROR_SUCCESS) {
      return result;
    };

    uint64_t xuid = 0;
    if (user_index < XUserMaxUserCount) {
      const auto profile =
          kernel_state()->xam_state()->profile_manager()->GetProfile(
              static_cast<uint8_t>(user_index));
      if (profile && content_data.content_type == XContentType::kSavedGame) {
        xuid = profile->xuid();
      }
    }

    std::string root_device_path = "";

    if (root_name_ptr) {
      // Check if root_name is valid.
      // root_device_path = std::string(root_name_ptr);
      // Unsupported for now.
      return X_ERROR_INVALID_PARAMETER;
    } else {
      if (content_data.device_id == static_cast<uint32_t>(DummyDeviceId::HDD)) {
        root_device_path = "\\Device\\Harddisk0\\Partition1\\Content\\";
      } else if (content_data.device_id ==
                 static_cast<uint32_t>(DummyDeviceId::ODD)) {
        // Or GAME, but D: usually means DVD drive meanwhile GAME always
        // pinpoints to game, even if it is running from HDD
        root_device_path = "D:\\content\\";
      } else {
        return X_ERROR_INVALID_PARAMETER;
      }
    }

    const std::string relative_path = fmt::format(
        "{:016X}\\{:08X}\\{:08X}\\{}", xuid, kernel_state()->title_id(),
        static_cast<uint32_t>(content_data.content_type.get()),
        content_data.file_name());

    string_util::copy_truncating(path_ptr, root_device_path + relative_path,
                                 path_size);

    // Check if it exists and try to mount that package
    // Result of buffer_ptr is sent to RtlInitAnsiString.
    // buffer_size is usually 260 (max path).
    return X_ERROR_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
}

dword_result_t XamContentResolve_entry(
    dword_t user_index, lpvoid_t content_data_ptr, lpstring_t path_ptr,
    dword_t path_size, dword_t create_directory, lpstring_t root_name_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return xeXamContentResolve(user_index, content_data_ptr,
                             sizeof(XCONTENT_DATA), path_ptr, path_size,
                             create_directory, root_name_ptr, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentResolve, kContent, kSketchy);

dword_result_t XamContentResolveInternal_entry(
    lpvoid_t content_data_ptr, lpstring_t path_ptr, dword_t path_size,
    dword_t create_directory, lpstring_t root_name_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return xeXamContentResolve(
      XUserIndexNone, content_data_ptr, sizeof(XCONTENT_DATA_INTERNAL),
      path_ptr, path_size, create_directory, root_name_ptr, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentResolveInternal, kContent, kSketchy);

// https://github.com/MrColdbird/gameservice/blob/master/ContentManager.cpp
dword_result_t XamContentCreateEnumeratorInternal_entry(
    qword_t xuid, dword_t device_id, dword_t content_type, dword_t title_id,
    dword_t content_flags, dword_t items_per_enumerate,
    lpdword_t buffer_size_ptr, lpdword_t handle_out) {
  assert_not_null(handle_out);

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    if (buffer_size_ptr) {
      *buffer_size_ptr = 0;
    }

    // TODO(benvanik): memset 0 the data?
    return X_E_INVALIDARG;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(XCONTENT_DATA) * items_per_enumerate;
  }

  auto e = object_ref<ContentEnumerator>(new ContentEnumerator(
      kernel_state(), items_per_enumerate, sizeof(XCONTENT_DATA)));

  auto result =
      e->Initialize(XUserIndexAny, 0xFE, 0x20005, 0x20007, 0, 0x98, nullptr);
  if (XFAILED(result)) {
    return result;
  }

  uint32_t title = title_id;
  if (!title) {
    title = kernel_state()->title_id();
  }

  std::vector<XCONTENT_DATA_INTERNAL> enumerated_content{};

  if (!device_info || device_info->device_id == DummyDeviceId::HDD) {
    std::vector<uint64_t> xuids_to_enumerate = {};
    if (xuid) {
      xuids_to_enumerate.push_back(xuid);
    }

    if (!xuid || !(content_flags & XContentFlag::kExcludeCommon)) {
      xuids_to_enumerate.push_back(0);  // Common content
    }

    for (const auto& xuid : xuids_to_enumerate) {
      auto user_enumerated_data =
          kernel_state()->content_manager()->ListContent(
              static_cast<uint32_t>(DummyDeviceId::HDD), xuid, title,
              static_cast<XContentType>(content_type.value()),
              static_cast<XContentFlag>(content_flags.value()));

      enumerated_content.insert(enumerated_content.end(),
                                user_enumerated_data.cbegin(),
                                user_enumerated_data.cend());
    }

    // Remove duplicates
    enumerated_content.erase(std::ranges::unique(enumerated_content).begin(),
                             enumerated_content.end());
  }

  if (!device_info || device_info->device_id == DummyDeviceId::ODD) {
    auto disc_enumerated_data =
        kernel_state()->content_manager()->ListContentODD(
            static_cast<uint32_t>(DummyDeviceId::ODD), 0, title,
            XContentType(uint32_t(content_type)));

    enumerated_content.insert(enumerated_content.end(),
                              disc_enumerated_data.cbegin(),
                              disc_enumerated_data.cend());
  }

  for (const auto& content_data : enumerated_content) {
    e->AppendItem(content_data);
    XELOGI("{}: Adding: {} (Filename: {}) to enumerator result", __func__,
           xe::to_utf8(content_data.display_name()), content_data.file_name());
  }

  XELOGD("XamContentCreateEnumeratorInternal: added {} items to enumerator",
         e->item_count());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentCreateEnumeratorInternal, kContent, kImplemented);

dword_result_t XamContentCreateEnumerator_entry(
    dword_t user_index, dword_t device_id, dword_t content_type,
    dword_t content_flags, dword_t items_per_enumerate,
    lpdword_t buffer_size_ptr, lpdword_t handle_out) {
  uint64_t xuid = 0;
  if (user_index < XUserMaxUserCount) {
    const auto& user = kernel_state()->xam_state()->GetUserProfile(user_index);

    if (!user) {
      return X_ERROR_NO_SUCH_USER;
    }

    xuid = user->xuid();
  }

  return XamContentCreateEnumeratorInternal_entry(
      xuid, device_id, content_type, 0, content_flags, items_per_enumerate,
      buffer_size_ptr, handle_out);
}
DECLARE_XAM_EXPORT1(XamContentCreateEnumerator, kContent, kImplemented);

enum class kDispositionState : uint32_t { Unknown = 0, Create = 1, Open = 2 };

dword_result_t xeXamContentCreate(dword_t user_index, lpstring_t root_name,
                                  lpvoid_t content_data_ptr,
                                  dword_t content_data_size, dword_t flags,
                                  lpdword_t disposition_ptr,
                                  lpdword_t license_mask_ptr,
                                  dword_t cache_size, qword_t content_size,
                                  pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  uint64_t xuid = 0;
  if (user_index != XUserIndexNone && user_index != XUserIndexAny) {
    const auto& user = kernel_state()->xam_state()->GetUserProfile(user_index);

    if (!user) {
      return X_ERROR_NO_SUCH_USER;
    }

    xuid = user->xuid();
  }

  if (!root_name || *root_name == '\0') {
    return X_ERROR_INVALID_NAME;
  }

  XCONTENT_DATA_INTERNAL content_data{};
  X_RESULT result =
      ReadContentData(content_data_ptr, content_data_size, content_data);
  if (result != X_ERROR_SUCCESS) {
    return result;
  };

  // Check if we have something under provided symlink.
  std::string symlink_path = root_name.value();
  if (!symlink_path.ends_with(':')) {
    symlink_path += ':';
  }

  // Check if the same package isn't already mounted under the same name.
  if (kernel_state()->file_system()->IsSymbolicLinkRegistered(symlink_path)) {
    const auto package =
        kernel_state()->content_manager()->FindPackage(root_name.value());

    if (!package) {
      return X_ERROR_INVALID_PARAMETER;
    }

    // Title can provide -1 as a title_id, so before comparation we need to
    // modify content_data.
    content_data.title_id = kernel_state()->title_id();

    return package->GetContentMetadata() == content_data
               ? X_ERROR_SUCCESS
               : X_ERROR_INVALID_PARAMETER;
  }

  if (content_data.content_type == XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto content_manager = kernel_state()->content_manager();

  if (overlapped_ptr && disposition_ptr) {
    *disposition_ptr = 0;
  }

  auto run = [content_manager, xuid, root_name = root_name.value(), flags,
              content_data, content_data_size, disposition_ptr,
              license_mask_ptr, overlapped_ptr](uint32_t& extended_error,
                                                uint32_t& length) -> X_RESULT {
    X_RESULT result = X_ERROR_INVALID_PARAMETER;
    kDispositionState disposition = kDispositionState::Unknown;
    switch (flags & 0xF) {
      case XContentFlag::kCreateNew:
        // Fail if exists.
        if (content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_ALREADY_EXISTS;
        } else {
          disposition = kDispositionState::Create;
        }
        break;
      case XContentFlag::kCreateAlways:
        // Overwrite existing, if any.
        if (content_manager->ContentExists(xuid, content_data)) {
          content_manager->DeleteContent(xuid, content_data);
        }
        disposition = kDispositionState::Create;
        break;
      case XContentFlag::kOpenExisting:
        // Open only if exists.
        if (!content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          disposition = kDispositionState::Open;
        }
        break;
      case XContentFlag::kOpenAlways:
        // Create if needed.
        if (!content_manager->ContentExists(xuid, content_data)) {
          disposition = kDispositionState::Create;
        } else {
          disposition = kDispositionState::Open;
        }
        break;
      case XContentFlag::kTruncateExisting:
        // Fail if doesn't exist, if does exist delete and recreate.
        if (!content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          content_manager->DeleteContent(xuid, content_data);
          disposition = kDispositionState::Create;
        }
        break;
      default:
        assert_unhandled_case(flags & 0xF);
        break;
    }

    uint32_t content_license = 0;
    if (disposition == kDispositionState::Create) {
      XCONTENT_DATA_INTERNAL content_data_internal = content_data;
      // We need to explicitly check these fields as they can be set from
      // XCONTENT_DATA_AGGREGATE version.
      if (!content_data_internal.title_id ||
          content_data_internal.title_id == kCurrentlyRunningTitleId) {
        content_data_internal.title_id = kernel_state()->title_id();
      }

      if (!content_data_internal.xuid) {
        content_data_internal.xuid = xuid;
      }

      content_data_internal.content_size =
          static_cast<uint64_t>(content_data_size);
      content_data_internal.set_title_name(
          xe::to_utf16(kernel_state()->emulator()->title_name()));

      result = content_manager->CreateContent(root_name, xuid,
                                              content_data_internal);
    } else if (disposition == kDispositionState::Open) {
      result = content_manager->OpenContent(root_name, xuid, content_data,
                                            content_license);
    }

    if (license_mask_ptr && XSUCCEEDED(result)) {
      *license_mask_ptr = content_license;
    }

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = static_cast<uint32_t>(disposition);

    if (disposition_ptr) {
      *disposition_ptr = static_cast<uint32_t>(disposition);
    }

    if (result && overlapped_ptr) {
      result = X_ERROR_FUNCTION_FAILED;
    }
    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
}

dword_result_t XamContentCreateEx_entry(
    dword_t user_index, lpstring_t root_name, lpvoid_t content_data_ptr,
    dword_t flags, lpdword_t disposition_ptr, lpdword_t license_mask_ptr,
    dword_t cache_size, qword_t content_size,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  auto content_data = *content_data_ptr.as<XCONTENT_DATA*>();
  if (content_data.file_name_raw[0] == '\0') {
    return X_ERROR_INVALID_NAME;
  }
  return xeXamContentCreate(user_index, root_name, content_data_ptr,
                            sizeof(XCONTENT_DATA), flags, disposition_ptr,
                            license_mask_ptr, cache_size, content_size,
                            overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentCreateEx, kContent, kImplemented);

dword_result_t XamContentCreate_entry(
    dword_t user_index, lpstring_t root_name, lpvoid_t content_data_ptr,
    dword_t flags, lpdword_t disposition_ptr, lpdword_t license_mask_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return XamContentCreateEx_entry(user_index, root_name, content_data_ptr,
                                  flags, disposition_ptr, license_mask_ptr, 0,
                                  0, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentCreate, kContent, kImplemented);

dword_result_t XamContentCreateInternal_entry(
    lpstring_t root_name, lpvoid_t content_data_ptr, dword_t flags,
    lpdword_t disposition_ptr, lpdword_t license_mask_ptr, dword_t cache_size,
    qword_t content_size, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  auto content_data_internal = *content_data_ptr.as<XCONTENT_DATA_INTERNAL*>();
  if (content_data_internal.file_name_raw[0] == '\0') {
    return X_ERROR_INVALID_NAME;
  }
  return xeXamContentCreate(XUserIndexNone, root_name, content_data_ptr,
                            sizeof(XCONTENT_DATA_INTERNAL), flags,
                            disposition_ptr, license_mask_ptr, cache_size,
                            content_size, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentCreateInternal, kContent, kImplemented);

dword_result_t XamContentOpenFile_entry(
    dword_t user_index, lpstring_t root_name, lpstring_t path, dword_t flags,
    lpdword_t disposition_ptr, lpdword_t license_mask_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // TODO(gibbed): arguments assumed based on XamContentCreate.
  return X_ERROR_FILE_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamContentOpenFile, kContent, kStub);

namespace {

std::mutex opened_roots_mutex;
std::map<std::string, std::string> opened_roots;
uint32_t opened_root_id = 0;

bool CloseOpenedRoot(const std::string& root_name) {
  std::lock_guard<std::mutex> lock(opened_roots_mutex);
  auto it = opened_roots.find(root_name);
  if (it == opened_roots.end()) {
    return false;
  }
  auto* file_system = kernel_state()->file_system();
  file_system->UnregisterSymbolicLink(root_name + ":");
  file_system->UnregisterDevice(it->second);
  opened_roots.erase(it);
  return true;
}

X_RESULT OpenContentFile(const std::string& root_name,
                         const std::string& path) {
  auto* file_system = kernel_state()->file_system();
  auto* entry =
      dynamic_cast<vfs::HostPathEntry*>(file_system->ResolvePath(path));
  if (!entry) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  CloseOpenedRoot(root_name);

  if (!(entry->attributes() & vfs::kFileAttributeDirectory)) {
    // A container is a package. The content manager owns it - its device reads
    // from the package's mapping - so XamContentClose and the title teardown
    // close it there.
    auto* content_manager = kernel_state()->content_manager();
    content_manager->CloseContent(root_name);
    return content_manager->OpenAndMountPackage(entry->host_path(), root_name)
               ? X_ERROR_SUCCESS
               : X_ERROR_FILE_NOT_FOUND;
  }

  std::lock_guard<std::mutex> lock(opened_roots_mutex);
  const std::string device_path =
      fmt::format("\\Device\\ContentFile\\{}\\", ++opened_root_id);
  std::unique_ptr<vfs::Device> device = std::make_unique<vfs::HostPathDevice>(
      device_path, entry->host_path(), true);
  if (!device->Initialize()) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  file_system->RegisterDevice(std::move(device));
  file_system->RegisterSymbolicLink(root_name + ":", device_path);
  opened_roots[root_name] = device_path;
  return X_ERROR_SUCCESS;
}

}  // namespace

void CloseOpenedContentFiles() {
  std::vector<std::string> roots;
  {
    std::lock_guard<std::mutex> lock(opened_roots_mutex);
    for (const auto& [root, device_path] : opened_roots) {
      roots.push_back(root);
    }
  }
  for (const std::string& root : roots) {
    CloseOpenedRoot(root);
  }
}

dword_result_t XamContentOpenFileInternal_entry(
    dword_t user_index, lpstring_t root_name_ptr, lpstring_t path_ptr,
    dword_t flags, dword_t unknown1, dword_t unknown2,
    lpdword_t license_mask_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (license_mask_ptr) {
    *license_mask_ptr = 0;
  }
  const std::string root_name =
      root_name_ptr ? std::string(root_name_ptr.value()) : std::string();
  const std::string path =
      path_ptr ? std::string(path_ptr.value()) : std::string();
  X_RESULT result = X_ERROR_INVALID_NAME;
  if (!root_name.empty() && !path.empty()) {
    result = OpenContentFile(root_name, path);
  }
  if (result == X_ERROR_SUCCESS) {
    if (license_mask_ptr) {
      *license_mask_ptr = static_cast<uint32_t>(cvars::license_mask);
    }
    XELOGI("XamContentOpenFileInternal: '{}' as {}:", path, root_name);
  } else {
    XELOGE("XamContentOpenFileInternal: '{}' as {}: failed {:08X}", path,
           root_name, result);
  }
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamContentOpenFileInternal, kContent, kImplemented);

dword_result_t XamContentFlush_entry(lpstring_t root_name,
                                     pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  X_RESULT result = X_ERROR_SUCCESS;

  // We do not buffer any device data, so let's just write header.
  auto package =
      kernel_state()->content_manager()->FindPackage(root_name.value());
  if (!package) {
    return X_STATUS_INVALID_PARAMETER;
  }

  package->Flush();

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamContentFlush, kContent, kStub);

dword_result_t XamContentClose_entry(lpstring_t root_name,
                                     pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // Closes a previously opened root from XamContentCreate*.
  const std::string root = std::string(root_name.value());
  const X_RESULT result =
      CloseOpenedRoot(root)
          ? X_RESULT(X_ERROR_SUCCESS)
          : X_RESULT(kernel_state()->content_manager()->CloseContent(root));

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediateEx(
        overlapped_ptr, result, X_HRESULT_FROM_WIN32(result), 0);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamContentClose, kContent, kImplemented);

dword_result_t XamContentGetCreator_entry(
    dword_t user_index, lpvoid_t content_data_ptr, lpdword_t is_creator_ptr,
    lpqword_t creator_xuid_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!is_creator_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const auto& user = kernel_state()->xam_state()->GetUserProfile(user_index);

  if (!user) {
    return X_ERROR_NO_SUCH_USER;
  }

  XCONTENT_DATA_AGGREGATE content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  auto run = [content_data, xuid = user->xuid(), user_index, is_creator_ptr,
              creator_xuid_ptr, overlapped_ptr](uint32_t& extended_error,
                                                uint32_t& length) -> X_RESULT {
    X_RESULT result = X_ERROR_SUCCESS;

    bool content_exists =
        kernel_state()->content_manager()->ContentExists(xuid, content_data);

    if (content_exists) {
      if (content_data.content_type == XContentType::kSavedGame) {
        // User always creates saves.
        *is_creator_ptr = 1;
        if (creator_xuid_ptr) {
          if (kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
            const auto& user_profile =
                kernel_state()->xam_state()->GetUserProfile(user_index);
            *creator_xuid_ptr = user_profile->xuid();
          } else {
            result = X_ERROR_NO_SUCH_USER;
          }
        }
      } else {
        *is_creator_ptr = 0;
        if (creator_xuid_ptr) {
          *creator_xuid_ptr = 0;
        }
      }
    } else {
      result = X_ERROR_PATH_NOT_FOUND;
    }

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = 0;

    if (result && overlapped_ptr) {
      result = X_ERROR_FUNCTION_FAILED;
    }

    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
}
DECLARE_XAM_EXPORT1(XamContentGetCreator, kContent, kImplemented);

dword_result_t XamContentGetThumbnail_entry(
    dword_t user_index, lpvoid_t content_data_ptr, lpvoid_t buffer_ptr,
    lpdword_t buffer_size_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  const auto& user = kernel_state()->xam_state()->GetUserProfile(user_index);

  if (!user) {
    return X_ERROR_NO_SUCH_USER;
  }

  assert_not_null(buffer_size_ptr);
  uint32_t buffer_size = *buffer_size_ptr;
  XCONTENT_DATA_INTERNAL content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  // Get thumbnail (if it exists).
  std::vector<uint8_t> buffer;
  auto result = kernel_state()->content_manager()->GetContentThumbnail(
      user->xuid(), content_data, buffer);

  *buffer_size_ptr = static_cast<uint32_t>(buffer.size());

  if (XSUCCEEDED(result)) {
    // Write data, if we were given a pointer.
    // This may have just been a size query.
    if (buffer_ptr) {
      if (buffer_size < buffer.size()) {
        // Dest buffer too small.
        result = X_ERROR_INSUFFICIENT_BUFFER;
      } else {
        // Copy data.
        std::memcpy((uint8_t*)buffer_ptr, buffer.data(), buffer.size());
      }
    }
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamContentGetThumbnail, kContent, kImplemented);

dword_result_t xeXamContentSetThumbnail(
    dword_t user_index, lpvoid_t content_data_ptr, dword_t content_data_size,
    lpvoid_t buffer_ptr, dword_t buffer_size,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  const auto& user = kernel_state()->xam_state()->GetUserProfile(user_index);

  if (!user) {
    return X_ERROR_NO_SUCH_USER;
  }
  if (buffer_size > XContentMetadata::kThumbLengthV2) {
    return X_E_INVALIDARG;
  }

  XCONTENT_DATA_INTERNAL content_data{};
  X_RESULT result =
      ReadContentData(content_data_ptr, content_data_size, content_data);
  if (result != X_ERROR_SUCCESS) {
    return result;
  };

  if (content_data_size == sizeof(XCONTENT_DATA) ||
      content_data_size == sizeof(XCONTENT_DATA_AGGREGATE)) {
    content_data.title_id = kernel_state()->title_id();
  }

  // Buffer is PNG data.
  auto buffer = std::vector<uint8_t>((uint8_t*)buffer_ptr,
                                     (uint8_t*)buffer_ptr + buffer_size);
  result = kernel_state()->content_manager()->SetContentThumbnail(
      user->xuid(), content_data, std::move(buffer));

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

dword_result_t XamContentSetThumbnail_entry(
    dword_t user_index, lpvoid_t content_data_ptr, lpvoid_t buffer_ptr,
    dword_t buffer_size, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return xeXamContentSetThumbnail(user_index, content_data_ptr,
                                  sizeof(XCONTENT_DATA), buffer_ptr,
                                  buffer_size, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentSetThumbnail, kContent, kImplemented);

dword_result_t XamContentSetThumbnailInternal_entry(
    lpvoid_t content_data_ptr, lpvoid_t buffer_ptr, dword_t buffer_size,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (buffer_size > XContentMetadata::kThumbLengthV2) {
    return X_E_INVALIDARG;
  }

  // Not a wrapper of xeXamContentSetThumbnail: that one needs a signed-in
  // user, and the dashboard calls this with none, for records it enumerated.
  X_RESULT result = X_ERROR_SUCCESS;
  if (content_data_ptr && buffer_ptr && buffer_size) {
    XCONTENT_DATA_INTERNAL content_data{};
    result = ReadContentData(content_data_ptr, sizeof(XCONTENT_DATA_INTERNAL),
                             content_data);
    if (result != X_ERROR_SUCCESS) {
      return result;
    }

    // A read-only container has the thumbnail in its own header, so it
    // refuses the write. That is not a failure the caller needs to hear about
    // - it already has the image.
    const X_RESULT write_result =
        kernel_state()->content_manager()->SetContentThumbnail(
            content_data.xuid, content_data,
            std::span<const uint8_t>((uint8_t*)buffer_ptr, buffer_size));
    if (write_result != X_ERROR_ACCESS_DENIED &&
        write_result != X_ERROR_NOT_SUPPORTED) {
      result = write_result;
    }
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamContentSetThumbnailInternal, kContent, kImplemented);

dword_result_t xeXamContentDelete(dword_t user_index, lpvoid_t content_data_ptr,
                                  dword_t content_data_size,
                                  pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  uint64_t xuid = 0;
  XCONTENT_DATA_INTERNAL content_data{};
  X_RESULT result =
      ReadContentData(content_data_ptr, content_data_size, content_data);
  if (result != X_ERROR_SUCCESS) {
    return result;
  };

  if (user_index != XUserIndexNone) {
    const auto& user = kernel_state()->xam_state()->GetUserProfile(user_index);

    if (!user) {
      return X_ERROR_NO_SUCH_USER;
    }

    xuid = user->xuid();
  }

  result = kernel_state()->content_manager()->DeleteContent(xuid, content_data);

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

dword_result_t XamContentDelete_entry(
    dword_t user_index, lpvoid_t content_data_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return xeXamContentDelete(user_index, content_data_ptr, sizeof(XCONTENT_DATA),
                            overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentDelete, kContent, kImplemented);

dword_result_t XamContentDeleteInternal_entry(
    lpvoid_t content_data_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // INFO: Analysis of xam.xex shows that "internal" functions are wrappers with
  // 0xFE as user_index.
  // In XAM content size is set to 0x200.
  return xeXamContentDelete(XUserIndexNone, content_data_ptr,
                            sizeof(XCONTENT_DATA_INTERNAL), overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentDeleteInternal, kContent, kImplemented);

typedef struct {
  xe::be<uint32_t> stringTitlePtr;
  xe::be<uint32_t> stringTextPtr;
  xe::be<uint32_t> stringBtnMsgPtr;
} X_SWAPDISC_ERROR_MESSAGE;
static_assert_size(X_SWAPDISC_ERROR_MESSAGE, 12);

dword_result_t XamSwapDisc_entry(
    dword_t disc_number, pointer_t<X_KEVENT> completion_handle,
    pointer_t<X_SWAPDISC_ERROR_MESSAGE> error_message) {
  xex2_opt_execution_info* info = kernel_state()->GetExecutionInfo();

  if (info->disc_number > info->disc_count) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto completion_event = [completion_handle]() -> void {
    auto kevent = xboxkrnl::xeKeSetEvent(completion_handle, 1, 0);

    // Release the completion handle
    auto object = XObject::GetNativeObject<XEvent>(
        kernel_state(), completion_handle, completion_handle->header.type);
    if (object) {
      object->Retain();
    }
  };

  if (info->disc_number == disc_number) {
    completion_event();
    return X_ERROR_SUCCESS;
  }

  auto filesystem = kernel_state()->file_system();
  auto mount_path = "\\Device\\LauncherData";

  if (filesystem->ResolvePath(mount_path) != NULL) {
    filesystem->UnregisterDevice(mount_path);
  }

  std::u16string text_message = xe::load_and_swap<std::u16string>(
      kernel_state()->memory()->TranslateVirtual(error_message->stringTextPtr));

  const std::filesystem::path new_disc_path =
      kernel_state()->emulator()->GetNewDiscPath(xe::to_utf8(text_message));
  XELOGI("GetNewDiscPath returned path {}.", new_disc_path.string().c_str());

  // TODO(Gliniak): Implement checking if inserted file is requested one
  kernel_state()->emulator()->MountPath(new_disc_path, mount_path);
  completion_event();

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamSwapDisc, kContent, kSketchy);

// XAM's tray numbering is not the SMC's. dash.xex toggles an empty tray with
// it (0x923324DC): 0-1 open the tray, 2-3 close it, 4 does nothing - so 0-1
// are closed, 2 open, 3 opening, 4 closing. XAM's own tray-change handler
// (0x816BB4D4) checks for media on 0, 1 and 4, which agrees. Passing the
// SMC's OPEN (0) through made the dashboard open an open tray.
dword_result_t XamLoaderGetDvdTrayState_entry() {
  switch (kernel_state()->smc()->GetTrayState()) {
    case X_DVD_TRAY_STATE::OPEN:
      return 2;
    case X_DVD_TRAY_STATE::OPENING:
      return 3;
    case X_DVD_TRAY_STATE::CLOSING:
      return 4;
    default:
      return 0;
  }
}
DECLARE_XAM_EXPORT1(XamLoaderGetDvdTrayState, kNone, kImplemented);

void XamLoaderGetMediaInfoEx_entry(lpdword_t media_type, lpdword_t title_id,
                                   lpdword_t unk3) {
  // A disc is in when the running title booted from optical media, or when
  // one was put in through the tray (Emulator::InsertDisc). The dashboard's
  // PollDisc (and titles) treat title_id as the disc identity: a non-zero,
  // stable id means a disc is present, 0 means an empty tray.
  uint32_t disc_title_id = 0;
  bool has_disc =
      kernel_state()->deployment_type_ == XDeploymentType::kOpticalDisc;
  if (has_disc) {
    disc_title_id = kernel_state()->title_id();
  } else {
    has_disc =
        kernel_state()->emulator()->GetInsertedDisc(nullptr, &disc_title_id);
  }
  if (media_type) {
    *media_type = has_disc ? X_DVD_DISC_STATE::XBOX_360_GAME_DISC
                           : X_DVD_DISC_STATE::NO_DISC;
  }
  if (title_id) {
    *title_id = has_disc ? disc_title_id : 0;
  }
  if (unk3) {
    *unk3 = 0;
  }
}
DECLARE_XAM_EXPORT1(XamLoaderGetMediaInfoEx, kNone, kStub);

void XamLoaderGetMediaInfo_entry(lpdword_t media_type, lpdword_t title_id) {
  XamLoaderGetMediaInfoEx_entry(media_type, title_id, 0);
}
DECLARE_XAM_EXPORT1(XamLoaderGetMediaInfo, kNone, kStub);

dword_result_t xeXamContentLaunchImage(dword_t user_index,
                                       lpstring_t image_location,
                                       lpvoid_t content_data_ptr,
                                       dword_t content_data_size,
                                       lpstring_t xex_path, dword_t flag) {
  /* Notes:
      - In code this subfunction is used by all XamContentLaunchImage
     functions
      - flags used by XamLoaderLaunchTitleEx
      - user_index used by xeXamContentOpenFile
      - if image_location null use xeXamContentOpenFile else use
     exXamContentCreate
      - root_name is "XSYSLAUNCH" while XamLoaderLaunchTitleEx uses
     "XSYSLAUNCH:\\""
      - title_id is usually written into first 8 characters of filename
  */
  std::filesystem::path host_path;

  if (!image_location) {
    XCONTENT_DATA_INTERNAL content_data{};
    X_RESULT result =
        ReadContentData(content_data_ptr, content_data_size, content_data);
    if (result != X_ERROR_SUCCESS) {
      return result;
    };

    // Only the internal record carries an owner. The others leave xuid 0.
    const uint64_t xuid = content_data.xuid;

    if (content_data.title_id == 0 ||
        content_data.title_id == kCurrentlyRunningTitleId) {
      content_data.title_id = kernel_state()->title_id();
    }

    // The content manager knows both roots - the global tree and the active
    // title update's - and whether the package is an extracted directory or
    // the container itself.
    host_path =
        kernel_state()->content_manager()->FindPackagePath(xuid, content_data);

    // An XNA title is not in the content tree - it lives in the library the
    // enumerator reported it from, under its own file name.
    if (host_path.empty() || !std::filesystem::exists(host_path)) {
      const auto library =
          kernel_state()->emulator()->storage_root() / "xna_library";
      const auto candidate = library / xe::to_path(content_data.file_name());
      xna::XnaPackageInfo xna_info;
      std::error_code ec;
      if (std::filesystem::exists(candidate, ec) &&
          xna::IsXnaPackage(candidate, &xna_info)) {
        host_path = candidate;
      }
    }

    if (host_path.empty() || !std::filesystem::exists(host_path)) {
      XELOGE(
          "XamContentLaunchImage: no package for title {:08X} type {:08X} "
          "'{}'",
          content_data.title_id.get(),
          static_cast<uint32_t>(content_data.content_type.get()),
          content_data.file_name());
      return X_STATUS_NO_SUCH_FILE;
    }

    // A container goes in whole - that is exactly what File > Open does with
    // one. An extracted package is a directory, so point at the executable
    // inside it. An XNA/XBLIG package is the exception: it has no default.xex,
    // and the launcher mounts the package itself.
    xna::XnaPackageInfo xna_info;
    if (std::filesystem::is_directory(host_path) &&
        !xna::IsXnaPackage(host_path, &xna_info)) {
      host_path /= xe::to_path(xex_path.value());
    }
  } else {
    vfs::Entry* entry =
        kernel_state()->file_system()->ResolvePath(image_location.value());
    if (!entry) {
      return X_STATUS_NO_SUCH_FILE;
    }

    host_path = kernel_state()->emulator()->content_root() / entry->name();
    if (!std::filesystem::exists(host_path)) {
      uint64_t progress = 0;
      vfs::VirtualFileSystem::ExtractDeviceFile(
          entry, kernel_state()->emulator()->content_root(), progress, true);
    }
  }

  RecordLaunchOrigin();

  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");

  auto& loader_data = xam->loader_data();
  loader_data.host_path = xe::path_to_utf8(host_path);
  loader_data.launch_path = xex_path.value();
  loader_data.command_line.clear();

  xam->SaveLoaderData();

  XELOGE("XamContentLaunchImage: host '{}' module '{}'", loader_data.host_path,
         loader_data.launch_path);

  ReloadForLaunch();
  return X_ERROR_SUCCESS;
}

// XdfLaunchNewImageFromCache(image, launch_data, launch_data_size, flags):
// start one of the dashboard's cached images with a launch blob. My Games
// starts a community game with it - sub_9274AA48 in the 17559 dash.xex builds
//
//   +0x00  be32  0xCAFEBABE
//   +0x04  char  XCONTENT_DATA.file_name (0x2A bytes, NUL at +0x2E)
//   +0x30  be32  XCONTENT_DATA.device_id
//
// and sub_9274AA00 hands it over as ("xna_titlelauncher.xex", blob, 0x34, 0),
// turning a failure into 0x80004002. That blob is exactly what
// Emulator::LaunchXnaPackage gives the launcher, so the content it names is
// launched as a package and takes that path: the package mounted, the
// launcher booted from the Dashboard folder with the same blob.
dword_result_t XdfLaunchNewImageFromCache_entry(lpstring_t image_ptr,
                                                lpvoid_t data_ptr,
                                                dword_t data_size,
                                                dword_t flags) {
  const std::string image = image_ptr ? image_ptr.value() : std::string();
  // Copied now: the blob is on the caller's stack.
  std::vector<uint8_t> data;
  if (data_ptr && data_size) {
    const uint8_t* bytes = data_ptr.as<const uint8_t*>();
    data.assign(bytes, bytes + data_size);
  }

  if (xe::utf8::lower_ascii(image) == "xna_titlelauncher.xex" &&
      data.size() >= 0x34 &&
      xe::load_and_swap<uint32_t>(data.data()) == 0xCAFEBABE) {
    const char* name = reinterpret_cast<const char*>(data.data() + 4);
    XCONTENT_DATA_INTERNAL content_data{};
    content_data.device_id = xe::load_and_swap<uint32_t>(data.data() + 0x30);
    content_data.content_type = XContentType::kMarketplaceContent;
    content_data.title_id = kXN_2002;
    content_data.set_file_name(std::string(name, strnlen(name, 0x2A)));

    const auto host_path =
        kernel_state()->content_manager()->FindPackagePath(0, content_data);
    std::error_code ec;
    if (host_path.empty() || !std::filesystem::exists(host_path, ec) ||
        !xna::IsXnaPackage(host_path, nullptr)) {
      XELOGE("XdfLaunchNewImageFromCache('{}'): no XNA package for '{}'", image,
             content_data.file_name());
      return X_E_FAIL;
    }

    RecordLaunchOrigin();
    auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
    auto& loader_data = xam->loader_data();
    loader_data.host_path = xe::path_to_utf8(host_path);
    loader_data.launch_path.clear();
    loader_data.command_line.clear();
    xam->SaveLoaderData();
    XELOGE("XdfLaunchNewImageFromCache('{}'): community game '{}' -> {}", image,
           content_data.file_name(), loader_data.host_path);
    ReloadForLaunch();
    return X_E_SUCCESS;
  }

  XELOGE(
      "XdfLaunchNewImageFromCache('{}', {} byte(s) of launch data, flags "
      "{:08X}): not handled",
      image, data.size(), flags.value());
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XdfLaunchNewImageFromCache, kContent, kSketchy);

dword_result_t XamContentLaunchImageFromFileInternal_entry(
    lpstring_t image_location, lpstring_t xex_name) {
  return xeXamContentLaunchImage(XUserIndexNone, image_location, nullptr, NULL,
                                 xex_name, NULL);
}
DECLARE_XAM_EXPORT1(XamContentLaunchImageFromFileInternal, kContent, kStub);

dword_result_t XamContentLaunchImage_entry(dword_t user_index,
                                           lpvoid_t content_data_ptr,
                                           lpstring_t xex_path) {
  return xeXamContentLaunchImage(user_index, nullptr, content_data_ptr,
                                 sizeof(XCONTENT_DATA), xex_path, NULL);
}
DECLARE_XAM_EXPORT1(XamContentLaunchImage, kContent, kStub);

dword_result_t XamContentLaunchImageInternal_entry(lpvoid_t content_data_ptr,
                                                   lpstring_t xex_path) {
  return xeXamContentLaunchImage(XUserIndexNone, nullptr, content_data_ptr,
                                 sizeof(XCONTENT_DATA_INTERNAL), xex_path,
                                 NULL);
}
DECLARE_XAM_EXPORT1(XamContentLaunchImageInternal, kContent, kStub);

dword_result_t XamContentLaunchImageInternalEx_entry(lpvoid_t content_data_ptr,
                                                     lpstring_t xex_path,
                                                     dword_t flags) {
  return xeXamContentLaunchImage(XUserIndexNone, nullptr, content_data_ptr,
                                 sizeof(XCONTENT_DATA_INTERNAL), xex_path,
                                 flags);
}
DECLARE_XAM_EXPORT1(XamContentLaunchImageInternalEx, kContent, kStub);

void XamContentRegisterChangeCallback_entry(dword_t callback) {
  kernel_state()->xam_state()->SetContentRegisterCallback(callback);
}
DECLARE_XAM_EXPORT1(XamContentRegisterChangeCallback, kContent, kImplemented);

dword_result_t XamContentGetDeviceVolumePath_entry(dword_t device_id,
                                                   lpvoid_t path_ptr,
                                                   dword_t path_size,
                                                   dword_t append_backslash) {
  std::string volume_path;
  switch (static_cast<DummyDeviceId>(static_cast<uint32_t>(device_id))) {
    case DummyDeviceId::HDD:
      volume_path = "hdd0\\";
      break;
    case DummyDeviceId::ODD:
      volume_path = "dvd0\\";
      break;
    case DummyDeviceId::MU:
      volume_path = "mu0\\";
      break;
    case DummyDeviceId::USB:
      volume_path = "usb0\\";
      break;
    default:
      return X_ERROR_FUNCTION_FAILED;
  }

  char* path =
      kernel_memory()->TranslateVirtual<char*>(path_ptr.guest_address());

  string_util::copy_truncating(path, volume_path, path_size);

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentGetDeviceVolumePath, kContent, kStub);

// The record the cross-title enumerator hands back carries no art, so this
// is the only route by which a screen like Settings > Storage gets an
// item's picture: it reads a whole XContentMetadata, thumbnail included.
// Failing it is what leaves every row on the dashboard's generic fallback
// icon. A package's own metadata is copied as it is; a record with no
// package behind it (an XNA library title, a profile made before profiles
// carried a header) gets one built from the record itself.
// The title's box art as a metadata thumbnail: a PNG small enough for the
// header's thumbnail field. Only for a game's own record - a save or DLC keeps
// its picture - and never for 584E07D2, the Indie Player every XNA game shares.
// Empty when there is no box art, and the caller keeps what it had.
static std::vector<uint8_t> BoxArtThumbnail(const XCONTENT_DATA_INTERNAL& data,
                                            size_t max_size) {
  switch (data.content_type.get()) {
    case XContentType::kArcadeTitle:
    case XContentType::kXbox360Title:
    case XContentType::kInstalledGame:
    case XContentType::kXboxTitle:
    case XContentType::kGameDemo:
      break;
    default:
      return {};
  }
  const uint32_t title_id = data.title_id;
  if (!title_id || title_id == kXN_2002 || title_id == kDashboardID) {
    return {};
  }

  static std::mutex cache_mutex;
  static std::map<uint32_t, std::vector<uint8_t>> cache;
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (const auto found = cache.find(title_id); found != cache.end()) {
      return found->second;
    }
  }

  std::vector<uint8_t> png;
  const std::vector<uint8_t> art = FetchTitleBoxArt(title_id);
  int width = 0;
  int height = 0;
  int channels = 0;
  uint8_t* pixels = art.empty() ? nullptr
                                : stbi_load_from_memory(
                                      art.data(), static_cast<int>(art.size()),
                                      &width, &height, &channels, 3);
  if (pixels) {
    // Box art is 219x300; a photo does not compress far as a PNG, so the
    // picture shrinks until it fits the field.
    for (int target_height = 128; target_height >= 48 && png.empty();
         target_height -= 8) {
      const int target_width =
          std::max(1, width * target_height / std::max(1, height));
      std::vector<uint8_t> scaled(size_t(target_width) * target_height * 3);
      for (int y = 0; y < target_height; ++y) {
        const int y0 = y * height / target_height;
        const int y1 = std::max(y0 + 1, (y + 1) * height / target_height);
        for (int x = 0; x < target_width; ++x) {
          const int x0 = x * width / target_width;
          const int x1 = std::max(x0 + 1, (x + 1) * width / target_width);
          uint32_t sum[3] = {};
          for (int sy = y0; sy < y1; ++sy) {
            for (int sx = x0; sx < x1; ++sx) {
              const uint8_t* p = pixels + (size_t(sy) * width + sx) * 3;
              sum[0] += p[0];
              sum[1] += p[1];
              sum[2] += p[2];
            }
          }
          const uint32_t count = uint32_t((y1 - y0) * (x1 - x0));
          uint8_t* q = scaled.data() + (size_t(y) * target_width + x) * 3;
          q[0] = uint8_t(sum[0] / count);
          q[1] = uint8_t(sum[1] / count);
          q[2] = uint8_t(sum[2] / count);
        }
      }
      std::vector<uint8_t> encoded;
      stbi_write_png_to_func(
          [](void* context, void* bytes, int size) {
            auto* out = static_cast<std::vector<uint8_t>*>(context);
            const auto* begin = static_cast<const uint8_t*>(bytes);
            out->insert(out->end(), begin, begin + size);
          },
          &encoded, target_width, target_height, 3, scaled.data(),
          target_width * 3);
      if (!encoded.empty() && encoded.size() <= max_size) {
        png = std::move(encoded);
      }
    }
    stbi_image_free(pixels);
  }

  std::lock_guard<std::mutex> lock(cache_mutex);
  cache[title_id] = png;
  return png;
}

// The title's cover as a PNG on the host, written once into the art cache. My
// Games draws the title image the dashboard loads (see
// PatchDashboardLibraryArt), so this is what a tile shows. The front of
// XboxUnity's official retail cover comes first - 427x600, twice the detail of
// the marketplace art - then the Xbox image server's boxartlg.jpg (219x300).
// Empty when neither has the title.
static std::filesystem::path BoxArtTilePath(uint32_t title_id) {
  static std::mutex tile_mutex;
  std::lock_guard<std::mutex> lock(tile_mutex);
  const std::filesystem::path path = kernel_state()->emulator()->cache_root() /
                                     "disc_art" /
                                     fmt::format("{:08X}_front.png", title_id);
  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec)) {
    return path;
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  uint8_t* pixels = nullptr;
  // A 360 case insert is 273mm across: back, a 14mm spine, and a 129.5mm
  // front on the right - the front is the right 129.5/273 of the jacket.
  int left = 0;
  int crop_width = 0;
  const std::vector<uint8_t> jacket = FetchTitleCoverJacket(title_id);
  if (!jacket.empty()) {
    pixels =
        stbi_load_from_memory(jacket.data(), static_cast<int>(jacket.size()),
                              &width, &height, &channels, 3);
    if (pixels && width > height) {
      crop_width = width * 1295 / 2730;
      left = width - crop_width;
    } else if (pixels) {
      // Not a jacket after all; take it whole.
      crop_width = width;
    }
  }
  if (!pixels) {
    const std::vector<uint8_t> art = FetchTitleBoxArt(title_id);
    if (art.empty()) {
      return {};
    }
    pixels = stbi_load_from_memory(art.data(), static_cast<int>(art.size()),
                                   &width, &height, &channels, 3);
    if (!pixels) {
      return {};
    }
    crop_width = width;
  }
  std::vector<uint8_t> png;
  stbi_write_png_to_func(
      [](void* context, void* bytes, int size) {
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        const auto* begin = static_cast<const uint8_t*>(bytes);
        out->insert(out->end(), begin, begin + size);
      },
      &png, crop_width, height, 3, pixels + size_t(left) * 3, width * 3);
  stbi_image_free(pixels);
  if (png.empty()) {
    return {};
  }
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(png.data()),
             static_cast<std::streamsize>(png.size()));
  return file ? path : std::filesystem::path();
}

// One attribute of the first <tag ...> in an ArcadeInfo.xml already reduced to
// ASCII. Empty when the tag or attribute is not there.
static std::string ArcadeInfoAttribute(const std::string& xml,
                                       std::string_view tag,
                                       std::string_view attribute) {
  const size_t open = xml.find(tag);
  if (open == std::string::npos) {
    return {};
  }
  const size_t close = xml.find('>', open);
  const std::string element = xml.substr(open, close - open);
  const std::string key = std::string(" ") + std::string(attribute) + "=\"";
  const size_t at = element.find(key);
  if (at == std::string::npos) {
    return {};
  }
  const size_t value = at + key.size();
  const size_t end = element.find('"', value);
  return end == std::string::npos ? std::string()
                                  : element.substr(value, end - value);
}

std::string DashboardTitleImageRedirect(const std::string& guest_path) {
  if (kernel_state()->title_id() != kDashboardID) {
    return {};
  }
  // "Content4:\game.png" - a file at the root of a mounted package.
  const size_t colon = guest_path.find(":\\");
  if (colon == std::string::npos) {
    return {};
  }
  const std::string name = guest_path.substr(colon + 2);
  if (name.empty() || name.find('\\') != std::string::npos ||
      !xe::utf8::lower_ascii(name).ends_with(".png")) {
    return {};
  }

  vfs::Entry* info_entry = kernel_state()->file_system()->ResolvePath(
      guest_path.substr(0, colon + 1) + "\\ArcadeInfo.xml");
  if (!info_entry) {
    return {};
  }
  vfs::File* file = nullptr;
  if (info_entry->Open(vfs::FileAccess::kGenericRead, &file) !=
          X_STATUS_SUCCESS ||
      !file) {
    return {};
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(info_entry->size()));
  size_t read = 0;
  const bool ok = bytes.empty() ||
                  file->ReadSync(std::span<uint8_t>(bytes.data(), bytes.size()),
                                 0, &read) == X_STATUS_SUCCESS;
  file->Destroy();
  if (!ok) {
    return {};
  }
  bytes.resize(read);

  // ArcadeInfo.xml is UTF-16 with a byte-order mark; the attributes wanted
  // are ASCII, so keep the low byte of each character.
  std::string xml;
  if (bytes.size() >= 2 && (bytes[0] == 0xFF || bytes[0] == 0xFE) &&
      (bytes[1] == 0xFE || bytes[1] == 0xFF)) {
    const size_t low = bytes[0] == 0xFF ? 0 : 1;
    for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
      xml.push_back(char(bytes[i + low]));
    }
  } else {
    xml.assign(bytes.begin(), bytes.end());
  }

  const std::string image = ArcadeInfoAttribute(xml, "<TitleInfo", "ImagePath");
  const std::string id = ArcadeInfoAttribute(xml, "<TitleInfo", "ID");
  if (image.empty() || id.empty() ||
      xe::utf8::lower_ascii(image) != xe::utf8::lower_ascii(name)) {
    return {};
  }
  const uint32_t title_id =
      static_cast<uint32_t>(std::strtoul(id.c_str(), nullptr, 10));
  if (!title_id || title_id == kXN_2002) {
    return {};
  }
  const std::filesystem::path tile = BoxArtTilePath(title_id);
  if (tile.empty()) {
    return {};
  }

  // The box art folder, mounted read-only the first time it is needed and
  // again if a title switch took the device down.
  constexpr std::string_view kDevice = "\\Device\\NexiaBoxArt";
  auto* file_system = kernel_state()->file_system();
  vfs::HostPathDevice* device = nullptr;
  if (vfs::Entry* root = file_system->ResolvePath(std::string(kDevice) + "\\")) {
    device = static_cast<vfs::HostPathDevice*>(root->device());
  } else {
    auto created = std::make_unique<vfs::HostPathDevice>(
        kDevice, tile.parent_path(), true);
    if (!created->Initialize()) {
      return {};
    }
    device = created.get();
    if (!file_system->RegisterDevice(std::move(created))) {
      return {};
    }
  }
  // A tile made after the device listed its folder is not in its tree; this
  // title's may be one of them.
  if (!device->AddRootFile(xe::path_to_utf8(tile.filename()))) {
    return {};
  }
  XELOGI("Dashboard: {} -> box art for {:08X}", guest_path, title_id);
  return fmt::format("{}\\{}", kDevice, xe::path_to_utf8(tile.filename()));
}

dword_result_t XamContentGetMetaDataInternal_entry(
    pointer_t<XCONTENT_DATA_INTERNAL> content_data_ptr,
    lpdword_t content_metadata_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!content_data_ptr || !content_metadata_ptr) {
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                  X_ERROR_INVALID_PARAMETER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_INVALID_PARAMETER;
  }

  auto* content_manager = kernel_state()->content_manager();
  const XCONTENT_DATA_INTERNAL& data = *content_data_ptr;
  auto* out =
      kernel_memory()->TranslateVirtual(content_metadata_ptr.guest_address());

  const auto package = content_manager->OpenPackage(data.xuid, data);
  if (package && package->IsValidPackage()) {
    memcpy(out, package->GetContainerMetadata(), sizeof(XContentMetadata));
  } else {
    auto metadata = std::make_unique<XContentMetadata>();
    std::memset(metadata.get(), 0, sizeof(XContentMetadata));
    metadata->content_type = data.content_type;
    metadata->metadata_version = 2;
    metadata->content_size = data.content_size;
    metadata->category = data.category;
    metadata->online_creator = data.xuid;
    metadata->set_display_name(XLanguage::kEnglish, data.display_name());
    metadata->set_title_name(data.title_name());

    // The picture, from wherever this build keeps it: a container's own
    // header, a profile's gamerpic, or the sidecar written into content kept
    // extracted. GetContentThumbnail already knows all three.
    std::vector<uint8_t> thumbnail;
    if (XSUCCEEDED(
            content_manager->GetContentThumbnail(data.xuid, data, thumbnail)) &&
        !thumbnail.empty() && thumbnail.size() <= sizeof(metadata->thumbnail)) {
      std::memcpy(metadata->thumbnail, thumbnail.data(), thumbnail.size());
      metadata->thumbnail_size = uint32_t(thumbnail.size());
    }
    memcpy(out, metadata.get(), sizeof(XContentMetadata));
  }

  // A game record's thumbnail is its box art when the Xbox image server has
  // it; otherwise the thumbnail above stands. (My Games' big tiles are drawn
  // from the title image instead - see DashboardTitleImageRedirect.)
  auto* out_metadata = reinterpret_cast<XContentMetadata*>(out);
  const std::vector<uint8_t> box_art =
      BoxArtThumbnail(data, sizeof(out_metadata->thumbnail));
  if (!box_art.empty()) {
    std::memset(out_metadata->thumbnail, 0, sizeof(out_metadata->thumbnail));
    std::memcpy(out_metadata->thumbnail, box_art.data(), box_art.size());
    out_metadata->thumbnail_size = uint32_t(box_art.size());
  }

  XELOGW("XamContentGetMetaDataInternal: '{}' type {:08X}{}", data.file_name(),
         uint32_t(data.content_type.get()),
         package ? "" : " (no package, built from the record)");

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, 0);
    return X_ERROR_IO_PENDING;
  }

  // INFO: Analysis of xam.xex shows that "internal" functions are wrappers with
  // 0xFE as user_index.
  // In XAM content size is set to 0x200.
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentGetMetaDataInternal, kContent, kImplemented);

dword_result_t XamContentGetAttributesInternal_entry(
    pointer_t<XCONTENT_DATA_INTERNAL> content_data_ptr,
    lpdword_t file_attribute_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!content_data_ptr || !file_attribute_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto info = kernel_memory()->TranslateVirtual<X_FILE_ATTRIBUTE_DATA*>(
      file_attribute_ptr.guest_address());
  // Nothing installed here is partial, corrupt or transfer-locked.
  std::memset(info, 0, sizeof(X_FILE_ATTRIBUTE_DATA));

  // The dashboard asks once per My Games item, including the ones that are
  // not in the content tree (XNA library titles, profiles). Failing here
  // drops the tile, so a record with no package behind it answers with no
  // attributes rather than an error.
  std::filesystem::path host_path;
  if (const auto package = kernel_state()->content_manager()->OpenPackage(
          0, *content_data_ptr)) {
    host_path = package->GetPackageHostPath();
  } else {
    // The cross-title enumerator reports an XNA library title under the
    // game's own id, which the content tree knows nothing of.
    const auto candidate =
        kernel_state()->emulator()->storage_root() / "xna_library" /
        xe::to_path(xe::string_util::trim(content_data_ptr->file_name()));
    if (xna::IsXnaPackage(candidate, nullptr)) {
      host_path = candidate;
    }
  }
  uint64_t size = 0;
  if (!host_path.empty()) {
    if (const auto file_info = xe::filesystem::GetInfo(host_path); file_info) {
      info->ftCreationTime = file_info->create_timestamp;
      info->ftLastAccessTime = file_info->access_timestamp;
      info->ftLastWriteTime = file_info->write_timestamp;
    }
    // A directory's own entry has no size; the files under it do.
    size = ContentManager::PackageSizeOnDisk(host_path);
    info->nFileSizeHigh = uint32_t(size >> 32);
    info->nFileSizeLow = uint32_t(size);
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, 0);
    return X_ERROR_IO_PENDING;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentGetAttributesInternal, kContent, kSketchy);

dword_result_t XamContentGetLocalizedString_entry(
    pointer_t<XContentMetadata> metadata_ptr, lpu16string_t display_name_buffer,
    dword_t display_name_buffer_size, lpu16string_t description_buffer,
    dword_t description_buffer_size) {
  if (!metadata_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // Get language from console settings
  const auto language = kernel_state()->xconfig()->ReadSetting<uint32_t>(
      kernel::XCONFIG_USER_CATEGORY, kernel::XCONFIG_USER_LANGUAGE);

  if (display_name_buffer) {
    const auto language_str =
        metadata_ptr->display_name(static_cast<XLanguage>(language));
    xe::string_util::copy_and_swap_truncating(display_name_buffer, language_str,
                                              display_name_buffer_size);
  }

  if (description_buffer) {
    const auto language_str =
        metadata_ptr->description(static_cast<XLanguage>(language));
    xe::string_util::copy_and_swap_truncating(description_buffer, language_str,
                                              description_buffer_size);
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentGetLocalizedString, kContent, kSketchy);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Content);
