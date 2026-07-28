/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_FIREWALL_H_
#define XENIA_BASE_FIREWALL_H_

#include <string>

namespace xe {
namespace firewall {

enum class RuleState {
  // No host firewall to speak of, or we have no way to inspect it.
  kUnsupported,
  // Inbound rules covering this executable are present and enabled.
  kPresent,
  // The firewall is running and has no inbound rules for this executable.
  kMissing,
  // The firewall could not be queried - service stopped, COM failure, or a
  // third-party firewall replacing the built-in one. Treated as "leave it
  // alone": guessing wrong here means nagging the user for elevation on
  // every launch.
  kUnknown,
};

// Inbound-rule state for the running executable. Read-only, no elevation.
RuleState QueryInboundRules();

// Adds inbound TCP and UDP allow rules for the running executable. Elevates
// via UAC - the user sees the prompt and can decline, which is reported as
// failure rather than retried. Blocks until the elevated helper exits.
// Both protocols are added in a single elevated invocation so the user is
// prompted once, not twice.
bool RequestInboundRules(std::string* error_message);

// Convenience: query, and only prompt when rules are genuinely missing.
// Returns true when rules are present afterwards. Never prompts on
// kUnsupported or kUnknown.
bool EnsureInboundRules(std::string* error_message);

}  // namespace firewall
}  // namespace xe

#endif  // XENIA_BASE_FIREWALL_H_
