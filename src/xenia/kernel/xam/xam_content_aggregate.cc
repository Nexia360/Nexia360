/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string_util.h"
#include "xenia/base/threading.h"
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
#include "xenia/kernel/xthread.h"
#include "xenia/vfs/file.h"
#include "xenia/xbox.h"

DECLARE_int32(license_mask);

namespace xe {
namespace kernel {
namespace xam {

void AddODDContentTest(object_ref<XStaticEnumerator<XCONTENT_DATA_AGGREGATE>> e,
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
using ItemSink = std::function<void(const XCONTENT_DATA_INTERNAL&)>;
using SeenItems =
    std::set<std::tuple<uint64_t, uint32_t, uint32_t, std::string>>;

static void AppendProfileItems(const ItemSink& add) {
  auto* profile_manager = kernel_state()->xam_state()->profile_manager();
  if (!profile_manager) {
    return;
  }
  const auto* accounts = profile_manager->GetAccounts();
  if (!accounts) {
    return;
  }

  for (const auto& [xuid, account] : *accounts) {
    XCONTENT_DATA_INTERNAL item{};
    item.device_id = static_cast<uint32_t>(DummyDeviceId::HDD);
    item.content_type = XContentType::kProfile;
    item.title_id = kDashboardID;
    item.xuid = xuid;
    item.license_mask = 0xFFFFFFFF;

    const std::string gamertag = account.GetGamertagString();
    item.set_display_name(xe::to_utf16(gamertag));
    item.set_file_name(fmt::format("{:016X}", xuid));
    item.set_title_name(xe::to_utf16(gamertag));

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
    item.content_size = bytes;

    XELOGD("  profile item xuid {:016X} '{}' {} bytes", xuid, gamertag, bytes);
    add(item);
  }
}

// Every package in the XNA library, as a content record the dashboard can
// show and launch. The file name is the package's own, which is what
// XamContentLaunchImage resolves back to a path.
//
// It is reported as what it is on a console: kMarketplaceContent of the Indie
// Player, 584E07D2. The 17559 dashboard's library sorts records by that pair
// (sub_922BE5A0): type 2 under 584E07D2 is a community game and goes in the
// community list, unless its display name is "XNA Creators Club Game
// Launcher", which is the player itself (sub_922C79C0 sets bit 4 at
// record+0x25F4). Reported as kArcadeTitle it took the arcade branch, keyed by
// title id, and My Games said "No games found" with Arcadecraft installed.
static void AppendXnaLibraryItems(const ItemSink& add, SeenItems* seen) {
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

    // Already reported out of the content tree.
    const std::string file_name = xe::path_to_utf8(entry.path().filename());
    if (!seen->emplace(uint64_t(0), kXN_2002,
                       uint32_t(XContentType::kMarketplaceContent), file_name)
             .second) {
      continue;
    }

    XCONTENT_DATA_INTERNAL item{};
    item.device_id = static_cast<uint32_t>(DummyDeviceId::HDD);
    item.content_type = XContentType::kMarketplaceContent;
    item.title_id = kXN_2002;
    item.xuid = 0;
    item.license_mask = 0xFFFFFFFF;

    // The display name carries the extension the exe had; the dashboard shows
    // this verbatim.
    std::string name = info.display_name;
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
      name = name.substr(0, dot);
    }
    item.set_display_name(xe::to_utf16(name));
    item.set_file_name(file_name);
    // The TITLE is the Indie Player's, not the game's: Storage groups records
    // by title and heads the group with this name, so the game's name here
    // labelled the whole group after whichever package came first.
    item.set_title_name(u"Indie Games");
    item.content_size = ContentManager::PackageSizeOnDisk(entry.path());

    XELOGD("  xna item title {:08X} '{}' file '{}' {} bytes",
           item.title_id.get(), name, item.file_name(),
           item.content_size.get());
    add(item);
  }
}

// One title's HDD records, exactly as the aggregate enumerator reports them.
static void AppendTitleItems(uint64_t xuid, uint32_t title_id,
                             XContentType content_type_enum,
                             bool append_profiles, SeenItems* seen,
                             const ItemSink& add) {
  auto* content_manager = kernel_state()->content_manager();
  const auto content_datas = content_manager->ListContent(
      static_cast<uint32_t>(DummyDeviceId::HDD), xuid, title_id,
      content_type_enum, XContentFlag::kNone);
  for (const auto& content_data : content_datas) {
    if (append_profiles &&
        content_data.content_type == XContentType::kProfile) {
      continue;
    }
    if (!seen->emplace(content_data.xuid.get(), content_data.title_id.get(),
                       uint32_t(content_data.content_type.get()),
                       content_data.file_name())
             .second) {
      continue;
    }

    // The package's own record: name, size and title name come from its
    // header (or from the folder, for content that has none).
    XCONTENT_DATA_INTERNAL item = content_data;
    item.padding[0] = 0;
    item.padding[1] = 0;

    const uint32_t license_cvar = static_cast<uint32_t>(cvars::license_mask);
    item.license_mask = item.license_mask.get() | license_cvar;
    if (license_cvar == 0xFFFFFFFF || license_cvar == 1) {
      // Bit 0 is the purchased bit - see ContentManager::OpenContent.
      item.license_mask = item.license_mask.get() | 1;
    }

    // The dashboard classifies on category as well as content type, and
    // the record ListContent builds does not carry it.
    const auto package_path =
        content_manager->FindPackagePath(xuid, content_data);
    if (auto package = content_manager->OpenPackage(package_path);
        package && package->IsValidPackage()) {
      const auto* metadata = package->GetContainerMetadata();
      item.category = metadata->category;
      if (!item.content_size) {
        item.content_size = metadata->content_size;
      }
    }
    // Still nothing - a directory Nexia extracted or wrote has no size in
    // its header - so Storage is told what it takes on disk.
    if (!item.content_size) {
      item.content_size = ContentManager::PackageSizeOnDisk(package_path);
    }

    // An XBLIG ships as kMarketplaceContent owned by the XNA Indie Player
    // (0x584E07D2) and stays that: it is the pair the dashboard's library
    // files a community game under - see AppendXnaLibraryItems.

    // Temporary: one line per item, so the exact record the dashboard is
    // handed is visible. Retire once My Games is settled.
    XELOGD(
        "  item title {:08X} type {:08X} device {:08X} license {:08X} "
        "cat {:08X} '{}' file '{}' {} bytes",
        item.title_id.get(), uint32_t(item.content_type.get()),
        item.device_id.get(), item.license_mask.get(), item.category.get(),
        xe::to_utf8(item.display_name()), item.file_name(),
        item.content_size.get());

    add(item);
  }
}

// What the dashboard's all-titles HDD query reports for one title.
static std::vector<XCONTENT_DATA_INTERNAL> DashboardTitleItems(
    uint32_t title_id) {
  std::vector<XCONTENT_DATA_INTERNAL> items;
  SeenItems seen;
  const ItemSink add = [&items](const XCONTENT_DATA_INTERNAL& item) {
    items.push_back(item);
  };
  AppendTitleItems(0, title_id, XContentType::kAll, true, &seen, add);
  if (title_id == kXN_2002) {
    AppendXnaLibraryItems(add, &seen);
  }
  return items;
}

static bool DashboardWantsContentChanges() {
  KernelState* state = kernel_state();
  return state && state->title_id() == kDashboardID &&
         state->xam_state()->content_register_callback;
}

static std::mutex content_snapshot_mutex;
static std::map<uint32_t, std::vector<XCONTENT_DATA_INTERNAL>>
    content_snapshots;

void BeginContentChange(uint32_t title_id) {
  if (!DashboardWantsContentChanges()) {
    return;
  }
  auto items = DashboardTitleItems(title_id);
  std::lock_guard<std::mutex> lock(content_snapshot_mutex);
  content_snapshots[title_id] = std::move(items);
}

// Hands each change to the callback the dashboard registered with
// XamContentRegisterChangeCallback (0 added, 1 removed), as xam does when
// content is created or deleted, then has the game library rebuild.
void EndContentChange(uint32_t title_id, std::string_view why) {
  std::vector<XCONTENT_DATA_INTERNAL> before;
  {
    std::lock_guard<std::mutex> lock(content_snapshot_mutex);
    if (auto it = content_snapshots.find(title_id);
        it != content_snapshots.end()) {
      before = std::move(it->second);
      content_snapshots.erase(it);
    }
  }
  if (!DashboardWantsContentChanges()) {
    return;
  }
  const auto after = DashboardTitleItems(title_id);
  auto key = [](const XCONTENT_DATA_INTERNAL& item) {
    return std::make_tuple(item.xuid.get(), item.title_id.get(),
                           uint32_t(item.content_type.get()), item.file_name());
  };
  auto contains = [&](const std::vector<XCONTENT_DATA_INTERNAL>& items,
                      const XCONTENT_DATA_INTERNAL& item) {
    return std::ranges::any_of(
        items, [&](const auto& other) { return key(other) == key(item); });
  };
  std::vector<std::pair<uint32_t, XCONTENT_DATA_INTERNAL>> changes;
  for (const auto& item : after) {
    if (!contains(before, item)) {
      changes.emplace_back(0, item);
    }
  }
  for (const auto& item : before) {
    if (!contains(after, item)) {
      changes.emplace_back(1, item);
    }
  }
  XELOGW("[mygames] {:08X} {}: {} content change(s) for the dashboard",
         title_id, why, changes.size());
  if (changes.empty()) {
    return;
  }

  KernelState* state = kernel_state();
  const uint32_t callback = state->xam_state()->content_register_callback;
  auto thread = object_ref<XHostThread>(new XHostThread(
      state, 64 * 1024, 0,
      [state, callback, changes]() {
        const uint32_t guest =
            state->memory()->SystemHeapAlloc(sizeof(XCONTENT_DATA_INTERNAL));
        if (!guest) {
          return 0;
        }
        for (const auto& [change, item] : changes) {
          if (state->title_id() != kDashboardID ||
              state->xam_state()->content_register_callback != callback) {
            break;
          }
          std::memcpy(state->memory()->TranslateVirtual(guest), &item,
                      sizeof(item));
          uint64_t args[] = {change, guest};
          state->processor()->Execute(
              XThread::GetCurrentThread()->thread_state(), callback, args,
              xe::countof(args));
          XELOGW("[mygames] dashboard told: {} '{}' ({:08X} type {:08X})",
                 change ? "removed" : "added", item.file_name(),
                 item.title_id.get(), uint32_t(item.content_type.get()));
        }
        state->memory()->SystemHeapFree(guest);
        xe::threading::Sleep(std::chrono::milliseconds(250));
        if (state->title_id() == kDashboardID) {
          state->BroadcastNotification(kXNotificationLiveContentInstalled, 0);
          RequestGameLibraryRebuild();
        }
        return 0;
      },
      state->GetTitleProcess()));
  thread->set_can_debugger_suspend(true);
  thread->set_name("Content Change Callback");
  thread->Create();
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

  const XContentType content_type_enum =
      static_cast<XContentType>(content_type.value());

  // Not sure if that's correct. In dashboard 1888 allocates memory for
  // XCONTENT_DATA_AGGREGATE struct, but in later calls use
  // XCONTENT_DATA_INTERNAL. The only difference is content_type, so let's use
  // it for now.
  const size_t data_size = content_type_enum == XContentType::kAll
                               ? sizeof(XCONTENT_DATA_INTERNAL)
                               : sizeof(XCONTENT_DATA_AGGREGATE);

  auto e = object_ref<ContentEnumerator>(
      new ContentEnumerator(kernel_state(), 1, data_size));
  X_KENUMERATOR_CONTENT_AGGREGATE* extra;
  auto result = e->Initialize(XUserIndexAny, 0xFE, 0x2000E, 0x20010, 0, &extra);
  if (XFAILED(result)) {
    return result;
  }

  extra->magic = kXObjSignature;
  extra->handle = e->handle();

  if (!device_info || device_info->device_type == DeviceType::HDD) {
    // Title id 0 means every title, not the running one - the dashboard asks
    // that way for My Games.
    std::vector<uint32_t> title_ids;
    if (title_id) {
      title_ids.push_back(title_id.value());
      // Fetch any alternate title IDs defined in the XEX header
      // (used by games to load saves from other titles, etc)
      auto exe_module = kernel_state()->GetExecutableModule();
      if (exe_module && exe_module->xex_module()) {
        const auto& alt_ids =
            exe_module->xex_module()->opt_alternate_title_ids();
        std::ranges::copy(alt_ids, std::back_inserter(title_ids));
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

    // Profiles are reported by AppendProfileItems below, with the gamertag and
    // size Storage shows. ListContent's own profile records would repeat them.
    const bool append_profiles = content_type_enum == XContentType::kProfile ||
                                 content_type_enum == XContentType::kInvalid ||
                                 content_type_enum == XContentType::kAll;
    if (content_type_enum == XContentType::kProfile) {
      title_ids.clear();
    }

    // One record per package. A title can reach the same package more than
    // once - the dashboard's own id widens to every common title, and a title
    // update's content root is walked as well as the global tree.
    SeenItems seen;
    const ItemSink add = [&e](const XCONTENT_DATA_INTERNAL& item) {
      e->AppendItem(item);
    };

    for (const auto& title_id : title_ids) {
      AppendTitleItems(xuid == -1 ? 0 : static_cast<uint64_t>(xuid), title_id,
                       content_type_enum, append_profiles, &seen, add);
    }

    // XNA titles live in their own library, not the content tree, so nothing
    // above finds them. They are the Indie Player's marketplace content, which
    // is what a query for that type, or for every type, asks for.
    if ((!title_id || title_id == kXN_2002) &&
        (content_type_enum == XContentType::kInvalid ||
         content_type_enum == XContentType::kMarketplaceContent ||
         content_type.value() == 0xFFFFFFFF)) {
      AppendXnaLibraryItems(add, &seen);
    }

    if (content_type_enum == XContentType::kProfile ||
        content_type_enum == XContentType::kInvalid ||
        content_type.value() == 0xFFFFFFFF) {
      AppendProfileItems(add);
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
