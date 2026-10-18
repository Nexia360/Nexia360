/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <filesystem>
#include <unordered_set>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string_util.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xna/xna_launcher.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/file.h"
#include "xenia/xbox.h"

DECLARE_int32(license_mask);

namespace xe {
namespace kernel {
namespace xam {

void AddODDContentTest(object_ref<XStaticEnumerator<XCONTENT_AGGREGATE_DATA>> e,
                       XContentType content_type) {
  auto root_entry = kernel_state()->file_system()->ResolvePath(
      "GAME:\\Content\\0000000000000000");
  if (!root_entry) {
    return;
  }

  auto content_type_path = fmt::format("{:08X}", uint32_t(content_type));

  xe::filesystem::WildcardEngine title_find_engine;
  title_find_engine.SetRule("????????");

  xe::filesystem::WildcardEngine content_find_engine;
  content_find_engine.SetRule("????????????????");

  size_t title_find_index = 0;
  vfs::Entry* title_entry;
  for (;;) {
    title_entry =
        root_entry->IterateChildren(title_find_engine, &title_find_index);
    if (!title_entry) {
      break;
    }

    auto title_id =
        string_util::from_string<uint32_t>(title_entry->name(), true);

    auto content_root_entry = title_entry->ResolvePath(content_type_path);
    if (content_root_entry) {
      size_t content_find_index = 0;
      vfs::Entry* content_entry;
      for (;;) {
        content_entry = content_root_entry->IterateChildren(
            content_find_engine, &content_find_index);
        if (!content_entry) {
          break;
        }

        auto item = e->AppendItem();
        assert_not_null(item);
        if (item) {
          item->device_id = static_cast<uint32_t>(DummyDeviceId::ODD);
          item->content_type = content_type;
          item->set_display_name(to_utf16(content_entry->name()));
          item->set_file_name(content_entry->name());
          item->title_id = title_id;
        }
      }
    }
  }
}

// Every profile on the machine, as a content record.
//
// A profile lives under its OWN xuid - content/<xuid>/FFFE07D1/00010000/ - so
// the title-id sweep above never reaches one: the dashboard asks with xuid 0,
// and a profile is not stored there. The profile manager already knows them
// all, which is where Settings > Storage gets its list.
static void AppendProfileItems(XStaticEnumerator<XCONTENT_DATA_INTERNAL>* e) {
  auto* profile_manager = kernel_state()->xam_state()->profile_manager();
  if (!profile_manager) {
    return;
  }
  const auto* accounts = profile_manager->GetAccounts();
  if (!accounts) {
    return;
  }

  for (const auto& [xuid, account] : *accounts) {
    auto item = e->AppendItem();
    if (!item) {
      continue;
    }
    std::memset(item, 0, sizeof(*item));
    item->device_id = static_cast<uint32_t>(DummyDeviceId::HDD);
    item->content_type = XContentType::kProfile;
    item->title_id = kDashboardID;
    item->xuid = xuid;
    item->license_mask = 0xFFFFFFFF;

    const std::string gamertag = account.GetGamertagString();
    item->set_display_name(xe::to_utf16(gamertag));
    item->set_file_name(fmt::format("{:016X}", xuid));
    xe::string_util::copy_and_swap_truncating(item->title_name,
                                              xe::to_utf16(gamertag),
                                              xe::countof(item->title_name));

    // What Storage shows as the profile's size.
    std::error_code ec;
    uint64_t bytes = 0;
    const auto root = profile_manager->GetProfilePath(xuid);
    if (std::filesystem::is_directory(root, ec)) {
      for (const auto& entry :
           std::filesystem::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec)) {
          bytes += entry.file_size(ec);
        }
      }
    }
    item->content_size = bytes;

