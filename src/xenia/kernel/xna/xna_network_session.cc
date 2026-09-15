/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_network_session.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xnet.h"
#include "xenia/kernel/xsession.h"
#include "xenia/kernel/xsocket.h"

namespace xe {
namespace kernel {
namespace xna {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kTypeSystemLink = 1;
constexpr uint32_t kTypePlayerMatch = 2;
constexpr uint32_t kTypeRanked = 3;
constexpr uint32_t kStateLobby = 0;
constexpr uint32_t kStatePlaying = 1;
constexpr uint32_t kStateEnded = 2;
constexpr uint32_t kEndHostEnded = 1;
constexpr uint32_t kEndRemovedByHost = 2;
constexpr uint32_t kEndDisconnected = 3;
constexpr uint32_t kEventStateChanged = 1;
constexpr uint32_t kEventGamerJoined = 2;
constexpr uint32_t kEventGamerLeft = 3;
constexpr uint32_t kEventReceivedData = 4;
constexpr uint32_t kEventSessionInfo = 5;
constexpr uint32_t kGamerHost = 1;
constexpr uint32_t kGamerLocal = 2;
constexpr uint32_t kGamerReady = 8;
constexpr uint32_t kRecordSendData = 1;
constexpr uint32_t kRecordEnableVoice = 2;
constexpr uint32_t kRecordSetReady = 3;
constexpr uint32_t kRecordStartGame = 4;
constexpr uint32_t kRecordEndGame = 5;
constexpr uint32_t kRecordResetReady = 6;
constexpr uint32_t kRecordRemoveMachine = 7;
constexpr uint32_t kRecordSetSlots = 8;
constexpr uint32_t kRecordAllowJoin = 9;
constexpr uint32_t kRecordAllowMigration = 10;
constexpr uint32_t kRecordProperty = 11;
constexpr uint32_t kRecordsOffset = 4;
constexpr uint32_t kAllRecipients = 0xFFFFFFFFu;
constexpr uint32_t kNewMachine = 0xFFFFFFFFu;
constexpr uint32_t kDepartedIndex = 0x80000000u;
constexpr uint32_t kLocalSlots = 4;
constexpr uint32_t kPropertySlots = 8;
constexpr uint32_t kGrowBuffer = 0x80040200u;
constexpr uint32_t kFailed = 0x80004005u;
constexpr uint32_t kFionbio = 0x8004667Eu;
constexpr uint32_t kRejectFull = 1;
constexpr uint32_t kRejectClosed = 2;
constexpr uint16_t kSessionPort = 1000;
constexpr uint32_t kWireMagic = 0x53414E58u;
constexpr uint8_t kAllGamers = 0xFF;
constexpr uint8_t kHostMachine = 0;
constexpr uint8_t kNoMachine = 0xFF;
constexpr size_t kMaxDatagram = 65536;
constexpr size_t kMaxFound = 32;
constexpr size_t kMaxTag = 64;
constexpr auto kPingInterval = std::chrono::seconds(1);
constexpr auto kPeerTimeout = std::chrono::seconds(10);
constexpr auto kJoinTimeout = std::chrono::seconds(8);
constexpr auto kJoinRetry = std::chrono::milliseconds(250);
constexpr auto kJoinPoll = std::chrono::milliseconds(10);

enum class Wire : uint8_t {
  kJoin = 1,
  kWelcome,
  kReject,
  kGamerJoined,
  kGamerLeft,
  kData,
  kState,
  kReady,
  kInfo,
  kLeave,
  kEnd,
  kPing,
};

struct EventWriter {
  std::vector<uint8_t> bytes;

  void Word(uint32_t value) {
    const size_t at = bytes.size();
    bytes.resize(at + sizeof(value));
    std::memcpy(bytes.data() + at, &value, sizeof(value));
  }
  void Data(const uint8_t* data, size_t size) {
    const size_t at = bytes.size();
    bytes.resize(at + ((size + 3) & ~size_t(3)), 0);
    if (size) {
      std::memcpy(bytes.data() + at, data, size);
    }
  }
  void String(const std::string& text) {
    const std::u16string wide = xe::to_utf16(text);
    std::vector<uint8_t> raw((wide.size() + 1) * 2, 0);
    std::memcpy(raw.data(), wide.data(), wide.size() * 2);
    Data(raw.data(), raw.size());
  }
};

struct Bytes {
  std::vector<uint8_t> data;

  void Raw(const void* source, size_t size) {
    const auto* begin = static_cast<const uint8_t*>(source);
    data.insert(data.end(), begin, begin + size);
  }
  void U8(uint8_t value) { data.push_back(value); }
  void U32(uint32_t value) { Raw(&value, sizeof(value)); }
  void U64(uint64_t value) { Raw(&value, sizeof(value)); }
  void Text(const std::string& text) {
    const uint16_t size =
        static_cast<uint16_t>((std::min)(text.size(), kMaxTag));
    Raw(&size, sizeof(size));
    Raw(text.data(), size);
  }
};

struct Reader {
  const uint8_t* at;
  const uint8_t* end;
  bool ok = true;

