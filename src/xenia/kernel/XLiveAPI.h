/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XLIVEAPI_H_
#define XENIA_KERNEL_XLIVEAPI_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_set>

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/upnp.h"
#include "xenia/kernel/util/net_utils.h"
#include "xenia/kernel/util/nexia_transport.h"
#include "xenia/kernel/util/xuid_handles.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xsession.h"
#include "xenia/ui/imgui_drawer.h"

#include "xenia/kernel/json/arbitration_object_json.h"
#include "xenia/kernel/json/delete_my_profiles_json.h"
#include "xenia/kernel/json/find_users_object_json.h"
#include "xenia/kernel/json/friend_presence_object_json.h"
#include "xenia/kernel/json/getusersettings_object_json.h"
#include "xenia/kernel/json/http_response_object_json.h"
#include "xenia/kernel/json/leaderboard_object_json.h"
#include "xenia/kernel/json/page_gamerpics_object_json.h"
#include "xenia/kernel/json/player_object_json.h"
#include "xenia/kernel/json/presence_object_json.h"
#include "xenia/kernel/json/properties_object_json.h"
#include "xenia/kernel/json/read_user_stats_object_json.h"
#include "xenia/kernel/json/services_json.h"
#include "xenia/kernel/json/session_object_json.h"
#include "xenia/kernel/json/setusersettings_object_json.h"
#include "xenia/kernel/json/title_gamerpics_object_json.h"
#include "xenia/kernel/json/xstorage_file_info_object_json.h"

#ifdef XE_PLATFORM_WIN32
#include <iphlpapi.h>
#endif  // XE_PLATFORM_WIN32

namespace xe {

// Settings must maintain order.
using user_settingids_map =
    std::map<uint64_t,
             std::map<uint32_t, std::vector<kernel::xam::UserSettingId>>>;

// Settings must maintain order.
using user_settings_map =
    std::map<uint64_t,
             std::map<uint32_t, std::vector<kernel::xam::UserSetting>>>;

using gamerpics_pair = std::pair<std::vector<uint8_t>, std::vector<uint8_t>>;

namespace kernel {

// One row of the hub's player browser: everyone the hub has seen recently.
// Player records carry a one-day lifetime server side, so this list is by
// definition "recently active".
struct HubPlayer {
  uint64_t xuid = 0;
  std::string gamertag;
  uint32_t title_id = 0;
  uint32_t state = 0;
  std::string rich_presence;
  std::string gamerpic_url;
};

// One message in the inbox. Text and voice share the envelope; `voice` says
// which payload it carries. The voice samples are NOT here - a listing would
// drag every recording across the wire - they are fetched per message when
// one is played.
struct HubMessage {
  std::string id;
  uint64_t from_xuid = 0;
  std::string from_name;
  bool voice = false;
  bool read = false;
  std::string text;
  uint32_t duration_ms = 0;
  std::string created_at;
};

// What the notification loop watches: how many of each are waiting unread.
struct MessageCounts {
  uint32_t text = 0;
  uint32_t voice = 0;

  uint32_t total() const { return text + voice; }
};

// Why a send did not happen. 'Denied' means the hub refused it: messages only
// travel between friends and players from the last 48 hours, and that rule is
// enforced there rather than here.
enum class SendMessageOutcome {
  kSent,
  kDenied,
  kInvalid,
};

class XLiveAPI {
 public:
  XLiveAPI();

  ~XLiveAPI();

  enum class InitState { Success, Failed, Pending };

  void PrintLibcurlDetails();

  static void IpGetConsoleXnAddr(XNADDR* XnAddr_ptr);

  static void GetXnAddrFromSessionObject(SessionObjectJSON session,
                                         XNADDR* XnAddr_ptr);

  std::vector<std::string> ParseAPIList() const;

  void AddAPIAddress(std::string address) const;

  void RemoveAPIAddress(std::string api_address) const;

  void SetAPIAddress(std::string address);

  void BroadcastNetworkStatus() const;

  void SetNetworkMode(uint32_t mode) const;

  bool SelectNetworkMode(uint32_t mode);

