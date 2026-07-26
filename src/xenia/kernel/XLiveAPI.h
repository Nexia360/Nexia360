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

#include <future>
#include <span>
#include <unordered_set>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/upnp.h"
#include "xenia/kernel/util/net_utils.h"
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

  static std::string GetApiAddress();

  static std::string BuildEndpoint(std::string endpoint);

  void Init();

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

  response_data QoSGet(uint64_t sessionId);

  void SessionModify(uint64_t sessionId, XGI_SESSION_MODIFY* data);

  std::vector<std::unique_ptr<SessionObjectJSON>> GetTitleSessions(
      uint32_t title_id = 0);

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

  sockaddr_in OnlineIP() const { return online_ip_; };

  std::string OnlineIP_str() const { return ip_to_string(online_ip_); };

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

  // Record identity + reachable port carried by a received VDP tag. A peer that
  // tags us is, by definition, capable, so mark it so.
  static void CachePacketXuid(uint32_t ip_be, uint16_t advertised_port,
                              uint64_t xuid) {
    ip_to_xuid[ip_be] = xuid;
    peer_supports_tag[xuid] = true;
    packet_port_cache[ip_be] = advertised_port;
  }

  // True only when we've positively confirmed the peer at this destination IP
  // can strip our tag. Unknown/old/unresolved -> false -> send plain.
  static bool PeerSupportsTag(uint32_t dest_ip_be) {
    if (!server_supports_tag) return false;
    auto ix = ip_to_xuid.find(dest_ip_be);
    if (ix == ip_to_xuid.end()) return false;
    auto cx = peer_supports_tag.find(ix->second);
    return cx != peer_supports_tag.end() && cx->second;
  }

 private:
  // Nexia: local reserved port (pre-UPnP). Was hardcoded 36000.
  uint16_t player_port_ = 36000;

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

  std::future<sockaddr_in> whoami_result_;

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