    XELOGD("  profile item xuid {:016X} '{}' {} bytes", xuid, gamertag, bytes);
  }
}

// Every package in the XNA library, as a content record the dashboard can
// show and launch. The file name is the package's own, which is what
// XamContentLaunchImage resolves back to a path.
static void AppendXnaLibraryItems(
    XStaticEnumerator<XCONTENT_DATA_INTERNAL>* e) {
  const auto library =
      kernel_state()->emulator()->storage_root() / "xna_library";
  std::error_code ec;
  if (!std::filesystem::is_directory(library, ec)) {
    return;
  }

  for (const auto& entry : std::filesystem::directory_iterator(library, ec)) {
    // A locally built title is a directory; a shipped one is a container.
    if (!entry.is_regular_file() && !entry.is_directory()) {
      continue;
    }
    xna::XnaPackageInfo info;
    if (!xna::IsXnaPackage(entry.path(), &info)) {
      continue;
    }

    auto item = e->AppendItem();
    if (!item) {
      continue;
    }
    std::memset(item, 0, sizeof(*item));
    item->device_id = static_cast<uint32_t>(DummyDeviceId::HDD);
    item->content_type = XContentType::kArcadeTitle;
    item->title_id = info.title_id;
    item->xuid = 0;
    item->license_mask = 0xFFFFFFFF;

    // The display name carries the extension the exe had; the dashboard shows
    // this verbatim.
    std::string name = info.display_name;
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
      name = name.substr(0, dot);
    }
    item->set_display_name(xe::to_utf16(name));
    item->set_file_name(xe::path_to_utf8(entry.path().filename()));
    xe::string_util::copy_and_swap_truncating(
        item->title_name, xe::to_utf16(name), xe::countof(item->title_name));

    XELOGD("  xna item title {:08X} '{}' file '{}'", item->title_id.get(), name,
           item->file_name());
  }
}

