/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/emulator_window.h"

#include <chrono>
#include <mutex>
#include <set>

#include "third_party/imgui/imgui.h"
#include "third_party/stb/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wabsolute-value"
#endif
#include "third_party/tomlplusplus/toml.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "xenia/app/console_settings_dialog.h"
#include "xenia/app/messages_dialog.h"
#include "xenia/app/recent_titles_dialog.h"
#include "xenia/app/title_update_dialog.h"
#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/string_util.h"
#include "xenia/base/system.h"
#include "xenia/base/threading.h"
#include "xenia/base/utf8.h"
#include "xenia/config.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/mousehook_config.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/util/title_update_downloader.h"
#include "xenia/kernel/util/title_update_manager.h"
#include "xenia/kernel/xam/avatar_editor/editor_session.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/kernel/xam/xui_assets.h"
#include "xenia/kernel/xam/xui_keyboard_backend.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ani.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/kernel/xna/xna_avatar_format.h"
#include "xenia/kernel/xna/xna_dependencies.h"
#include "xenia/kernel/xna/xna_launcher.h"
#include "xenia/kernel/xna/xna_runtime_install.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/ui_event.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

#include "version.h"

DECLARE_bool(debug);

DECLARE_string(hid);

DECLARE_bool(guide_button);

DECLARE_bool(clear_memory_page_state);

DECLARE_bool(readback_memexport);

DECLARE_string(api_address);

DECLARE_string(api_list);

DECLARE_bool(upnp);

DECLARE_string(network_guid);

DECLARE_bool(title_switch_in_process);

DEFINE_bool(title_switch_clear_handles, true,
            "Release the previous title's handles when a title switch happens "
            "inside the running emulator.",
            "General");

DEFINE_bool(fullscreen, false, "Whether to launch the emulator in fullscreen.",
            "Display");

DEFINE_bool(controller_hotkeys, false, "Hotkeys for Xbox and PS controllers.",
            "General");

DEFINE_bool(auto_check_updates, true,
            "Automatically check for updates on startup and notify if any are "
            "available.",
            "General");

DEFINE_bool(dashboard_coldboot, true,
            "Tear a title down the way a console reboot does when launching "
            "from the dashboard.",
            "General");

DEFINE_string(
    postprocess_antialiasing, "",
    "Post-processing anti-aliasing effect to apply to the image output of the "
    "game.\n"
    "Using post-process anti-aliasing is heavily recommended when AMD "
    "FidelityFX Contrast Adaptive Sharpening or Super Resolution 1.0 is "
    "active.\n"
    "Use: [none, fxaa, fxaa_extreme]\n"
    " none (or any value not listed here):\n"
    "  Don't alter the original image.\n"
    " fxaa:\n"
    "  NVIDIA Fast Approximate Anti-Aliasing 3.11, normal quality preset (12)."
    "\n"
    " fxaa_extreme:\n"
    "  NVIDIA Fast Approximate Anti-Aliasing 3.11, extreme quality preset "
    "(39).",
    "Display");
DEFINE_string(
    postprocess_scaling_and_sharpening, "",
    "Post-processing effect to use for resampling and/or sharpening of the "
    "final display output.\n"
    "Use: [bilinear, cas, fsr]\n"
    " bilinear (or any value not listed here):\n"
    "  Original image at 1:1, simple bilinear stretching for resampling.\n"
    " cas:\n"
    "  Use AMD FidelityFX Contrast Adaptive Sharpening (CAS) for sharpening "
    "at scaling factors of up to 2x2, with additional bilinear stretching for "
    "larger factors.\n"
    " fsr:\n"
    "  Use AMD FidelityFX Super Resolution 1.0 (FSR) for highest-quality "
    "upscaling, or AMD FidelityFX Contrast Adaptive Sharpening for sharpening "
    "while not scaling or downsampling.\n"
    "  For scaling by factors of more than 2x2, multiple FSR passes are done.",
    "Display");
DEFINE_double(
    postprocess_ffx_cas_additional_sharpness,
    xe::ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessDefault,
    "Additional sharpness for AMD FidelityFX Contrast Adaptive Sharpening "
    "(CAS), from 0 to 1.\n"
    "Higher is sharper.",
    "Display");
DEFINE_uint32(
    postprocess_ffx_fsr_max_upsampling_passes,
    xe::ui::Presenter::GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax,
    "Maximum number of upsampling passes performed in AMD FidelityFX Super "
    "Resolution 1.0 (FSR) before falling back to bilinear stretching after the "
    "final pass.\n"
    "Each pass upscales only to up to 2x2 the previous size. If the game "
    "outputs a 1280x720 image, 1 pass will upscale it to up to 2560x1440 "
    "(below 4K), after 2 passes it will be upscaled to a maximum of 5120x2880 "
    "(including 3840x2160 for 4K), and so on.\n"
    "This variable has no effect if the display resolution isn't very high, "
    "but may be reduced on resolutions like 4K or 8K in case the performance "
    "impact of multiple FSR upsampling passes is too high, or if softer edges "
    "are desired.\n"
    "The default value is the maximum internally supported by Xenia.",
    "Display");
DEFINE_double(
    postprocess_ffx_fsr_sharpness_reduction,
    xe::ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionDefault,
    "Sharpness reduction for AMD FidelityFX Super Resolution 1.0 (FSR), in "
    "stops.\n"
    "Lower is sharper.",
    "Display");
// Dithering to 8bpc is enabled by default since the effect is minor, only
// effects what can't be shown normally by host displays, and nothing is changed
// by it for 8bpc source without resampling.
DEFINE_bool(
    postprocess_dither, true,
    "Dither the final image output from the internal precision to 8 bits per "
    "channel so gradients are smoother.\n"
    "On a 10bpc display, the lower 2 bits will still be kept, but noise will "
    "be added to them - disabling may be recommended for 10bpc, but it "
    "depends on the 10bpc displaying capabilities of the actual display used.",
    "Display");

DEFINE_int32(recent_titles_entry_amount, 10,
             "Allows user to define how many titles is saved in list of "
             "recently played titles.",
             "General");
DEFINE_bool(disable_doubleclick_fullscreen, false,
            "Allows the user to disable the behavior where a fast double-click "
            "causes Xenia to enter fullscreen mode.",
            "General");

namespace xe {
namespace app {

using xe::ui::FileDropEvent;
using xe::ui::KeyEvent;
using xe::ui::MenuItem;
using xe::ui::UIEvent;

using namespace xe::hid;
using namespace xe::gpu;

constexpr std::string_view kBaseTitle = "Nexia360";

EmulatorWindow::EmulatorWindow(Emulator* emulator,
                               ui::WindowedAppContext& app_context,
                               uint32_t width, uint32_t height)
    : emulator_(emulator),
      app_context_(app_context),
      window_listener_(*this),
      window_(ui::Window::Create(app_context, kBaseTitle, width, height)),
      imgui_drawer_(
          std::make_shared<ui::ImGuiDrawer>(window_.get(), kZOrderImGui)),
      display_config_game_config_load_callback_(
          new DisplayConfigGameConfigLoadCallback(*emulator, *this)) {
  base_title_ = std::string(kBaseTitle) +
#ifdef DEBUG
#if _NO_DEBUG_HEAP == 1
                " DEBUG"
#else
                " CHECKED"
#endif
#endif
                " ("
#ifdef XE_BUILD_IS_PR
                "PR#" XE_BUILD_PR_NUMBER " - "
#endif
                XE_BUILD_BRANCH "@" XE_BUILD_COMMIT_SHORT " on " XE_BUILD_DATE
                ")";

  updater_ = std::make_shared<Updater>("AdrianCassar", "xenia-canary");

  LoadRecentlyLaunchedTitles();
}

std::unique_ptr<EmulatorWindow> EmulatorWindow::Create(
    Emulator* emulator, ui::WindowedAppContext& app_context, uint32_t width,
    uint32_t height) {
  assert_true(app_context.IsInUIThread());
  std::unique_ptr<EmulatorWindow> emulator_window(
      new EmulatorWindow(emulator, app_context, width, height));
  if (!emulator_window->Initialize()) {
    return nullptr;
  }
  return emulator_window;
}

EmulatorWindow::~EmulatorWindow() {
  // Before anything else: the boot animation's timer captures `this` and would
  // otherwise still be counting down to a dashboard that has nowhere to go.
  CancelPendingDashboard();
  // Notify the ImGui drawer that the immediate drawer is being destroyed.
  ShutdownGraphicsSystemPresenterPainting();
}

void EmulatorWindow::ShutdownUpdaterDialog() {
  // Cancel checking for updates.
  cancel_request = true;
  updater_dialog_.reset();
}

ui::Presenter* EmulatorWindow::GetGraphicsSystemPresenter() const {
  gpu::GraphicsSystem* graphics_system = emulator_->graphics_system();
  return graphics_system ? graphics_system->presenter() : nullptr;
}

void EmulatorWindow::SetupGraphicsSystemPresenterPainting() {
  ShutdownGraphicsSystemPresenterPainting();

  if (!window_) {
    return;
  }

  ui::Presenter* presenter = GetGraphicsSystemPresenter();
  if (!presenter) {
    return;
  }

  ApplyDisplayConfigForCvars();

  window_->SetPresenter(presenter);

  immediate_drawer_ =
      emulator_->graphics_system()->provider()->CreateImmediateDrawer();
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(presenter);
    imgui_drawer_->SetPresenterAndImmediateDrawer(presenter,
                                                  immediate_drawer_.get());
    Profiler::SetUserIO(kZOrderProfiler, window_.get(), presenter,
                        immediate_drawer_.get());
    // With the console's UI assets imported, text entry uses the 360's own
    // keyboard; without them the ImGui one stays.
    kernel::xam::xui::InstallKeyboardBackend(
        presenter, immediate_drawer_.get(),
        kernel::xam::xui::DefaultAssetDirectory());
  }
}

// Picks up assets imported while the emulator is already running, so an
// Install Content run does not need a restart to take effect.
void EmulatorWindow::ReloadDashboardUIAssets() {
  if (!immediate_drawer_) {
    return;
  }
  kernel::xam::xui::InstallKeyboardBackend(
      GetGraphicsSystemPresenter(), immediate_drawer_.get(),
      kernel::xam::xui::DefaultAssetDirectory());
}

void EmulatorWindow::ShutdownGraphicsSystemPresenterPainting() {
  kernel::xam::xui::UninstallKeyboardBackend();
  Profiler::SetUserIO(kZOrderProfiler, window_.get(), nullptr, nullptr);
  imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
  immediate_drawer_.reset();
  if (window_) {
    window_->SetPresenter(nullptr);
  }
}

void EmulatorWindow::OnEmulatorInitialized() {
  if (!emulator_->kernel_state()
           ->xam_state()
           ->profile_manager()
           ->GetAccountCount()) {
    new NoProfileDialog(imgui_drawer_.get(), this);
    disable_hotkeys_ = true;
  }

  emulator_initialized_ = true;
  window_->SetMainMenuEnabled(true);
  // When the user can see that the emulator isn't initializing anymore (the
  // menu isn't disabled), enter fullscreen if requested.
  if (cvars::fullscreen) {
    SetFullscreen(true);
  }

  if (IsUseNexusForGameBarEnabled()) {
    XELOGE(
        "Xbox Gamebar Enabled, using BACK button instead of GUIDE for "
        "controller hotkeys!!!");
  }

  // Create a thread to listen for controller hotkeys. Started unconditionally:
  // it also carries the Guide and Back menu presses, which are console
  // behaviour rather than debug hotkeys. The button combinations themselves
  // stay behind cvars::controller_hotkeys.
  Gamepad_HotKeys_Listener =
      threading::Thread::Create({}, [&] { GamepadHotKeys(); });
  Gamepad_HotKeys_Listener->set_name("Gamepad HotKeys Listener");

  // Startup auto-update check disabled (Nexia does not use the Xenia updater).
#if 0
  bool should_check_update = cvars::auto_check_updates &&
                             !(cvar::updated_arg_present && cvar::updated);

  auto callback = [this](CheckForUpdateInfo update_info) {
    if (update_info.update_available) {
      app_context_.CallInUIThread([this, update_info]() {
        ShowUpdateAvailableDialog(update_info.metadata.commit_hash,
                                  update_info.metadata.commit_date);
      });
    }
  };

  if (should_check_update) {
    update_info_ = updater_->StartupUpdateCheckAsync(cancel_request, callback);
  }
#endif

  if (emulator_->kernel_state()
          ->xam_state()
          ->user_tracker()
          ->LoggedInToLive()) {
    emulator()->GetXboxLiveAPI()->StartWhoamiAsync();
  }

  // The message notification loop. Started unconditionally and left running:
  // it resolves the signed-in profile itself on every tick, so signing in
  // later, or switching profiles, needs no hook here. It only calls back when
  // the counts actually change, and the menu can only be touched from the UI
  // thread.
  emulator()->GetXboxLiveAPI()->SetMessageCountsCallback([this]() {
    app_context_.CallInUIThread([this]() { UpdateSocialMenu(); });
  });

  emulator()->GetXboxLiveAPI()->StartMessageNotifications();

  UpdateSocialMenu();
}

void EmulatorWindow::ShowUpdateAvailableDialog(const std::string& commit,
                                               const std::string& date) {
  std::string title_text = "Update Available";
  std::string short_commit = commit.substr(0, 9);
  std::string message = fmt::format(
      "Date: {} ({})\n\n"
      "You can update via the Netplay -> Update Checker menu",
      date, short_commit);

  new xe::ui::HostNotificationWindow(imgui_drawer_.get(), title_text, message,
                                     0, 9);
}

void EmulatorWindow::EmulatorWindowListener::OnClosing(ui::UIEvent& e) {
  // Drop mouse capture before quitting. While mousehook is active every mouse
  // move re-centres the cursor, which keeps generating fresh mouse messages on
  // the UI thread - exactly the thread that has to drain its queue to process
  // the quit. Suspended in memory only, so mousehook.json keeps the user's
  // choice for next launch.
  auto& mousehook = hid::MousehookConfig::Get();
  if (mousehook.enabled()) {
    mousehook.set_enabled(false);
    mousehook.ResetMouseState();
  }

  emulator_window_.app_context_.QuitFromUIThread();
}

void EmulatorWindow::EmulatorWindowListener::OnFileDrop(ui::FileDropEvent& e) {
  emulator_window_.FileDrop(e.filename());
}

void EmulatorWindow::EmulatorWindowListener::OnKeyDown(ui::KeyEvent& e) {
  emulator_window_.OnKeyDown(e);
}

void EmulatorWindow::EmulatorWindowListener::OnMouseDown(ui::MouseEvent& e) {
  emulator_window_.OnMouseDown(e);
}

void EmulatorWindow::EmulatorWindowListener::OnMouseUp(ui::MouseEvent& e) {
  emulator_window_.OnMouseUp(e);
}

void EmulatorWindow::EmulatorWindowListener::OnUsbDeviceChanged(
    bool is_arrival) {
  if (!emulator_window_.emulator()) {
    return;
  }

  if (!emulator_window_.emulator()->input_system()) {
    return;
  }

  auto* portal = emulator_window_.emulator()->input_system()->GetPortal();
  if (!portal) {
    return;
  }

  if (is_arrival) {
    portal->OnDeviceArrival();
  } else {
    portal->OnDeviceRemoval();
  }
}

void EmulatorWindow::DisplayConfigGameConfigLoadCallback::PostGameConfigLoad() {
  emulator_window_.ApplyDisplayConfigForCvars();
}

void EmulatorWindow::DisplayConfigDialog::OnDraw(ImGuiIO& io) {
  gpu::GraphicsSystem* graphics_system =
      emulator_window_.emulator_->graphics_system();
  if (!graphics_system) {
    return;
  }

  // In the top-left corner so it's close to the menu bar from where it was
  // opened.
  // Origin Y coordinate 20 was taken from the Dear ImGui demo.
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  // Alpha from Dear ImGui tooltips (0.35 from the overlay provides too low
  // visibility). Translucent so some effect of the changes can still be seen
  // through it.
  ImGui::SetNextWindowBgAlpha(0.6f);
  bool dialog_open = true;
  if (!ImGui::Begin("Post-processing", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::End();
    Close();
    return;
  }

  // Even if the close button has been pressed, still paint everything not to
  // have one frame with an empty window.

  // Prevent user confusion which has been reported multiple times.
  ImGui::TextUnformatted("All effects can be used on GPUs of any brand.");
  ImGui::Spacing();

  gpu::CommandProcessor* command_processor =
      graphics_system->command_processor();
  if (command_processor) {
    if (ImGui::TreeNodeEx(
            "Anti-aliasing",
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      gpu::CommandProcessor::SwapPostEffect current_swap_post_effect =
          command_processor->GetDesiredSwapPostEffect();
      int new_swap_post_effect_index = int(current_swap_post_effect);
      ImGui::RadioButton("None", &new_swap_post_effect_index,
                         int(gpu::CommandProcessor::SwapPostEffect::kNone));
      ImGui::RadioButton(
          "NVIDIA Fast Approximate Anti-Aliasing (FXAA) [Normal Quality]",
          &new_swap_post_effect_index,
          int(gpu::CommandProcessor::SwapPostEffect::kFxaa));
      ImGui::RadioButton(
          "NVIDIA Fast Approximate Anti-Aliasing (FXAA) [Extreme Quality]",
          &new_swap_post_effect_index,
          int(gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme));
      gpu::CommandProcessor::SwapPostEffect new_swap_post_effect =
          gpu::CommandProcessor::SwapPostEffect(new_swap_post_effect_index);
      if (current_swap_post_effect != new_swap_post_effect) {
        command_processor->SetDesiredSwapPostEffect(new_swap_post_effect);
      }

      // Override the values in the cvars to save them to the config at exit if
      // the user has set them to anything new.
      if (GetSwapPostEffectForCvarValue(cvars::postprocess_antialiasing) !=
          new_swap_post_effect) {
        OVERRIDE_string(postprocess_antialiasing,
                        GetCvarValueForSwapPostEffect(new_swap_post_effect));
      }

      ImGui::TreePop();
    }
  }

  ui::Presenter* presenter = graphics_system->presenter();
  if (presenter) {
    const ui::Presenter::GuestOutputPaintConfig& current_presenter_config =
        presenter->GetGuestOutputPaintConfigFromUIThread();
    ui::Presenter::GuestOutputPaintConfig new_presenter_config =
        current_presenter_config;

    if (ImGui::TreeNodeEx(
            "Resampling and sharpening",
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      // Filtering effect.
      int new_effect_index = int(new_presenter_config.GetEffect());
      ImGui::RadioButton(
          "None / Bilinear", &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear));
      ImGui::RadioButton(
          "AMD FidelityFX Contrast Adaptive Sharpening (CAS)",
          &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kCas));
      ImGui::RadioButton(
          "AMD FidelityFX Super Resolution 1.0 (FSR)", &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kFsr));
      new_presenter_config.SetEffect(
          ui::Presenter::GuestOutputPaintConfig::Effect(new_effect_index));

      // effect_description must be one complete, but short enough, sentence per
      // line, as TextWrapped doesn't work correctly in auto-resizing windows
      // (in the initial frames, the window becomes extremely tall, and widgets
      // added after the wrapped text have no effect on the width of the text).
      const char* effect_description = nullptr;
      switch (new_presenter_config.GetEffect()) {
        case ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear:
          effect_description =
              "Simple bilinear filtering is done if resampling is needed.\n"
              "Otherwise, only anti-aliasing is done if enabled, or displaying "
              "as is.";
          break;
        case ui::Presenter::GuestOutputPaintConfig::Effect::kCas:
          effect_description =
              "Sharpening and resampling to up to 2x2 to improve the fidelity "
              "of details.\n"
              "For scaling by more than 2x2, bilinear stretching is done "
              "afterwards.";
          break;
        case ui::Presenter::GuestOutputPaintConfig::Effect::kFsr:
          effect_description =
              "High-quality edge-preserving upscaling to arbitrary target "
              "resolutions.\n"
              "For scaling by more than 2x2, multiple upsampling passes are "
              "done.\n"
              "If not upscaling, Contrast Adaptive Sharpening (CAS) is used "
              "instead.";
          break;
      }
      if (effect_description) {
        ImGui::TextUnformatted(effect_description);
      }

      if (new_presenter_config.GetEffect() ==
              ui::Presenter::GuestOutputPaintConfig::Effect::kCas ||
          new_presenter_config.GetEffect() ==
              ui::Presenter::GuestOutputPaintConfig::Effect::kFsr) {
        if (effect_description) {
          ImGui::Spacing();
        }

        ImGui::TextUnformatted(
            "FXAA is highly recommended when using CAS or FSR.");

        ImGui::Spacing();

        // 2 decimal places is more or less enough precision for the sharpness
        // given the minor visual effect of small changes, the width of the
        // slider, and readability convenience (2 decimal places is like an
        // integer percentage). However, because Dear ImGui parses the string
        // representation of the number and snaps the value to it internally,
        // 2 decimal places actually offer less precision than the slider itself
        // does. This is especially prominent in the low range of the non-linear
        // FSR sharpness reduction slider. 3 decimal places are optimal in this
        // case.

        if (new_presenter_config.GetEffect() ==
            ui::Presenter::GuestOutputPaintConfig::Effect::kFsr) {
          float fsr_sharpness_reduction =
              new_presenter_config.GetFsrSharpnessReduction();
          ImGui::TextUnformatted(
              "FSR sharpness reduction when upscaling (lower is sharper):");
          const auto label = fmt::format(
              "{} %%", static_cast<int>(fsr_sharpness_reduction * 100));
          // Power 2.0 scaling as the reduction is in stops, used in exp2.
          fsr_sharpness_reduction = sqrt(2.f * fsr_sharpness_reduction);
          ImGui::SliderFloat(
              "##FSRSharpnessReduction", &fsr_sharpness_reduction,
              ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMin,
              ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMax,
              label.c_str(), ImGuiSliderFlags_NoInput);
          fsr_sharpness_reduction =
              .5f * fsr_sharpness_reduction * fsr_sharpness_reduction;
          ImGui::SameLine();
          if (ImGui::Button("Reset##ResetFSRSharpnessReduction")) {
            fsr_sharpness_reduction = ui::Presenter::GuestOutputPaintConfig ::
                kFsrSharpnessReductionDefault;
          }
          new_presenter_config.SetFsrSharpnessReduction(
              fsr_sharpness_reduction);
        }

        float cas_additional_sharpness =
            new_presenter_config.GetCasAdditionalSharpness();
        ImGui::TextUnformatted(
            new_presenter_config.GetEffect() ==
                    ui::Presenter::GuestOutputPaintConfig::Effect::kFsr
                ? "CAS additional sharpness when not upscaling (higher is "
                  "sharper):"
                : "CAS additional sharpness (higher is sharper):");
        const auto label = fmt::format(
            "{} %%", static_cast<int>(cas_additional_sharpness * 100));
        ImGui::SliderFloat(
            "##CASAdditionalSharpness", &cas_additional_sharpness,
            ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMin,
            ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMax,
            label.c_str(), ImGuiSliderFlags_NoInput);
        ImGui::SameLine();
        if (ImGui::Button("Reset##ResetCASAdditionalSharpness")) {
          cas_additional_sharpness = ui::Presenter::GuestOutputPaintConfig ::
              kCasAdditionalSharpnessDefault;
        }
        new_presenter_config.SetCasAdditionalSharpness(
            cas_additional_sharpness);

        // There's no need to expose the setting for the maximum number of FSR
        // EASU passes as it's largely meaningless if the user doesn't have a
        // very high-resolution monitor compared to the original image size as
        // most of the values of the slider will have no effect, and that's just
        // very fine-grained performance control for a fixed-overhead pass only
        // for huge screen resolutions.
      }

      ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Dithering", ImGuiTreeNodeFlags_Framed |
                                           ImGuiTreeNodeFlags_DefaultOpen)) {
      bool dither = current_presenter_config.GetDither();
      ImGui::Checkbox(
          "Dither the final output to 8bpc to make gradients smoother",
          &dither);
      new_presenter_config.SetDither(dither);

      ImGui::TreePop();
    }

    presenter->SetGuestOutputPaintConfigFromUIThread(new_presenter_config);

    // Override the values in the cvars to save them to the config at exit if
    // the user has set them to anything new.
    ui::Presenter::GuestOutputPaintConfig cvars_presenter_config =
        GetGuestOutputPaintConfigForCvars();
    if (cvars_presenter_config.GetEffect() !=
        new_presenter_config.GetEffect()) {
      OVERRIDE_string(postprocess_scaling_and_sharpening,
                      GetCvarValueForGuestOutputPaintEffect(
                          new_presenter_config.GetEffect()));
    }
    if (cvars_presenter_config.GetCasAdditionalSharpness() !=
        new_presenter_config.GetCasAdditionalSharpness()) {
      OVERRIDE_double(postprocess_ffx_cas_additional_sharpness,
                      new_presenter_config.GetCasAdditionalSharpness());
    }
    if (cvars_presenter_config.GetFsrSharpnessReduction() !=
        new_presenter_config.GetFsrSharpnessReduction()) {
      OVERRIDE_double(postprocess_ffx_fsr_sharpness_reduction,
                      new_presenter_config.GetFsrSharpnessReduction());
    }
    if (cvars_presenter_config.GetDither() !=
        new_presenter_config.GetDither()) {
      OVERRIDE_bool(postprocess_dither, new_presenter_config.GetDither());
    }
  }

