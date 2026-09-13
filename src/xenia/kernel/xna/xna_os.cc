/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xna/xna_os.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/keyboard_ui.h"

namespace xe {
namespace kernel {
namespace xna {

namespace {

// Everything below can be called before a title is running - the managed side
// builds its SignedInGamers collection early - so nothing here may assume the
// kernel, xam or a profile exists.
xam::ProfileManager* profile_manager() {
  auto* state = kernel_state();
  if (!state || !state->xam_state()) {
    return nullptr;
  }
  return state->xam_state()->profile_manager();
}

void CopyUtf8(char* buffer, size_t capacity, const std::string& value) {
  if (!buffer || !capacity) {
    return;
  }
  const size_t length = std::min(value.size(), capacity - 1);
  std::memcpy(buffer, value.data(), length);
  buffer[length] = '\0';
}

uint32_t GetUserSlotCount() { return 4; }

int32_t GetUser(uint32_t slot, XnaOsUser* out) {
  if (!out || slot >= GetUserSlotCount()) {
    return 1;
  }
  std::memset(out, 0, sizeof(*out));

  auto* profiles = profile_manager();
  auto* profile =
      profiles ? profiles->GetProfile(static_cast<uint8_t>(slot)) : nullptr;
  if (!profile) {
    // An empty slot is a normal answer, not a failure: XNA asks about all four
    // and expects the unoccupied ones to say nobody is there.
    out->signin_state = kXnaOsNotSignedIn;
    return 0;
  }

  out->xuid = profile->xuid();
  out->online_xuid = profile->GetOnlineXUID();
  out->signin_state = static_cast<uint32_t>(profile->signin_state());
  out->is_live_enabled = profile->IsLiveEnabled() ? 1 : 0;
  out->is_guest = 0;
  out->country = profile->GetCountry();
  out->language = profile->GetLanguage();
  CopyUtf8(out->gamertag, sizeof(out->gamertag), profile->name());
  return 0;
}

uint32_t GetTitleId() {
  auto* state = kernel_state();
  if (!state) {
    return 0;
  }
  // KernelState reads this from the running executable module, and an XNA
  // title never loads one - the emulator's copy is the one that was set from
  // the package.
  const uint32_t from_kernel = state->title_id();
  if (from_kernel) {
    return from_kernel;
  }
  return state->emulator() ? state->emulator()->title_id() : 0;
}

uint32_t GetTitleName(char* buffer, uint32_t capacity) {
  auto* state = kernel_state();
  const std::string name =
      state && state->emulator() ? state->emulator()->title_name() : "";
  if (buffer && capacity) {
    CopyUtf8(buffer, capacity, name);
  }
  return static_cast<uint32_t>(name.size() + 1);
}

xam::ContentManager* content_manager() {
  auto* state = kernel_state();
  if (!state || !state->xam_state()) {
    return nullptr;
  }
  return state->xam_state()->content_manager();
}

// A container name comes from the title and lands on a real filesystem, so it
// has to survive the trip: Windows rejects <>:"/\|?* and any name ending in a
// dot or a space, and a name containing "/" or ".." would escape the profile.
std::string SanitizeContainerName(const char* display_name) {
  constexpr size_t kMaxLength = 64;
  std::string result;
  if (display_name) {
    for (const char* p = display_name; *p && result.size() < kMaxLength; ++p) {
      const unsigned char c = static_cast<unsigned char>(*p);
      const bool safe = c >= 0x20 && c < 0x7F &&
                        !std::strchr("<>:\"/\\|?*", static_cast<char>(c));
      result.push_back(safe ? static_cast<char>(c) : '_');
    }
  }
  while (!result.empty() && (result.back() == '.' || result.back() == ' ')) {
    result.pop_back();
  }
  return result.empty() ? std::string("SaveData") : result;
}

uint32_t ResolveStorageContainer(uint32_t slot, const char* display_name,
                                 char* buffer, uint32_t capacity) {
  auto* content = content_manager();
  if (!content || slot >= GetUserSlotCount()) {
    return 0;
  }

  auto* profiles = profile_manager();
  auto* profile =
      profiles ? profiles->GetProfile(static_cast<uint8_t>(slot)) : nullptr;
  // No profile in the slot still gets a home, under xuid 0 - a title that saves
  // without anyone signed in should not simply fail.
  const uint64_t xuid = profile ? profile->xuid() : 0;

  const auto path = content->ResolveGameUserContentPath(xuid) /
                    xe::to_path(SanitizeContainerName(display_name));

  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    XELOGE("XnaOs: could not create storage container {}: {}",
           xe::path_to_utf8(path), ec.message());
    return 0;
  }

