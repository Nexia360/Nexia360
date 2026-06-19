/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <future>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>

#include <third_party/miniupnp/miniupnpc/include/miniupnpc.h>

#include "xenia/base/threading_timer_queue.h"

namespace xe {
namespace kernel {

class UPnP {
 public:
  UPnP();
  ~UPnP();

  void Initialize();

  void SearchUPnP();

  bool is_active() const { return active_; }

  // internal port is in BE notation.
  uint32_t AddPort(std::string_view addr, uint16_t internal_port,
                   std::string_view protocol);

  // internal port is in BE notation.
  void RemovePort(uint16_t internal_port, std::string_view protocol);

  void RefreshPorts(std::string_view addr);

  void AddMappedConnectPort(uint16_t port, uint16_t mapped_port) {
    mapped_connect_ports_.insert({port, mapped_port});
  }

  void AddMappedBindPort(uint16_t port, uint16_t mapped_port) {
    mapped_bind_ports_.insert({port, mapped_port});
  }

  uint16_t GetMappedConnectPort(uint16_t port);

  uint16_t GetMappedBindPort(uint16_t external_port);

  std::map<std::string, std::map<uint16_t, int32_t>>* port_binding_results() {
    return &port_binding_results_;
  };

  const bool GetRefreshedUnauthorized() const;

  void SetRefreshedUnauthorized(const bool refreshed);

  static const std::string GetLocalIP();

  // === Xenia-Canary menu/UI compatibility shims ===
  // This fork's UPnP menu (xam_ui.cc), xam_net/xsession, and the Emulator call
  // these; they are mapped onto the Nexia UPnP implementation above.
  bool IsActive() const { return active_; }
  bool IsVariableLeaseSupported() const { return leases_supported_; }
  void Start() {
    if (!active_) {
      Initialize();
    }
  }
  void TrackPort(uint16_t port, std::string protocol);
  void OpenTrackedPorts();
  void CloseOpenPorts();
  void RefreshPorts();  // no-arg overload; uses GetLocalIP()
  std::future<int32_t> AddPortAsync(std::string addr, uint16_t internal_port,
                                    std::string protocol);
  std::future<int32_t> RemovePortAsync(uint16_t port, std::string protocol);
  const std::map<std::string, std::map<uint16_t, uint16_t>> GetOpenedPorts();
  const std::map<std::string, std::map<uint16_t, int32_t>>
  GetPortBindingResults();
  const std::map<std::string, std::set<uint16_t>> GetTrackedPorts();
  static void SetUPnPState(bool upnp_state);
  static std::string_view GetMiniUPnPcErrorCodeToDesc(int32_t error) noexcept;
  static std::string_view GetUPnPErrorCodeToDesc(int32_t error) noexcept;
  static std::string GetLocalIP_wget();

 private:
  // https://openconnectivity.org/developer/specifications/upnp-resources/upnp/internet-gateway-device-igd-v-2-0/
  // http://upnp.org/specs/gw/UPnP-gw-WANIPConnection-v2-Service.pdf
  enum UPnPErrorCodes : int { OnlyPermanentLeasesSupported = 725 };

  typedef std::map<uint16_t, uint16_t> port_binding;

  void RemovePortExternal(uint16_t external_port, std::string_view protocol,
                          bool verbose = true);
  void RefreshPortsTimer();

  bool LoadSavedUPnPDevice();
  const UPNPDev* DiscoverUPnPDevice();
  const UPNPDev* GetDeviceByName(const UPNPDev* device_list,
                                 std::string device_name);
  bool GetAndParseUPnPXmlData(std::string url);

  std::shared_mutex mutex_;
  std::atomic<bool> active_ = false;
  std::atomic<bool> leases_supported_ = true;
  std::atomic<bool> refreshed_unauthorized_ = false;

  IGDdatas* igd_data_ = new IGDdatas();
  UPNPUrls* igd_urls_ = new UPNPUrls();

  std::weak_ptr<xe::threading::TimerQueueWaitItem> wait_item_;

  std::map<std::string, port_binding> port_bindings_;
  std::map<std::string, std::map<uint16_t, int32_t>> port_binding_results_;

  // Canary-menu compatibility: tracked (not-yet-opened) ports, protocol -> set.
  std::mutex tracked_ports_mutex_;
  std::map<std::string, std::set<uint16_t>> tracked_ports_;

  port_binding mapped_connect_ports_;
  port_binding mapped_bind_ports_;
};
}  // namespace kernel
}  // namespace xe