  ImGui::End();

  if (!dialog_open) {
    Close();
    emulator_window_.ToggleDisplayConfigDialog();
    // `this` might have been destroyed by ToggleDisplayConfigDialog.
    return;
  }
}

void EmulatorWindow::DlcTargetDialog::OnDraw(ImGuiIO& io) {
  if (done_) {
    return;
  }

  if (!initialized_) {
    initialized_ = true;

    auto* tu_manager = emulator_window_.emulator()->title_update_manager();

    for (size_t i = 0; i < installation_entries_->size(); ++i) {
      const auto& entry = installation_entries_->at(i);
      if (entry.content_type_ != XContentType::kMarketplaceContent &&
          entry.content_type_ != XContentType::kPublisher) {
        continue;
      }

      std::vector<std::string> ids;
      std::vector<std::string> labels;

      // "None" is a real overlay, not an absence of one, so it is always a
      // valid destination.
      ids.push_back(kernel::util::kNoTitleUpdateId);
      labels.push_back("None (no title update)");

      std::string active;
      if (tu_manager) {
        active = tu_manager->GetActive(entry.title_id_);
        for (const auto& update : tu_manager->List(entry.title_id_)) {
          ids.push_back(update.id);
          labels.push_back(update.name.empty() ? update.id : update.name);
        }
      }

      // Default to whatever is active, since that is what the user is
      // playing and almost always what the DLC is meant for.
      int selected = 0;
      for (size_t opt = 0; opt < ids.size(); ++opt) {
        if (!active.empty() && ids[opt] == active) {
          selected = static_cast<int>(opt);
          break;
        }
      }

      dlc_indices_.push_back(i);
      entry_option_ids_.push_back(std::move(ids));
      entry_option_labels_.push_back(std::move(labels));
      selection_.push_back(selected);
    }

    // Nothing to ask about - let the install proceed untouched.
    if (dlc_indices_.empty()) {
      done_ = true;
      if (on_confirmed_) {
        on_confirmed_();
      }
      Close();
      return;
    }
  }

  ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);

  bool dialog_open = true;
  if (!ImGui::Begin(
          fmt::format("Install DLC###{}", window_id_).c_str(), &dialog_open,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  ImGui::TextWrapped(
      "Downloadable content is stored per title update. Choose which update "
      "each package belongs to - it will only be visible while that update is "
      "active.");
  ImGui::Separator();

  for (size_t row = 0; row < dlc_indices_.size(); ++row) {
    const auto& entry = installation_entries_->at(dlc_indices_[row]);

    ImGui::PushID(static_cast<int>(row));
    ImGui::Text("%s", entry.name_.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%08X)", entry.title_id_);

    std::vector<const char*> labels;
    labels.reserve(entry_option_labels_[row].size());
    for (const auto& label : entry_option_labels_[row]) {
      labels.push_back(label.c_str());
    }

    ImGui::SetNextItemWidth(320.0f);
    ImGui::Combo("##target", &selection_[row], labels.data(),
                 static_cast<int>(labels.size()));
    ImGui::PopID();
  }

  ImGui::Separator();

  if (ImGui::Button("Install")) {
    for (size_t row = 0; row < dlc_indices_.size(); ++row) {
      installation_entries_->at(dlc_indices_[row]).target_update_id_ =
          entry_option_ids_[row][selection_[row]];
    }
    done_ = true;
    if (on_confirmed_) {
      on_confirmed_();
    }
    ImGui::End();
    Close();
    return;
  }

  ImGui::SameLine();
  if (ImGui::Button("Cancel") || !dialog_open) {
    done_ = true;
    ImGui::End();
    Close();
    return;
  }

  ImGui::End();
}

void EmulatorWindow::ContentInstallDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);

  bool dialog_open = true;
  if (!ImGui::Begin(
          fmt::format("Installation Progress###{}", window_id_).c_str(),
          &dialog_open,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
              ImGuiWindowFlags_HorizontalScrollbar)) {
    Close();
    ImGui::End();
    return;
  }

  bool is_everything_installed = true;
  for (const auto& entry : *installation_entries_) {
    ImGui::BeginTable(fmt::format("table_{}", entry.name_).c_str(), 2);
    ImGui::TableNextRow(0);
    ImGui::TableSetColumnIndex(0);
    if (entry.icon_) {
      ImGui::Image(reinterpret_cast<ImTextureID>(entry.icon_.get()),
                   ui::default_image_icon_size);
    } else {
      ImGui::Dummy(ui::default_image_icon_size);
    }
    ImGui::TableNextColumn();

    ImGui::Text("Name: %s", entry.name_.c_str());
    ImGui::Text("Installation Path:");
    ImGui::SameLine();
    if (ImGui::TextLink(
            xe::path_to_utf8(entry.data_installation_path_).c_str())) {
      LaunchFileExplorer(emulator_window_.emulator_->content_root() /
                         entry.data_installation_path_);
    }

    if (entry.content_type_ != xe::XContentType::kInvalid) {
      ImGui::Text("Content Type: %s",
                  XContentTypeMap.at(entry.content_type_).c_str());
    }

    std::string result = fmt::format(
        "Status: {}", xe::Emulator::installStateStringName[static_cast<uint8_t>(
                          entry.installation_state_)]);

    if (entry.installation_state_ == xe::Emulator::InstallState::failed) {
      result += fmt::format(" - {} ({:08X})",
                            entry.installation_error_message_.c_str(),
                            entry.installation_result_);
    }

    ImGui::Text("%s", result.c_str());
    ImGui::EndTable();

    if (entry.content_size_ > 0) {
      ImGui::ProgressBar(static_cast<float>(entry.currently_installed_size_) /
                         entry.content_size_);

      if (entry.currently_installed_size_ != entry.content_size_ &&
          entry.installation_result_ == X_ERROR_SUCCESS) {
        is_everything_installed = false;
      }
    } else {
      ImGui::ProgressBar(0.0f);
    }

    if (installation_entries_->size() > 1) {
      ImGui::Separator();
    }
  }
  ImGui::Spacing();

  ImGui::BeginDisabled(!is_everything_installed);
  if (ImGui::Button("Close")) {
    ImGui::EndDisabled();
    Close();
    ImGui::End();
    return;
  }
  ImGui::EndDisabled();

  if (!dialog_open && is_everything_installed) {
    Close();
    ImGui::End();
    return;
  }
  ImGui::End();
}

void EmulatorWindow::XMPConfigDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);

  bool dialog_open = true;
  if (!ImGui::Begin("Audio Player Menu", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    Close();
    ImGui::End();
    return;
  }

  auto audio_player = emulator_window_.emulator_->audio_media_player();
  using xmp_state = kernel::xam::apps::XmpApp::State;
  if (audio_player) {
    ImGui::Text("Audio player status:");
    ImGui::SameLine();
    switch (audio_player->GetState()) {
      case xmp_state::kIdle:
        ImGui::Text("Idle");
        break;
      case xmp_state::kPaused:
        ImGui::Text("Paused");
        break;
      case xmp_state::kPlaying:
        ImGui::Text("Playing");
        break;
      default:
        break;
    }

    if (audio_player->IsPlaying()) {
      if (ImGui::Button("Pause")) {
        audio_player->Pause();
      }
    } else if (audio_player->IsPaused()) {
      if (ImGui::Button("Resume")) {
        audio_player->Continue();
      }
    }

    volume_ =
        emulator_window_.emulator_->audio_media_player()->GetVolume()->load();

    if (ImGui::SliderFloat("Audio player volume", &volume_, 0.0f, 1.0f,
                           "%.2f")) {
      audio_player->SetVolume(volume_);
    }
  }

  ImGui::End();

  if (!dialog_open) {
    Close();
    emulator_window_.xmp_config_dialog_.release();
    return;
  }
}

bool EmulatorWindow::Initialize() {
  window_->AddListener(&window_listener_);
  window_->AddInputListener(&window_listener_, kZOrderEmulatorWindowInput);

  // Main menu.
  // FIXME: This code is really messy.
  auto main_menu = MenuItem::Create(MenuItem::Type::kNormal);
  auto file_menu = MenuItem::Create(MenuItem::Type::kPopup, "&File");
  auto zar_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Zar Package");
  auto xna_menu = MenuItem::Create(MenuItem::Type::kPopup, "&XNA Titles");
  {
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Open...", "Ctrl+O",
                         std::bind(&EmulatorWindow::FileOpen, this)));
    // A dialog rather than a submenu: a native menu neither scrolls nor
    // carries an icon, and the recent list is no longer a handful of entries.
    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Open Recent...", "",
        std::bind(&EmulatorWindow::ShowRecentTitlesDialog, this)));
    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Open Recent with &TU...", "",
        std::bind(&EmulatorWindow::ShowRecentTitlesWithTuDialog, this)));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Install Content...",
                         std::bind(&EmulatorWindow::InstallContent, this)));
    zar_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Create",
                         std::bind(&EmulatorWindow::CreateZarchive, this)));
    zar_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Extract",
                         std::bind(&EmulatorWindow::ExtractZarchive, this)));
    file_menu->AddChild(std::move(zar_menu));

    xna_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Setup XNA...",
                         std::bind(&EmulatorWindow::SetupXna, this)));
    xna_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Install XNA Package...",
                         std::bind(&EmulatorWindow::InstallXnaPackage, this)));
    xna_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Find XNA Dependencies...",
        std::bind(&EmulatorWindow::FindXnaDependencies, this)));
    xna_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Check Dependencies",
        std::bind(&EmulatorWindow::ShowXnaDependencies, this)));
    xna_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Avatar Editor...",
        std::bind(&EmulatorWindow::ToggleAvatarEditorDialog, this)));
    xna_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    FillXnaTitlesMenu(xna_menu.get());
    file_menu->AddChild(std::move(xna_menu));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "E&xit Title", "",
                         std::bind(&EmulatorWindow::ExitTitle, this)));
