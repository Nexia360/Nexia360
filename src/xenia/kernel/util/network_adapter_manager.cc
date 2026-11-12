/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/network_adapter_manager.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/string_util.h"

#ifdef XE_PLATFORM_WIN32
#include <Iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

DEFINE_string(network_guid, "", "Network Interface GUID", "Live");

namespace xe {
namespace kernel {

namespace {

// Same shape as nexia_transport.cc: the Winsock spellings only exist on
// Windows, so name them once here rather than guarding every call.
#ifdef XE_PLATFORM_WIN32
using native_socket = SOCKET;
constexpr native_socket kInvalidSocket = INVALID_SOCKET;
inline int CloseSocket(native_socket socket) { return closesocket(socket); }
#else
using native_socket = int;
constexpr native_socket kInvalidSocket = -1;
inline int CloseSocket(native_socket socket) { return ::close(socket); }
#endif

constexpr int kSocketError = -1;

}  // namespace

NetworkAdapterManager::NetworkAdapterManager() {}

void NetworkAdapterManager::Initialize() {
  adapter_addresses_ = DiscoverNetworkAdapters();

  if (!adapter_addresses_.empty()) {
    // In-case our selected adapter was removed during runtime reselect best
    // interface index.
    best_interface_IfIndex_ = GetBestInterfaceIfIndex();

    // If we cannot find a best interface then default to first interface.
    if (best_interface_IfIndex_ == -1) {
      AutoSelectNetworkAdapter(adapter_addresses_.front().if_index);
    } else {
      AutoSelectNetworkAdapter(best_interface_IfIndex_);
    }
  }
}

void NetworkAdapterManager::SelectBestInterface() {
  ResetSelectedAdapter();
  Initialize();
}

void NetworkAdapterManager::SetSelectedAdapterGUID(std::string guid) {
  ResetSelectedAdapter();

  const auto adapter = GetAdapterFromGUID(guid);

  if (adapter.has_value()) {
    UpdateNetworkInterface(adapter.value());
  }

  XELOGI(GetSelectedAdapterDesciption());
}

std::string NetworkAdapterManager::GetSelectedAdapaterGUID() const {
  return cvars::network_guid;
}

std::string NetworkAdapterManager::GetSelectedAdapterDesciption() const {
  return fmt::format("Network Interface: {} {} {}", GetSelectedAdapterName(),
                     GetSelectedAdapterLocalIPString(),
                     is_WAN_routing_ ? "(WAN Routing)" : "(Non-WAN Routing)");
}

std::optional<NetworkAdapter> NetworkAdapterManager::GetAdapterFromGUID(
    std::string guid) const {
  const auto found_adapter = std::find_if(
      adapter_addresses_.cbegin(), adapter_addresses_.cend(),
      [guid](const NetworkAdapter& adapter) { return guid == adapter.guid; });

  std::optional<NetworkAdapter> adapter = std::nullopt;

  if (found_adapter != adapter_addresses_.cend()) {
    adapter = *found_adapter;
  }

  return adapter;
}

std::optional<NetworkAdapter> NetworkAdapterManager::GetAdapterFromIfIndex(
    const int32_t IfIndex) const {
  const auto found_adapter =
      std::find_if(adapter_addresses_.cbegin(), adapter_addresses_.cend(),
                   [IfIndex](const NetworkAdapter& adapter) {
                     return static_cast<int32_t>(adapter.if_index) == IfIndex;
                   });

  std::optional<NetworkAdapter> adapter = std::nullopt;

  if (found_adapter != adapter_addresses_.cend()) {
    adapter = *found_adapter;
  }

  return adapter;
}

std::string NetworkAdapterManager::GetAdapterFriendlyName(
    const NetworkAdapter& adapter) const {
  return adapter.friendly_name;
}

std::vector<std::string> NetworkAdapterManager::GetAdaptersNames() const {
  std::vector<std::string> adapter_names = {};

  for (const auto& adapter : adapter_addresses_) {
    adapter_names.push_back(GetAdapterFriendlyName(adapter));
  }

  return adapter_names;
};

MacAddress NetworkAdapterManager::GetAdapterMacAddressFromGUID(
    std::string guid) const {
  const auto adapter = GetAdapterFromGUID(guid);

  if (adapter.has_value() &&
      adapter->physical_address_length == MacAddress::MacAddressSize) {
    return adapter->physical_address;
  } else {
    // If there are no adapters generate a mac address.
    return GenerateMacAddress();
  }
}

std::optional<NetworkAdapter> NetworkAdapterManager::GetSelectedAdapter()
    const {
  return GetAdapterFromGUID(cvars::network_guid);
}

std::string NetworkAdapterManager::GetSelectedAdapterName() const {
  const auto adapter = GetSelectedAdapter();
  return adapter.has_value() ? GetAdapterFriendlyName(adapter.value()) : "";
}

std::string NetworkAdapterManager::GetSelectedAdapterLocalIPString() const {
  return ip_to_string(local_ip_);
}

bool NetworkAdapterManager::IsInterfaceSelected() const {
  return local_ip_.sin_addr.s_addr;
}

void NetworkAdapterManager::ResetSelectedAdapter() {
  local_ip_ = {};
  is_WAN_routing_ = false;
  OVERRIDE_string(network_guid, "");
}

#ifdef XE_PLATFORM_WIN32

std::vector<NetworkAdapter> NetworkAdapterManager::DiscoverNetworkAdapters() {
  XELOGI("Discovering network interfaces...");

  uint32_t ret = 0;
  ULONG buffer_length = 0;

  std::vector<NetworkAdapter> adapter_addresses = {};

  const uint32_t flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                         GAA_FLAG_SKIP_DNS_SERVER;

  ret = GetAdaptersAddresses(AF_INET, flags, 0, 0, &buffer_length);

  if (ret != ERROR_BUFFER_OVERFLOW) {
    return adapter_addresses;
  }

  std::vector<uint8_t> buffer(buffer_length);

  IP_ADAPTER_ADDRESSES* adapters_ptr =
      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

  ret = GetAdaptersAddresses(AF_INET, flags, 0, adapters_ptr, &buffer_length);

  if (ret != NO_ERROR) {
    return adapter_addresses;
  }

  std::string networks = "Network Interfaces:\n";

  for (IP_ADAPTER_ADDRESSES* adapter_ptr = adapters_ptr; adapter_ptr != nullptr;
       adapter_ptr = adapter_ptr->Next) {
    if (adapter_ptr->OperStatus == IfOperStatusUp &&
        (adapter_ptr->IfType == IF_TYPE_IEEE80211 ||
         adapter_ptr->IfType == IF_TYPE_ETHERNET_CSMACD ||
         adapter_ptr->IfType == IF_TYPE_PROP_VIRTUAL ||
         adapter_ptr->IfType == IF_TYPE_TUNNEL)) {
      for (PIP_ADAPTER_UNICAST_ADDRESS_LH adapter_address =
               adapter_ptr->FirstUnicastAddress;
           adapter_address != nullptr;
           adapter_address = adapter_address->Next) {
        sockaddr_in addr_ptr = *reinterpret_cast<sockaddr_in*>(
            adapter_address->Address.lpSockaddr);

        if (addr_ptr.sin_family != AF_INET) {
          continue;
        }

        NetworkAdapter adapter = {};
        adapter.if_index = adapter_ptr->IfIndex;
        adapter.guid = adapter_ptr->AdapterName;
        adapter.friendly_name =
            xe::to_utf8(reinterpret_cast<char16_t*>(adapter_ptr->FriendlyName));
        adapter.physical_address_length = std::min<uint32_t>(
            adapter_ptr->PhysicalAddressLength, MacAddress::MacAddressSize);
        std::memcpy(adapter.physical_address, adapter_ptr->PhysicalAddress,
                    adapter.physical_address_length);
        adapter.address = addr_ptr;

        networks += fmt::format("{} {}: {}\n", adapter.friendly_name,
                                adapter.guid, ip_to_string(addr_ptr));

        adapter_addresses.push_back(adapter);
      }
    }
  }

  if (adapter_addresses.empty()) {
    XELOGI("No network interfaces detected!\n");
  } else {
    XELOGI("Found {} network interfaces!\n", adapter_addresses.size());
    XELOGI("{}", xe::string_util::trim(networks));
  }

  return adapter_addresses;
}

// Get the index of the best interface to reach that WAN address
int32_t NetworkAdapterManager::GetBestInterfaceIfIndex() {
  const in_addr destAddr = ip_to_in_addr("8.8.8.8");

  DWORD bestIfIndex = -1;
  DWORD result = GetBestInterface(destAddr.S_un.S_addr, &bestIfIndex);

  if (result != NO_ERROR) {
    XELOGI("Error finding best interface: {}", result);
  }

  return bestIfIndex;
}

#else

std::vector<NetworkAdapter> NetworkAdapterManager::DiscoverNetworkAdapters() {
  XELOGI("Discovering network interfaces...");

  std::vector<NetworkAdapter> adapter_addresses = {};

  ifaddrs* interfaces = nullptr;

  if (getifaddrs(&interfaces) != 0 || !interfaces) {
    XELOGI("No network interfaces detected!\n");
    return adapter_addresses;
  }

  // The hardware address arrives as its own AF_PACKET entry, separate from the
  // AF_INET one, so collect those first and attach them by interface name.
  std::unordered_map<std::string, std::vector<uint8_t> > mac_addresses;

  for (ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
    if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_PACKET) {
      continue;
    }

    const sockaddr_ll* link =
        reinterpret_cast<const sockaddr_ll*>(entry->ifa_addr);

    if (link->sll_halen != MacAddress::MacAddressSize) {
      continue;
    }

    mac_addresses[entry->ifa_name] =
        std::vector<uint8_t>(link->sll_addr, link->sll_addr + link->sll_halen);
  }