  const uint8_t* Take(size_t size) {
    if (!ok || static_cast<size_t>(end - at) < size) {
      ok = false;
      return nullptr;
    }
    const uint8_t* taken = at;
    at += size;
    return taken;
  }
  template <typename T>
  T Get() {
    T value{};
    if (const uint8_t* source = Take(sizeof(T))) {
      std::memcpy(&value, source, sizeof(T));
    }
    return value;
  }
  std::string Text() {
    const uint16_t size = Get<uint16_t>();
    const uint8_t* source = Take(size);
    return source ? std::string(reinterpret_cast<const char*>(source), size)
                  : std::string();
  }
};

struct Gamer {
  uint8_t id = 0;
  uint8_t machine = 0;
  uint64_t xuid = 0;
  uint32_t slot = 0;
  uint32_t flags = 0;
  std::string gamertag;
};

struct Peer {
  uint8_t machine = 0;
  uint32_t ip = 0;
  uint16_t port = 0;
  uint64_t xuid = 0;
  Clock::time_point heard;
};

struct Session {
  uint32_t handle = 0;
  uint32_t type = 0;
  uint32_t state = kStateLobby;
  uint32_t max_gamers = 0;
  uint32_t private_slots = 0;
  uint32_t allow_join = 0;
  uint32_t allow_migration = 0;
  std::vector<uint32_t> properties;
  bool host = true;
  bool registered = false;
  bool ended = false;
  bool skip_records = false;
  uint8_t machine = kHostMachine;
  uint8_t next_machine = 1;
  uint8_t next_gamer_id = 0;
  uint64_t wire_id = 0;
  uint32_t departed = 0;
  std::vector<Gamer> gamers;
  std::vector<Peer> peers;
  EventWriter pending;
  object_ref<XSocket> socket;
  Clock::time_point last_ping;
};

struct LocalGamer {
  uint32_t slot = 0;
  uint64_t xuid = 0;
  std::string gamertag;
};

struct Found {
  uint64_t session_id = 0;
  uint64_t host_xuid = 0;
  std::string host_ip;
  std::string host_tag;
  uint32_t type = 0;
  uint32_t gamers = 0;
  uint32_t open_public = 0;
  uint32_t open_private = 0;
};

struct Finder {
  XnaSessionRequest request;
  std::vector<Found> found;
};

struct Joining {
  std::unique_ptr<Session> session;
  bool ok = false;
};

std::mutex sessions_mutex;
std::map<uint32_t, std::unique_ptr<Session>> sessions;
std::map<uint32_t, XnaSessionRequest> creating;
std::map<uint32_t, uint32_t> finding;
std::map<uint32_t, Finder> finders;
std::map<uint32_t, Joining> joining;
uint32_t next_handle = 1;
uint32_t next_finder = 1;

const char* StateName(uint32_t state) {
  switch (state) {
    case kStateLobby:
      return "Lobby";
    case kStatePlaying:
      return "Playing";
    case kStateEnded:
      return "Ended";
    default:
      return "unknown";
  }
}

bool IsNetworkType(uint32_t type) {
  return type == kTypeSystemLink || type == kTypePlayerMatch ||
         type == kTypeRanked;
}

uint32_t HubFlags(uint32_t type) {
  switch (type) {
    case kTypeSystemLink:
      return SessionFlags::HOST | SessionFlags::GROUP_LOBBY;
    case kTypeRanked:
      return SessionFlags::HOST | SessionFlags::LIVE_MULTIPLAYER_RANKED;
    default:
      return SessionFlags::HOST | SessionFlags::LIVE_MULTIPLAYER_STANDARD;
  }
}

uint32_t TypeFromHubFlags(uint32_t flags) {
  if (flags & SessionFlags::ARBITRATION) {
    return kTypeRanked;
  }
  if (flags & SessionFlags::MATCHMAKING) {
    return kTypePlayerMatch;
  }
  return kTypeSystemLink;
}

uint32_t ParseIpv4(const std::string& text) {
  uint32_t parts[4] = {};
  uint32_t count = 0;
  uint32_t value = 0;
  bool digit = false;
  for (const char c : text) {
    if (c >= '0' && c <= '9') {
      value = value * 10 + static_cast<uint32_t>(c - '0');
      digit = true;
      if (value > 255) {
        return 0;
      }
    } else if (c == '.' && digit && count < 3) {
      parts[count++] = value;
      value = 0;
      digit = false;
    } else {
      return 0;
    }
  }
  if (!digit || count != 3) {
    return 0;
  }
  parts[3] = value;
  return parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

XLiveAPI* LiveApi() {
  auto* state = kernel_state();
  return state ? state->GetXboxLiveAPI() : nullptr;
}

uint32_t PeerAddress(uint64_t xuid, const std::string& host_ip) {
  auto* api = LiveApi();
  if (api && xuid && api->transport() && api->transport()->is_ready()) {
    return XLiveAPI::RegisterXuidHandle(xuid);
  }
  return ParseIpv4(host_ip);
}

void HubAsync(std::function<void(XLiveAPI*)> work) {
  auto* api = LiveApi();
  if (!api) {
    return;
  }
  std::thread([api, work = std::move(work)] { work(api); }).detach();
}

std::vector<LocalGamer> SelectLocalGamers(const XnaSessionRequest& request) {
  std::vector<LocalGamer> gamers;
  auto* state = kernel_state();
  auto* profiles = state && state->xam_state()
                       ? state->xam_state()->profile_manager()
                       : nullptr;
  if (!profiles) {
    return gamers;
  }
  const uint32_t limit = request.max_local ? request.max_local : kLocalSlots;
  const auto consider = [&](uint32_t slot) {
    if (gamers.size() >= limit || slot >= kLocalSlots) {
      return;
    }
    if (request.local_mask && !(request.local_mask & (1u << slot))) {
      return;
    }
    for (const LocalGamer& gamer : gamers) {
      if (gamer.slot == slot) {
        return;
      }
    }
    const auto* profile = profiles->GetProfile(static_cast<uint8_t>(slot));
    if (!profile || static_cast<uint32_t>(profile->signin_state()) == 0) {
      return;
    }
    gamers.push_back(
        LocalGamer{slot, profile->GetOnlineXUID(), profile->name()});
  };
  consider(request.first_local);
  for (uint32_t slot = 0; slot < kLocalSlots; ++slot) {
    consider(slot);
  }
  return gamers;
}

uint32_t GamerState(const Session& s, const Gamer& gamer) {
  return gamer.flags | (gamer.machine == s.machine ? kGamerLocal : 0);
}

int IndexOfGamer(const Session& s, uint8_t id) {
  for (size_t i = 0; i < s.gamers.size(); ++i) {
    if (s.gamers[i].id == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void AnnounceJoin(Session& s, const Gamer& gamer) {
  uint32_t machine_index = kNewMachine;
  for (size_t i = 0; i < s.gamers.size(); ++i) {
    if (s.gamers[i].machine == gamer.machine) {
      machine_index = static_cast<uint32_t>(i);
      break;
    }
  }
  s.pending.Word(kEventGamerJoined);
  s.pending.Word(GamerState(s, gamer));
  s.pending.Word(gamer.id);
  s.pending.Word(static_cast<uint32_t>(s.gamers.size()));
  s.pending.Word(machine_index);
  s.pending.Word(gamer.slot);
  s.pending.String(gamer.gamertag);
  s.gamers.push_back(gamer);
}

void AnnounceLeave(Session& s, size_t index) {
  s.pending.Word(kEventGamerLeft);
  s.pending.Word(static_cast<uint32_t>(index));
  s.pending.Word(kDepartedIndex | s.departed++);
  s.pending.Word(0);
  s.gamers.erase(s.gamers.begin() + static_cast<std::ptrdiff_t>(index));
}

void AnnounceInfo(Session& s) {
  s.pending.Word(kEventSessionInfo);
  s.pending.Word(s.max_gamers);
  s.pending.Word(s.private_slots);
  s.pending.Word(s.allow_join ? 1 : 0);
  s.pending.Word(s.allow_migration ? 1 : 0);
  for (const Gamer& gamer : s.gamers) {
    s.pending.Word(GamerState(s, gamer));
  }
}

void ChangeState(Session& s, uint32_t state, uint32_t reason) {
  if (s.state == state) {
    return;
  }
  XELOGI("[xna] network session {}: {} -> {}", s.handle, StateName(s.state),
         StateName(state));
  s.state = state;
  s.pending.Word(kEventStateChanged);
  s.pending.Word(state);
  if (state == kStateEnded) {
    s.pending.Word(reason);
  }
}

void DeliverLocally(Session& s, uint8_t from, uint8_t to, const uint8_t* data,
                    uint32_t length) {
  const int sender = IndexOfGamer(s, from);
  if (sender < 0) {
    return;
  }
  std::vector<uint32_t> recipients;
  for (size_t i = 0; i < s.gamers.size(); ++i) {
    const Gamer& gamer = s.gamers[i];
    if (gamer.machine == s.machine && (to == kAllGamers || gamer.id == to)) {
      recipients.push_back(static_cast<uint32_t>(i));
    }
  }
  if (recipients.empty()) {
    return;
  }
  s.pending.Word(kEventReceivedData);
  s.pending.Word(static_cast<uint32_t>(sender));
  s.pending.Word(static_cast<uint32_t>(recipients.size()));
  for (uint32_t index : recipients) {
    s.pending.Word(index);
  }
  s.pending.Word(length);
  s.pending.Data(data, length);
}

Bytes Message(const Session& s, Wire kind) {
  Bytes message;
  message.U32(kWireMagic);
  message.U64(s.wire_id);
  message.U8(static_cast<uint8_t>(kind));
  return message;
}

void WriteGamer(Bytes& message, const Gamer& gamer) {
  message.U8(gamer.id);
  message.U8(gamer.machine);
  message.U64(gamer.xuid);
  message.U32(gamer.slot);
  message.U32(gamer.flags);
  message.Text(gamer.gamertag);
}

bool ReadGamer(Reader& reader, Gamer* gamer) {
  gamer->id = reader.Get<uint8_t>();
  gamer->machine = reader.Get<uint8_t>();
  gamer->xuid = reader.Get<uint64_t>();
  gamer->slot = reader.Get<uint32_t>();
  gamer->flags = reader.Get<uint32_t>();
  gamer->gamertag = reader.Text();
  return reader.ok;
}

Bytes DataMessage(const Session& s, uint8_t from, uint8_t to, uint32_t options,
                  const uint8_t* data, uint32_t length) {
  Bytes message = Message(s, Wire::kData);
  message.U8(from);
  message.U8(to);
  message.U32(options);
  message.U32(length);
  message.Raw(data, length);
  return message;
}

Bytes ReadyMessage(const Session& s, uint8_t id, bool ready) {
  Bytes message = Message(s, Wire::kReady);
  message.U8(id);
  message.U8(ready ? 1 : 0);
  return message;
}

Bytes StateMessage(const Session& s, uint32_t state, uint32_t reason) {
  Bytes message = Message(s, Wire::kState);
  message.U32(state);
  message.U32(reason);
  return message;
}

Bytes EndMessage(const Session& s, uint32_t reason) {
  Bytes message = Message(s, Wire::kEnd);
  message.U32(reason);
  return message;
}

Bytes InfoMessage(const Session& s) {
  Bytes message = Message(s, Wire::kInfo);
  message.U32(s.max_gamers);
  message.U32(s.private_slots);
  message.U32(s.allow_join);
  message.U32(s.allow_migration);
  message.U8(static_cast<uint8_t>(s.gamers.size()));
  for (const Gamer& gamer : s.gamers) {
    message.U8(gamer.id);
    message.U32(gamer.flags);
  }
  return message;
}

Bytes WelcomeMessage(const Session& s, uint8_t machine) {
  Bytes message = Message(s, Wire::kWelcome);
  message.U8(machine);
  message.U32(s.type);
  message.U32(s.state);
  message.U32(s.max_gamers);
  message.U32(s.private_slots);
  message.U32(s.allow_join);
  message.U32(s.allow_migration);
  message.U8(static_cast<uint8_t>(s.gamers.size()));
  for (const Gamer& gamer : s.gamers) {
    WriteGamer(message, gamer);
  }
  return message;
}

void Send(Session& s, const Peer& peer, const Bytes& message) {
  if (!s.socket || !peer.ip) {
    return;
  }
  XSOCKADDR_IN to = {};
  to.address_family = static_cast<uint16_t>(XSocket::X_AF_INET);
  to.address_port = peer.port;
  to.address_ip.s_addr = peer.ip;
  s.socket->SendTo(const_cast<uint8_t*>(message.data.data()),
                   static_cast<uint32_t>(message.data.size()), 0, &to,
                   sizeof(to));
}

void SendAll(Session& s, const Bytes& message, uint8_t except) {
  for (const Peer& peer : s.peers) {
    if (peer.machine != except) {
      Send(s, peer, message);
    }
  }
}

Peer* PeerFor(Session& s, uint8_t machine) {
  for (Peer& peer : s.peers) {
    if (peer.machine == machine) {
      return &peer;
    }
  }
  return nullptr;
}

Peer* PeerAt(Session& s, uint32_t ip, uint16_t port) {
  for (Peer& peer : s.peers) {
    if (peer.ip == ip && peer.port == port) {
      return &peer;
    }
  }
  return nullptr;
}

object_ref<XSocket> OpenSocket() {
  auto* state = kernel_state();
  if (!state) {
    return object_ref<XSocket>();
  }
  auto socket = object_ref<XSocket>(new XSocket(state));
  if (XFAILED(socket->Initialize(XSocket::X_AF_INET, XSocket::X_SOCK_DGRAM,
                                 XSocket::X_IPPROTO_UDP))) {
    socket->ReleaseHandle();
    return object_ref<XSocket>();
  }
  uint32_t nonblocking = xe::byte_swap(uint32_t(1));
  XSOCKADDR_IN address = {};
  address.address_family = static_cast<uint16_t>(XSocket::X_AF_INET);
  address.address_port = kSessionPort;
  if (XFAILED(socket->IOControl(kFionbio, &nonblocking)) ||
      XFAILED(socket->Bind(&address, sizeof(address)))) {
    socket->Close();
    socket->ReleaseHandle();
    return object_ref<XSocket>();
  }
  return socket;
}

void CloseSocket(Session& s) {
  if (!s.socket) {
    return;
  }
  s.socket->Close();
  s.socket->ReleaseHandle();
  s.socket.reset();
}

void RemoveMachine(Session& s, uint8_t machine) {
  std::vector<xe::be<uint64_t>> xuids;
  for (size_t i = s.gamers.size(); i-- > 0;) {
    if (s.gamers[i].machine != machine) {
      continue;
    }
    const Gamer gamer = s.gamers[i];
    xuids.push_back(gamer.xuid);
    AnnounceLeave(s, i);
    if (s.host) {
      Bytes left = Message(s, Wire::kGamerLeft);
      left.U8(gamer.id);
      SendAll(s, left, machine);
    }
  }
  s.peers.erase(std::remove_if(s.peers.begin(), s.peers.end(),
                               [machine](const Peer& peer) {
                                 return peer.machine == machine;
                               }),
                s.peers.end());
  XELOGI("[xna] network session {}: machine {} left", s.handle, machine);
  if (s.host && s.registered && !xuids.empty()) {
    const uint64_t id = s.wire_id;
    HubAsync(
        [id, xuids](XLiveAPI* api) { api->SessionLeaveRemote(id, xuids); });
  }
}

void HostHandleJoin(Session& s, uint32_t ip, uint16_t port, Reader& reader) {
  const uint8_t count = reader.Get<uint8_t>();
  std::vector<Gamer> incoming;
  for (uint8_t i = 0; i < count && reader.ok; ++i) {
    Gamer gamer;
    gamer.xuid = reader.Get<uint64_t>();
    gamer.slot = reader.Get<uint32_t>();
    gamer.gamertag = reader.Text();
    incoming.push_back(gamer);
  }
  if (!reader.ok || incoming.empty()) {
    return;
  }
  if (Peer* known = PeerAt(s, ip, port)) {
    known->heard = Clock::now();
    Send(s, *known, WelcomeMessage(s, known->machine));
    return;
  }
  const bool full = s.gamers.size() + incoming.size() > s.max_gamers;
  const bool closed = s.state != kStateLobby && !s.allow_join;
  if (full || closed || s.next_machine == kNoMachine) {
    Peer stranger;
    stranger.ip = ip;
    stranger.port = port;
    Bytes reject = Message(s, Wire::kReject);
    reject.U32(full ? kRejectFull : kRejectClosed);
    Send(s, stranger, reject);
    XELOGW("[xna] network session {}: refused {} ({})", s.handle,
           incoming[0].gamertag, full ? "full" : "closed to joins");
    return;
  }
  XLiveAPI::RegisterXuidHandle(incoming[0].xuid);
  Peer peer;
  peer.machine = s.next_machine++;
  peer.ip = ip;
  peer.port = port;
  peer.xuid = incoming[0].xuid;
  peer.heard = Clock::now();
  std::unordered_map<uint64_t, bool> members;
  for (Gamer& gamer : incoming) {
    gamer.id = s.next_gamer_id++;
    gamer.machine = peer.machine;
    gamer.flags = 0;
    Bytes joined = Message(s, Wire::kGamerJoined);
    WriteGamer(joined, gamer);
    SendAll(s, joined, kNoMachine);
    AnnounceJoin(s, gamer);
    members[gamer.xuid] = false;
  }
  s.peers.push_back(peer);
  Send(s, peer, WelcomeMessage(s, peer.machine));
  XELOGI("[xna] network session {}: {} joined as machine {} with {} gamer(s)",
         s.handle, incoming[0].gamertag, peer.machine, incoming.size());
  if (s.registered) {
    const uint64_t id = s.wire_id;
    HubAsync(
        [id, members](XLiveAPI* api) { api->SessionJoinRemote(id, members); });
  }
}

void HostHandle(Session& s, Peer& peer, Wire kind, Reader& reader) {
  switch (kind) {
    case Wire::kData: {
      const uint8_t from = reader.Get<uint8_t>();
      const uint8_t to = reader.Get<uint8_t>();
      const uint32_t options = reader.Get<uint32_t>();
      const uint32_t length = reader.Get<uint32_t>();
      const uint8_t* data = reader.Take(length);
      const int sender = IndexOfGamer(s, from);
      if (!reader.ok || sender < 0 ||
          s.gamers[sender].machine != peer.machine) {
        return;
      }
      DeliverLocally(s, from, to, data, length);
      const Bytes forward = DataMessage(s, from, to, options, data, length);
      if (to == kAllGamers) {
        SendAll(s, forward, peer.machine);
        return;
      }
      const int target = IndexOfGamer(s, to);
      if (target >= 0 && s.gamers[target].machine != s.machine &&
          s.gamers[target].machine != peer.machine) {
        if (Peer* owner = PeerFor(s, s.gamers[target].machine)) {
          Send(s, *owner, forward);
        }
      }
      return;
    }
    case Wire::kReady: {
      const uint8_t id = reader.Get<uint8_t>();
      const bool ready = reader.Get<uint8_t>() != 0;
      const int index = IndexOfGamer(s, id);
      if (!reader.ok || index < 0 || s.gamers[index].machine != peer.machine) {
        return;
      }
      if (ready) {
        s.gamers[index].flags |= kGamerReady;
      } else {
        s.gamers[index].flags &= ~kGamerReady;
      }
      AnnounceInfo(s);
      SendAll(s, ReadyMessage(s, id, ready), peer.machine);
      return;
    }
    case Wire::kLeave:
      RemoveMachine(s, peer.machine);
      return;
    default:
      return;
  }
}

void ClientHandle(Session& s, Wire kind, Reader& reader) {
  switch (kind) {
    case Wire::kGamerJoined: {
      Gamer gamer;
      if (ReadGamer(reader, &gamer) && IndexOfGamer(s, gamer.id) < 0) {
        AnnounceJoin(s, gamer);
      }
      return;
    }
    case Wire::kGamerLeft: {
      const uint8_t id = reader.Get<uint8_t>();
      const int index = IndexOfGamer(s, id);
      if (reader.ok && index >= 0) {
        AnnounceLeave(s, static_cast<size_t>(index));
      }
      return;
    }
    case Wire::kData: {
      const uint8_t from = reader.Get<uint8_t>();
      const uint8_t to = reader.Get<uint8_t>();
      reader.Get<uint32_t>();
      const uint32_t length = reader.Get<uint32_t>();
      const uint8_t* data = reader.Take(length);
      if (reader.ok) {
        DeliverLocally(s, from, to, data, length);
      }
      return;
    }
    case Wire::kState: {
      const uint32_t state = reader.Get<uint32_t>();
      const uint32_t reason = reader.Get<uint32_t>();
      if (reader.ok) {
        ChangeState(s, state, reason);
        s.ended = state == kStateEnded;
      }
      return;
    }
    case Wire::kReady: {
      const uint8_t id = reader.Get<uint8_t>();
      const bool ready = reader.Get<uint8_t>() != 0;
      const int index = IndexOfGamer(s, id);
      if (!reader.ok || index < 0) {
        return;
      }
      if (ready) {
        s.gamers[index].flags |= kGamerReady;
      } else {
        s.gamers[index].flags &= ~kGamerReady;
      }
      AnnounceInfo(s);
      return;
    }
    case Wire::kInfo: {
      s.max_gamers = reader.Get<uint32_t>();
      s.private_slots = reader.Get<uint32_t>();
      s.allow_join = reader.Get<uint32_t>();
      s.allow_migration = reader.Get<uint32_t>();
      const uint8_t count = reader.Get<uint8_t>();
      for (uint8_t i = 0; i < count && reader.ok; ++i) {
        const uint8_t id = reader.Get<uint8_t>();
        const uint32_t flags = reader.Get<uint32_t>();
        const int index = IndexOfGamer(s, id);
        if (reader.ok && index >= 0) {
          s.gamers[index].flags = flags & ~kGamerLocal;
        }
      }
      AnnounceInfo(s);
      return;
    }
    case Wire::kEnd: {
      const uint32_t reason = reader.Get<uint32_t>();
      ChangeState(s, kStateEnded, reader.ok ? reason : kEndHostEnded);
      s.ended = true;
      return;
    }
    default:
      return;
  }
}

void Poll(Session& s) {
  if (!s.socket) {
    return;
  }
  static thread_local std::vector<uint8_t> buffer(kMaxDatagram);
  for (int i = 0; i < 512; ++i) {
    XSOCKADDR_IN from = {};
    socklen_t from_len = static_cast<socklen_t>(sizeof(from));
    const int got =
        s.socket->RecvFrom(buffer.data(), static_cast<uint32_t>(buffer.size()),
                           0, &from, &from_len);
    if (got <= 0) {
      break;
    }
    Reader reader{buffer.data(), buffer.data() + got};
    const uint32_t magic = reader.Get<uint32_t>();
    const uint64_t id = reader.Get<uint64_t>();
    const auto kind = static_cast<Wire>(reader.Get<uint8_t>());
    if (!reader.ok || magic != kWireMagic || id != s.wire_id) {
      continue;
    }
    const uint32_t ip = from.address_ip.s_addr;
    const uint16_t port = from.address_port;
    if (s.host) {
      if (kind == Wire::kJoin) {
        HostHandleJoin(s, ip, port, reader);
        continue;
      }
      Peer* peer = PeerAt(s, ip, port);
      if (!peer) {
        continue;
      }
      peer->heard = Clock::now();
      HostHandle(s, *peer, kind, reader);
    } else {
      if (s.peers.empty() || s.peers[0].ip != ip) {
        continue;
      }
      s.peers[0].heard = Clock::now();
      ClientHandle(s, kind, reader);
    }
  }

  const auto now = Clock::now();
  if (now - s.last_ping >= kPingInterval) {
    s.last_ping = now;
    SendAll(s, Message(s, Wire::kPing), kNoMachine);
  }
  if (s.host) {
    std::vector<uint8_t> silent;
    for (const Peer& peer : s.peers) {
      if (now - peer.heard > kPeerTimeout) {
        silent.push_back(peer.machine);
      }
    }
    for (const uint8_t machine : silent) {
      XELOGW("[xna] network session {}: machine {} stopped answering", s.handle,
             machine);
      RemoveMachine(s, machine);
    }
  } else if (!s.ended && !s.peers.empty() &&
             now - s.peers[0].heard > kPeerTimeout) {
    XELOGW("[xna] network session {}: the host stopped answering", s.handle);
    ChangeState(s, kStateEnded, kEndDisconnected);
    s.ended = true;
  }
}

void ApplyRecords(Session& s, const uint8_t* records, size_t size,
                  std::string* problem, uint32_t* packets) {
  Reader reader{records, records + size};
  while (reader.at < reader.end) {
    const size_t start = static_cast<size_t>(reader.at - records);
    const uint32_t kind = reader.Get<uint32_t>();
    bool known = true;
    switch (kind) {
      case kRecordSendData: {
        const uint32_t sender = reader.Get<uint32_t>();
        const uint32_t recipient = reader.Get<uint32_t>();
        const uint32_t options = reader.Get<uint32_t>();
        const uint32_t length = reader.Get<uint32_t>();
        const uint8_t* data = reader.Take(length);
        reader.Take(((length + 3) & ~3u) - length);
        if (!reader.ok || sender >= s.gamers.size() ||
            s.gamers[sender].machine != s.machine) {
          break;
        }
        uint8_t to = kAllGamers;
        bool remote = true;
        if (recipient != kAllRecipients) {
          if (recipient >= s.gamers.size()) {
            break;
          }
          to = s.gamers[recipient].id;
          remote = s.gamers[recipient].machine != s.machine;
        }
        const uint8_t from = s.gamers[sender].id;
        DeliverLocally(s, from, to, data, length);
        ++*packets;
        if (!remote || s.peers.empty()) {
          break;
        }
        const Bytes message = DataMessage(s, from, to, options, data, length);
        if (s.host && to != kAllGamers) {
          if (Peer* owner = PeerFor(s, s.gamers[recipient].machine)) {
            Send(s, *owner, message);
          }
        } else {
          SendAll(s, message, kNoMachine);
        }
        break;
      }
      case kRecordEnableVoice:
        reader.Get<uint32_t>();
        reader.Get<uint32_t>();
        reader.Get<uint32_t>();
        break;
      case kRecordSetReady: {
        const uint32_t index = reader.Get<uint32_t>();
        const bool ready = reader.Get<uint32_t>() != 0;
        if (!reader.ok || index >= s.gamers.size()) {
          break;
        }
        if (ready) {
          s.gamers[index].flags |= kGamerReady;
        } else {
          s.gamers[index].flags &= ~kGamerReady;
        }
        SendAll(s, ReadyMessage(s, s.gamers[index].id, ready), kNoMachine);
        break;
      }
      case kRecordStartGame:
        if (s.host && s.state == kStateLobby) {
          ChangeState(s, kStatePlaying, 0);
          SendAll(s, StateMessage(s, kStatePlaying, 0), kNoMachine);
        }
        break;
      case kRecordEndGame:
        if (s.host && s.state == kStatePlaying) {
          ChangeState(s, kStateLobby, 0);
          SendAll(s, StateMessage(s, kStateLobby, 0), kNoMachine);
        }
        break;
      case kRecordResetReady:
        for (Gamer& gamer : s.gamers) {
          gamer.flags &= ~kGamerReady;
        }
        AnnounceInfo(s);
        if (s.host) {
          SendAll(s, InfoMessage(s), kNoMachine);
        }
        break;
      case kRecordRemoveMachine: {
        const uint32_t index = reader.Get<uint32_t>();
        if (!reader.ok || !s.host || index >= s.gamers.size() ||
            s.gamers[index].machine == s.machine) {
          break;
        }
        const uint8_t machine = s.gamers[index].machine;
        if (Peer* owner = PeerFor(s, machine)) {
          Send(s, *owner, EndMessage(s, kEndRemovedByHost));
        }
        RemoveMachine(s, machine);
        break;
      }
      case kRecordSetSlots:
        s.max_gamers = reader.Get<uint32_t>();
        s.private_slots = reader.Get<uint32_t>();
        if (reader.ok && s.host) {
          SendAll(s, InfoMessage(s), kNoMachine);
        }
        break;
      case kRecordAllowJoin:
        s.allow_join = reader.Get<uint32_t>();
        if (reader.ok && s.host) {
          SendAll(s, InfoMessage(s), kNoMachine);
        }
        break;
      case kRecordAllowMigration:
        s.allow_migration = reader.Get<uint32_t>();
        if (reader.ok && s.host) {
          SendAll(s, InfoMessage(s), kNoMachine);
        }
        break;
      case kRecordProperty: {
        reader.Get<uint32_t>();
        if (reader.Get<uint32_t>()) {
          reader.Get<uint32_t>();
        }
        break;
      }
      default:
        known = false;
        break;
    }
    if (!known) {
      *problem = fmt::format("unknown record {} at byte {}", kind,
                             start + kRecordsOffset);
      return;
    }
    if (!reader.ok) {
      *problem = fmt::format("record {} at byte {} is truncated", kind,
                             start + kRecordsOffset);
      return;
    }
  }
}

void StartHosting(Session& s) {
  s.wire_id = GenerateSessionId(XNKID_ONLINE);
  s.last_ping = Clock::now();
  s.socket = OpenSocket();
  if (!s.socket) {
    XELOGW(
        "[xna] network session: could not open the session socket on port {}; "
        "the session stays on this machine",
        kSessionPort);
    return;
  }
  if (!LiveApi()) {
    return;
  }
  XGI_SESSION_CREATE data = {};
  data.flags = HubFlags(s.type);
  data.num_slots_public =
      s.max_gamers > s.private_slots ? s.max_gamers - s.private_slots : 0;
  data.num_slots_private = s.private_slots;
  data.user_index = s.gamers.empty() ? 0 : s.gamers[0].slot;
  std::unordered_map<uint64_t, bool> members;
  for (const Gamer& gamer : s.gamers) {
    members[gamer.xuid] = false;
  }
  s.registered = true;
  const uint64_t id = s.wire_id;
  HubAsync([id, data, members](XLiveAPI* api) mutable {
    api->XSessionCreate(id, &data);
    api->SessionJoinRemote(id, members);
  });
  XELOGI("[xna] network session: hosting {:016X} on port {}", id, kSessionPort);
}

std::vector<uint32_t> EmptyProperties() {
  return std::vector<uint32_t>(kPropertySlots, 0);
}

void JoinWorker(uint32_t operation, Found target, XnaSessionRequest request) {
  auto session = std::make_unique<Session>();
  session->host = false;
  session->type = target.type;
  session->wire_id = target.session_id;
  session->properties = EmptyProperties();
  const std::vector<LocalGamer> locals = SelectLocalGamers(request);
  session->socket = OpenSocket();

  Peer host;
  host.machine = kHostMachine;
  host.ip = PeerAddress(target.host_xuid, target.host_ip);
  host.port = kSessionPort;
  host.xuid = target.host_xuid;
  host.heard = Clock::now();

  bool ok = false;
  std::string reason;
  if (!session->socket) {
    reason = "the session socket could not be opened";
  } else if (!host.ip) {
    reason = "the host has no address";
  } else if (locals.empty()) {
    reason = "nobody is signed in";
  } else {
    Bytes join = Message(*session, Wire::kJoin);
    join.U8(static_cast<uint8_t>(locals.size()));
    for (const LocalGamer& local : locals) {
      join.U64(local.xuid);
      join.U32(local.slot);
      join.Text(local.gamertag);
    }
    std::vector<uint8_t> buffer(kMaxDatagram);
    const auto deadline = Clock::now() + kJoinTimeout;
    auto next_send = Clock::now();
    while (!ok && reason.empty() && Clock::now() < deadline) {
      if (Clock::now() >= next_send) {
        Send(*session, host, join);
        next_send = Clock::now() + kJoinRetry;
      }
      XSOCKADDR_IN from = {};
      socklen_t from_len = static_cast<socklen_t>(sizeof(from));
      const int got = session->socket->RecvFrom(
          buffer.data(), static_cast<uint32_t>(buffer.size()), 0, &from,
          &from_len);
      if (got <= 0) {
        std::this_thread::sleep_for(kJoinPoll);
        continue;
      }
      Reader reader{buffer.data(), buffer.data() + got};
      const uint32_t magic = reader.Get<uint32_t>();
      const uint64_t id = reader.Get<uint64_t>();
      const auto kind = static_cast<Wire>(reader.Get<uint8_t>());
      if (!reader.ok || magic != kWireMagic || id != session->wire_id ||
          from.address_ip.s_addr != host.ip) {
        continue;
      }
      if (kind == Wire::kReject) {
        reason = reader.Get<uint32_t>() == kRejectFull
                     ? "the session is full"
                     : "the session is closed to joins";
        break;
      }
      if (kind != Wire::kWelcome) {
        continue;
      }
      session->machine = reader.Get<uint8_t>();
      session->type = reader.Get<uint32_t>();
      const uint32_t state = reader.Get<uint32_t>();
      session->max_gamers = reader.Get<uint32_t>();
      session->private_slots = reader.Get<uint32_t>();
      session->allow_join = reader.Get<uint32_t>();
      session->allow_migration = reader.Get<uint32_t>();
      const uint8_t count = reader.Get<uint8_t>();
      std::vector<Gamer> gamers;
      for (uint8_t i = 0; i < count && reader.ok; ++i) {
        Gamer gamer;
        if (ReadGamer(reader, &gamer)) {
          gamers.push_back(gamer);
        }
      }
      if (!reader.ok) {
        continue;
      }
      for (const Gamer& gamer : gamers) {
        AnnounceJoin(*session, gamer);
      }
      ChangeState(*session, state, kEndHostEnded);
      host.heard = Clock::now();
      ok = true;
    }
    if (!ok && reason.empty()) {
      reason = "the host did not answer";
    }
  }

  session->peers.push_back(host);
  session->last_ping = Clock::now();
  if (ok) {
    XELOGI("[xna] network session: joined {:016X} as machine {} ({} gamer(s))",
           session->wire_id, session->machine, session->gamers.size());
  } else {
    XELOGW("[xna] network session: could not join {:016X}: {}",
           target.session_id, reason);
    CloseSocket(*session);
  }
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    Joining& entry = joining[operation];
    entry.ok = ok;
    entry.session = std::move(session);
  }
  XnaCompleteAsyncOperation(operation);
}

}  // namespace

uint32_t XnaSessionBeginCreate(const XnaSessionRequest& request) {
  const uint32_t operation = XnaReserveAsyncOperation();
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    creating[operation] = request;
  }
  XnaCompleteAsyncOperation(operation);
  return operation;
}

uint32_t XnaSessionEndCreate(uint32_t operation, XnaSessionSummary* out) {
  XnaSessionRequest request;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    auto found = creating.find(operation);
    if (found == creating.end()) {
      XELOGW("[xna] network session: no create operation {}", operation);
      return kFailed;
    }
    request = std::move(found->second);
    creating.erase(found);
  }

  auto session = std::make_unique<Session>();
  session->type = request.type;
  session->max_gamers = request.max_gamers;
  session->private_slots = request.private_slots;
  session->properties =
      request.properties.empty() ? EmptyProperties() : request.properties;
  const std::vector<LocalGamer> locals = SelectLocalGamers(request);
  for (size_t i = 0; i < locals.size(); ++i) {
    Gamer gamer;
    gamer.id = session->next_gamer_id++;
    gamer.machine = kHostMachine;
    gamer.xuid = locals[i].xuid;
    gamer.slot = locals[i].slot;
    gamer.flags = i == 0 ? kGamerHost : 0;
    gamer.gamertag = locals[i].gamertag;
    AnnounceJoin(*session, gamer);
  }
  if (IsNetworkType(session->type)) {
    StartHosting(*session);
  }

  std::lock_guard<std::mutex> lock(sessions_mutex);
  const uint32_t handle = next_handle++;
  session->handle = handle;
  out->handle = handle;
  out->type = session->type;
  out->max_gamers = session->max_gamers;
  out->private_slots = session->private_slots;
  out->properties = session->properties;
  XELOGI(
      "[xna] network session: operation {} created session {} (type {}) with "
      "{} local gamer(s)",
      operation, handle, session->type, locals.size());
  sessions[handle] = std::move(session);
  return 0;
}

uint32_t XnaSessionBeginFind(const XnaSessionRequest& request) {
  const uint32_t operation = XnaReserveAsyncOperation();
  uint32_t finder = 0;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    finder = next_finder++;
    finders[finder].request = request;
    finding[operation] = finder;
  }
  const uint32_t type = request.type;
  std::thread([operation, finder, type] {
    std::vector<Found> found;
    if (auto* api = LiveApi()) {
      for (const auto& session : api->GetTitleSessions()) {
        if (!session) {
          continue;
        }
        const uint64_t host = session->XUID_UInt();
        if (!host || host == XLiveAPI::local_online_xuid) {
          continue;
        }
        if (TypeFromHubFlags(static_cast<uint32_t>(session->Flags())) != type) {
          continue;
        }
        Found entry;
        entry.session_id = session->SessionID_UInt();
        entry.host_xuid = host;
        entry.host_ip = session->HostAddress();
        entry.type = type;
        const auto& players = session->Players();
        for (const auto& player : players) {
          if (static_cast<uint64_t>(player.XUID()) == host) {
            entry.host_tag = player.Gamertag();
          }
        }
        if (entry.host_tag.empty() && !players.empty()) {
          entry.host_tag = players.front().Gamertag();
        }
        entry.gamers =
            static_cast<uint32_t>((std::max)(players.size(), size_t(1)));
        const uint32_t public_slots =
            static_cast<uint32_t>(session->PublicSlotsCount());
        entry.open_public =
            public_slots > entry.gamers ? public_slots - entry.gamers : 0;
        entry.open_private =
            static_cast<uint32_t>(session->PrivateSlotsCount());
        found.push_back(entry);
        if (found.size() >= kMaxFound) {
          break;
        }
      }
    }
    XELOGI("[xna] network session: found {} session(s) of type {}",
           found.size(), type);
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      auto entry = finders.find(finder);
      if (entry != finders.end()) {
        entry->second.found = std::move(found);
      }
    }
    XnaCompleteAsyncOperation(operation);
  }).detach();
  return operation;
}

uint32_t XnaSessionEndFind(uint32_t operation, std::vector<uint8_t>* reply) {
  std::lock_guard<std::mutex> lock(sessions_mutex);
  auto op = finding.find(operation);
  if (op == finding.end()) {
    return kFailed;
  }
  const uint32_t finder = op->second;
  finding.erase(op);
  const Finder& entry = finders[finder];
  EventWriter out;
  out.Word(finder);
  out.Word(static_cast<uint32_t>(entry.found.size()));
  for (const Found& found : entry.found) {
    out.String(found.host_tag);
    out.Word(found.gamers);
    out.Word(found.open_public);
    out.Word(found.open_private);
    for (uint32_t slot = 0; slot < kPropertySlots; ++slot) {
      out.Word(0);
    }
  }
  *reply = std::move(out.bytes);
  return 0;
}

void XnaSessionDestroyFinder(uint32_t finder) {
  std::lock_guard<std::mutex> lock(sessions_mutex);
  finders.erase(finder);
}

uint32_t XnaSessionBeginJoin(uint32_t finder, uint32_t index) {
  const uint32_t operation = XnaReserveAsyncOperation();
  Found target;
  XnaSessionRequest request;
  bool have = false;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    joining[operation] = Joining{};
    auto entry = finders.find(finder);
    if (entry != finders.end() && index < entry->second.found.size()) {
      target = entry->second.found[index];
      request = entry->second.request;
      have = true;
    }
  }
  if (!have) {
    XELOGW("[xna] network session: finder {} has no session {}", finder, index);
    XnaCompleteAsyncOperation(operation);
    return operation;
  }
  XELOGI("[xna] network session: joining {:016X} hosted by {}",
         target.session_id, target.host_tag);
  std::thread([operation, target, request] {
    JoinWorker(operation, target, request);
  }).detach();
  return operation;
}

