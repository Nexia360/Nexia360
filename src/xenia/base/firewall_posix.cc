/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/firewall.h"

namespace xe {
namespace firewall {

// No host firewall is manipulated on POSIX. Distributions vary too much
// (ufw, firewalld, raw nftables, none at all) for an automatic rule to be
// anything but a surprise, and none of them share a single elevation prompt
// the way UAC does.

RuleState QueryInboundRules() { return RuleState::kUnsupported; }

bool RequestInboundRules(std::string* error_message) {
  if (error_message) {
    *error_message = "Automatic firewall setup is only supported on Windows.";
  }
  return false;
}

bool EnsureInboundRules(std::string* error_message) { return false; }

}  // namespace firewall
}  // namespace xe