  std::string networks = "Network Interfaces:\n";

  for (ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
    if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) {
      continue;
    }

    // Windows keeps interfaces that are operationally up and not loopback;
    // IFF_RUNNING is the carrier, which is what OperStatus reports.
    if (!(entry->ifa_flags & IFF_UP) || !(entry->ifa_flags & IFF_RUNNING) ||
        (entry->ifa_flags & IFF_LOOPBACK)) {
      continue;
    }

    NetworkAdapter adapter = {};
    adapter.if_index = if_nametoindex(entry->ifa_name);
    adapter.guid = entry->ifa_name;
    adapter.friendly_name = entry->ifa_name;
    adapter.address = *reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);

    const auto mac = mac_addresses.find(entry->ifa_name);
    if (mac != mac_addresses.cend()) {
      adapter.physical_address_length =
          static_cast<uint32_t>(mac->second.size());
      std::memcpy(adapter.physical_address, mac->second.data(),
                  adapter.physical_address_length);
    }

    networks += fmt::format("{} {}: {}\n", adapter.friendly_name, adapter.guid,
                            ip_to_string(adapter.address));

    adapter_addresses.push_back(adapter);
  }

  freeifaddrs(interfaces);

  if (adapter_addresses.empty()) {
    XELOGI("No network interfaces detected!\n");
  } else {
    XELOGI("Found {} network interfaces!\n", adapter_addresses.size());
    XELOGI("{}", xe::string_util::trim(networks));
  }

  return adapter_addresses;
}