  void SetLogging(bool state) const;

  void SetXHttp(bool state) const;

  void SetBindInterface(bool state) const;

  void SetNexiaHubTransport(bool state);
  void SetNexiaHubTransportTcpFallback(bool state) const;

  // Brings up the Nexia Hub Transport relay. No-op unless
  // nexiahub_transport is enabled.
  bool StartTransport();

  NexiaTransport* transport() { return &transport_; }

  static std::string GetApiAddress();

  static std::string BuildEndpoint(std::string endpoint);

  void Init();

  // Asks for consent, then (if given) adds the inbound firewall rules behind
  // a UAC prompt. No-op when there is no window to ask in.
  void RequestFirewallSetup();

  NETWORK_MODE RefreshNetworkMode(bool lan_limit);

  InitState GetInitState() const;

  uint32_t GetNatType() const;

  bool IsConnectedToServer() const;

  uint16_t GetPlayerPort() const;

  // Nexia: the local (pre-UPnP) port this instance reserved on the hub.
  uint16_t GetLocalPlayerPort() const { return player_port_; }
  void SetPlayerPort(uint16_t port) { player_port_ = port; }

  // Nexia: hub capability negotiation. Runs once a mode reaches Success.
  void ProbeServerCapabilities();

  // Nexia: hub-arbitrated port reservation so two instances behind one IP
  // (or on one machine) never collide. owner is a per-process instance id.
  bool ReservePort(const std::string& host_address, uint16_t port,
                   const std::string& owner);
  uint16_t AllocateHostPort(uint16_t base_port = 20000);
  uint64_t GetInstanceId();
  void ReleaseReservedPorts();

  // Nexia: identity lookup by XUID instead of by (shared) IP.
  std::unique_ptr<PlayerObjectJSON> FindPlayerByXuid(uint64_t xuid);

  // Nexia: delete only this profile's sessions (legacy delete was by IP and
  // wiped same-IP peers' sessions).
  void DeleteMySessions();

  int8_t GetVersionStatus() const;

  void clearXnaddrCache();

  void StartWhoamiAsync();

  sockaddr_in Getwhoami();

  void DownloadPortMappings();

  std::unique_ptr<HTTPResponseObjectJSON> RegisterPlayer(const uint64_t xuid);

  const std::map<uint64_t, std::string> DeleteMyProfiles();

  std::unique_ptr<PlayerObjectJSON> FindPlayer(std::string ip);

  bool UpdateQoSCache(const uint64_t sessionId,
                      const std::vector<uint8_t> qos_payloade);

  void QoSPost(uint64_t sessionId, uint8_t* qosData, size_t qosLength);

  // Queues a QoS payload for upload on the single QoS worker instead of
  // spawning a thread per call. Titles re-arm their QoS blob constantly, and
  // each upload is a blocking HTTP round trip - one detached thread per
  // change piles up dozens of concurrent posts and stalls the game for every
  // player already connected. Only the newest payload per session is kept;
  // anything it supersedes was already stale.
  void QoSPostAsync(uint64_t sessionId, std::vector<uint8_t> qosData);

  response_data QoSGet(uint64_t sessionId);

  void SessionModify(uint64_t sessionId, XGI_SESSION_MODIFY* data);

  std::vector<std::unique_ptr<SessionObjectJSON>> GetTitleSessions(
      uint32_t title_id = 0);

  // Player browser. Empty search = recently active, newest first. A non-empty
  // search matches a gamertag substring, case-insensitively.
  std::vector<HubPlayer> GetHubPlayers(const std::string& search = "",
                                       uint32_t limit = 0);

  std::future<std::vector<HubPlayer>> GetHubPlayersAsync(
      const std::string& search = "", uint32_t limit = 0);

  // Who may add this player as a friend.
  enum class FriendPrivacy : uint32_t {
    kAnyone = 0,
    kFriendsOfFriends = 1,
    kApproval = 2,
  };

  // What came back from asking to add someone. The hub decides this from the
  // target's own setting, so it cannot be bypassed by a modified client.
  enum class FriendRequestOutcome {
    kAccepted,
    kPending,
  };