uint32_t XnaSessionEndJoin(uint32_t operation, XnaSessionSummary* out) {
  std::lock_guard<std::mutex> lock(sessions_mutex);
  auto entry = joining.find(operation);
  if (entry == joining.end()) {
    return kFailed;
  }
  const bool ok = entry->second.ok && entry->second.session;
  std::unique_ptr<Session> session = std::move(entry->second.session);
  joining.erase(entry);
  if (!ok) {
    return kFailed;
  }
  const uint32_t handle = next_handle++;
  session->handle = handle;
  out->handle = handle;
  out->type = session->type;
  out->max_gamers = session->max_gamers;
  out->private_slots = session->private_slots;
  out->properties = session->properties;
  sessions[handle] = std::move(session);
  XELOGI("[xna] network session: operation {} joined as session {}", operation,
         handle);
  return 0;
}

bool XnaSessionPrepareUpdate(uint32_t handle, uint32_t current_size) {
  std::lock_guard<std::mutex> lock(sessions_mutex);
  auto found = sessions.find(handle);
  if (found == sessions.end()) {
    return false;
  }
  Session& s = *found->second;
  Poll(s);
  return s.skip_records || !s.pending.bytes.empty() ||
         current_size > kRecordsOffset;
}

uint32_t XnaSessionUpdate(uint32_t handle, const uint8_t* records, size_t size,
                          uint32_t buffer_total, std::vector<uint8_t>* events,
                          uint32_t* needed) {
  std::lock_guard<std::mutex> lock(sessions_mutex);
  auto found = sessions.find(handle);
  if (found == sessions.end()) {
    return 0;
  }
  Session& s = *found->second;
  uint32_t packets = 0;
  std::string problem;
  if (!s.skip_records) {
    ApplyRecords(s, records, size, &problem, &packets);
  }
  s.skip_records = false;
  if (!problem.empty()) {
    XELOGW("[xna] network session {}: outgoing records stopped: {}", handle,
           problem);
  }
  if (packets) {
    XELOGD("[xna] network session {}: {} packet(s) sent", handle, packets);
  }
  if (s.pending.bytes.size() > buffer_total) {
    s.skip_records = true;
    *needed = static_cast<uint32_t>(s.pending.bytes.size());
    XELOGI(
        "[xna] network session {}: {} bytes of events, asking for a buffer "
        "larger than {}",
        handle, s.pending.bytes.size(), buffer_total);
    return kGrowBuffer;
  }
  events->swap(s.pending.bytes);
  s.pending.bytes.clear();
  return 0;
}

void XnaSessionDestroy(uint32_t handle) {
  std::unique_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    auto found = sessions.find(handle);
    if (found == sessions.end()) {
      return;
    }
    session = std::move(found->second);
    sessions.erase(found);
  }
  if (session->host) {
    SendAll(*session, EndMessage(*session, kEndHostEnded), kNoMachine);
    if (session->registered) {
      const uint64_t id = session->wire_id;
      HubAsync([id](XLiveAPI* api) { api->DeleteSession(id); });
    }
  } else {
    SendAll(*session, Message(*session, Wire::kLeave), kNoMachine);
  }
  CloseSocket(*session);
  XELOGI("[xna] network session {}: destroyed", handle);
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
