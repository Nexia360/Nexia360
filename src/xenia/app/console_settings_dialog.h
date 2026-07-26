/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_CONSOLE_SETTING_DIALOG_H_
#define XENIA_APP_CONSOLE_SETTING_DIALOG_H_

#include <functional>
#include <string>

#include "xenia/hid/mousehook_config.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/xbox.h"

#include "xenia/kernel/xconfig.h"

namespace xe {
namespace app {

class EmulatorWindow;

class ConsoleSettingsDialog final : public ui::ImGuiDialog {
 public:
  ConsoleSettingsDialog(ui::ImGuiDrawer* imgui_drawer,
                        EmulatorWindow& emulator_window,
                        kernel::XConfig* xconfig)
      : ui::ImGuiDialog(imgui_drawer),
        emulator_window_(emulator_window),
        xconfig_(xconfig),
        xconfig_data_(*xconfig->GetXConfig()) {
    const auto profiles = emulator_window.emulator()
                              ->kernel_state()
                              ->xam_state()
                              ->profile_manager()
                              ->GetAccounts();

    for (const auto& [xuid, profile] : *profiles) {
      profiles_.insert({xuid, profile.GetGamertagString()});
    }

    // Mousehook is suspended for as long as this dialog is open - it locks the
    // cursor to the window centre, which would make the settings UI unusable.
    // The desired state is remembered here and applied in ApplyMousehookState()
    // when the dialog closes.
    pending_mousehook_enabled_ = hid::MousehookConfig::Get().enabled();
    hid::MousehookConfig::Get().set_enabled(false);
  }

  // Selects the Mousehook tab on the next draw (Ctrl+Shift+M).
  void FocusMousehookTab() { focus_mousehook_tab_ = true; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void SaveConfig();

  // Voice chat device pickers (Voice tab).
  void DrawVoiceMicCombo();
  void DrawVoiceOutputCombo();

  // Mouse button action picker (Mousehook tab).
  void DrawMouseButtonCombo(
      const char* label, hid::MouseButtonAction current,
      std::function<void(hid::MouseButtonAction)> on_change);

  // Commits the deferred mousehook enable state (see the constructor) and
  // toasts when it comes on. Called on every path that closes the dialog.
  void ApplyMousehookState();

  bool pending_mousehook_enabled_ = false;
  bool focus_mousehook_tab_ = false;
  // Name of the keybind currently waiting for a key press ("" = none).
  std::string capturing_keybind_;

  EmulatorWindow& emulator_window_;
  kernel::XConfig* xconfig_ = nullptr;
  kernel::XConfigData xconfig_data_{};
  std::map<uint64_t, std::string> profiles_;

  // UI specific variables
  double save_confirmation_disappearance_ = 0.0;
};

}  // namespace app
}  // namespace xe

#endif