  // Publishes our setting and, so the hub can judge a friend-of-friends
  // request, our friend XUIDs. The hub keeps no friend graph of its own.
  void PublishFriendPrivacy(uint64_t xuid, FriendPrivacy privacy,
                            const std::vector<uint64_t>& friends);

  FriendPrivacy GetFriendPrivacy(uint64_t xuid);

  FriendRequestOutcome SendFriendRequest(uint64_t from_xuid, uint64_t to_xuid);

  // Requests waiting for this player to approve or decline.
  std::vector<HubPlayer> GetIncomingFriendRequests(uint64_t xuid);

  std::future<std::vector<HubPlayer>> GetIncomingFriendRequestsAsync(
      uint64_t xuid);

  std::future<std::vector<uint64_t>> DrainFriendApprovalsAsync(uint64_t xuid);

  void RespondToFriendRequest(uint64_t xuid, uint64_t from_xuid, bool approve);

  // Approvals addressed to us. Reading consumes them, so each is acted on once.
  std::vector<uint64_t> DrainFriendApprovals(uint64_t xuid);

  void BlockPlayer(uint64_t xuid, uint64_t target_xuid, bool block = true);

  // Everyone this player has blocked, named so the list can show who they are.
  std::vector<HubPlayer> GetBlockedPlayers(uint64_t xuid);

  // Everyone this player shared a session with in the last 48 hours, newest
  // first. The window is the hub's, not ours.
  std::vector<HubPlayer> GetRecentPlayers(uint64_t xuid);

  std::future<std::vector<HubPlayer>> GetRecentPlayersAsync(uint64_t xuid);

  std::future<std::vector<HubPlayer>> GetBlockedPlayersAsync(uint64_t xuid);

  // ---- Offline identity cache -------------------------------------------
  // Every player the hub names for us is written to the SeenPlayers table, so
  // a list can still be shown - with names, and with faces where one was ever
  // downloaded - when the hub cannot be reached.
  void RememberPlayers(const std::vector<HubPlayer>& players);

  // The saved players, most recently seen first. Used in place of a hub list
  // when the request to the hub fails outright.
  std::vector<HubPlayer> PlayersFromCache(size_t limit = 0);

  // ---- Messages ---------------------------------------------------------
  // Text and voice mail between friends and recent players. The hub decides
  // who may send to whom; a refusal comes back as kDenied.
  SendMessageOutcome SendTextMessage(uint64_t from_xuid, uint64_t to_xuid,
                                     const std::string& text);

  // Voice mail as signed 16-bit mono PCM at sample_rate, sent as-is. No codec
  // on either end: playback hands the samples straight to the voice mixer.
  SendMessageOutcome SendVoiceMessage(uint64_t from_xuid, uint64_t to_xuid,
                                      const std::vector<int16_t>& pcm,
                                      uint32_t sample_rate);

  // kind is "text", "voice", or empty for both. Newest first.
  std::vector<HubMessage> GetMessages(uint64_t xuid, const std::string& kind);

  std::future<std::vector<HubMessage>> GetMessagesAsync(
      uint64_t xuid, const std::string& kind);

  // The samples of one voice message. False when it is gone or not ours.
  bool GetMessageAudio(uint64_t xuid, const std::string& id,
                       std::vector<int16_t>& pcm_out, uint32_t& sample_rate);

  // Unread counts - what the notification loop polls.
  MessageCounts GetMessageCounts(uint64_t xuid);

  void MarkMessageRead(uint64_t xuid, const std::string& id);

  void DeleteMessage(uint64_t xuid, const std::string& id);

  // The notification loop itself. Started once and left running: it polls the
  // unread counts on a timer so the Social menu and the fullscreen badge have
  // something to show without any UI being open. It resolves the signed-in
  // profile on every tick rather than being told once, so signing in or
  // switching profiles needs no hook anywhere. Reading the counts costs
  // nothing - they are plain atomics.
  void StartMessageNotifications();
  void StopMessageNotifications();