#ifdef DEBUG
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Close",
                         std::bind(&EmulatorWindow::FileClose, this)));
#endif  // #ifdef DEBUG
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Show content directory...",
        std::bind(&EmulatorWindow::ShowContentDirectory, this)));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Dump XLast",
                         std::bind(&EmulatorWindow::DumpXLast, this)));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "E&xit", "Alt+F4",
                         std::bind(&EmulatorWindow::FileExit, this)));
  }
  main_menu->AddChild(std::move(file_menu));

  // Profile Menu
  auto profile_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Profile");
  {
    profile_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Show Profile Menu", "",
        std::bind(&EmulatorWindow::ToggleProfilesConfigDialog, this)));
    profile_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Gamerpic Browser", "",
        std::bind(&EmulatorWindow::ToggleGamerpicBrowserDialog, this)));
    profile_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Avatar Editor", "",
        std::bind(&EmulatorWindow::ToggleAvatarEditorDialog, this)));
    profile_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Dashboard Scene...", "",
        std::bind(&EmulatorWindow::ToggleXuiSceneDialog, this)));
  }
  main_menu->AddChild(std::move(profile_menu));

  // CPU menu.
  auto cpu_menu = MenuItem::Create(MenuItem::Type::kPopup, "&CPU");
  {
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Reset Time Scalar", "Numpad *",
        std::bind(&EmulatorWindow::CpuTimeScalarReset, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Time Scalar /= 2", "Numpad -",
        std::bind(&EmulatorWindow::CpuTimeScalarSetHalf, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Time Scalar *= 2", "Numpad +",
        std::bind(&EmulatorWindow::CpuTimeScalarSetDouble, this)));
  }
#if XE_OPTION_PROFILING
  cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        "Toggle Profiler &Display", "F3",
                                        []() { Profiler::ToggleDisplay(); }));
    cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        "&Pause/Resume Profiler", "`",
                                        []() { Profiler::TogglePause(); }));
  }
#endif
  cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Break and Show Guest Debugger",
        "Pause/Break", std::bind(&EmulatorWindow::CpuBreakIntoDebugger, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Break into Host Debugger",
        "Ctrl+Pause/Break",
        std::bind(&EmulatorWindow::CpuBreakIntoHostDebugger, this)));
  }
  main_menu->AddChild(std::move(cpu_menu));

  // GPU menu.
  auto gpu_menu = MenuItem::Create(MenuItem::Type::kPopup, "&GPU");
  {
    gpu_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Trace Frame", "F4",
                         std::bind(&EmulatorWindow::GpuTraceFrame, this)));
  }
  gpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    gpu_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Clear Runtime Caches", "F5",
                         std::bind(&EmulatorWindow::GpuClearCaches, this)));
  }
  main_menu->AddChild(std::move(gpu_menu));

  // Display menu.
  auto display_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Display");
  {
    display_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Post-processing settings", "F6",
        std::bind(&EmulatorWindow::ToggleDisplayConfigDialog, this)));
  }
  display_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    display_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Fullscreen", "F11",
                         std::bind(&EmulatorWindow::ToggleFullscreen, this)));
    display_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Take Screenshot", "F12",
                         std::bind(&EmulatorWindow::TakeScreenshot, this)));
  }
  main_menu->AddChild(std::move(display_menu));

  // HID menu.
  auto hid_menu = MenuItem::Create(MenuItem::Type::kPopup, "&HID");
  {
    hid_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Toggle controller vibration", "",
        std::bind(&EmulatorWindow::ToggleControllerVibration, this)));
    hid_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Display controller hotkeys", "",
        std::bind(&EmulatorWindow::DisplayHotKeysConfig, this)));
  }
  main_menu->AddChild(std::move(hid_menu));

  // XMP menu
  auto xmp_menu = MenuItem::Create(MenuItem::Type::kPopup, "&XMP");
  {
    xmp_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Show XMP Menu", "",
        std::bind(&EmulatorWindow::ToggleXMPConfigDialog, this)));
  }
  main_menu->AddChild(std::move(xmp_menu));

  // Console menu
  auto console_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Console");
  {
    console_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Open console settings", "",
        std::bind(&EmulatorWindow::ToggleConsoleSettingsDialog, this)));

    console_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));

    console_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Enable Cold &Boot", "",
        std::bind(&EmulatorWindow::SetDashboardColdBoot, this, true)));

    console_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Disable Cold Boo&t", "",
        std::bind(&EmulatorWindow::SetDashboardColdBoot, this, false)));
  }
  main_menu->AddChild(std::move(console_menu));

  // Netplay menu.
  auto Netplay_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Netplay");
  {
    Netplay_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Status", "",
        std::bind(&EmulatorWindow::ToggleNetplayStatusDialog, this)));

    Netplay_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Settings", "",
        std::bind(&EmulatorWindow::ToggleNetplaySettingsDialog, this)));

    Netplay_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Manager", "",
        std::bind(&EmulatorWindow::ToggleFriendsDialog, this)));
  }
  main_menu->AddChild(std::move(Netplay_menu));

  // Social menu. The counts in the labels are filled in by UpdateSocialMenu as
  // the notification loop reports them - the text here is only what shows
  // before the first poll comes back.
  {
    auto social_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Social");

    auto texts_item = MenuItem::Create(
        MenuItem::Type::kString, "Texts (0)", "",
        std::bind(&EmulatorWindow::ToggleTextMessagesDialog, this));
    social_texts_item_ = texts_item.get();
    social_menu->AddChild(std::move(texts_item));

    auto vm_item = MenuItem::Create(
        MenuItem::Type::kString, "VM (0)", "",
        std::bind(&EmulatorWindow::ToggleVoiceMessagesDialog, this));
    social_vm_item_ = vm_item.get();
    social_menu->AddChild(std::move(vm_item));

    social_menu_item_ = social_menu.get();
    main_menu->AddChild(std::move(social_menu));
  }

  // Help menu.
  auto help_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Help");
  {
    help_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "FA&Q...", "F1",
                         std::bind(&EmulatorWindow::ShowFAQ, this)));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Game &compatibility...",
                         std::bind(&EmulatorWindow::ShowCompatibility, this)));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Build commit on GitHub...", "F2",
        std::bind(&EmulatorWindow::ShowBuildCommit, this)));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Recent changes on GitHub...", []() {
          LaunchWebBrowser(
              "https://github.com/Nexia360/Nexia360/"
              "compare/" XE_BUILD_COMMIT "..." XE_BUILD_BRANCH);
        }));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&About...",
        []() { LaunchWebBrowser("https://xenia.jp/about/"); }));
  }
  main_menu->AddChild(std::move(help_menu));

  window_->SetMainMenu(std::move(main_menu));

  window_->SetMainMenuEnabled(false);

  UpdateTitle();

  if (!window_->Open()) {
    XELOGE("Failed to open the platform window");
    return false;
  }

  Profiler::SetUserIO(kZOrderProfiler, window_.get(), nullptr, nullptr);

  return true;
}

const char* EmulatorWindow::GetCvarValueForSwapPostEffect(
    gpu::CommandProcessor::SwapPostEffect effect) {
  switch (effect) {
    case gpu::CommandProcessor::SwapPostEffect::kFxaa:
      return "fxaa";
    case gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme:
      return "fxaa_extreme";
    default:
      return "";
  }
}

