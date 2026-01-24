/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_UI_H_
#define XENIA_KERNEL_XAM_XAM_UI_H_

#include "xenia/kernel/json/friend_presence_object_json.h"
#include "xenia/kernel/json/session_object_json.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/ui/netplay_manager_util.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/keyboard_ui.h"
#include "xenia/ui/ui_focus_manager.h"

namespace xe {
namespace kernel {
namespace xam {

// Global keyboard focus state - use to skip parent dialog rendering while keyboard is open
bool XamKeyboardGetFocus();
void XamKeyboardSetFocus(bool has_focus);

class XamDialog : public xe::ui::ImGuiDialog {
 public:
  void set_close_callback(std::function<void()> close_callback) {
    close_callback_ = close_callback;
  }

 protected:
  XamDialog(xe::ui::ImGuiDrawer* imgui_drawer)
      : xe::ui::ImGuiDialog(imgui_drawer) {}

  virtual ~XamDialog() {}
  void OnClose() override {
    if (close_callback_) {
      close_callback_();
    }
  }

 private:
  std::function<void()> close_callback_ = nullptr;
};

class MessageBoxDialog : public XamDialog {
 public:
  MessageBoxDialog(xe::ui::ImGuiDrawer* imgui_drawer, std::string& title,
                   std::string& description, std::vector<std::string> buttons,
                   uint32_t default_button)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        chosen_button_(default_button) {
    if (!title_.size()) {
      title_ = "Message Box";
    }
  }

  uint32_t chosen_button() const { return chosen_button_; }

  void OnDraw(ImGuiIO& io) override;
  virtual ~MessageBoxDialog() {}

 private:
  bool has_opened_ = false;
  bool pending_close_ = false;  // Wait for button release before closing
  std::string title_;
  std::string description_;
  std::vector<std::string> buttons_;
  uint32_t default_button_ = 0;
  uint32_t chosen_button_ = 0;
};

// KeyboardInputDialog - XAM keyboard dialog with embedded on-screen keyboard
// Uses UIFocusManager for proper input routing in dialog stacks
class KeyboardInputDialog : public XamDialog {
 public:
  using CompletionCallback = std::function<void(const std::string&, bool)>;
  
  KeyboardInputDialog(xe::ui::ImGuiDrawer* imgui_drawer, std::string& title,
                      std::string& description, std::string& default_text,
                      size_t max_length)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        default_text_(default_text),
        max_length_(max_length),
        text_buffer_() {
    if (!title_.size()) {
      if (!description_.size()) {
        title_ = "Keyboard Input";
      } else {
        title_ = description_;
        description_ = "";
      }
    }
    text_ = default_text;
    text_buffer_.resize(max_length);
    xe::string_util::copy_truncating(text_buffer_.data(), default_text_,
                                     text_buffer_.size());
  }
  
  // Constructor with callback for async usage
  KeyboardInputDialog(xe::ui::ImGuiDrawer* imgui_drawer, 
                      const std::string& title,
                      const std::string& description, 
                      const std::string& default_text,
                      size_t max_length,
                      CompletionCallback callback)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        default_text_(default_text),
        max_length_(max_length),
        text_buffer_(),
        completion_callback_(std::move(callback)) {
    if (!title_.size()) {
      if (!description_.size()) {
        title_ = "Keyboard Input";
      } else {
        title_ = description_;
        description_ = "";
      }
    }
    text_ = default_text;
    text_buffer_.resize(max_length);
    xe::string_util::copy_truncating(text_buffer_.data(), default_text_,
                                     text_buffer_.size());
  }
  
  virtual ~KeyboardInputDialog() {}

  const std::string& text() const { return text_; }
  bool cancelled() const { return cancelled_; }

  void OnDraw(ImGuiIO& io) override;

 private:
  void DrawOnScreenKeyboard();
  
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::string default_text_;
  size_t max_length_ = 0;
  std::vector<char> text_buffer_;
  std::string text_ = "";
  bool cancelled_ = true;
  
  // On-screen keyboard state
  bool keyboard_shift_ = false;
  bool keyboard_caps_ = false;
  
  // The actual keyboard dialog from ui/keyboard_ui.h
  xe::ui::KeyboardDialog* keyboard_dialog_ = nullptr;
  