  // Raised on the poll thread whenever the counts actually change. Set it
  // before starting the loop; the callee is responsible for getting itself
  // onto whichever thread it needs.
  void SetMessageCountsCallback(std::function<void()> callback);

  // The profile the loop is currently watching, or 0 when signed out.
  uint64_t message_xuid() const {
    return watched_xuid_.load(std::memory_order_relaxed);
  }

  MessageCounts message_counts() const {
    MessageCounts counts;
    counts.text = unread_text_.load(std::memory_order_relaxed);
    counts.voice = unread_voice_.load(std::memory_order_relaxed);
    return counts;
  }

  // Bumped every time the counts change, so a consumer can tell "nothing new"
  // from "polled again" without comparing fields.
  uint32_t message_counts_revision() const {
    return counts_revision_.load(std::memory_order_relaxed);
  }

  // Called after the inbox is read so the badge clears immediately instead of
  // waiting out the poll interval.
  void RefreshMessageCounts();

  // "Later" on an approval prompt: the request stays on the hub undrained, but
  // is not shown again for the rest of this run. Deliberately session-scoped
  // and not persisted, so it comes back on the next launch.
  void DeferFriendRequest(uint64_t from_xuid) {
    deferred_friend_requests_.insert(from_xuid);
  }

  bool IsFriendRequestDeferred(uint64_t from_xuid) const {
    return deferred_friend_requests_.count(from_xuid) != 0;
  }

  const std::vector<std::unique_ptr<SessionObjectJSON>> SessionSearch(
      XGI_SESSION_SEARCH* data, uint32_t num_users);

  bool SessionPropertiesSet(uint64_t session_id, const uint64_t xuid);

  const std::vector<xam::Property> SessionPropertiesGet(uint64_t session_id);

  const std::unique_ptr<SessionObjectJSON> SessionDetails(uint64_t sessionId);

  std::unique_ptr<SessionObjectJSON> XSessionMigration(
      uint64_t sessionId, XGI_SESSION_MIGRATE* data);

  std::unique_ptr<ArbitrationObjectJSON> XSessionArbitration(
      uint64_t sessionId);

  bool SessionFlushStats(uint64_t sessionId,
                         view_properties_unordered_map stats);

  std::unique_ptr<LeaderboardObjectJSON> LeaderboardsFind(
      const XGI_XUSER_READ_STATS stats);

  void DeleteSession(uint64_t sessionId);

  void DeleteAllSessionsByMac();

  void DeleteAllSessions();

  void XSessionCreate(uint64_t sessionId, XGI_SESSION_CREATE* data);

  SessionObjectJSON XSessionGet(uint64_t sessionId);

  std::vector<X_TITLE_SERVER> GetServers();

  std::unique_ptr<ServicesObjectJSON> GetServices();

  bool Heartbeat() const;

  void SessionJoinRemote(uint64_t sessionId,
                         const std::unordered_map<uint64_t, bool> members);

  void SessionLeaveRemote(uint64_t sessionId,
                          const std::vector<xe::be<uint64_t>> xuids);

  void SessionPreJoin(uint64_t sessionId, const std::set<uint64_t>& xuids);

  // A game invite in transit. The hub stores one of these per invitee until
  // that player's client picks it up.
  struct InviteRecord {
    uint64_t inviter_xuid = 0;
    uint64_t session_id = 0;
    uint32_t title_id = 0;
  };

  // Queue an invite on the hub for each invitee (online xuids).
  bool InviteSend(uint64_t inviter_xuid, const std::set<uint64_t>& invitees,
                  uint64_t session_id);

  // DESTRUCTIVE READ: the hub clears whatever it returns, so an invite is
  // delivered exactly once and nothing stays persisted after it arrives.
  std::vector<InviteRecord> InviteDrain(uint64_t invitee_xuid);

  std::unique_ptr<FriendsPresenceObjectJSON> GetFriendsPresence(
      const std::set<uint64_t>& xuids);

  X_STORAGE_BUILD_SERVER_PATH_RESULT XStorageBuildServerPath(
      std::string server_path);