// Get the index of the best interface to reach that WAN address. There is no
// GetBestInterface here, so ask the kernel's routing table the only way a
// user-space process can: connect a UDP socket, which picks a route without
// sending anything, and see which local address it chose.
int32_t NetworkAdapterManager::GetBestInterfaceIfIndex() {
  native_socket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

  if (sock == kInvalidSocket) {
    XELOGI("Error finding best interface: no socket");
    return -1;
  }

  sockaddr_in remote_addr{};
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = htons(53);
  remote_addr.sin_addr = ip_to_in_addr("8.8.8.8");

  int32_t best_if_index = -1;

  if (connect(sock, reinterpret_cast<sockaddr*>(&remote_addr),
              sizeof(remote_addr)) != kSocketError) {
    sockaddr_in local_addr{};
    socklen_t local_addr_length = sizeof(local_addr);

    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local_addr),
                    &local_addr_length) != kSocketError) {
      for (const auto& adapter : adapter_addresses_) {
        if (adapter.address.sin_addr.s_addr == local_addr.sin_addr.s_addr) {
          best_if_index = static_cast<int32_t>(adapter.if_index);
          break;
        }
      }
    }
  }

  CloseSocket(sock);

  if (best_if_index == -1) {
    XELOGI("Error finding best interface: no route to 8.8.8.8");
  }

  return best_if_index;
}