gpu::CommandProcessor::SwapPostEffect
EmulatorWindow::GetSwapPostEffectForCvarValue(const std::string& cvar_value) {
  if (cvar_value == GetCvarValueForSwapPostEffect(
                        gpu::CommandProcessor::SwapPostEffect::kFxaa)) {
    return gpu::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (cvar_value == GetCvarValueForSwapPostEffect(
                        gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme)) {
    return gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return gpu::CommandProcessor::SwapPostEffect::kNone;
}

const char* EmulatorWindow::GetCvarValueForGuestOutputPaintEffect(
    ui::Presenter::GuestOutputPaintConfig::Effect effect) {
  switch (effect) {
    case ui::Presenter::GuestOutputPaintConfig::Effect::kCas:
      return "cas";
    case ui::Presenter::GuestOutputPaintConfig::Effect::kFsr:
      return "fsr";
    default:
      return "";
  }
}

ui::Presenter::GuestOutputPaintConfig::Effect
EmulatorWindow::GetGuestOutputPaintEffectForCvarValue(
    const std::string& cvar_value) {
  if (cvar_value == GetCvarValueForGuestOutputPaintEffect(
                        ui::Presenter::GuestOutputPaintConfig::Effect::kCas)) {
    return ui::Presenter::GuestOutputPaintConfig::Effect::kCas;
  }
  if (cvar_value == GetCvarValueForGuestOutputPaintEffect(
                        ui::Presenter::GuestOutputPaintConfig::Effect::kFsr)) {
    return ui::Presenter::GuestOutputPaintConfig::Effect::kFsr;
  }
  return ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear;
}

ui::Presenter::GuestOutputPaintConfig
EmulatorWindow::GetGuestOutputPaintConfigForCvars() {
  ui::Presenter::GuestOutputPaintConfig paint_config;
  paint_config.SetAllowOverscanCutoff(true);
  paint_config.SetEffect(GetGuestOutputPaintEffectForCvarValue(
      cvars::postprocess_scaling_and_sharpening));
  paint_config.SetCasAdditionalSharpness(
      float(cvars::postprocess_ffx_cas_additional_sharpness));
  paint_config.SetFsrMaxUpsamplingPasses(
      cvars::postprocess_ffx_fsr_max_upsampling_passes);
  paint_config.SetFsrSharpnessReduction(
      float(cvars::postprocess_ffx_fsr_sharpness_reduction));
  paint_config.SetDither(cvars::postprocess_dither);
  return paint_config;
}

void EmulatorWindow::ApplyDisplayConfigForCvars() {
  gpu::GraphicsSystem* graphics_system = emulator_->graphics_system();
  if (!graphics_system) {
    return;
  }

  gpu::CommandProcessor* command_processor =
      graphics_system->command_processor();
  if (command_processor) {
    command_processor->SetDesiredSwapPostEffect(
        GetSwapPostEffectForCvarValue(cvars::postprocess_antialiasing));
  }

  ui::Presenter* presenter = graphics_system->presenter();
  if (presenter) {
    presenter->SetGuestOutputPaintConfigFromUIThread(
        GetGuestOutputPaintConfigForCvars());
  }
}

void EmulatorWindow::OnKeyDown(ui::KeyEvent& e) {
  if (!emulator_initialized_) {
    return;
  }

  switch (e.virtual_key()) {
    case ui::VirtualKey::kO: {
      if (!e.is_ctrl_pressed()) {
        return;
      }
      FileOpen();
    } break;
    case ui::VirtualKey::kM: {
      if (!e.is_ctrl_pressed()) {
        return;
      }
      if (e.is_shift_pressed()) {
        // Ctrl+Shift+M: open the mousehook config (suspends mousehook while
        // the menu is up so the mouse is usable).
        OpenMousehookConfig();
      } else {
        ToggleMousehook();
      }
    } break;
    case ui::VirtualKey::kMultiply: {
      CpuTimeScalarReset();
    } break;
    case ui::VirtualKey::kSubtract: {
      CpuTimeScalarSetHalf();
    } break;
    case ui::VirtualKey::kAdd: {
      CpuTimeScalarSetDouble();
    } break;

    case ui::VirtualKey::kF3: {
      Profiler::ToggleDisplay();
    } break;

    case ui::VirtualKey::kF4: {
      GpuTraceFrame();
    } break;
    case ui::VirtualKey::kF5: {
      GpuClearCaches();
    } break;

    case ui::VirtualKey::kF6: {
      ToggleDisplayConfigDialog();
    } break;
    case ui::VirtualKey::kF11: {
      ToggleFullscreen();
    } break;
    case ui::VirtualKey::kF12: {
      TakeScreenshot();
    } break;

    case ui::VirtualKey::kEscape: {
      // Allow users to escape fullscreen (but not enter it).
      if (!window_->IsFullscreen()) {
        return;
      }
      SetFullscreen(false);
    } break;

#ifdef DEBUG
    case ui::VirtualKey::kF7: {
      // Save to file
      // TODO: Choose path based on user input, or from options
      // TODO: Spawn a new thread to do this.
      emulator()->SaveToFile("test.sav");
    } break;
    case ui::VirtualKey::kF8: {
      // Restore from file
      // TODO: Choose path from user
      // TODO: Spawn a new thread to do this.
      emulator()->RestoreFromFile("test.sav");
    } break;
#endif  // #ifdef DEBUG

    case ui::VirtualKey::kPause: {
      CpuBreakIntoDebugger();
    } break;
    case ui::VirtualKey::kCancel: {
      CpuBreakIntoHostDebugger();
    } break;

    case ui::VirtualKey::kF1: {
      ShowFAQ();
    } break;

    case ui::VirtualKey::kF2: {
      ShowBuildCommit();
    } break;

    case ui::VirtualKey::kF9: {
      if (e.is_shift_pressed() && !recently_launched_titles_.empty()) {
        // Shift+F9: pick a title update before launching the last title.
        const RecentTitleEntry& recent = recently_launched_titles_[0];
        OpenTitleUpdateSelector(recent.path_to_file, recent.title_id);
      } else {
        RunPreviouslyPlayedTitle();
      }
    } break;

    default:
      return;
  }

  e.set_handled(true);
}

void EmulatorWindow::OnMouseDown(const ui::MouseEvent& e) {
  if (imgui_drawer_->IsAnyDialogOpen()) {
    return;
  }

  if (e.button() == ui::MouseEvent::Button::kLeft) {
    ToggleFullscreenOnDoubleClick();
  }
}

void EmulatorWindow::OnMouseUp(const ui::MouseEvent& e) {
  last_mouse_up = steady_clock::now();
}

void EmulatorWindow::TakeScreenshot() {
  xe::ui::RawImage image;

  imgui_drawer_->EnableNotifications(false);

  if (!GetGraphicsSystemPresenter()->CaptureGuestOutput(image) ||
      GetGraphicsSystemPresenter() == nullptr) {
    XELOGE("Failed to capture guest output for screenshot");
    return;
  }

  imgui_drawer_->EnableNotifications(true);
  ExportScreenshot(image);
}

void EmulatorWindow::ExportScreenshot(const xe::ui::RawImage& image) {
  auto t = std::time(nullptr);

  // The format is: Year-Month-DayTHours-Minutes-Seconds based off ISO 8601
  std::string datetime =
      fmt::format("{:%Y-%m-%dT%H-%M-%S}", *std::localtime(&t));

  // Get the title id of the game because some titles contain characters that
  // cannot be used as a directory
  std::string title_id;
  if (emulator()->title_id()) {
    title_id = fmt::format("{:08X}", emulator()->title_id());
  } else {
    XELOGE("Failed to get the current title id");
    return;
  }

  // Find where xenia.exe or xenia_canary.exe is located and create a
  // screenshots folder
  auto screenshot_path =
      (xe::filesystem::GetExecutableFolder() / "screenshots" / title_id);

  if (!std::filesystem::exists(screenshot_path)) {
    std::filesystem::create_directories(screenshot_path);
  }

  std::string filename = fmt::format("{} - {}.png", title_id, datetime);
  SaveImage(screenshot_path / filename, image);

  const std::string notification_text =
      fmt::format("Screenshot saved: {}", filename);

  app_context_.CallInUIThread([&, notification_text]() {
    new xe::ui::HostNotificationWindow(imgui_drawer(), "Screenshot Created!",
                                       notification_text, 0);
  });
}

// Converts a RawImage into a PNG file
void EmulatorWindow::SaveImage(const std::filesystem::path& filepath,
                               const xe::ui::RawImage& image) {
  auto file = std::ofstream(filepath, std::ios::binary);
  if (!file.is_open()) {
    XELOGE("Failed to open file for writing: {}", filepath);
    return;
  }

  auto result = stbi_write_png_to_func(
      [](void* context, void* data, int size) {
        auto file = reinterpret_cast<std::ofstream*>(context);
        file->write(reinterpret_cast<const char*>(data), size);
      },
      &file, image.width, image.height, 4, image.data.data(),
      (int)image.stride);
  if (result == 0) {
    XELOGE("Failed to write PNG to file: {}", filepath);
    return;
  }
}

void EmulatorWindow::ToggleFullscreenOnDoubleClick() {
  if (cvars::disable_doubleclick_fullscreen) {
    return;
  }

  // this function tests if user has double clicked.
  // if double click was achieved the fullscreen gets toggled
  const auto now = steady_clock::now();  // current mouse event time
  constexpr int16_t mouse_down_max_threshold = 250;
  constexpr int16_t mouse_up_max_threshold = 250;
  constexpr int16_t mouse_up_down_max_delta = 100;
  // max delta to prevent 'chaining' of double clicks with next mouse events

  const auto last_mouse_down_delta = diff_in_ms(now, last_mouse_down);
  if (last_mouse_down_delta >= mouse_down_max_threshold) {
    last_mouse_down = now;
    return;
  }

  const auto last_mouse_up_delta = diff_in_ms(now, last_mouse_up);
  const auto mouse_event_deltas = diff_in_ms(last_mouse_up, last_mouse_down);
  if (last_mouse_up_delta >= mouse_up_max_threshold) {
    return;
  }

  if (mouse_event_deltas < mouse_up_down_max_delta) {
    ToggleFullscreen();
  }
}

void EmulatorWindow::FileDrop(const std::filesystem::path& path) {
  if (!emulator_initialized_) {
    return;
  }

  RunTitle(path);
}

void EmulatorWindow::FileOpen() {
  std::filesystem::path path;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title("Select Content Package");
  file_picker->set_extensions({
      {"Supported Files", "*.iso;*.xex;*.zar;*.*"},
      {"Disc Image (*.iso)", "*.iso"},
      {"Disc Archive (*.zar)", "*.zar"},
      {"Xbox Executable (*.xex)", "*.xex"},
      //{"Content Package (*.xcp)", "*.xcp" },
      {"All Files (*.*)", "*.*"},
  });
  if (file_picker->Show(window_.get())) {
    auto selected_files = file_picker->selected_files();
    if (!selected_files.empty()) {
      path = selected_files[0];
    }
    // Only run the title if a file is selected
    RunTitle(path);
  }
}

void EmulatorWindow::FileClose() { emulator_->TerminateTitle(); }

std::filesystem::path EmulatorWindow::DashboardFile(
    const std::string& name) const {
  return xe::filesystem::GetExecutableFolder() / "Dashboard" / name;
}

// Starting with nothing to run: play the boot animation, then the dashboard.
//
// The animation is a KERNEL CALL, not a title launch. AniStartBootAnimation
// loads bootanim.xex and runs its ordinal 1 on its own guest thread, exactly
// as xboxkrnl does - see xboxkrnl_ani.cc, which was read out of the real
// 17559 kernel. Nothing here executes bootanim.xex as a title.
//
// The animation LOOPS - it never ends on its own - so this runs it for a set
// time and then terminates it, which is what the console's own warm boot
// does. Waiting for it to finish waits forever.
void EmulatorWindow::BootToDashboard() {
  if (!cvars::dashboard_coldboot) {
    XELOGI("Boot: cold boot disabled; skipping the boot animation and dash");
    return;
  }
  std::error_code ec;
  const auto dashboard = DashboardFile("dash.xex");
  if (!std::filesystem::exists(dashboard, ec)) {
    XELOGI("Boot: no dashboard installed at {}; staying idle",
           xe::path_to_utf8(dashboard));
    return;
  }

  if (XFAILED(kernel::xboxkrnl::StartBootAnimation())) {
    XELOGI("Boot: no boot animation; going straight to the dash");
    RunTitle(dashboard);
    return;
  }

  // Armed AFTER the animation starts. RunTitle cancels a pending switch, so
  // anything the user opens during the animation wins and the dash never
  // yanks the screen away.
  CancelPendingDashboard();
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  dashboard_pending_cancelled_ = cancelled;
  dashboard_pending_thread_ =
      threading::Thread::Create({}, [this, cancelled, dashboard]() {
        xe::threading::Sleep(std::chrono::seconds(kBootAnimationSeconds));
        if (cancelled->load()) {
          return;
        }
        // Stopped the kernel way - through bootanim's own ordinal 2 - before
        // the dash takes the screen.
        kernel::xboxkrnl::TerminateBootAnimation();
        app_context().CallInUIThread([this, dashboard, cancelled]() {
          if (!cancelled->load()) {
            RunTitle(dashboard);
          }
        });
      });
  if (dashboard_pending_thread_) {
    dashboard_pending_thread_->set_name("Boot Animation");
  }
}

void EmulatorWindow::CancelPendingDashboard() {
  if (dashboard_pending_cancelled_) {
    dashboard_pending_cancelled_->store(true);
    dashboard_pending_cancelled_.reset();
  }
  dashboard_pending_thread_.reset();
}

// Stops the running title and goes back to an empty emulator, rather than
// closing the emulator with it. The title's filesystem is dropped the same way
// a failed launch drops it - leaving GAME:/UPDATE: pointing at a finished
// title is what put a dead device in front of the next one.
void EmulatorWindow::ExitTitle() {
  if (!emulator_->is_title_open()) {
    return;
  }
  ClearDialogs();
  emulator_->TerminateTitle(cvars::title_switch_clear_handles);
  emulator_->file_system()->Clear();
  // The recent list gained this title's play time when the session ended.
  LoadRecentlyLaunchedTitles();
  UpdateTitle();
}

// A title switch - a game changing mode, or launching another title - is done
// by the guest writing launch data and terminating. That data is only read
// when the emulator starts, so the switch cannot happen inside this process:
// something has to run again. If the user exits with one waiting, this is that
// something.
bool EmulatorWindow::ShouldRelaunchOnExit() const {
  if (!exit_requested_from_menu_) {
    return false;
  }

  std::error_code error;
  const std::filesystem::path folder = xe::filesystem::GetExecutableFolder();

  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (error) {
      break;
    }

    if (!entry.is_regular_file(error) || error) {
      continue;
    }

    // launch_data.bin is the one the kernel writes; the pattern is wider so a
    // file dropped in by anything else that drives a launch counts too.
    const std::string name =
        xe::utf8::lower_ascii(xe::path_to_utf8(entry.path().filename()));

    if (name.starts_with("launch") && name.ends_with(".bin")) {
      return true;
    }
  }

  return false;
}

void EmulatorWindow::RelaunchForPendingLaunchData() const { xe::LaunchSelf(); }

void EmulatorWindow::FileExit() {
  exit_requested_from_menu_ = true;
  window_->RequestClose();
}

void EmulatorWindow::InstallContent() {
  std::vector<std::filesystem::path> paths;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(true);
  file_picker->set_title("Select Content Package");
  file_picker->set_extensions({
      {"All Files (*.*)", "*.*"},
      {"Content Archive (*.zip)", "*.zip"},
  });
  if (file_picker->Show(window_.get())) {
    paths = file_picker->selected_files();
  }

  InstallContentPackages(paths);
}

namespace {
// Defined with the rest of the setup helpers, below.
bool IsSystemUpdateTree(const std::filesystem::path& staging);
std::string InstallSystemUpdateTree(
    const std::filesystem::path& staging,
    std::vector<std::filesystem::path>* out_packages);
std::string ReplaceDashboardFontsFromFlash(
    const std::filesystem::path& dashboard);
}  // namespace

// The install half of InstallContent, without the file picker, so a package
// obtained some other way - a downloaded title update - goes through exactly
// the same path: header scan, DLC/TU targeting, then the install dialog.
void EmulatorWindow::InstallContentPackages(
    const std::vector<std::filesystem::path>& paths,
    const std::string& title_update_name) {
  if (paths.empty()) {
    return;
  }

  std::vector<std::filesystem::path> packages;
  auto staging_dirs = std::make_shared<std::vector<std::filesystem::path>>();
  std::string archive_report;

  for (const auto& path : paths) {
    // A raw NAND dump is not an XContent package; it is where the 360 UI
    // assets come from. Recognise it here and route it to the extractor,
    // which runs synchronously so the report below is the real outcome.
    if (CanonicalizeFileExtension(path) != ".zip" &&
        kernel::xam::xui::IsFlashImage(path)) {
      const auto report = kernel::xam::xui::InstallFromFlashImage(
          path, kernel::xam::xui::DefaultAssetDirectory());
      archive_report += report.text;
      if (report.ok) {
        const auto dashboard =
            xe::filesystem::GetExecutableFolder() / "Dashboard";
        // A system title loads its typefaces off media:, so the flash ones go
        // into the Dashboard folder as well - whichever order the two installs
        // happen in, the flash faces are the ones left standing.
        archive_report += ReplaceDashboardFontsFromFlash(dashboard);
        // The boot animation is a whole module, and NAND is the only place it
        // exists - a system update does not carry it. Without this the
        // dashboard comes up with no animation before it.
        if (kernel::xam::xui::ExtractFlashModule(path, "bootanim.dll",
                                                 dashboard / "bootanim.xex")) {
          archive_report += "Dashboard: bootanim.xex taken from the flash\n";
        } else {
          archive_report += "Dashboard: no bootanim.xex in this flash image\n";
        }
        ReloadDashboardUIAssets();
      }
      continue;
    }

    if (CanonicalizeFileExtension(path) != ".zip") {
      packages.push_back(path);
      continue;
    }

    std::error_code ec;
    const auto staging = std::filesystem::temp_directory_path(ec) /
                         ("nexia-content-" + xe::path_to_utf8(path.stem()));
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec || !kernel::xna::ExtractZipArchive(path, staging)) {
      XELOGE("InstallContent: could not extract {}", xe::path_to_utf8(path));
      archive_report += "Could not extract " + xe::path_to_utf8(path) + "\n";
      std::filesystem::remove_all(staging, ec);
      continue;
    }
    staging_dirs->push_back(staging);

    // The USB system update: mostly the console's own modules, which belong
    // beside dash.xex. It is NOT free of XContent, though - the avatar asset
    // packs ride along in it, and those have to be installed into the content
    // tree like any other package. Anything found is appended to `packages`
    // and goes through the normal install below.
    if (IsSystemUpdateTree(staging)) {
      archive_report += InstallSystemUpdateTree(staging, &packages);
      ReloadDashboardUIAssets();
      continue;
    }

    // A zip made by the UI asset packager carries nexia-ui.json, not XContent.
    if (kernel::xam::xui::IsAssetArchiveRoot(staging)) {
      const auto report = kernel::xam::xui::InstallFromArchiveRoot(
          staging, kernel::xam::xui::DefaultAssetDirectory());
      archive_report += report.text;
      if (report.ok) {
        ReloadDashboardUIAssets();
      }
      continue;
    }

    size_t found = 0;
    for (const auto& file : std::filesystem::recursive_directory_iterator(
             staging,
             std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (!file.is_regular_file(ec)) {
        continue;
      }
      const auto header =
          vfs::XContentContainerDevice::ReadContainerHeader(file.path());
      if (!header || !header->content_header.is_magic_valid()) {
        continue;
      }
      if (header->content_metadata.content_type == XContentType::kInstaller &&
          header->content_metadata.execution_info.title_id.get() ==
              kernel::kDashboardID) {
        XELOGI("InstallContent: skipping system update {}",
               xe::path_to_utf8(file.path().filename()));
        continue;
      }
      XELOGI(
          "InstallContent: {} holds {} (title {:08X}, type {:08X})",
          xe::path_to_utf8(path.filename()),
          xe::path_to_utf8(file.path().filename()),
          header->content_metadata.execution_info.title_id.get(),
          static_cast<uint32_t>(header->content_metadata.content_type.get()));
      packages.push_back(file.path());
      ++found;
    }
    if (!found) {
      archive_report +=
          "No content packages in " + xe::path_to_utf8(path) + "\n";
    }
  }

  if (!archive_report.empty()) {
    new xe::ui::HostNotificationWindow(imgui_drawer(), "Install Content",
                                       archive_report, 0);
  }

  if (packages.empty()) {
    std::error_code ec;
    for (const auto& staging : *staging_dirs) {
      std::filesystem::remove_all(staging, ec);
    }
    return;
  }

  std::shared_ptr<std::vector<Emulator::ContentInstallEntry>>
      content_installation_status =
          std::make_shared<std::vector<Emulator::ContentInstallEntry>>();

  for (const auto& path : packages) {
    content_installation_status->push_back({path});
    content_installation_status->back().title_update_name_ = title_update_name;
  }

  for (auto& entry : *content_installation_status) {
    emulator_->ProcessContentPackageHeader(entry.path_, entry);
  }

  // Saves and DLC belong to a specific title update, so the target has to be
  // settled before anything is written. Default everything to the active
  // update; the DLC dialog overrides its own rows if the user picks another.
  if (auto* tu_manager = emulator_->title_update_manager()) {
    for (auto& entry : *content_installation_status) {
      if (entry.content_type_ == XContentType::kInstaller) {
        continue;
      }
      std::string active = tu_manager->GetActive(entry.title_id_);
      entry.target_update_id_ =
          active.empty() ? kernel::util::kNoTitleUpdateId : active;
    }
  }

  auto start_install = [this, content_installation_status, staging_dirs]() {
    auto installationThread =
        std::thread([this, content_installation_status, staging_dirs] {
          for (auto& entry : *content_installation_status) {
            emulator_->InstallContentPackage(entry.path_, entry);
          }
          std::error_code ec;
          for (const auto& staging : *staging_dirs) {
            std::filesystem::remove_all(staging, ec);
          }
        });
    installationThread.detach();

    new ContentInstallDialog(imgui_drawer_.get(), *this,
                             content_installation_status);
  };

  // The DLC dialog closes itself immediately (and calls straight through) when
  // the batch contains no add-on content, so this stays a single popup level.
  new DlcTargetDialog(imgui_drawer_.get(), *this, content_installation_status,
                      start_install);
}

void EmulatorWindow::ExtractZarchive() {
  std::vector<std::filesystem::path> zarchive_files;
  std::filesystem::path extract_dir;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(true);
  file_picker->set_title("Select Zar Package");
  file_picker->set_extensions({
      {"Zarchive Files (*.zar)", "*.zar"},
  });

  if (file_picker->Show(window_.get())) {
    zarchive_files = file_picker->selected_files();
  }

  if (zarchive_files.empty()) {
    return;
  }

  file_picker->set_type(ui::FilePicker::Type::kDirectory);
  file_picker->set_title("Select Directory to Extract");

  if (file_picker->Show(window_.get())) {
    extract_dir = file_picker->selected_files().front();
  }

  if (extract_dir.empty()) {
    return;
  }

  std::string extract_overview = "";

  for (auto& zarchive_file_path : zarchive_files) {
    extract_overview += "\n" + path_to_utf8(zarchive_file_path);
  }

  app_context_.CallInUIThread([&]() {
    new xe::ui::HostNotificationWindow(imgui_drawer(), "Extracting...",
                                       string_util::trim(extract_overview), 0);
  });

  auto run = [this, extract_dir, zarchive_files]() -> void {
    std::string summary = "";

    for (auto& zarchive_file_path : zarchive_files) {
      // Normalize the path and make absolute.
      auto abs_path = std::filesystem::absolute(zarchive_file_path);
      std::filesystem::path abs_extract_dir;

      if (zarchive_files.size() > 1) {
        abs_extract_dir =
            std::filesystem::absolute((extract_dir / abs_path.stem()));
      } else {
        abs_extract_dir = std::filesystem::absolute(extract_dir);
      }

      XELOGI("Extracting zar package: {}\n",
             zarchive_file_path.filename().string());

      auto result =
          emulator_->ExtractZarchivePackage(abs_path, abs_extract_dir);

      if (result != X_STATUS_SUCCESS) {
        std::error_code ec;

        if (!std::filesystem::is_empty(abs_extract_dir)) {
          std::filesystem::remove(abs_extract_dir, ec);
        }

        summary += fmt::format("\nFailed: {}", zarchive_file_path);

        XELOGE("Failed to extract Zarchive package.", result);
      } else {
        summary += fmt::format("\nSuccess: {}", abs_extract_dir);
      }
    }

    new xe::ui::HostNotificationWindow(imgui_drawer(), "Zar Extraction Summary",
                                       string_util::trim(summary), 0);
  };

  auto zarThread = std::thread(run);
  zarThread.detach();
}

void EmulatorWindow::CreateZarchive() {
  std::vector<std::filesystem::path> content_dirs;
  std::filesystem::path zarchive_dir;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kDirectory);
  file_picker->set_multi_selection(true);
  file_picker->set_title("Select Contents");

  if (file_picker->Show(window_.get())) {
    content_dirs = file_picker->selected_files();
  }

  if (content_dirs.empty()) {
    return;
  }

  if (content_dirs.size() == 1) {
    file_picker->set_mode(ui::FilePicker::Mode::kSave);
    file_picker->set_type(ui::FilePicker::Type::kFile);
    file_picker->set_multi_selection(false);
    file_picker->set_file_name(content_dirs.front().filename().string());
    file_picker->set_default_extension("zar");
    file_picker->set_title("Zarchive File");
    file_picker->set_extensions({
        {"Zarchive File (*.zar)", "*.zar"},
    });
  } else {
    file_picker->set_title("Output Directory");
  }

  if (file_picker->Show(window_.get())) {
    zarchive_dir = file_picker->selected_files().front();
  }

  if (zarchive_dir.empty()) {
    return;
  }

  std::string create_overview = "";

  std::map<std::filesystem::path, std::filesystem::path> zarchive_files{};

  for (auto& content_path : content_dirs) {
    // Normalize the path and make absolute.
    auto abs_content_dir = std::filesystem::absolute(content_path);
    std::filesystem::path abs_zarchive_file;

    if (content_dirs.size() > 1) {
      abs_zarchive_file = std::filesystem::absolute(
          (zarchive_dir / abs_content_dir.filename().concat(".zar")));
    } else {
      abs_zarchive_file = std::filesystem::absolute(zarchive_dir);
    }

    zarchive_files[content_path] = abs_zarchive_file;

    create_overview += "\n" + path_to_utf8(abs_zarchive_file);
  }

  app_context_.CallInUIThread([&]() {
    new xe::ui::HostNotificationWindow(imgui_drawer(), "Creating...",
                                       string_util::trim(create_overview), 0);
  });

  auto run = [this, zarchive_files]() -> void {
    std::string summary = "";

    for (auto const& [content_path, zarchive_file] : zarchive_files) {
      // Normalize the path and make absolute.
      auto abs_content_dir = std::filesystem::absolute(content_path);

      XELOGI("Creating zar package: {}\n", zarchive_file.filename().string());

      auto result =
          emulator_->CreateZarchivePackage(abs_content_dir, zarchive_file);

      if (result != X_ERROR_SUCCESS) {
        std::error_code ec;

        // delete incomplete output file
        std::filesystem::remove(zarchive_file, ec);

        summary += fmt::format("\nFailed: {}", abs_content_dir);

        XELOGE("Failed to create Zarchive package.", result);
      } else {
        summary += fmt::format("\nSuccess: {}", zarchive_file);
      }
    }

    new xe::ui::HostNotificationWindow(imgui_drawer(), "Zar Creation Summary",
                                       string_util::trim(summary), 0);
  };

  auto zarThread = std::thread(run);
  zarThread.detach();
}