  bool XStorageDelete(std::string server_path);

  std::vector<uint8_t> XStorageDownload(std::string server_path);

  X_STORAGE_UPLOAD_RESULT XStorageUpload(std::string server_path,
                                         std::span<uint8_t> buffer);

  std::pair<std::unique_ptr<XStorageFilesInfoObjectJSON>, bool>
  XStorageEnumerate(std::string server_path, uint32_t max_items);

  std::unique_ptr<FindUsersObjectJSON> GetFindUsers(
      const std::vector<FIND_USER_INFO>& find_users_info);

  PresenceObjectJSON BuildRichPresenceRequest(const std::set<uint64_t> xuids);

  void SetPresence(const std::set<uint64_t> xuids);

  bool SetUsersSettings(user_settingids_map settings);

  user_settings_map GetUsersSettings(user_settingids_map settings);

  std::vector<uint8_t> GetUserGamerpicTile(uint64_t xuid, bool small_tile);

  TitleGamerpicsObjectJSON GetTitleGamerpic(uint32_t title_id);

  std::set<uint32_t> GetSupportedGamerpicTitles();

  std::optional<PageGamerpicsObjectJSON> GetGamerpicPage(
      uint32_t page, uint32_t per_page, std::string type_query);

  std::map<uint32_t, std::vector<uint8_t>> GetMultiGameInfo(
      std::unordered_map<uint32_t, std::string> images_data);

  std::map<uint32_t, std::vector<uint8_t>> GetMultiGamerpics(
      std::vector<std::string> cdn_parts);

  std::vector<uint8_t> DownloadGamerpicTile(const uint32_t title_id,
                                            const uint32_t tile_id);

  std::future<std::vector<uint8_t>> DownloadGamerpicTileAsync(uint32_t title_id,
                                                              uint32_t tile_id);

  std::shared_future<gamerpics_pair> DownloadCompleteGamerpic(
      xam::GamerPictureKey gamerpic_key);

  std::map<uint64_t, std::vector<uint8_t>> GetMultiGamerpicsFromXUIDs(
      std::set<uint64_t> xuids, bool fsmall = false);

  std::vector<uint8_t> DownloadRandomGamerpic();

  std::future<std::map<uint64_t, std::shared_ptr<xe::ui::ImmediateTexture>>>
  GetFriendsGamerpicsAsync(const uint64_t xuid, ui::ImGuiDrawer* imgui_drawer);

  std::unique_ptr<HTTPResponseObjectJSON> PraseResponse(response_data response);

  // Always goes through the transport substitution, because the XUID may not be
  // known yet when Init() runs - and every caller must agree on one address or
  // peers cache one value and receive another.
  sockaddr_in OnlineIP() const {
    sockaddr_in ip = online_ip_;
    ip.sin_addr.s_addr = EffectiveOnlineAddr(ip.sin_addr.s_addr);
    return ip;
  };

  std::string OnlineIP_str() const {
    return ip_to_string(OnlineIP().sin_addr);
  };

  std::string GetDefaultLocalServer() const { return default_local_server_; };

  std::string GetDefaultPublicServer() const { return default_public_server_; };

  bool IsXUIDMismatched() const { return xuid_mismatch_; };

  void SetXUIDMismatch(bool state) { xuid_mismatch_ = state; };

  uint32_t GetDummyFriendsCount() const { return dummy_friends_count_; };

  void SetDummyFriendsCount(const uint32_t count) {
    dummy_friends_count_ = count;
  };

  void AddCachedGamerpic(uint32_t id, std::vector<uint8_t> data) {
    cached_gamerpics[id] = data;
  };

  std::optional<std::vector<uint8_t>> GetCachedGamerpic(uint32_t gamerpic_id) {
    if (cached_gamerpics.contains(gamerpic_id)) {
      return cached_gamerpics.at(gamerpic_id);
    }

    return std::nullopt;
  };

  void SetSystemlinkID(const uint64_t systemlink_xnkid) {
    systemlink_id_ = systemlink_xnkid;
  };

  uint64_t GetSystemlinkID() const { return systemlink_id_; };

