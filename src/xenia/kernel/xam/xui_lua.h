/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_LUA_H_
#define XENIA_KERNEL_XAM_XUI_LUA_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xam/xui_runtime.h"
#include "xenia/kernel/xam/xui_timeline.h"

struct lua_State;

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

// The scene behaviour of the 17559 dashboard is a Lua program. Each XUI class
// is a pair: a script module `Xbox.XuiElement` compiled into the module's own
// package, and a native table `Xbox.XuiElementImpl` the script builds that
// class over. This is the native half, plus the loader that finds the script
// half.
//
// The chunks are big-endian 32-bit Lua 5.1; third_party/lua's undump is
// patched to take them.
class LuaHost {
 public:
  LuaHost();
  ~LuaHost();

  // Scripts come out of the module's own XZP, where they sit as lower-case
  // file names: `Xbox.XuiElement` is `xuielement.lub`. More than one package
  // can be searched, in order.
  void AddScriptPackage(Package* package);

  // The element tree the bindings act on. Handles are indices into a registry
  // built from this tree, so a rebuilt tree invalidates them - call again.
  // The scene is what the per-element timeline players are sampled from.
  void SetSceneRoot(const Scene* scene, Element* root,
                    const std::string_view name = "");
  // The skin a visual prefix is resolved against.
  void SetSkin(const Skin* skin) { skin_ = skin; }

  // A prefix picks a different entry in the skin for the same control, which
  // is how one scene's buttons come up in several looks.
  bool SetVisualPrefix(Element* element, const std::string_view prefix);
  std::string VisualPrefix(Element* element) const;

  bool Open();
  void Close();

  // Runs a module by its script name, e.g. "main" or "Xbox.XuiScene".
  bool RunModule(const std::string_view name);

  // Pumps the timers the scripts set and moves every playing timeline on.
  // Returns how many timers fired.
  uint32_t Tick(float seconds);

  // --- focus -------------------------------------------------------------
  // A control shows that it has focus by playing its own Focus span, and
  // gives it up by playing KillFocus, which is the only thing the console
  // ever does about it.
  bool SetFocus(Element* element);
  Element* focus() const { return focus_; }
  // Focuses the first control that can take it, in tree order.
  bool InitFocus();
  // Moves focus to whichever control lies that way, by where the controls
  // actually are rather than by their order in the scene.
  bool MoveFocus(int dx, int dy);
  bool TreeHasFocus(Element* root) const;

  // --- input -------------------------------------------------------------
  enum class Button {
    kUp,
    kDown,
    kLeft,
    kRight,
    kAccept,
    kCancel,
  };
  // Returns true when the press was used. Direction moves focus, accept
  // presses the focused control, cancel navigates back.
  bool HandleInput(Button button);
  void SetInputEnabled(Element* element, bool enabled);
  bool InputEnabled(Element* element) const;

  // --- scenes ------------------------------------------------------------
  // The scene stack. Navigating forward plays the new scene in and remembers
  // what it came from; back plays it out again.
  bool NavigateForward(const std::string_view name);
  bool NavigateBack();
  bool NavigateFirst(const std::string_view name);
  Element* back_scene() const;
  void SetBackScene(Element* root);
  // A scene loaded so it can be navigated to by name.
  void RegisterScene(const std::string_view name, const Scene* scene,
                     Element* root);

  const std::string& last_error() const { return last_error_; }
  bool opened() const { return state_ != nullptr; }
  lua_State* state() const { return state_; }

  // Registry access for the bindings.
  Element* ElementFromHandle(uint32_t handle) const;
  uint32_t HandleForElement(Element* element);
  // The playhead of one element, created on first use from the node's own
  // timelines. Null when the element has no states to play.
  TimelinePlayer* PlayerFor(Element* element);
  // Gives an element a playhead built from a node that is not its own. A
  // control whose look lives in a shared skin visual has no states of its
  // own to play, and several controls can share one visual while each holds
  // its own place in it.
  TimelinePlayer* AttachPlayer(Element* element, const Scene* scene,
                               const Node* node);
  // Focus without the focusability test, for a screen that decides for
  // itself what is focusable - the avatar editor's slots come from the
  // translated title, not from the scene tree.
  bool ForceFocus(Element* element);
  TimelinePlayer* player() const { return player_; }
  Element* scene_root() const { return scene_root_; }
  const Scene* scene() const { return scene_; }

  // Finds a script in the packages; empty when it is not there.
  std::vector<uint8_t> FindScript(const std::string_view module_name) const;

  // Localised text by its IDS_* key, out of the .xus tables in the packages.
  void AddStringTable(const std::string_view file);
  std::string LookupString(const std::string_view key) const;

  // Raises an event on an element, which is how a control tells its script
  // that something happened to it. Returns true when a handler took it.
  bool RaiseEvent(Element* element, const std::string_view name);

 private:
  void RegisterNatives();
  void InstallLoader();
  // Calls a method on the Lua class the element's XUI class was declared as,
  // if that class defines one. Absent handlers are the normal case.
  bool CallClassHandler(Element* element, const char* method);
  void CollectFocusable(Element* root, std::vector<Element*>* out) const;
  bool IsFocusable(Element* element) const;

  struct SceneEntry {
    std::string name;
    const Scene* scene = nullptr;
    Element* root = nullptr;
  };

  lua_State* state_ = nullptr;
  std::vector<Package*> packages_;
  const Scene* scene_ = nullptr;
  Element* scene_root_ = nullptr;
  TimelinePlayer* player_ = nullptr;
  std::map<Element*, std::unique_ptr<TimelinePlayer>> players_;
  Element* focus_ = nullptr;
  std::map<Element*, bool> input_disabled_;
  const Skin* skin_ = nullptr;
  std::map<Element*, std::string> visual_prefix_;
  std::map<std::string, std::string> strings_;
  std::map<std::string, SceneEntry> known_scenes_;
  std::vector<SceneEntry> scene_stack_;
  Element* back_scene_ = nullptr;
  std::vector<Element*> handles_;
  std::map<Element*, uint32_t> handle_of_;
  std::string last_error_;

 public:
  // A timer a script asked for through SetTimer. Public so the binding can
  // reach it without a friend declaration.
  struct Timer {
    uint32_t id = 0;
    float remaining = 0.0f;
    float interval = 0.0f;
    bool repeating = false;
    int callback_ref = 0;
    bool dead = false;
  };
  std::vector<Timer> timers_;
  uint32_t next_timer_id_ = 1;
};

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