void EmulatorWindow::ShowContentDirectory() {
  auto content_root = emulator_->content_root();

  if (!std::filesystem::exists(content_root)) {
    std::filesystem::create_directories(content_root);
  }

  LaunchFileExplorer(content_root);
}

void EmulatorWindow::DumpXLast() { emulator()->DumpXLast(); }

void EmulatorWindow::CpuTimeScalarReset() {
  Clock::set_guest_time_scalar(1.0);
  UpdateTitle();
}

void EmulatorWindow::CpuTimeScalarSetHalf() {
  Clock::set_guest_time_scalar(Clock::guest_time_scalar() / 2.0);
  UpdateTitle();
}

void EmulatorWindow::CpuTimeScalarSetDouble() {
  Clock::set_guest_time_scalar(Clock::guest_time_scalar() * 2.0);
  UpdateTitle();
}

void EmulatorWindow::CpuBreakIntoDebugger() {
  if (!cvars::debug) {
    xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(), "Xenia Debugger",
                                        "Xenia must be launched with the "
                                        "--debug flag in order to enable "
                                        "debugging.");
    return;
  }
  auto processor = emulator()->processor();
  if (processor->execution_state() == cpu::ExecutionState::kRunning) {
    // Currently running, so interrupt (and show the debugger).
    processor->Pause();
  } else {
    // Not running, so just bring the debugger into focus.
    processor->ShowDebugger();
  }
}

void EmulatorWindow::CpuBreakIntoHostDebugger() { xe::debugging::Break(); }

void EmulatorWindow::GpuTraceFrame() {
  emulator()->graphics_system()->RequestFrameTrace();
}

void EmulatorWindow::GpuClearCaches() {
  emulator()->graphics_system()->ClearCaches();
}

void EmulatorWindow::SetFullscreen(bool fullscreen_) {
  if (window_->IsFullscreen() == fullscreen_) {
    return;
  }

  OVERRIDE_bool(fullscreen, fullscreen_);

  window_->SetFullscreen(fullscreen_);
  window_->SetCursorVisibility(fullscreen_
                                   ? ui::Window::CursorVisibility::kAutoHidden
                                   : ui::Window::CursorVisibility::kVisible);
}

void EmulatorWindow::ToggleFullscreen() {
  SetFullscreen(!window_->IsFullscreen());
}

void EmulatorWindow::SetAutoCheckForUpdates(bool state) {
  OVERRIDE_bool(auto_check_updates, state);
}

void EmulatorWindow::UpdateCompletionNotification() {
  app_context_.CallInUIThread([&]() {
    std::string message = fmt::format("Build Date: {} ({})", XE_BUILD_DATE,
                                      XE_BUILD_COMMIT_SHORT);

    new xe::ui::HostNotificationWindow(imgui_drawer(), "Update Completed",
                                       message.c_str(), 0, 9);
  });
}

void EmulatorWindow::ToggleDisplayConfigDialog() {
  if (!display_config_dialog_) {
    display_config_dialog_ =
        std::make_unique<DisplayConfigDialog>(imgui_drawer_.get(), *this);
  } else {
    if (display_config_dialog_->IsClosing()) {
      display_config_dialog_.release();
    } else {
      display_config_dialog_.reset();
    }
  }
}

void EmulatorWindow::ToggleProfilesConfigDialog() {
  if (!profile_config_dialog_) {
    disable_hotkeys_ = true;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 1);
    profile_config_dialog_ =
        std::make_unique<ProfileConfigDialog>(imgui_drawer_.get(), this);
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_++;
    emulator_->kernel_state()->xam_state()->set_profile_dialog_open(true);
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 0);
    if (profile_config_dialog_->IsClosing()) {
      profile_config_dialog_.release();
    } else {
      profile_config_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
    emulator_->kernel_state()->xam_state()->set_profile_dialog_open(false);
  }
}

void EmulatorWindow::ToggleGamerpicBrowserDialog() {
  if (!gamerpic_browser_dialog_) {
    disable_hotkeys_ = true;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 1);
    gamerpic_browser_dialog_ =
        TitleGamerpicBrowser::Create(imgui_drawer_.get(), this);
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_++;
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 0);
    if (gamerpic_browser_dialog_->IsClosing()) {
      gamerpic_browser_dialog_.release();
    } else {
      gamerpic_browser_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }
}

void EmulatorWindow::ToggleXMPConfigDialog() {
  if (!xmp_config_dialog_) {
    xmp_config_dialog_ = std::unique_ptr<XMPConfigDialog>(
        new XMPConfigDialog(imgui_drawer_.get(), *this));
  } else {
    xmp_config_dialog_.reset();
  }
}

void EmulatorWindow::ToggleConsoleSettingsDialog() {
  if (!console_settings_dialog_) {
    console_settings_dialog_ =
        std::unique_ptr<ConsoleSettingsDialog>(new ConsoleSettingsDialog(
            imgui_drawer_.get(), *this, emulator_->kernel_state()->xconfig()));
  } else {
    if (console_settings_dialog_->IsClosing()) {
      console_settings_dialog_.release();
    } else {
      console_settings_dialog_.reset();
    }
  }
}

void EmulatorWindow::ToggleFriendsDialog() {
  if (!friends_manager_dialog_) {
    disable_hotkeys_ = true;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 1);
    friends_manager_dialog_ =
        std::make_unique<ManagerDialog>(imgui_drawer_.get(), this);
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_++;
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 0);
    if (friends_manager_dialog_->IsClosing()) {
      friends_manager_dialog_.release();
    } else {
      friends_manager_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }
}

// An ImGuiDialog owns itself: the drawer deletes it the frame after it closes.
// So opening one hands it over, and closing one only asks - the teardown
// below runs from the dialog's own destructor, whether it went away because
// the menu item was used again or because the user pressed Close inside it.
void EmulatorWindow::OnMessagesDialogClosed(MessagesDialog** slot) {
  *slot = nullptr;

  disable_hotkeys_ = false;
  emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 0);
  emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
}

void EmulatorWindow::ToggleTextMessagesDialog() {
  if (text_messages_dialog_) {
    text_messages_dialog_->Close();
    return;
  }

  disable_hotkeys_ = true;
  emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 1);
  emulator_->kernel_state()->xam_state()->xam_dialogs_shown_++;

  text_messages_dialog_ = new MessagesDialog(imgui_drawer_.get(), this,
                                             MessagesDialog::Mode::kText);

  text_messages_dialog_->set_closed_callback(
      [this]() { OnMessagesDialogClosed(&text_messages_dialog_); });
}

// The console's own editor, installed whole from the system update. Empty when
// the update has not been imported, which is the normal case.
std::filesystem::path EmulatorWindow::AvatarEditorTitlePath() const {
  std::error_code ec;
  const auto path =
      xe::filesystem::GetExecutableFolder() / "Dashboard" / "AvatarEditor.xex";
  return std::filesystem::exists(path, ec) ? path : std::filesystem::path();
}

void EmulatorWindow::ToggleAvatarEditorDialog() {
  if (avatar_editor_dialog_) {
    avatar_editor_dialog_->Close();
    return;
  }

  // With the system update imported, run the real thing. There is no launch
  // blob to carry here - the menu is not a guest launch - so RunTitle's own
  // switch-in-place path does the work.
  const auto title = AvatarEditorTitlePath();
  if (!title.empty()) {
    XELOGI("Avatar Editor: running {}", xe::path_to_utf8(title));
    RunTitle(title);
    return;
  }
  OpenAvatarEditorDialog();
}

void EmulatorWindow::OpenAvatarEditorDialog() {
  if (avatar_editor_dialog_) {
    return;
  }
  disable_hotkeys_ = true;
  emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 1);
  emulator_->kernel_state()->xam_state()->xam_dialogs_shown_++;

  avatar_editor_dialog_ = new AvatarEditorDialog(imgui_drawer_.get(), this);
  avatar_editor_dialog_->set_closed_callback([this]() {
    const uint32_t saved_users =
        avatar_editor_dialog_ ? avatar_editor_dialog_->saved_user_mask() : 0;
    avatar_editor_dialog_ = nullptr;
    disable_hotkeys_ = false;
    auto* kernel = emulator_->kernel_state();
    kernel->xam_state()->xam_dialogs_shown_--;
    std::thread([kernel, saved_users]() {
      xe::threading::Sleep(std::chrono::milliseconds(100));
      if (saved_users) {
        kernel->BroadcastNotification(kXNotificationSystemProfileSettingChanged,
                                      saved_users);
        kernel->BroadcastNotification(kXNotificationSystemAvatarChanged,
                                      saved_users);
      }
      kernel->BroadcastNotification(kXNotificationSystemUI, 0);
    }).detach();
  });
}

// What a guest asks for through XamLaunchAvatarEditor - the dashboard's Avatar
// Editor tile, among others. It is a LAUNCH: the caller expects to be replaced
// by AvatarEditor.xex carrying the blob XamLaunchAvatarEditor just packed, and
// to be brought back afterwards. Only a tree without the system update gets the
// built-in dialog instead.
void EmulatorWindow::ShowAvatarEditorDialog() {
  const auto title = AvatarEditorTitlePath();
  if (title.empty()) {
    OpenAvatarEditorDialog();
    return;
  }
  auto xam = emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
      "xam.xex");
  // RunTitle clears the loader data when it finds a title open, so the blob is
  // carried across the teardown by hand and the title is stopped here first -
  // the same order SwitchTitle uses.
  std::vector<uint8_t> launch_data;
  if (xam) {
    launch_data = xam->loader_data().launch_data;
  }
  if (emulator_->is_title_open()) {
    kernel::xam::RecordLaunchOrigin();
    emulator_->TerminateTitle(cvars::title_switch_clear_handles);
  }
  if (xam) {
    auto& loader_data = xam->loader_data();
    loader_data.host_path = xe::path_to_utf8(std::filesystem::absolute(title));
    loader_data.launch_path.clear();
    loader_data.launch_flags = 0;
    loader_data.launch_data = std::move(launch_data);
    loader_data.command_line.clear();
  }
  XELOGI("Avatar Editor: the guest asked for it; running {}",
         xe::path_to_utf8(title));
  RunTitle(title);
}

void EmulatorWindow::ToggleXuiSceneDialog() {
  if (xui_scene_dialog_) {
    xui_scene_dialog_->Close();
    return;
  }
  xui_scene_dialog_ = new XuiSceneDialog(imgui_drawer_.get(), this);
  xui_scene_dialog_->set_closed_callback(
      [this]() { xui_scene_dialog_ = nullptr; });
}

void EmulatorWindow::SwitchTitle() {
  auto xam = emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
      "xam.xex");
  if (!xam) {
    return;
  }
  const auto [host_path, launch_path] = xam->ResolveLaunchTarget();
  XELOGI("Switching title in place to {} '{}'", xe::path_to_utf8(host_path),
         launch_path);
  emulator_->TerminateTitle(cvars::title_switch_clear_handles);
  xam->loader_data().host_path = xe::path_to_utf8(host_path);
  xam->loader_data().launch_path = launch_path;
  const X_STATUS result = RunTitle(host_path);
  xam->loader_data().launch_path.clear();
  if (XSUCCEEDED(result)) {
    std::error_code error;
    std::filesystem::remove(std::filesystem::path(std::string(
                                kernel::xam::kXamModuleLoaderDataFileName)),
                            error);
    return;
  }
  XELOGE("In-place title switch failed ({:08X}); restarting the emulator",
         result);
  config::SaveConfig();
  xe::LaunchSelf();
  xe::FlushLog();
  std::quick_exit(0);
}

void EmulatorWindow::ToggleVoiceMessagesDialog() {
  if (voice_messages_dialog_) {
    voice_messages_dialog_->Close();
    return;
  }

  disable_hotkeys_ = true;
  emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 1);
  emulator_->kernel_state()->xam_state()->xam_dialogs_shown_++;

  voice_messages_dialog_ = new MessagesDialog(imgui_drawer_.get(), this,
                                              MessagesDialog::Mode::kVoice);

  voice_messages_dialog_->set_closed_callback(
      [this]() { OnMessagesDialogClosed(&voice_messages_dialog_); });
}

void EmulatorWindow::UpdateSocialMenu() {
  if (!social_menu_item_) {
    return;
  }

  const kernel::MessageCounts counts =
      emulator_->GetXboxLiveAPI()->message_counts();

  social_texts_item_->SetText(fmt::format("Texts ({})", counts.text));
  social_vm_item_->SetText(fmt::format("VM ({})", counts.voice));

  // The dot goes to the right of the label, where a submenu arrow would sit on
  // a child item. A native menu bar has no way to colour part of a label, so
  // this is the glyph itself rather than a tinted draw.
  social_menu_item_->SetText(counts.total() ? "&Social \xE2\x97\x8F"
                                            : "&Social");

  window_->CompleteMainMenuItemsUpdate();
}

void EmulatorWindow::ToggleUpdaterDialog() {
  if (!updater_dialog_) {
    const bool auto_check_update =
        update_info_.valid() ? update_info_.get().update_available : false;

    disable_hotkeys_ = true;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 1);
    updater_dialog_ = std::make_unique<UpdaterDialog>(
        updater_, auto_check_update, imgui_drawer_.get(), this);
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_++;
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 0);
    if (updater_dialog_->IsClosing()) {
      updater_dialog_.release();
    } else {
      updater_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }
}

void EmulatorWindow::ToggleCompletionDialog() {
  if (!updater_completion_dialog_) {
    disable_hotkeys_ = true;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 1);
    updater_completion_dialog_ = std::make_unique<UpdaterCompletionDialog>(
        imgui_drawer_.get(), this, cvar::updated);
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_++;
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 0);
    if (updater_completion_dialog_->IsClosing()) {
      updater_completion_dialog_.release();
    } else {
      updater_completion_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }
}

void EmulatorWindow::ToggleNetplaySettingsDialog() {
  if (!netplay_settings_dialog_) {
    disable_hotkeys_ = true;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 1);
    netplay_settings_dialog_ = std::make_unique<NetplaySettingsDialog>(
        imgui_drawer_.get(), this, emulator_->GetNetworkAdapterManager());
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_++;
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 0);
    if (netplay_settings_dialog_->IsClosing()) {
      netplay_settings_dialog_.release();
    } else {
      netplay_settings_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }
}

void EmulatorWindow::ToggleNetplayStatusDialog() {
  if (!netplay_status_dialog_) {
    disable_hotkeys_ = true;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 1);
    netplay_status_dialog_ = std::make_unique<NetplayStatusDialog>(
        imgui_drawer_.get(), this, emulator_->GetNetworkAdapterManager());
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_++;
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI, 0);
    if (netplay_status_dialog_->IsClosing()) {
      netplay_status_dialog_.release();
    } else {
      netplay_status_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }
}

void EmulatorWindow::SetDashboardColdBoot(bool enabled) {
  OVERRIDE_bool(dashboard_coldboot, enabled);
  config::SaveConfig();
  XELOGI("Dashboard cold boot {}", enabled ? "enabled" : "disabled");
}

void EmulatorWindow::ToggleControllerVibration() {
  auto input_sys = emulator()->input_system();
  if (input_sys) {
    auto input_lock = input_sys->lock();

    input_sys->ToggleVibration();

    if (emulator_->kernel_state()) {
      emulator_->kernel_state()->BroadcastNotification(
          kXNotificationSystemProfileSettingChanged,
          static_cast<uint32_t>(input_sys->GetConnectedSlots().count()));
    }
  }
}