  const std::string utf8 = xe::path_to_utf8(path);
  if (buffer && capacity) {
    CopyUtf8(buffer, capacity, utf8);
  }
  return static_cast<uint32_t>(utf8.size() + 1);
}

uint32_t GetStorageDeviceName(char* buffer, uint32_t capacity) {
  static const std::string kName = "Hard Drive";
  if (buffer && capacity) {
    CopyUtf8(buffer, capacity, kName);
  }
  return static_cast<uint32_t>(kName.size() + 1);
}

uint64_t GetStorageTotalSpace() {
  auto* content = content_manager();
  return content ? content->GetContentTotalSpace() : 0;
}

uint64_t GetStorageFreeSpace() {
  auto* content = content_manager();
  return content ? content->GetContentFreeSpace() : 0;
}

// ---- guide -----------------------------------------------------------------

// A dialog's outcome is written by the UI thread when it closes and read by the
// title's thread when it polls, so the state lives here rather than on the
// dialog - which deletes itself the moment it is done.
struct GuideRequest {
  int32_t status = kXnaOsGuidePending;
  int32_t button = -1;
  std::string text;
};

std::mutex guide_mutex;
std::map<uint32_t, GuideRequest> guide_requests;
uint32_t next_guide_request = 1;

xe::ui::ImGuiDrawer* imgui_drawer() {
  auto* state = kernel_state();
  auto* emulator = state ? state->emulator() : nullptr;
  return emulator ? emulator->imgui_drawer() : nullptr;
}

uint32_t NewGuideRequest() {
  std::lock_guard<std::mutex> lock(guide_mutex);
  const uint32_t id = next_guide_request++;
  guide_requests[id] = GuideRequest();
  return id;
}

void FinishGuideRequest(uint32_t id, int32_t status, int32_t button,
                        std::string text) {
  std::lock_guard<std::mutex> lock(guide_mutex);
  auto it = guide_requests.find(id);
  if (it == guide_requests.end()) {
    // Released while the dialog was still up - the title gave up on it.
    return;
  }
  it->second.status = status;
  it->second.button = button;
  it->second.text = std::move(text);
}

uint32_t GuideIsVisible() {
  auto* state = kernel_state();
  if (!state || !state->xam_state()) {
    return 0;
  }
  return state->xam_state()->IsUIActive() ? 1 : 0;
}

// Returns 0 if there is nowhere to draw, which is a real possibility during
// shutdown and should not take the title down with it.
void ShowSystemUi(bool shown) {
  auto* state = kernel_state();
  if (!state || !state->xam_state()) {
    return;
  }
  if (shown) {
    state->xam_state()->xam_dialogs_shown_++;
  } else {
    state->xam_state()->xam_dialogs_shown_--;
  }
}

uint32_t GuideBeginMessageBox(uint32_t slot, const char* title, const char* text,
                              const char* const* buttons, uint32_t button_count,
                              uint32_t focus_button) {
  auto* drawer = imgui_drawer();
  if (!drawer) {
    return 0;
  }

  std::vector<std::string> button_labels;
  for (uint32_t i = 0; i < button_count && buttons; ++i) {
    button_labels.emplace_back(buttons[i] ? buttons[i] : "");
  }
  if (button_labels.empty()) {
    button_labels.emplace_back("OK");
  }

  // MessageBoxDialog takes its strings by non-const reference and may rewrite
  // an empty title, so they cannot be temporaries.
  std::string title_string = title ? title : "";
  std::string text_string = text ? text : "";

  const uint32_t id = NewGuideRequest();
  auto* dialog = new xam::MessageBoxDialog(drawer, title_string, text_string,
                                           button_labels, focus_button);
  ShowSystemUi(true);
  // Read the answer inside the callback: the dialog destroys itself right
  // after, so there is no later moment to ask.
  dialog->set_close_callback([id, dialog]() {
    ShowSystemUi(false);
    FinishGuideRequest(id, kXnaOsGuideCompleted,
                       static_cast<int32_t>(dialog->chosen_button()),
                       std::string());
  });
  return id;
}

uint32_t GuideBeginKeyboard(uint32_t slot, const char* title,
                            const char* description, const char* default_text,
                            uint32_t max_length) {
  return XnaGuideBeginKeyboard(slot, title, description, default_text,
                               max_length, false);
}

int32_t GuidePoll(uint32_t request, int32_t* out_button,
                  uint32_t* out_text_bytes) {
  std::lock_guard<std::mutex> lock(guide_mutex);
  auto it = guide_requests.find(request);
  if (it == guide_requests.end()) {
    return kXnaOsGuideUnknownRequest;
  }
  if (out_button) {
    *out_button = it->second.button;
  }
  if (out_text_bytes) {
    *out_text_bytes = static_cast<uint32_t>(it->second.text.size() + 1);
  }
  return it->second.status;
}

uint32_t GuideGetText(uint32_t request, char* buffer, uint32_t capacity) {
  std::lock_guard<std::mutex> lock(guide_mutex);
  auto it = guide_requests.find(request);
  if (it == guide_requests.end()) {
    return 0;
  }
  if (buffer && capacity) {
    CopyUtf8(buffer, capacity, it->second.text);
  }
  return static_cast<uint32_t>(it->second.text.size() + 1);
}

void Log(uint32_t level, const char* message) {
  if (!message) {
    return;
  }
  // Prefixed so a managed line is obvious next to the emulator's own.
  switch (level) {
    case 0:
      XELOGD("[xna] {}", message);
      break;
    case 2:
      XELOGW("[xna] {}", message);
      break;
    case 3:
      XELOGE("[xna] {}", message);
      break;
    default:
      XELOGI("[xna] {}", message);
      break;
  }
}

void GuideRelease(uint32_t request) {
  std::lock_guard<std::mutex> lock(guide_mutex);
  guide_requests.erase(request);
}

}  // namespace

uint32_t XnaGuideBeginKeyboard(uint32_t slot, const char* title,
                               const char* description,
                               const char* default_text, uint32_t max_length,
                               bool password) {
  auto* drawer = imgui_drawer();
  if (!drawer) {
    return 0;
  }

  std::string heading = title ? title : "";
  if (heading.empty() && description) {
    heading = description;
  }
  const std::string initial = default_text ? default_text : "";
  const size_t length = max_length ? max_length : 256;

  const uint32_t id = NewGuideRequest();
  auto* dialog = xe::ui::KeyboardDialog::ShowKeyboard(
      drawer, heading, initial,
      password ? xe::ui::KeyboardDialog::InputType::kPassword
               : xe::ui::KeyboardDialog::InputType::kText,
      nullptr);
  ShowSystemUi(true);
  dialog->set_close_callback([id, dialog, length]() {
    ShowSystemUi(false);
    if (dialog->was_cancelled()) {
      FinishGuideRequest(id, kXnaOsGuideCancelled, -1, std::string());
      return;
    }
    std::string text = dialog->result_text();
    if (text.size() > length) {
      text.resize(length);
    }
    FinishGuideRequest(id, kXnaOsGuideCompleted, -1, text);
  });
  return id;
}

const XnaOsTable* GetXnaOsTable() {
  static const XnaOsTable table = {
      sizeof(XnaOsTable),
      kXnaOsAbiVersion,
      &GetUserSlotCount,
      &GetUser,
      &GetTitleId,
      &GetTitleName,
      &ResolveStorageContainer,
      &GetStorageDeviceName,
      &GetStorageTotalSpace,
      &GetStorageFreeSpace,
      &GuideIsVisible,
      &GuideBeginMessageBox,
      &GuideBeginKeyboard,
      &GuidePoll,
      &GuideGetText,
      &GuideRelease,
      &Log,
  };
  return &table;
}

}  // namespace xna
}  // namespace kernel
}  // namespace xe