  // Optional completion callback
  CompletionCallback completion_callback_;
};

bool xeDrawProfileContent(xe::ui::ImGuiDrawer* imgui_drawer,
                          const uint64_t xuid, const uint8_t user_index,
                          const X_XAMACCOUNTINFO* account,
                          const xe::ui::ImmediateTexture* profile_icon,
                          std::function<bool()> context_menu,
                          std::function<void()> on_profile_change,
                          uint64_t* selected_xuid);

bool xeDrawFriendsContent(xe::ui::ImGuiDrawer* imgui_drawer,
                          xe::ui::UIFocusManager* focus_manager,
                          UserProfile* profile, ui::FriendsContentArgs& args,
                          std::vector<FriendPresenceObjectJSON>* presences);

// Overload without focus_manager for legacy callers
inline bool xeDrawFriendsContent(xe::ui::ImGuiDrawer* imgui_drawer,
                                 UserProfile* profile, ui::FriendsContentArgs& args,
                                 std::vector<FriendPresenceObjectJSON>* presences) {
  return xeDrawFriendsContent(imgui_drawer, imgui_drawer->GetFocusManager(),
                              profile, args, presences);
}

bool xeDrawFriendContent(xe::ui::ImGuiDrawer* imgui_drawer,
                         xe::ui::UIFocusManager* focus_manager,
                         UserProfile* profile,
                         FriendPresenceObjectJSON& presence,
                         uint64_t* selected_xuid_, uint64_t* removed_xuid_);

// Overload without focus_manager for legacy callers
inline bool xeDrawFriendContent(xe::ui::ImGuiDrawer* imgui_drawer,
                                UserProfile* profile,
                                FriendPresenceObjectJSON& presence,
                                uint64_t* selected_xuid_, uint64_t* removed_xuid_) {
  return xeDrawFriendContent(imgui_drawer, imgui_drawer->GetFocusManager(),
                             profile, presence, selected_xuid_, removed_xuid_);
}

bool xeDrawAddFriend(xe::ui::ImGuiDrawer* imgui_drawer,
                     xe::ui::UIFocusManager* focus_manager,
                     UserProfile* profile,
                     ui::AddFriendArgs& args);

// Overload without focus_manager for legacy callers
inline bool xeDrawAddFriend(xe::ui::ImGuiDrawer* imgui_drawer,
                            UserProfile* profile,
                            ui::AddFriendArgs& args) {
  return xeDrawAddFriend(imgui_drawer, imgui_drawer->GetFocusManager(),
                         profile, args);
}

bool xeDrawSessionsContent(
    xe::ui::ImGuiDrawer* imgui_drawer,
    xe::ui::UIFocusManager* focus_manager,
    UserProfile* profile,
    ui::SessionsContentArgs& sessions_args,
    std::vector<std::unique_ptr<SessionObjectJSON>>* sessions);

// Overload without focus_manager for legacy callers
inline bool xeDrawSessionsContent(
    xe::ui::ImGuiDrawer* imgui_drawer,
    UserProfile* profile,
    ui::SessionsContentArgs& sessions_args,
    std::vector<std::unique_ptr<SessionObjectJSON>>* sessions) {
  return xeDrawSessionsContent(imgui_drawer, imgui_drawer->GetFocusManager(),
                               profile, sessions_args, sessions);
}

bool xeDrawSessionContent(xe::ui::ImGuiDrawer* imgui_drawer,
                          xe::ui::UIFocusManager* focus_manager,
                          UserProfile* profile,
                          std::unique_ptr<SessionObjectJSON>& session);

// Overload without focus_manager for legacy callers
inline bool xeDrawSessionContent(xe::ui::ImGuiDrawer* imgui_drawer,
                                 UserProfile* profile,
                                 std::unique_ptr<SessionObjectJSON>& session) {
  return xeDrawSessionContent(imgui_drawer, imgui_drawer->GetFocusManager(),
                              profile, session);
}

bool xeDrawMyDeletedProfiles(xe::ui::ImGuiDrawer* imgui_drawer,
                             xe::ui::UIFocusManager* focus_manager,
                             ui::MyDeletedProfilesArgs& args,
                             std::map<uint64_t, std::string>* deleted_profiles);

// Overload without focus_manager for legacy callers
inline bool xeDrawMyDeletedProfiles(xe::ui::ImGuiDrawer* imgui_drawer,
                                    ui::MyDeletedProfilesArgs& args,
                                    std::map<uint64_t, std::string>* deleted_profiles) {
  return xeDrawMyDeletedProfiles(imgui_drawer, imgui_drawer->GetFocusManager(),
                                 args, deleted_profiles);
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