// Alias XContentCreateCrossTitleEnumerator
dword_result_t XamContentAggregateCreateEnumerator_entry(qword_t xuid,
                                                         dword_t device_id,
                                                         dword_t content_type,
                                                         dword_t title_id,
                                                         lpdword_t handle_out) {
  assert_not_null(handle_out);

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    return X_E_INVALIDARG;
  }

  auto e =
      make_object<XStaticEnumerator<XCONTENT_DATA_INTERNAL>>(kernel_state(), 1);
  X_KENUMERATOR_CONTENT_AGGREGATE* extra;
  auto result = e->Initialize(XUserIndexAny, 0xFE, 0x2000E, 0x20010, 0, &extra);
  if (XFAILED(result)) {
    return result;
  }

  extra->magic = kXObjSignature;
  extra->handle = e->handle();

  const XContentType content_type_enum =
      static_cast<XContentType>(content_type.value());

  if (!device_info || device_info->device_type == DeviceType::HDD) {
    std::vector<uint32_t> title_ids;
    if (title_id) {
      title_ids.push_back(title_id.value());
      // Fetch any alternate title IDs defined in the XEX header
      // (used by games to load saves from other titles, etc)
      auto exe_module = kernel_state()->GetExecutableModule();
      if (exe_module && exe_module->xex_module()) {
        const auto& alt_ids =
            exe_module->xex_module()->opt_alternate_title_ids();
        std::copy(alt_ids.cbegin(), alt_ids.cend(),
                  std::back_inserter(title_ids));
      }
    } else {
      auto* content_manager = kernel_state()->content_manager();
      std::unordered_set<uint32_t> all_ids =
          content_manager->FindAllTitleIds(xuid == -1 ? 0 : uint64_t(xuid));
      if (xuid && xuid != -1) {
        const auto common_ids = content_manager->FindAllTitleIds(0);
        all_ids.insert(common_ids.cbegin(), common_ids.cend());
      }
      title_ids.assign(all_ids.cbegin(), all_ids.cend());
    }

    for (const auto& title_id : title_ids) {
      // Get all content data.
      auto content_datas = kernel_state()->content_manager()->ListContent(
          static_cast<uint32_t>(DummyDeviceId::HDD),
          xuid == -1 ? 0 : static_cast<uint64_t>(xuid), title_id,
          content_type_enum);
      for (const auto& content_data : content_datas) {
        auto item = e->AppendItem();
        assert_not_null(item);
        if (!item) {
          continue;
        }
        std::memset(item, 0, sizeof(*item));
        item->device_id = content_data.device_id;
        item->content_type = content_data.content_type;
        item->display_name_raw = content_data.display_name_raw;
        std::memcpy(item->file_name_raw, content_data.file_name_raw,
                    sizeof(content_data.file_name_raw));
        item->padding[0] = 0;
        item->padding[1] = 0;
        item->title_id = content_data.title_id;
        item->xuid = content_data.xuid;

        const uint32_t license_cvar =
            static_cast<uint32_t>(cvars::license_mask);
        item->license_mask = license_cvar;
        if (license_cvar == 0xFFFFFFFF || license_cvar == 1) {
          // Bit 0 is the purchased bit - see ContentManager::OpenContent.
          item->license_mask = item->license_mask.get() | 1;
        }

        const auto package_path =
            kernel_state()->content_manager()->FindPackagePath(
                xuid == -1 ? 0 : uint64_t(xuid), content_data);
        if (std::filesystem::is_regular_file(package_path)) {
          auto header =
              vfs::XContentContainerDevice::ReadContainerHeader(package_path);
          if (header) {
            const auto& metadata = header->content_metadata;
            item->category = metadata.category;
            item->content_size = metadata.content_size;
            xe::string_util::copy_and_swap_truncating(
                item->title_name, metadata.title_name(),
                xe::countof(item->title_name));
          }
        }

        // An XBLIG ships as kMarketplaceContent owned by the XNA Indie Player
        // (0x584E07D2) - on a console it is that app, not the dashboard, which
        // launches it. We have no Indie Player, so the dashboard sees DLC in
        // My Games, has nothing to launch it with, and drops the tile.
        // Reporting it as an arcade title is what makes it launchable, and
        // Nexia hosts it directly.
        if (item->content_type == XContentType::kMarketplaceContent &&
            xna::IsXnaPackage(package_path, nullptr)) {
          item->content_type = XContentType::kArcadeTitle;
        }

        // Temporary: one line per item, so the exact record the dashboard is
        // handed is visible. Retire once My Games is settled.
        XELOGE(
            "  item title {:08X} type {:08X} device {:08X} license {:08X} "
            "cat {:08X} '{}' file '{}'",
            item->title_id.get(), uint32_t(item->content_type.get()),
            item->device_id.get(), item->license_mask.get(),
            item->category.get(), xe::to_utf8(item->display_name()),
            item->file_name());
      }
    }

    // XNA titles live in their own library, not the content tree - they are
    // games, not content belonging to a title - so nothing above finds them.
    // They are reported as arcade titles because that is what the dashboard
    // will offer to launch; the package itself says kMarketplaceContent, which
    // is true of an XBLIG on a console and useless to us without the Indie
    // Player to launch it.
    if (!title_id && (content_type_enum == XContentType::kInvalid ||
                      content_type_enum == XContentType::kArcadeTitle ||
                      content_type.value() == 0xFFFFFFFF)) {
      AppendXnaLibraryItems(e.get());
    }

    if (content_type_enum == XContentType::kProfile ||
        content_type_enum == XContentType::kInvalid ||
        content_type.value() == 0xFFFFFFFF) {
      AppendProfileItems(e.get());
    }
  }

  // if (!device_info || device_info->device_type == DeviceType::ODD) {
  //   AddODDContentTest(e, content_type_enum);
  // }

  XELOGE(
      "XamContentAggregateCreateEnumerator(xuid {:016X}, device {:08X}, "
      "type {:08X}, title {:08X}) -> {} items",
      uint64_t(xuid), uint32_t(device_id), uint32_t(content_type),
      uint32_t(title_id), e->item_count());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentAggregateCreateEnumerator, kContent, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(ContentAggregate);