void EmulatorWindow::ShowCompatibility() {
  const std::string_view base_url =
      "https://github.com/xenia-canary/game-compatibility/issues";
  std::string url;
  // Avoid searching for a title ID of "00000000".
  uint32_t title_id = emulator_->title_id();
  if (!title_id) {
    url = base_url;
  } else {
    url = fmt::format("{}?q=is%3Aissue+is%3Aopen+{:08X}", base_url, title_id);
  }
  LaunchWebBrowser(url);
}

void EmulatorWindow::ShowFAQ() {
  LaunchWebBrowser("https://github.com/xenia-canary/xenia-canary/wiki/FAQ");
}

void EmulatorWindow::ShowBuildCommit() {
#ifdef XE_BUILD_IS_PR
  LaunchWebBrowser(
      "https://github.com/Nexia360/Nexia360/pull/" XE_BUILD_PR_NUMBER);
#else
  LaunchWebBrowser(
      "https://github.com/Nexia360/Nexia360/commit/" XE_BUILD_COMMIT);
#endif
}

void EmulatorWindow::UpdateTitle() {
  xe::StringBuffer sb;
  sb.Append(base_title_);

  // Title information, if available
  if (emulator()->is_title_open()) {
    sb.AppendFormat(" | [{:08X}", emulator()->title_id());
    auto title_version = emulator()->title_version();
    if (!title_version.empty()) {
      sb.Append(" v");
      sb.Append(title_version);
    }
    sb.Append("]");

    auto title_name = emulator()->title_name();
    if (!title_name.empty()) {
      sb.Append(" ");
      sb.Append(title_name);
    }
  }

  // Graphics system name, if available
  auto graphics_system = emulator()->graphics_system();
  if (graphics_system) {
    auto graphics_name = graphics_system->name();
    if (!graphics_name.empty()) {
      sb.Append(" <");
      sb.Append(graphics_name);
      sb.Append(">");
    }
  }

  if (Clock::guest_time_scalar() != 1.0) {
    sb.AppendFormat(" (@{:.2f}x)", Clock::guest_time_scalar());
  }

  if (initializing_shader_storage_) {
    sb.Append(" (Preloading shaders\u2026)");
  }

  patcher::Patcher* patcher = emulator()->patcher();
  if (patcher && patcher->IsAnyPatchApplied()) {
    sb.Append(" [Patches Applied]");
  }

  patcher::PluginLoader* pluginloader = emulator()->plugin_loader();
  if (pluginloader && pluginloader->IsAnyPluginLoaded()) {
    sb.Append(" [Plugins Loaded]");
  }

  window_->SetTitle(sb.to_string_view());
}

void EmulatorWindow::SetInitializingShaderStorage(bool initializing) {
  if (initializing_shader_storage_ == initializing) {
    return;
  }
  initializing_shader_storage_ = initializing;
  UpdateTitle();
}

// Notes:
// SDL and XInput both support the guide button
//
// Assumes titles do not use the guide button.
// For titles that do such as dashboards these titles could be excluded based on
// their title ID.
//
// Xbox Gamebar:
// If the Xbox Gamebar overlay is enabled Windows will consume the guide
// button's input, this can be seen using hid-demo.
//
// Workaround: Detect if the Xbox Gamebar overlay is enabled then use the BACK
// button instead of the GUIDE button. Therefore BACK and GUIDE are reserved
// buttons for hotkeys.
//
// This is not an issue with DualShock controllers because Windows will not
// open the gamebar overlay using the PlayStation menu button.
//
// Xbox One S Controller:
// The guide button on this controller is very buggy no idea why.
// Using xinput usually registers after a double tap.
// Doesn't work at all using SDL.
// Needs more testing.
//
// Steam:
// If guide button focus is enabled steam will open.
// Steam uses BACK + GUIDE to open an On-Screen keyboard, however this is not a
// problem since both these buttons are reserved.
const std::map<int, EmulatorWindow::ControllerHotKey> controller_hotkey_map = {
    // Must use the Guide Button for all pass through hotkeys
    {X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleLogging,
         "B + Guide = Toggle between loglevel set in config and the 'Disabled' "
         "loglevel.",
         true, true)},
    {X_INPUT_GAMEPAD_Y | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleFullscreen,
         "Y + Guide = Toggle Fullscreen", true)},
    {X_INPUT_GAMEPAD_X | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ClearMemoryPageState,
         "X + Guide = Toggle Clear Memory Page State", true)},

    {X_INPUT_GAMEPAD_RIGHT_SHOULDER | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ClearGPUCache,
         "Right Shoulder + Guide = Clear GPU Cache", true)},
    {X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleControllerVibration,
         "Left Shoulder + Guide = Toggle Controller Vibration", true)},

    // CPU Time Scalar with no rumble feedback
    {X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::CpuTimeScalarSetHalf,
         "D-PAD Down + Guide = Half CPU Scalar")},
    {X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::CpuTimeScalarSetDouble,
         "D-PAD Up + Guide = Double CPU Scalar")},
    {X_INPUT_GAMEPAD_DPAD_RIGHT | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::CpuTimeScalarReset,
         "D-PAD Right + Guide = Reset CPU Scalar")},

    // non-pass through hotkeys
    {X_INPUT_GAMEPAD_Y, EmulatorWindow::ControllerHotKey(
                            EmulatorWindow::ButtonFunctions::ToggleFullscreen,
                            "Y = Toggle Fullscreen", true, false)},
    {X_INPUT_GAMEPAD_START, EmulatorWindow::ControllerHotKey(
                                EmulatorWindow::ButtonFunctions::RunTitle,
                                "Start = Run Selected Title", false, false)},
    {X_INPUT_GAMEPAD_BACK | X_INPUT_GAMEPAD_START,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleLogging,
         "Back + Start = Toggle between loglevel set in config and the "
         "'Disabled' loglevel.",
         false, false)},
    {X_INPUT_GAMEPAD_DPAD_DOWN,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::IncTitleSelect,
         "D-PAD Down = Title Selection +1", true, false)},
    {X_INPUT_GAMEPAD_DPAD_UP,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::DecTitleSelect,
         "D-PAD Up = Title Selection -1", true, false)}};

EmulatorWindow::ControllerHotKey EmulatorWindow::ProcessControllerHotkey(
    int buttons) {
  // Default return value
  EmulatorWindow::ControllerHotKey Unknown_hotkey = {};

  if (buttons == 0) {
    return Unknown_hotkey;
  }

  if (disable_hotkeys_.load()) {
    return Unknown_hotkey;
  }

  // Hotkey cool-down to prevent toggling too fast
  constexpr std::chrono::milliseconds delay(75);

  // If the Xbox Gamebar is enabled or the Guide button is disabled then
  // replace the Guide button with the Back button without redeclaring the key
  // mappings
  if (IsUseNexusForGameBarEnabled() || !cvars::guide_button) {
    if ((buttons & X_INPUT_GAMEPAD_BACK) == X_INPUT_GAMEPAD_BACK) {
      buttons &= ~X_INPUT_GAMEPAD_BACK;
      buttons |= X_INPUT_GAMEPAD_GUIDE;
    }
  }

  auto it = controller_hotkey_map.find(buttons);
  if (it == controller_hotkey_map.end()) {
    return Unknown_hotkey;
  }

  // Do not activate hotkeys that are not intended for activation during
  // gameplay
  if (emulator_->is_title_open()) {
    // If non-pass through (menu hoykeys) or hotkeys disabled then return
    if (!it->second.title_passthru || !cvars::controller_hotkeys) {
      return Unknown_hotkey;
    }
  }

  std::string notificationTitle = "";
  std::string notificationDesc = "";

  EmulatorWindow::ControllerHotKey button_combination = it->second;

  switch (button_combination.function) {
    case ButtonFunctions::ToggleFullscreen:
      app_context().CallInUIThread([this]() { ToggleFullscreen(); });

      // Extra Sleep
      xe::threading::Sleep(delay);
      break;
    case ButtonFunctions::RunTitle: {
      if (selected_title_index == -1) {
        selected_title_index++;
      }

      if (selected_title_index < recently_launched_titles_.size()) {
        app_context().CallInUIThread([this]() {
          RunTitle(
              recently_launched_titles_[selected_title_index].path_to_file);
        });
      }
    } break;
    case ButtonFunctions::ClearMemoryPageState:
      ToggleGPUSetting(GPUSetting::ClearMemoryPageState);

      // Assume the user wants ClearCaches as well
      if (cvars::clear_memory_page_state) {
        GpuClearCaches();
      }

      notificationTitle = "Toggle Clear Memory Page State";
      notificationDesc =
          cvars::clear_memory_page_state ? "Enabled" : "Disabled";

      // Extra Sleep
      xe::threading::Sleep(delay);
      break;
    case ButtonFunctions::CpuTimeScalarSetHalf:
      CpuTimeScalarSetHalf();

      notificationTitle = "Time Scalar";
      notificationDesc =
          fmt::format("Decreased to {}", Clock::guest_time_scalar());
      break;
    case ButtonFunctions::CpuTimeScalarSetDouble:
      CpuTimeScalarSetDouble();

      notificationTitle = "Time Scalar";
      notificationDesc =
          fmt::format("Increased to {}", Clock::guest_time_scalar());
      break;
    case ButtonFunctions::CpuTimeScalarReset:
      CpuTimeScalarReset();

      notificationTitle = "Time Scalar";
      notificationDesc = fmt::format("Reset to {}", Clock::guest_time_scalar());
      break;
    case ButtonFunctions::ClearGPUCache:
      GpuClearCaches();

      notificationTitle = "Clear GPU Cache";
      notificationDesc = "Complete";

      // Extra Sleep
      xe::threading::Sleep(delay);
      break;
    case ButtonFunctions::ToggleControllerVibration: {
      ToggleControllerVibration();

      bool vibration = false;

      auto input_sys = emulator()->input_system();
      if (input_sys) {
        vibration = input_sys->GetVibrationCvar();
      }

      notificationTitle = "Toggle Controller Vibration";
      notificationDesc = vibration ? "Enabled" : "Disabled";

      // Extra Sleep
      xe::threading::Sleep(delay);
    } break;
    case ButtonFunctions::IncTitleSelect:
      selected_title_index++;
      break;
    case ButtonFunctions::DecTitleSelect:
      selected_title_index--;
      break;
    case ButtonFunctions::ToggleLogging: {
      logging::ToggleLogLevel();

      notificationTitle = "Toggle Logging";

      LogLevel level = static_cast<LogLevel>(logging::internal::GetLogLevel());
      notificationDesc = level == LogLevel::Disabled ? "Disabled" : "Enabled";
    } break;
    case ButtonFunctions::Unknown:
    default:
      break;
  }

  if ((button_combination.function == ButtonFunctions::IncTitleSelect ||
       button_combination.function == ButtonFunctions::DecTitleSelect) &&
      recently_launched_titles_.size() > 0) {
    selected_title_index =
        std::clamp(selected_title_index, 0,
                   static_cast<int32_t>(recently_launched_titles_.size() - 1));

    // Must clear dialogs to prevent stacking
    ClearDialogs();

    // Titles may contain Unicode characters such as At World’s End
    // Must use ImGUI font that can render these Unicode characters
    std::string title_name;

    // Use filename if title name is empty
    if (recently_launched_titles_[selected_title_index].title_name.empty()) {
      title_name = recently_launched_titles_[selected_title_index]
                       .path_to_file.filename()
                       .string();
    } else {
      title_name = recently_launched_titles_[selected_title_index].title_name;
    }

    std::string title = fmt::format(
        "{}: {}\n\n{}", selected_title_index + 1, title_name,
        controller_hotkey_map.find(X_INPUT_GAMEPAD_START)->second.pretty);

    xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(), "Title Selection",
                                        title);
  }

  if (!notificationTitle.empty()) {
    app_context_.CallInUIThread(
        [imgui_drawer = imgui_drawer(), notificationTitle, notificationDesc]() {
          new xe::ui::HostNotificationWindow(imgui_drawer, notificationTitle,
                                             notificationDesc, 0);
        });
  }

  xe::threading::Sleep(delay);

  return it->second;
}

void EmulatorWindow::VibrateController(xe::hid::InputSystem* input_sys,
                                       uint32_t user_index,
                                       bool toggle_rumble) {
  constexpr std::chrono::milliseconds rumble_duration(100);

  // Hold lock while sleeping this thread for the duration of the rumble,
  // otherwise the rumble may fail.
  auto input_lock = input_sys->lock();

  X_INPUT_VIBRATION vibration = {};

  vibration.left_motor_speed = toggle_rumble ? UINT16_MAX : 0;
  vibration.right_motor_speed = toggle_rumble ? UINT16_MAX : 0;

  input_sys->SetState(user_index, &vibration);

  // Vibration duration
  if (toggle_rumble) {
    xe::threading::Sleep(rumble_duration);
  }
}

void EmulatorWindow::GamepadHotKeys() {
  X_INPUT_STATE state;

  constexpr std::chrono::milliseconds thread_delay(75);

  auto input_sys = emulator_->input_system();

  // Monotonic millisecond clock for guide-button long-press timing.
  auto now_ms = []() -> uint64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };

  if (input_sys) {
    while (true) {
      // Collect controller states while holding the lock
      std::array<std::pair<bool, X_INPUT_STATE>, XUserMaxUserCount>
          controller_states;
      {
        auto input_lock = input_sys->lock();
        for (uint32_t user_index = 0; user_index < XUserMaxUserCount;
             ++user_index) {
          X_RESULT result = input_sys->GetState(
              user_index, X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD, &state);
          controller_states[user_index] = {result == X_ERROR_SUCCESS, state};
        }
      }  // Lock is released here when input_lock goes out of scope

      // Process hotkeys without holding the lock
      for (uint32_t user_index = 0; user_index < XUserMaxUserCount;
           ++user_index) {
        if (controller_states[user_index].first) {
          const uint16_t buttons =
              controller_states[user_index].second.gamepad.buttons;

          if (buttons != last_hotkey_buttons_[user_index]) {
            last_hotkey_buttons_[user_index] = buttons;
            XELOGD("GamepadHotKeys: user {} buttons {:04X}{}", user_index,
                   buttons, (buttons & X_INPUT_GAMEPAD_GUIDE) ? " GUIDE" : "");
          }

          // Guide is Guide. Back is NEVER substituted for it: Back is Select,
          // which the running game uses, so standing it in for Guide would
          // fire emulator menus on a button the title is reading.
          //
          // Guide button: short press opens the profile selector, long press
          // opens console settings. Marshalled to the UI thread.
          bool guide_pressed = (buttons & X_INPUT_GAMEPAD_GUIDE) != 0;
          bool solo_guide =
              guide_pressed && (buttons & ~X_INPUT_GAMEPAD_GUIDE) == 0;
          if (solo_guide && !guide_button_was_pressed_[user_index]) {
            guide_button_was_pressed_[user_index] = true;
            guide_button_press_time_[user_index] = now_ms();
          } else if (!guide_pressed && guide_button_was_pressed_[user_index]) {
            guide_button_was_pressed_[user_index] = false;
            uint64_t duration = now_ms() - guide_button_press_time_[user_index];
            if (duration >= kGuideLongPressMs) {
              // Long press - console settings.
              app_context_.CallInUIThread(
                  [this]() { ToggleConsoleSettingsDialog(); });
            } else if (duration > 50) {
              // Short press - profile selector (debounce very short presses).
              app_context_.CallInUIThread(
                  [this]() { ToggleProfilesConfigDialog(); });
            }
          } else if (guide_pressed && !solo_guide) {
            // Guide with other buttons - cancel solo tracking, let the hotkey
            // map handle the combo.
            guide_button_was_pressed_[user_index] = false;
          }

          // Back button: LONG press only opens the netplay menu. A short press
          // is left alone - it is Select, and the game is using it.
          {
            bool back_pressed = (buttons & X_INPUT_GAMEPAD_BACK) != 0;
            bool solo_back =
                back_pressed && (buttons & ~X_INPUT_GAMEPAD_BACK) == 0;
            if (solo_back && !back_button_was_pressed_[user_index]) {
              back_button_was_pressed_[user_index] = true;
              back_button_press_time_[user_index] = now_ms();
            } else if (!back_pressed && back_button_was_pressed_[user_index]) {
              back_button_was_pressed_[user_index] = false;
              uint64_t duration =
                  now_ms() - back_button_press_time_[user_index];
              if (duration >= kGuideLongPressMs) {
                // Long press - netplay menu.
                app_context_.CallInUIThread(
                    [this]() { ToggleFriendsDialog(); });
              }
            } else if (back_pressed && !solo_back) {
              // Back with other buttons (e.g. Back + Start) - cancel solo
              // tracking and let the hotkey map handle the combo.
              back_button_was_pressed_[user_index] = false;
            }
          }

          if (cvars::controller_hotkeys &&
              ProcessControllerHotkey(buttons).rumble) {
            // Enable Vibration
            VibrateController(input_sys, user_index, true);

            // Disable Vibration
            VibrateController(input_sys, user_index, false);
          }
        }
      }

      xe::threading::Sleep(thread_delay);
    }
  }
}