  inline static std::map<uint32_t, uint64_t> sessionIdCache = {};
  inline static std::map<uint32_t, uint64_t> macAddressCache = {};

  // --- Nexia in-packet XUID identity tag (VDP) -----------------------------
  // Bump when the on-wire tag format changes. Advertised to the hub via
  // POST /players/clientVersion and compared against peers' advertised
  // versions.
  static constexpr uint32_t kNexiaNetProtocolVersion = 1;

  // Set true only when the hub advertises the "xuidTag" capability (GET
  // /capabilities). Old hubs 404 the probe -> stays false -> feature fully off.
  inline static bool server_supports_tag = false;

  // Set true when the hub advertises "deleteMySessions" (xuid-scoped session
  // delete). Old hubs stay false -> we fall back to the legacy IP-based
  // DeleteAllSessions().
  inline static bool server_supports_delete_my_sessions = false;

  // Set true when the hub advertises "hostXuidDelete": it will authorise
  // session deletes against the host XUID we send. Old hubs stay false -> we
  // send no xuid and they keep using the legacy IP check.
  inline static bool server_supports_host_xuid_delete = false;

  // Our own online XUID, cached at register time so the socket send path can
  // stamp outgoing VDP packets without a profile lookup.
  inline static uint64_t local_online_xuid = 0;

  // Per-peer capability, keyed on the UNIQUE XUID (never the IP, since two
  // consoles can share one public IP). Populated from a peer's advertised
  // clientVersion during resolution.
  inline static std::map<uint64_t, bool> peer_supports_tag = {};

  // Resolved peer online-IP(network order) -> XUID. Feeds the send gate
  // (which only knows a destination IP) and the port-less XnAddr resolver.
  inline static std::map<uint32_t, uint64_t> ip_to_xuid = {};

  // Peer online-IP(be) -> the port the peer told us to reach it on, learned
  // from its VDP tag. This is how a host learns a client's mapped port (the
  // client can't be found by the shared public IP alone).
  inline static std::map<uint32_t, uint16_t> packet_port_cache = {};

  // Hub ports this instance has reserved, released on shutdown.
  inline static std::unordered_set<uint16_t> reserved_ports_ = {};

  // Handle <-> XUID lives in util/xuid_handles.h; see there for why every XUID
  // we learn has to be registered.
  static uint32_t SyntheticOnlineIP(uint64_t xuid) {
    return XuidToHandle(xuid);
  }

  static uint32_t RegisterXuidHandle(uint64_t xuid) {
    xe::kernel::RegisterXuidHandle(xuid);
    return XuidToHandle(xuid);
  }

  static uint64_t XuidForHandle(uint32_t handle) {
    return xe::kernel::XuidForHandle(handle);
  }

  // Host of the session we are in, 0 when we are the host or not in one. Used
  // as the routing fallback for a relayed send whose handle does not resolve.
  inline static uint64_t session_host_xuid = 0;

  static void SetSessionHostXuid(uint64_t xuid) {
    if (!xuid || xuid == local_online_xuid) {
      return;
    }
    RegisterXuidHandle(xuid);
    session_host_xuid = xuid;
  }

  // The address this instance should present, given a real one.
  static uint32_t EffectiveOnlineAddr(uint32_t real_ip_be);

  // Record identity + reachable port carried by a received VDP tag. A peer that
  // tags us is, by definition, capable, so mark it so.
  static void CachePacketXuid(uint32_t ip_be, uint16_t advertised_port,
                              uint64_t xuid) {
    ip_to_xuid[ip_be] = xuid;
    peer_supports_tag[xuid] = true;
    packet_port_cache[ip_be] = advertised_port;
    RegisterXuidHandle(xuid);
  }

  // True only when we've positively confirmed the peer at this destination IP
  // can strip our tag. Unknown/old/unresolved -> false -> send plain.
  static bool PeerSupportsTag(uint32_t dest_ip_be) {
    if (!server_supports_tag) {
      return false;
    }
    auto ix = ip_to_xuid.find(dest_ip_be);
    if (ix == ip_to_xuid.end()) {
      return false;
    }
    auto cx = peer_supports_tag.find(ix->second);
    return cx != peer_supports_tag.end() && cx->second;
  }