#endif  // XE_PLATFORM_WIN32

bool NetworkAdapterManager::UpdateNetworkInterface(
    const NetworkAdapter& adapter) {
  const std::optional<sockaddr_in> adapter_addr =
      GetInterfaceIPFromIfIndex(adapter.if_index);

  bool updated = adapter_addr.has_value();

  if (updated) {
    local_ip_ = adapter_addr.value();
    is_WAN_routing_ = IsInterfaceWANRouting(adapter_addr.value());
    OVERRIDE_string(network_guid, adapter.guid);
  }

  return updated;
}

bool NetworkAdapterManager::IsInterfaceWANRouting(sockaddr_in interface_addr) {
  native_socket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

  if (sock == kInvalidSocket) {
    return false;
  }

  // Force socket to use interface address for connection attempt.
  if (bind(sock, reinterpret_cast<sockaddr*>(&interface_addr),
           sizeof(sockaddr)) == kSocketError) {
    CloseSocket(sock);
    return false;
  }

#ifdef XE_PLATFORM_WIN32
  int timeout = 3000;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout,
             sizeof(timeout));
#else
  // SO_SNDTIMEO is a timeval here, not milliseconds.
  timeval timeout{};
  timeout.tv_sec = 3;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

  sockaddr_in remoteAddr{};
  remoteAddr.sin_family = AF_INET;
  remoteAddr.sin_port = htons(53);
  remoteAddr.sin_addr = ip_to_in_addr("8.8.8.8");  // Google DNS

  bool success = connect(sock, reinterpret_cast<sockaddr*>(&remoteAddr),
                         sizeof(remoteAddr)) != kSocketError;

  CloseSocket(sock);

  return success;
}

std::optional<sockaddr_in> NetworkAdapterManager::GetInterfaceIPFromIfIndex(
    int32_t IfIndex) const {
  if (IfIndex < 0) {
    return std::nullopt;
  }

  for (const auto& adapter : adapter_addresses_) {
    if (static_cast<int32_t>(adapter.if_index) == IfIndex &&
        adapter.address.sin_family == AF_INET) {
      return adapter.address;
    }
  }

  return std::nullopt;
}

// Select our saved network GUID if it's available.
bool NetworkAdapterManager::SelectSavedNetworkAdapter() {
  const std::optional<NetworkAdapter> adapter = GetSelectedAdapter();

  if (adapter.has_value()) {
    return UpdateNetworkInterface(adapter.value());
  } else {
    XELOGI("Interface GUID: {} not found!", cvars::network_guid);
  }

  return false;
}

// Select saved network GUID interface if available, otherwise fallback to best
// interface IfIndex.
void NetworkAdapterManager::AutoSelectNetworkAdapter(
    int32_t best_interface_IfIndex) {
  bool selected = SelectSavedNetworkAdapter();

  // Fallback to best interface.
  if (!selected) {
    const auto adapter = GetAdapterFromIfIndex(best_interface_IfIndex);

    if (adapter.has_value()) {
      selected = UpdateNetworkInterface(adapter.value());
    }
  }

  if (selected) {
    XELOGI(GetSelectedAdapterDesciption());
  } else {
    XELOGI("Unspecified Network Interface!");
  }
}

}  // namespace kernel
}  // namespace xe