void EmulatorWindow::ToggleGPUSetting(gpu::GPUSetting setting) {
  switch (setting) {
    case GPUSetting::ClearMemoryPageState:
      SaveGPUSetting(GPUSetting::ClearMemoryPageState,
                     !cvars::clear_memory_page_state);
      break;
    case GPUSetting::ReadbackMemexport:
      SaveGPUSetting(GPUSetting::ReadbackMemexport, !cvars::readback_memexport);
      break;
  }
}

void EmulatorWindow::DisplayHotKeysConfig() {
  std::string msg = "";
  std::string msg_passthru = "";

  bool guide_enabled = !IsUseNexusForGameBarEnabled() && cvars::guide_button;

  for (auto const& [key, val] : controller_hotkey_map) {
    std::string pretty_text = val.pretty;

    if (!guide_enabled) {
      pretty_text = std::regex_replace(
          pretty_text, std::regex("Guide", std::regex_constants::icase),
          "Back");
    }

    if (emulator_->is_title_open() && !val.title_passthru) {
      pretty_text += " (Disabled)";
    }

    if (val.title_passthru && !cvars::controller_hotkeys) {
      pretty_text += " (Disabled)";
    }

    if (val.title_passthru) {
      msg += pretty_text + "\n";
    } else {
      msg_passthru += pretty_text + "\n";
    }
  }

  // Add Title
  msg.insert(0, "Gameplay Hotkeys\n");

  // Prepend non-passthru hotkeys
  msg_passthru += "\n";
  msg.insert(0, msg_passthru);
  msg += "\n";

  msg += "Clear Memory Page State: " +
         xe::string_util::BoolToString(cvars::clear_memory_page_state);
  msg += "\n";

  msg += "Controller Hotkeys: " +
         xe::string_util::BoolToString(cvars::controller_hotkeys);

  ClearDialogs();
  xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(), "Controller Hotkeys",
                                      msg);
}

std::string EmulatorWindow::CanonicalizeFileExtension(
    const std::filesystem::path& path) {
  return xe::utf8::lower_ascii(xe::path_to_utf8(path.extension()));
}

xe::X_STATUS EmulatorWindow::RunTitle(
    const std::filesystem::path& path_to_file) {
  // Whatever is starting now takes precedence over a boot animation still
  // counting down to the dashboard.
  CancelPendingDashboard();

  std::error_code ec = {};
  bool titleExists = std::filesystem::exists(path_to_file, ec);

  if (path_to_file.empty() || !titleExists) {
    std::string log_msg =
        fmt::format("Failed to launch title path is {}.",
                    path_to_file.empty() ? "empty" : "invalid");

    if (!path_to_file.empty() && !titleExists) {
      log_msg.append(fmt::format("\nProvided Path: {}", path_to_file));
    }

    if (ec) {
      log_msg.append(fmt::format("\nExtended message info: {} ({:08X})",
                                 ec.message(), ec.value()));
    }

    XELOGE("{}", log_msg);

    ClearDialogs();

    xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(),
                                        "Title Launch Failed!", log_msg);

    return X_STATUS_NO_SUCH_FILE;
  }

  if (emulator_->is_title_open()) {
    auto xam =
        emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
            "xam.xex");
    const std::string next_host_path =
        xe::path_to_utf8(std::filesystem::absolute(path_to_file));
    if (xam) {
      kernel::xam::RecordLaunchOrigin();
      auto& loader_data = xam->loader_data();
      loader_data.host_path = next_host_path;
      loader_data.launch_path.clear();
      loader_data.launch_flags = 0;
      loader_data.launch_data.clear();
      loader_data.command_line.clear();
    }
    if (!cvars::title_switch_in_process && xam) {
      XELOGI("User launch of {} while a title runs; restarting the emulator",
             next_host_path);
      xam->SaveLoaderData();
      config::SaveConfig();
      xe::LaunchSelf();
      xe::FlushLog();
      std::quick_exit(0);
    }
    XELOGI("User launch of {} while a title runs; switching in place",
           next_host_path);
    emulator_->TerminateTitle(cvars::title_switch_clear_handles);
  }

  // Prevent crashing the emulator by not loading a game if a game is already
  // loaded.
  auto abs_path = std::filesystem::absolute(path_to_file);

  auto extension = CanonicalizeFileExtension(abs_path);

  if (extension == ".7z" || extension == ".zip" || extension == ".rar" ||
      extension == ".tar" || extension == ".gz") {
    xe::ShowSimpleMessageBox(
        xe::SimpleMessageBoxType::Error,
        fmt::format(
            "Unsupported format!\n"
            "Xenia does not support running software in an archived format."));

    return X_STATUS_UNSUCCESSFUL;
  }

  auto result = emulator_->LaunchPath(abs_path);

  disable_hotkeys_ = false;

  if (profile_config_dialog_) {
    profile_config_dialog_.reset();
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
    emulator_->kernel_state()->xam_state()->set_profile_dialog_open(false);
  }

  if (gamerpic_browser_dialog_) {
    gamerpic_browser_dialog_.reset();
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }

  if (display_config_dialog_) {
    display_config_dialog_.reset();
  }

  if (friends_manager_dialog_) {
    friends_manager_dialog_.reset();
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }

  if (updater_dialog_) {
    updater_dialog_.reset();
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }

  if (updater_completion_dialog_) {
    updater_completion_dialog_.reset();
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }

  if (netplay_settings_dialog_) {
    netplay_settings_dialog_.reset();
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }

  if (netplay_status_dialog_) {
    netplay_status_dialog_.reset();
    emulator_->kernel_state()->xam_state()->xam_dialogs_shown_--;
  }

  ClearDialogs();

  if (result) {
    XELOGE("Failed to launch target: {:08X}", result);

    if (!kernel::xna::XnaRuntimeInstallPending()) {
      xe::ui::ImGuiDialog::ShowMessageBox(
          imgui_drawer_.get(), "Title Launch Failed!",
          "Failed to launch title.\n\nCheck xenia.log for technical details.");
    }

    emulator_->file_system()->Clear();
  } else {
    // Emulator::RecordTitleLaunch has already written the row - it is the only
    // place with the title's icon and its mounts - so the menu just re-reads.
    LoadRecentlyLaunchedTitles();

    auto xam =
        emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
            "xam.xex");

    xam->loader_data().host_path = xe::path_to_utf8(abs_path);
  }

  return result;
}

void EmulatorWindow::OpenMousehookConfig() {
  // Opening the console settings dialog suspends mousehook for as long as it
  // is up (see ConsoleSettingsDialog's constructor) so the cursor is free to
  // drive the UI. Closing it applies the checkbox state and resumes.
  if (!console_settings_dialog_) {
    ToggleConsoleSettingsDialog();
  }

  if (console_settings_dialog_) {
    console_settings_dialog_->FocusMousehookTab();
  }
}

void EmulatorWindow::ToggleMousehook() {
  auto& mh = hid::MousehookConfig::Get();
  const bool enabled = !mh.enabled();
  mh.set_enabled(enabled);
  mh.Save();

  new xe::ui::HostNotificationWindow(
      imgui_drawer(),
      enabled ? "Mousehook Enabled, CTRL+M To disable" : "Mousehook Disabled",
      "Mousehook", 0);
}

void EmulatorWindow::OpenTitleUpdateSelector(const std::filesystem::path& path,
                                             uint32_t title_id) {
  if (title_id == 0) {
    auto header = xe::vfs::XContentContainerDevice::ReadContainerHeader(path);
    if (header && header->content_header.is_magic_valid()) {
      title_id = header->content_metadata.execution_info.title_id.get();
    }
  }
  if (title_id != 0) {
    new TitleUpdateDialog(imgui_drawer(), this, title_id, path);
  } else {
    // Couldn't resolve a title id (e.g. a folder/disc) - just launch it.
    RunTitle(path);
  }
}

void EmulatorWindow::RunPreviouslyPlayedTitle() {
  if (recently_launched_titles_.size() >= 1) {
    RunTitle(recently_launched_titles_[0].path_to_file);
  }
}

std::filesystem::path EmulatorWindow::GetXnaLibraryPath() const {
  return emulator_->storage_root() / "xna_library";
}

void EmulatorWindow::InstallXnaPackage() {
  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title("Select an XNA Package");
  file_picker->set_extensions({
      {"XNA Package", "*.*"},
  });
  if (!file_picker->Show(window_.get())) {
    return;
  }
  const auto selected_files = file_picker->selected_files();
  if (selected_files.empty()) {
    return;
  }
  const auto& source = selected_files[0];

  // Checked before copying, so a package that could never launch is refused
  // where the user can still see what they picked.
  kernel::xna::XnaPackageInfo info;
  if (!kernel::xna::IsXnaPackage(source, &info)) {
    new xe::ui::HostNotificationWindow(
        imgui_drawer(), "Not an XNA title",
        "That package holds no managed XNA assembly.", 0);
    return;
  }

  std::error_code ec;
  const auto library = GetXnaLibraryPath();
  std::filesystem::create_directories(library, ec);
  const auto destination = library / source.filename();
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (ec) {
    XELOGE("Could not install XNA package: {}", ec.message());
    new xe::ui::HostNotificationWindow(imgui_drawer(), "Install failed",
                                       ec.message(), 0);
    return;
  }

  XELOGI("Installed XNA title {} to {}", info.display_name,
         xe::path_to_utf8(destination));
  // The menu is built once at startup, so a freshly installed title only
  // appears next time - say so rather than leaving the user hunting for it.
  new xe::ui::HostNotificationWindow(
      imgui_drawer(), "XNA title installed",
      info.display_name +
          " will appear in the XNA Titles menu after a "
          "restart.",
      0);
}

namespace {

constexpr const char* kXnaSetupSystemUpdateUrl =
    "https://download.microsoft.com/download/8/f/4/"
    "8f456817-e264-4207-9b95-6efc990fee98/SystemUpdate_17559_USB.zip";

struct XnaSetupState {
  std::mutex mutex;
  std::string stage;
  std::string report;
  std::filesystem::path package;
  std::atomic<uint64_t> received{0};
  std::atomic<uint64_t> total{0};
  std::atomic<bool> downloading{false};
  std::atomic<bool> cancel{false};
  std::atomic<bool> finished{false};