 private:
  // Friend requests the player answered with "Later". Held in memory only and
  // never written anywhere, so they are shown again on the next launch. The
  // hub still has them - "Later" answers nothing.
  std::unordered_set<uint64_t> deferred_friend_requests_;

  // The LOCAL bound port of the player (VDP) socket. Set from the actual bind
  // in XSocket::Bind - never hardcoded. ports.json (DownloadPortMappings)
  // supplies the guest->external mapping that GetPlayerPort() resolves this
  // through; it does not identify which port is the player socket, so the bind
  // is the only ground truth. 0 until the guest has bound.
  uint16_t player_port_ = 0;

  const std::string default_local_server_ = "192.168.0.1:36000/";

  const std::string default_public_server_ =
      "https://xenia-netplay-2a0298c0e3f4.herokuapp.com/";

  sockaddr_in online_ip_ = {};

  InitState initialized_ = InitState::Pending;

  bool xuid_mismatch_ = false;

  int8_t version_status_ = 0;

  bool xlsp_servers_cached_ = false;

  std::vector<X_TITLE_SERVER> xlsp_servers_ = {};

  uint64_t systemlink_id_ = 0;

  uint32_t dummy_friends_count_ = 0;

  std::map<uint64_t, std::vector<uint8_t>> qos_payload_cache_ = {};

  // Relay client; see nexia_transport.h.
  NexiaTransport transport_;

  // Single QoS upload worker. Pending posts are keyed by session so a newer
  // payload replaces an unsent older one rather than queueing behind it -
  // uploading a superseded blob costs a round trip and tells the hub nothing.
  std::map<uint64_t, std::vector<uint8_t>> qos_pending_ = {};
  std::mutex qos_mutex_;
  std::condition_variable qos_cv_;
  std::thread qos_thread_;
  bool qos_thread_started_ = false;
  bool qos_thread_stopping_ = false;

  void QoSWorkerMain();
  void StopQoSWorker();

  std::future<sockaddr_in> whoami_result_;

  // ---- Message notification loop ----------------------------------------
  // One thread, sleeping on a condition variable so a shutdown or a manual
  // refresh does not wait out the interval.
  void MessageNotificationMain();

  std::thread message_poll_thread_;
  std::mutex message_poll_mutex_;
  std::condition_variable message_poll_cv_;
  bool message_poll_running_ = false;
  bool message_poll_wake_ = false;
  std::function<void()> counts_callback_;

  std::atomic<uint64_t> watched_xuid_{0};
  std::atomic<uint32_t> unread_text_{0};
  std::atomic<uint32_t> unread_voice_{0};
  std::atomic<uint32_t> counts_revision_{0};

  std::map<uint32_t, std::vector<uint8_t>> cached_gamerpics = {};

  std::unique_ptr<HTTPResponseObjectJSON> Get(const std::string endpoint,
                                              const uint32_t timeout = 0);

  std::unique_ptr<HTTPResponseObjectJSON> Post(const std::string endpoint,
                                               const uint8_t* data,
                                               size_t data_size = 0);

  std::unique_ptr<HTTPResponseObjectJSON> Delete(const std::string endpoint);

  std::vector<HTTPResponseObjectJSON> GetMulti(
      std::vector<std::string> urls, const uint32_t per_request_timeout = 0);

  // https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION.html
  static size_t callback(void* data, size_t size, size_t nmemb, void* clientp) {
    size_t realsize = size * nmemb;
    struct response_data* mem = (struct response_data*)clientp;

    char* ptr = (char*)realloc(mem->response, mem->size + realsize + 1);
    if (ptr == NULL) {
      return 0; /* out of memory! */
    }

    mem->response = ptr;
    memcpy(&(mem->response[mem->size]), data, realsize);
    mem->size += realsize;
    mem->response[mem->size] = 0;

    return realsize;
  };
};
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XLIVEAPI_H_
