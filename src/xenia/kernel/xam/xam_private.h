/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_PRIVATE_H_
#define XENIA_KERNEL_XAM_XAM_PRIVATE_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/xam/xam_ordinals.h"

namespace xe {
namespace kernel {
namespace xam {

xe::cpu::Export* RegisterExport_xam(xe::cpu::Export* export_entry);

void RecordLaunchOrigin();
void ReloadForLaunch();
void CloseOpenedContentFiles();

// Builds the 0x10C launch blob the Avatar Editor expects, in the exact shape
// the real XamLaunchAvatarEditor produces: 'EPIX' then the item string at
// +0x0C. Defined in xam_info.cc, used there and by XamLaunchAvatarEditor.
std::vector<uint8_t> BuildAvatarEditorLaunchData(const std::u16string& item);

void RegisterIssuedToken(uint32_t token_ptr, uint32_t size);

// The title's boxartlg.jpg from the Xbox image server, cached on disk once
// fetched. Empty when there is none, which callers fall through on. Defined in
// xam_xhttp.cc beside the disc tray's use of it.
std::vector<uint8_t> FetchTitleBoxArt(uint32_t title_id);

// The title's official retail cover from XboxUnity - the whole 900x600 case
// jacket, front on the right - cached on disk. Empty when there is none.
std::vector<uint8_t> FetchTitleCoverJacket(uint32_t title_id);

// When the dashboard opens an arcade package's title image (ArcadeInfo.xml's
// <TitleInfo ImagePath>), the guest path of that title's box art to open
// instead; empty to open the package's own file. Defined in xam_content.cc.
std::string DashboardTitleImageRedirect(const std::string& guest_path);

// When the dashboard opens a channel package (.xzp) the hub publishes, the
// \Device\NexiaDashModules path to open instead (fetched first when stale);
// empty for every other file. Defined in xam_info.cc.
std::string DashChannelPackageRedirect(const std::string& guest_path);
// For the dashboard's hub apps (Dash.NexiaAddFriend/Messages/Voice.lex),
// whose Lua has no API for these: xam_xhttp.cc answers their local
// /nexia/... routes with them. Defined in xam_ui.cc.
//
// Opens the gamercard of any XUID the way XamShowGamerCardUIForXUID does.
// Returns an X_ERROR_* code.
uint32_t ShowHubGamerCard(uint32_t user_index, uint64_t xuid);
// Opens the on-screen keyboard without blocking; 0 when it cannot open (a
// dialog is already up). Poll the ticket: false while it is still open.
uint32_t OpenHubKeyboard(const std::string& title, const std::string& text);
bool PollHubKeyboard(uint32_t ticket, bool* out_cancelled,
                     std::string* out_text);
// Add Friend, as the Friends dialog adds: 0 added, 1 waiting for approval,
// 2 failed.
uint32_t RequestHubFriend(uint32_t user_index, uint64_t target_xuid,
                          const std::string& gamertag = {});
// Remove a friend, as the Friends dialog removes (and republishes the list).
bool RemoveHubFriend(uint32_t user_index, uint64_t target_xuid);

// Join Game for the hub apps: whether a friend's game (title, and the title
// update they run - its label and content hash, both "" for none) is one this
// machine can launch the same way.
struct HubJoinTarget {
  bool ok = false;
  std::string path;       // the played.db copy to launch
  std::string title_name;
  std::string update_id;  // library id to make active (NO_TU for none)
  std::string reason;     // why not, for the app to show
};
HubJoinTarget FindHubJoinTarget(uint32_t title_id, const std::string& update,
                                const std::string& hash);
// Activates the update, launches the game, and leaves the friend's session to
// be accepted once the game listens. False when it cannot start.
bool StartHubJoin(uint64_t inviter_xuid, uint32_t title_id,
                  const std::string& update, const std::string& hash);
// The join waiting for this title, taken once (KernelState hands it to the
// game's first Live listener as an accepted invite).
bool TakePendingHubJoin(uint32_t title_id, uint64_t* out_inviter);
// This profile's friends as Nexia keeps them (the list the Friends dialog
// shows), for the hub apps: (xuid, gamertag) pairs.
std::vector<std::pair<uint64_t, std::string>> HubFriends(uint32_t user_index);

// Registration functions, one per file.
#define XE_MODULE_EXPORT_GROUP(m, n)                                  \
  void Register##n##Exports(xe::cpu::ExportResolver* export_resolver, \
                            KernelState* kernel_state);
#include "xam_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_PRIVATE_H_
