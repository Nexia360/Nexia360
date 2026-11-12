/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NETWORK_ADAPTER_MANAGER
#define XENIA_KERNEL_XAM_NETWORK_ADAPTER_MANAGER

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xenia/kernel/util/net_utils.h"

namespace xe {
namespace kernel {

// One interface as this emulator cares about it. Windows fills this from
// IP_ADAPTER_ADDRESSES and Linux from getifaddrs, so nothing above this struct
// sees an OS type. It owns its strings: the Win32 structure points into a
// buffer that is thrown away on the next discovery pass.
struct NetworkAdapter {
  uint32_t if_index = 0;
  // The stable name the selection is saved under (cvar network_guid). Windows
  // uses the adapter GUID, Linux the interface name.
  std::string guid;
  std::string friendly_name;
  uint8_t physical_address[MacAddress::MacAddressSize] = {};
  uint32_t physical_address_length = 0;
  sockaddr_in address = {};
};

class NetworkAdapterManager {
 public:
  NetworkAdapterManager();

  void Initialize();

  void SelectBestInterface();

  void SetSelectedAdapterGUID(const std::string guid);

  std::string GetSelectedAdapaterGUID() const;

  std::string GetSelectedAdapterDesciption() const;

  std::optional<NetworkAdapter> GetAdapterFromGUID(
      const std::string guid) const;

  std::optional<NetworkAdapter> GetAdapterFromIfIndex(int32_t IfIndex) const;

  std::string GetAdapterFriendlyName(const NetworkAdapter& adapter) const;

  std::vector<std::string> GetAdaptersNames() const;

  MacAddress GetAdapterMacAddressFromGUID(const std::string guid) const;

  std::optional<NetworkAdapter> GetSelectedAdapter() const;

  std::string GetSelectedAdapterName() const;

  sockaddr_in GetSelectedAdapterLocalIP() const { return local_ip_; }

  std::string GetSelectedAdapterLocalIPString() const;

  const std::vector<NetworkAdapter>& GetAdapters() const {
    return adapter_addresses_;
  }

  bool IsSelectedAdapterWANRouting() const { return is_WAN_routing_; }

  bool IsInterfaceSelected() const;

 private:
  void ResetSelectedAdapter();

  std::vector<NetworkAdapter> DiscoverNetworkAdapters();

  bool UpdateNetworkInterface(const NetworkAdapter& adapter);

  int32_t GetBestInterfaceIfIndex();

  bool IsInterfaceWANRouting(const sockaddr_in interface_addr);

  std::optional<sockaddr_in> GetInterfaceIPFromIfIndex(
      const int32_t IfIndex) const;

  bool SelectSavedNetworkAdapter();

  void AutoSelectNetworkAdapter(const int32_t best_interface_IfIndex);

  std::vector<NetworkAdapter> adapter_addresses_;

  int32_t best_interface_IfIndex_ = -1;

  sockaddr_in local_ip_ = {};

  bool is_WAN_routing_ = false;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NETWORK_ADAPTER_MANAGER
