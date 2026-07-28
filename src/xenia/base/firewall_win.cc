/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/firewall.h"

#include "xenia/base/platform_win.h"

#include <netfw.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"

namespace xe {
namespace firewall {

namespace {

constexpr const wchar_t* kRuleNameTcp = L"Nexia360 (TCP-In)";
constexpr const wchar_t* kRuleNameUdp = L"Nexia360 (UDP-In)";

std::wstring NormalizePath(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t c) { return std::towlower(c); });
  return value;
}

// Scoped CoInitializeEx that tolerates the apartment already being
// initialized - the UI thread has usually done so already, and treating that
// as an error would make the query fail for no reason.
class ComScope {
 public:
  ComScope() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ok_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    // Only balance a call that actually initialized this thread.
    owns_ = SUCCEEDED(hr);
  }
  ~ComScope() {
    if (owns_) {
      CoUninitialize();
    }
  }
  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
  bool owns_ = false;
};

}  // namespace

RuleState QueryInboundRules() {
  const auto exe_path = xe::filesystem::GetExecutablePath();
  if (exe_path.empty()) {
    return RuleState::kUnknown;
  }
  const std::wstring wanted = NormalizePath(exe_path.wstring());

  ComScope com;
  if (!com.ok()) {
    return RuleState::kUnknown;
  }

  INetFwPolicy2* policy = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(INetFwPolicy2),
                                reinterpret_cast<void**>(&policy));
  if (FAILED(hr) || !policy) {
    // No Windows Firewall service, or a third-party product in its place.
    return RuleState::kUnsupported;
  }

  INetFwRules* rules = nullptr;
  hr = policy->get_Rules(&rules);
  if (FAILED(hr) || !rules) {
    policy->Release();
    return RuleState::kUnknown;
  }

  IUnknown* enumerator_unk = nullptr;
  hr = rules->get__NewEnum(&enumerator_unk);
  IEnumVARIANT* enumerator = nullptr;
  if (SUCCEEDED(hr) && enumerator_unk) {
    hr = enumerator_unk->QueryInterface(__uuidof(IEnumVARIANT),
                                        reinterpret_cast<void**>(&enumerator));
    enumerator_unk->Release();
  }

  if (FAILED(hr) || !enumerator) {
    rules->Release();
    policy->Release();
    return RuleState::kUnknown;
  }

  bool has_tcp = false;
  bool has_udp = false;

  VARIANT item;
  VariantInit(&item);
  ULONG fetched = 0;

  while (!(has_tcp && has_udp) &&
         enumerator->Next(1, &item, &fetched) == S_OK && fetched == 1) {
    INetFwRule* rule = nullptr;
    if (item.vt == VT_DISPATCH && item.pdispVal &&
        SUCCEEDED(item.pdispVal->QueryInterface(
            __uuidof(INetFwRule), reinterpret_cast<void**>(&rule))) &&
        rule) {
      BSTR app = nullptr;
      NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_IN;
      NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
      VARIANT_BOOL enabled = VARIANT_FALSE;
      LONG protocol = 0;

      if (SUCCEEDED(rule->get_ApplicationName(&app)) && app) {
        rule->get_Direction(&direction);
        rule->get_Action(&action);
        rule->get_Enabled(&enabled);
        rule->get_Protocol(&protocol);

        // Only an enabled inbound ALLOW for this exact executable counts.
        // A disabled or blocking rule is worse than none: it would make us
        // report "present" while the traffic is still dropped.
        if (direction == NET_FW_RULE_DIR_IN &&
            action == NET_FW_ACTION_ALLOW && enabled != VARIANT_FALSE &&
            NormalizePath(std::wstring(app, SysStringLen(app))) == wanted) {
          if (protocol == NET_FW_IP_PROTOCOL_TCP) {
            has_tcp = true;
          } else if (protocol == NET_FW_IP_PROTOCOL_UDP) {
            has_udp = true;
          } else if (protocol == NET_FW_IP_PROTOCOL_ANY) {
            has_tcp = true;
            has_udp = true;
          }
        }
      }

      if (app) {
        SysFreeString(app);
      }
      rule->Release();
    }

    VariantClear(&item);
    VariantInit(&item);
    fetched = 0;
  }

  VariantClear(&item);
  enumerator->Release();
  rules->Release();
  policy->Release();

  // Netplay needs both. Half the pair present still means inbound traffic is
  // being dropped on the other protocol.
  return (has_tcp && has_udp) ? RuleState::kPresent : RuleState::kMissing;
}

bool RequestInboundRules(std::string* error_message) {
  const auto exe_path = xe::filesystem::GetExecutablePath();
  if (exe_path.empty()) {
    if (error_message) {
      *error_message = "Could not determine the executable path.";
    }
    return false;
  }

  const std::wstring exe = exe_path.wstring();

  // Delete any stale rules of the same name first: the executable may have
  // moved, which would otherwise leave a rule pointing at the old path and a
  // duplicate pointing at the new one. Both protocols go in one elevated
  // invocation so the user is prompted exactly once.
  //
  // Unquoted at the outer level on purpose - cmd treats '&' as the separator
  // and would otherwise strip the surrounding quotes and mangle the inner
  // ones.
  std::wstring parameters =
      L"/c netsh advfirewall firewall delete rule name=\"" +
      std::wstring(kRuleNameTcp) + L"\" >nul 2>&1 & " +
      L"netsh advfirewall firewall delete rule name=\"" +
      std::wstring(kRuleNameUdp) + L"\" >nul 2>&1 & " +
      L"netsh advfirewall firewall add rule name=\"" +
      std::wstring(kRuleNameTcp) + L"\" dir=in action=allow program=\"" + exe +
      L"\" enable=yes profile=any protocol=TCP & " +
      L"netsh advfirewall firewall add rule name=\"" +
      std::wstring(kRuleNameUdp) + L"\" dir=in action=allow program=\"" + exe +
      L"\" enable=yes profile=any protocol=UDP";

  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  info.lpVerb = L"runas";  // triggers the UAC prompt
  info.lpFile = L"cmd.exe";
  info.lpParameters = parameters.c_str();
  info.nShow = SW_HIDE;

  if (!ShellExecuteExW(&info)) {
    const DWORD err = GetLastError();
    if (error_message) {
      *error_message = (err == ERROR_CANCELLED)
                           ? "Elevation was declined."
                           : fmt::format("Failed to elevate ({}).", err);
    }
    XELOGW("Firewall rule creation not elevated (error {})", err);
    return false;
  }

  if (info.hProcess) {
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);

    if (exit_code != 0) {
      if (error_message) {
        *error_message = fmt::format("netsh failed (exit code {}).", exit_code);
      }
      XELOGE("Firewall rule creation failed with exit code {}", exit_code);
      return false;
    }
  }

  XELOGI("Firewall inbound rules created for {}", xe::path_to_utf8(exe_path));
  return true;
}

bool EnsureInboundRules(std::string* error_message) {
  const RuleState state = QueryInboundRules();

  switch (state) {
    case RuleState::kPresent:
      return true;
    case RuleState::kUnsupported:
    case RuleState::kUnknown:
      // Do not prompt for elevation on a guess.
      XELOGD("Skipping firewall setup - state could not be established");
      return false;
    case RuleState::kMissing:
      break;
  }

  return RequestInboundRules(error_message);
}

}  // namespace firewall
}  // namespace xe