  void SetStage(std::string text) {
    std::lock_guard<std::mutex> lock(mutex);
    stage = std::move(text);
  }
  void AddReport(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    report += line + "\n";
  }
};

// What has to be on disk for the dashboard to count as installed. The whole
// update is copied, not just these - a system title loads its own modules and
// fonts off media:, which is this folder - but these are the ones worth
// naming: dash.xex, and AvatarEditor.xex so the Avatar Editor menu can run the
// console's own editor instead of the built-in one.
constexpr const char* kXnaSetupDashboardFiles[] = {"dash.xex",
                                                   "AvatarEditor.xex"};

std::string LowerAscii(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return text;
}

// The typefaces XamGetLanguageTypeface hands a system title, by the exact
// names it returns. They are loaded off media:, which is the folder the title
// runs from.
constexpr const char* kDashboardTypefaces[] = {
    "SegoeXbox-Light.xtt", "xenonclatin.xtt", "xenonjklatin.xtt"};

// The update ships its own copies of those faces and they render wrong. The
// flash image's are the ones the console actually draws with, and the UI
// installer has already put them next door - so whatever it extracted wins
// over what the zip held.
//
// BY WHAT IS INSIDE THE FILE, NOT BY ITS NAME. The extractor names a font from
// its own sfnt table, and two of the three faces call themselves "Xbox TC" and
// "Xbox JK" there, so they landed as font1.xtt and font2.xtt and this matched
// nothing but Segoe. Every .xtt in the asset folder is opened and asked which
// typeface it is.
std::string ReplaceDashboardFontsFromFlash(
    const std::filesystem::path& dashboard) {
  std::error_code ec;
  const auto ui = kernel::xam::xui::DefaultAssetDirectory();
  std::string report;
  std::set<std::string> placed;
  size_t seen = 0;
  // The two installs happen in either order, and on a clean machine the flash
  // image is often first - so the folder the faces go into may not exist yet.
  std::filesystem::create_directories(dashboard, ec);
  ec.clear();
  for (const auto& file : std::filesystem::directory_iterator(
           ui, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (ec) {
      break;
    }
    if (!file.is_regular_file(ec) || file.path().extension() != ".xtt") {
      continue;
    }
    ++seen;
    const std::string name =
        kernel::xam::xui::DashboardTypefaceFor(file.path());
    if (name.empty() || placed.count(name)) {
      continue;
    }
    std::filesystem::copy_file(
        file.path(), dashboard / name,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      ec.clear();
      continue;
    }
    placed.insert(name);
    report += fmt::format("Dashboard: {} taken from the flash image ({})\n",
                          name, xe::path_to_utf8(file.path().filename()));
  }
  // Only worth saying once there ARE flash assets to have missed it in. A user
  // who has installed the system update and not a NAND dump yet gets the
  // one line that tells them what to do, not three failures.
  if (!seen) {
    return "Dashboard: no flash typefaces installed - install a NAND dump "
           "through Install Content for the console's own fonts\n";
  }
  for (const char* name : kDashboardTypefaces) {
    if (!placed.count(name)) {
      report += std::string("Dashboard: ") + name +
                " is not in the extracted flash assets\n";
    }
  }
  return report;
}

// The whole extracted system update, flattened into the Dashboard folder. A
// system title run from there reads its modules, its .lex overlays and its
// typefaces off media:, which IS this folder, so cherry-picking three files
// left it short.
// An XContent package, by its own signature. The system update carries the
// avatar asset packs as extensionless files (FFFE07DF00000001 and friends),
// so the magic is the only thing separating them from a module.
bool IsXContentPackageFile(const std::filesystem::path& path) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  char magic[4] = {};
  const size_t read = fread(magic, 1, sizeof(magic), file);
  fclose(file);
  if (read != sizeof(magic)) {
    return false;
  }
  return !std::memcmp(magic, "CON ", 4) || !std::memcmp(magic, "PIRS", 4) ||
         !std::memcmp(magic, "LIVE", 4);
}

std::string CopyDashboardModules(
    const std::filesystem::path& staging,
    std::vector<std::filesystem::path>* out_packages) {
  std::error_code ec;
  const auto dashboard = xe::filesystem::GetExecutableFolder() / "Dashboard";
  std::filesystem::create_directories(dashboard, ec);
  size_t copied = 0;
  size_t failed = 0;
  size_t packages = 0;
  for (const auto& file : std::filesystem::recursive_directory_iterator(
           staging, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (!file.is_regular_file(ec)) {
      continue;
    }
    // The avatar asset packs are why this check exists. Copied beside
    // dash.xex they are invisible - the catalogue opens
    // content/0000000000000000/FFFE07DF/00008000/FFFE07DF00000002/
    // AvatarAssetPack.toc - so leaving them unextracted in Dashboard makes
    // XamAvatarInitialize report the Avatar update as missing, every
    // XamAvatarGetAssets return 80004005, and the avatar editor spin.
    if (out_packages && IsXContentPackageFile(file.path())) {
      out_packages->push_back(file.path());
      ++packages;
      continue;
    }
    std::filesystem::copy_file(
        file.path(), dashboard / file.path().filename(),
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      ec.clear();
      ++failed;
      continue;
    }
    ++copied;
  }
  std::string report = fmt::format("Dashboard: {} file(s) -> {}\n", copied,
                                   xe::path_to_utf8(dashboard));
  if (packages) {
    report += fmt::format(
        "Avatar data: {} content package(s) sent to the installer\n", packages);
  }
  if (failed) {
    report +=
        fmt::format("Dashboard: {} file(s) could not be copied\n", failed);
  }
  for (const char* name : kXnaSetupDashboardFiles) {
    if (!std::filesystem::exists(dashboard / name, ec)) {
      report += std::string("Dashboard: FAILED - ") + name +
                " is not in the system update\n";
    }
  }
  return report + ReplaceDashboardFontsFromFlash(dashboard);
}

// An extracted USB system update holds the console's modules and no XContent.
// dash.xex is the one that is always there and never anywhere else.
bool IsSystemUpdateTree(const std::filesystem::path& staging) {
  std::error_code ec;
  for (const auto& file : std::filesystem::recursive_directory_iterator(
           staging, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (!file.is_regular_file(ec)) {
      continue;
    }
    const std::string name =
        LowerAscii(xe::path_to_utf8(file.path().filename()));
    if (name == "dash.xex" || name == "avatareditor.xex") {
      return true;
    }
  }
  return false;
}

std::string InstallSystemUpdateTree(
    const std::filesystem::path& staging,
    std::vector<std::filesystem::path>* out_packages) {
  std::string report = CopyDashboardModules(staging, out_packages);
  const auto ui = kernel::xam::xui::InstallFromSystemUpdate(
      staging, kernel::xam::xui::DefaultAssetDirectory());
  return report + "Avatar UI: " + ui.text;
}

void InstallXnaSetupDashboard(XnaSetupState* state,
                              const std::filesystem::path& zip,
                              const std::filesystem::path& staging) {
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);
  std::filesystem::create_directories(staging, ec);
  if (ec || !kernel::xna::ExtractZipArchive(zip, staging)) {
    state->AddReport("Dashboard: FAILED - could not extract " +
                     xe::path_to_utf8(zip));
    std::filesystem::remove_all(staging, ec);
    return;
  }

  // The Avatar Editor's XUI packages ride in the same update, inside
  // AvatarEditor.xex and Guide.AvatarMiniCreator.xex.
  // Packages are not installed here - the avatar step hands the same zip to
  // the content installer after the user presses OK.
  state->AddReport(InstallSystemUpdateTree(staging, nullptr));

  std::filesystem::remove_all(staging, ec);
}

void RunXnaSetup(std::shared_ptr<XnaSetupState> state,
                 std::filesystem::path content_root) {
  state->SetStage("Copying Nexia's XNA host files...");
  std::string message;
  if (kernel::xna::DeployXnaHostPayload(&message)) {
    state->AddReport("XNA host files: ready (" + message + ")");
  } else {
    state->AddReport("XNA host files: FAILED - " + message);
  }

  state->SetStage("Checking the Microsoft XNA runtime...");
  std::string missing;
  if (kernel::xna::XnaRuntimeInstalled(&missing)) {
    state->AddReport("Microsoft XNA runtime: already installed");
  } else {
    state->SetStage(
        "Installing the Microsoft XNA runtime.\n"
        "Windows asks for administrator permission, and a PowerShell\n"
        "window shows the progress.");
    std::string error;
    const bool ran = kernel::xna::InstallXnaRuntime(&error);
    if (kernel::xna::XnaRuntimeInstalled(&missing)) {
      state->AddReport("Microsoft XNA runtime: installed");
    } else if (!ran) {
      state->AddReport("Microsoft XNA runtime: NOT installed - " + error);
    } else {
      state->AddReport("Microsoft XNA runtime: still missing " + missing +
                       " (a Windows restart may be needed)");
    }
  }

  std::error_code ec;
  const auto dashboard = xe::filesystem::GetExecutableFolder() / "Dashboard";
  const bool avatars_installed = std::filesystem::exists(
      kernel::xna::avatar::CatalogPath(content_root), ec);
  bool dashboard_installed = true;
  for (const char* name : kXnaSetupDashboardFiles) {
    if (!std::filesystem::exists(dashboard / name, ec)) {
      dashboard_installed = false;
    }
  }
  // The avatar UI packages come out of the same update, so a tree that has
  // dash.xex but not them still needs this step.
  const auto ui_directory = kernel::xam::xui::DefaultAssetDirectory();
  for (const std::string& name : kernel::xam::xui::SystemUpdatePackageNames()) {
    if (!std::filesystem::exists(ui_directory / name, ec)) {
      dashboard_installed = false;
    }
  }
  if (avatars_installed) {
    state->AddReport("Avatar data: already installed");
  }
  if (dashboard_installed) {
    state->AddReport("Dashboard: already installed in " +
                     xe::path_to_utf8(dashboard));
  }

  if (avatars_installed && dashboard_installed) {
    state->AddReport("System update download: not needed");
  } else if (state->cancel.load()) {
    state->AddReport("System update download: cancelled");
  } else {
    state->SetStage(
        "Downloading the Xbox 360 system update (avatar data and\n"
        "dashboard) from download.microsoft.com...");
    const auto directory =
        std::filesystem::temp_directory_path(ec) / "Nexia" / "XNA_Setup";
    std::filesystem::create_directories(directory, ec);
    const auto zip = directory / "SystemUpdate_17559_USB.zip";
    state->downloading.store(true);
    const bool downloaded = kernel::util::TitleUpdateDownloader::DownloadUrl(
        kXnaSetupSystemUpdateUrl, zip,
        [state](uint64_t received, uint64_t total) {
          state->received.store(received);
          state->total.store(total);
        },
        &state->cancel);
    state->downloading.store(false);
    if (!downloaded) {
      state->AddReport(state->cancel.load()
                           ? "System update download: cancelled"
                           : "System update download: FAILED - see the log");
    } else {
      if (!dashboard_installed) {
        state->SetStage("Copying the dashboard...");
        InstallXnaSetupDashboard(state.get(), zip, directory / "extract");
      }
      if (!avatars_installed) {
        state->AddReport("Avatar data: downloaded, installs when you press OK");
        std::lock_guard<std::mutex> lock(state->mutex);
        state->package = zip;
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    XELOGE("Setup XNA:\n{}", state->report);
  }
  state->finished.store(true);
}

class XnaSetupDialog final : public ui::ImGuiDialog {
 public:
  XnaSetupDialog(ui::ImGuiDrawer* imgui_drawer,
                 std::shared_ptr<XnaSetupState> state,
                 std::function<void(std::filesystem::path)> install)
      : ui::ImGuiDialog(imgui_drawer),
        state_(std::move(state)),
        install_(std::move(install)),
        window_id_(GetWindowId()) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin(
            fmt::format("Setup XNA###{}", window_id_).c_str(), &open,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    std::string stage;
    std::string report;
    std::filesystem::path package;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      stage = state_->stage;
      report = state_->report;
      package = state_->package;
    }
    const bool finished = state_->finished.load();

    if (!report.empty()) {
      ImGui::TextUnformatted(report.c_str());
    }
    if (!finished) {
      if (!report.empty()) {
        ImGui::Separator();
      }
      ImGui::TextUnformatted(stage.c_str());
    }
    if (state_->downloading.load()) {
      const uint64_t received = state_->received.load();
      const uint64_t total = state_->total.load();
      const std::string overlay =
          total ? fmt::format("{:.1f} / {:.1f} MB", received / 1048576.0,
                              total / 1048576.0)
                : fmt::format("{:.1f} MB", received / 1048576.0);
      ImGui::ProgressBar(total ? float(double(received) / double(total)) : 0.0f,
                         ImVec2(420.0f, 0.0f), overlay.c_str());
    }

    ImGui::Spacing();
    if (finished) {
      if (ImGui::Button("OK") || !open) {
        ImGui::End();
        Close();
        if (!package.empty()) {
          install_(package);
        }
        return;
      }
    } else if (state_->cancel.load()) {
      ImGui::TextUnformatted("Cancelling...");
    } else if (ImGui::Button("Cancel") || !open) {
      state_->cancel.store(true);
    }
    ImGui::End();
  }

 private:
  std::shared_ptr<XnaSetupState> state_;
  std::function<void(std::filesystem::path)> install_;
  uint64_t window_id_;
};

}  // namespace

void EmulatorWindow::SetupXna() {
  std::string title = "Setup XNA";
  std::string body =
      "Sets up this PC to run Xbox Live Indie Games (XNA titles) in Nexia.\n\n"
      "Choosing Accept does three things:\n"
      "  1. Copies Nexia's XNA host files out of Nexia360.exe into its xna\n"
      "     folder.\n"
      "  2. If anything is missing, runs Nexia's XNA runtime installer. It\n"
      "     downloads these from Microsoft's own servers, checks that each is\n"
      "     signed by Microsoft, and installs them:\n"
      "       - .NET Framework 3.5 (a Windows feature)\n"
      "       - .NET 9 Runtime (runs Nexia's XNA host)\n"
      "       - XNA Framework 3.1 and XNA Framework 4.0 Refresh\n"
      "       - DirectX End-User Runtimes (June 2010)\n"
      "     Windows asks for administrator permission for this step, and a\n"
      "     PowerShell window shows the progress.\n"
      "  3. If the avatar data or the dashboard is not installed, downloads\n"
      "     Microsoft's Xbox 360 system update 17559 from\n"
      "     download.microsoft.com. It copies the dashboard (dash.xex and\n"
      "     XenonSCLatin.xtt) into the Dashboard folder next to Nexia360.exe,\n"
      "     and installs the avatar, dashboard and Kinect data packages. The\n"
      "     system update itself is not installed.\n\n"
      "Choosing Cancel changes nothing.";

  auto* dialog = new kernel::xam::MessageBoxDialog(imgui_drawer(), title, body,
                                                   {"Accept", "Cancel"}, 1);
  dialog->set_close_callback([this, dialog]() {
    if (dialog->chosen_button() != 0) {
      XELOGI("Setup XNA: declined");
      return;
    }
    XELOGI("Setup XNA: accepted");
    app_context().CallInUIThread([this]() {
      auto state = std::make_shared<XnaSetupState>();
      state->SetStage("Starting...");
      new XnaSetupDialog(
          imgui_drawer(), state, [this](std::filesystem::path package) {
            app_context().CallInUIThread(
                [this, package]() { InstallContentPackages({package}); });
          });
      std::thread(RunXnaSetup, state, emulator_->content_root()).detach();
    });
  });
}

void EmulatorWindow::ShowXnaDependencies() {
  // To the log as well as the dialog: the list is long enough to run off
  // the bottom of the window, and the log copy can be read afterwards.
  kernel::xna::LogXnaDependencies();
  new xe::ui::HostNotificationWindow(imgui_drawer(), "XNA Dependencies",
                                     kernel::xna::DescribeXnaDependencies(), 0);
}

void EmulatorWindow::FindXnaDependencies() {
  std::string title = "Find XNA Dependencies";
  std::string body =
      "Xbox Live Indie Games need the Xbox 360's own XNA runtime to run in\n"
      "Nexia. This finds those files on your PC and packs them into one "
      "zip.\n\n"
      "It looks for:\n"
      "  - the console XNA runtime: MXF.dlx, MXF.Graphics.dlx and the other\n"
      "    MXF*.dlx files, plus mscorlib.dlx and the System*.dlx files\n"
      "  - SDL2.dll and openal.dll\n"
      "  - MonoGame.Framework.dll, if there is one\n\n"
      "They usually come from the XNA Indie Player title update, or from the\n"
      "xna folder of another Nexia install. Pick the folder that holds them;\n"
      "every folder inside it is searched too.\n\n"
      "The zip is saved as XNA_Dependencies.zip next to Nexia360.exe, and\n"
      "that folder opens when it is done.";

  auto* dialog = new kernel::xam::MessageBoxDialog(imgui_drawer(), title, body,
                                                   {"Find Files", "Cancel"}, 1);
  dialog->set_close_callback([this, dialog]() {
    if (dialog->chosen_button() != 0) {
      return;
    }
    app_context().CallInUIThread([this]() { ScanXnaDependencies(); });
  });
}

void EmulatorWindow::ScanXnaDependencies() {
  auto folder_picker = xe::ui::FilePicker::Create();
  folder_picker->set_mode(ui::FilePicker::Mode::kOpen);
  folder_picker->set_type(ui::FilePicker::Type::kDirectory);
  folder_picker->set_multi_selection(false);
  folder_picker->set_title("Select the folder to search for XNA files");
  if (!folder_picker->Show(window_.get())) {
    return;
  }
  const auto folders = folder_picker->selected_files();
  if (folders.empty()) {
    return;
  }
  const auto source = folders[0];
  const auto zip_path =
      xe::filesystem::GetExecutableFolder() / "XNA_Dependencies.zip";

  std::thread([this, source, zip_path]() {
    std::string report;
    const bool written =
        kernel::xna::BuildXnaDependencyZip(source, zip_path, &report);
    XELOGE("XNA dependency scan of {}:\n{}", xe::path_to_utf8(source), report);
    app_context().CallInUIThread([this, written, report, zip_path]() {
      new xe::ui::HostNotificationWindow(
          imgui_drawer(),
          written ? "XNA dependency package written"
                  : "XNA dependency package not written",
          report, 0);
      if (written) {
        std::thread(LaunchFileExplorer, zip_path.parent_path()).detach();
      }
    });
  }).detach();
}

void EmulatorWindow::InstallXnaDependencyPackage() {
  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title("Select an XNA dependency package");
  file_picker->set_extensions({
      {"Dependency Package (*.zip)", "*.zip"},
      {"All Files (*.*)", "*.*"},
  });
  if (!file_picker->Show(window_.get())) {
    return;
  }
  const auto selected = file_picker->selected_files();
  if (selected.empty()) {
    return;
  }

  std::string message;
  const bool installed =
      kernel::xna::InstallXnaDependenciesFromArchive(selected[0], &message);
  XELOGE("XNA dependency package {}:\n{}", xe::path_to_utf8(selected[0]),
         message);
  new xe::ui::HostNotificationWindow(
      imgui_drawer(),
      installed ? "XNA dependencies installed" : "XNA dependencies incomplete",
      message, 0);
}

void EmulatorWindow::InstallXnaDependencies() {
  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  // A folder, not a file: what is needed is MonoGame plus its native
  // companions, which never live in one file.
  file_picker->set_type(ui::FilePicker::Type::kDirectory);
  file_picker->set_multi_selection(false);
  file_picker->set_title("Select a folder containing MonoGame");
  if (!file_picker->Show(window_.get())) {
    return;
  }
  const auto selected = file_picker->selected_files();
  if (selected.empty()) {
    return;
  }

  std::string message;
  const bool installed =
      kernel::xna::InstallXnaDependenciesFrom(selected[0], &message);
  XELOGE("XNA dependency install from {}:\n{}", xe::path_to_utf8(selected[0]),
         message);
  new xe::ui::HostNotificationWindow(
      imgui_drawer(),
      installed ? "XNA dependencies installed" : "XNA dependencies incomplete",
      message, 0);
}

void EmulatorWindow::FillXnaTitlesMenu(xe::ui::MenuItem* xna_menu) {
  std::error_code ec;
  const auto library = GetXnaLibraryPath();
  if (!std::filesystem::is_directory(library, ec)) {
    return;
  }

  // Every candidate is opened to read its name, which is why this reads a
  // dedicated folder rather than the whole content root - the check has to
  // parse an STFS container per file.
  for (const auto& item : std::filesystem::directory_iterator(library, ec)) {
    // A directory counts: a title built locally has no STFS container to
    // arrive in, and IsXnaPackage accepts either.
    if (!item.is_regular_file() && !item.is_directory()) {
      continue;
    }
    kernel::xna::XnaPackageInfo info;
    if (!kernel::xna::IsXnaPackage(item.path(), &info)) {
      continue;
    }
    std::string label = info.display_name;
    const size_t dot = label.find_last_of('.');
    if (dot != std::string::npos) {
      label = label.substr(0, dot);
    }
    xna_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, label, "",
        std::bind(&EmulatorWindow::RunTitle, this, item.path())));
  }
}

void EmulatorWindow::ShowRecentTitlesDialog() {
  if (recent_titles_dialog_) {
    return;
  }
  recent_titles_dialog_ = new RecentTitlesDialog(
      imgui_drawer_.get(), this, RecentTitlesDialog::Mode::kLaunch);
  recent_titles_dialog_->set_closed_callback(
      [this]() { recent_titles_dialog_ = nullptr; });
}

void EmulatorWindow::ShowRecentTitlesWithTuDialog() {
  if (recent_titles_dialog_) {
    return;
  }
  recent_titles_dialog_ = new RecentTitlesDialog(
      imgui_drawer_.get(), this, RecentTitlesDialog::Mode::kTitleUpdate);
  recent_titles_dialog_->set_closed_callback(
      [this]() { recent_titles_dialog_ = nullptr; });
}

const RecentTitleEntry* EmulatorWindow::FindRecentTitle(
    uint32_t title_id) const {
  if (!title_id) {
    return nullptr;
  }
  for (const RecentTitleEntry& entry : recently_launched_titles_) {
    if (entry.title_id == title_id) {
      return &entry;
    }
  }
  return nullptr;
}

std::string EmulatorWindow::GetRecentMediaId(
    const std::filesystem::path& path) const {
  for (const RecentTitleEntry& entry : recently_launched_titles_) {
    if (entry.path_to_file == path) {
      return entry.media_id;
    }
  }

  return "";
}

void EmulatorWindow::LoadRecentlyLaunchedTitles() {
  // played.db is the store now - see PlayedDB. The old recent.toml is taken
  // in once, by the database itself, the first time it is opened.
  recently_launched_titles_.clear();
  const size_t limit = cvars::recent_titles_entry_amount > 0
                           ? size_t(cvars::recent_titles_entry_amount)
                           : 0;
  for (const kernel::PlayedTitle& title : emulator()->played_db()->GetRecent(
           limit, kernel::PlayedSort::kMostRecentlyPlayed)) {
    std::error_code ec;
    if (title.path.empty() || !std::filesystem::exists(title.path, ec)) {
      continue;
    }
    recently_launched_titles_.push_back(
        {title.title_name, title.path, std::time_t(title.last_run_utc),
         title.title_id, title.media_id, title.icon});
  }
}

void EmulatorWindow::ClearDialogs() {
  if (profile_config_dialog_) {
    profile_config_dialog_.reset();
    emulator_->kernel_state()->xam_state()->set_profile_dialog_open(false);
  }

  if (display_config_dialog_) {
    display_config_dialog_.reset();
  }

  if (console_settings_dialog_) {
    console_settings_dialog_.reset();
  }

  if (friends_manager_dialog_) {
    friends_manager_dialog_.reset();
  }

  if (gamerpic_browser_dialog_) {
    gamerpic_browser_dialog_.reset();
  }

  if (updater_dialog_) {
    updater_dialog_.reset();
  }

  if (updater_completion_dialog_) {
    updater_completion_dialog_.reset();
  }

  imgui_drawer_.get()->ClearDialogs();
  emulator_->kernel_state()->xam_state()->xam_dialogs_shown_ = 0;
}

}  // namespace app
}  // namespace xe
