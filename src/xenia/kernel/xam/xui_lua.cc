/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_lua.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/xam/xui_game_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

constexpr const char* kHostKey = "nexia.xui.host";
constexpr const char* kModulePrefix = "Xbox.";
// XUIS, u8 major, u8 kind, u32 size, u16 count, entries from 0x0C.
constexpr uint32_t kStringTableHeader = 0x0C;

// The globals the console's own Lua host installs before any script runs. No
// script defines them and the entire class library is built on them:
// ImplementXuiClass calls _BuildClass and then setfenv's the caller into the
// class it returns, which is how a module's functions become methods.
//
// Class names are module paths ("Xbox.XuiControl"), and `module` publishes a
// module as a nested global, so a name is a path through _G rather than a key
// in it.
constexpr const char* kBootstrap = R"LUA(
local function pathOf(name)
  local parts = {}
  for part in string.gmatch(name, "[^%.]+") do
    parts[#parts + 1] = part
  end
  return parts
end

function _GetGlobal(name)
  if name == nil then return nil end
  local at = _G
  for _, part in ipairs(pathOf(name)) do
    if type(at) ~= "table" then return nil end
    at = rawget(at, part)
    if at == nil then return nil end
  end
  return at
end

function _BuildClass(name, baseName)
  local base = _GetGlobal(baseName)
  local class = {}
  class.__index = class
  class._className = name
  class._baseClass = base
  -- A class is also the environment its own module runs in, so what it does
  -- not define falls through to the base and then to the globals; without
  -- that the module cannot even reach `require`.
  setmetatable(class, { __index = base or _G })
  local parts = pathOf(name)
  local at = _G
  for i = 1, #parts - 1 do
    local nested = rawget(at, parts[i])
    if type(nested) ~= "table" then
      nested = {}
      rawset(at, parts[i], nested)
    end
    at = nested
  end
  rawset(at, parts[#parts], class)
  -- A class module never calls `module`, so without this require would hand
  -- its callers true instead of the class they asked for.
  package.loaded[name] = class
  return class
end

-- Class declares a plain class the same way, with no XUI class behind it.
function Class(name, baseName)
  local class = _BuildClass(name, baseName)
  setfenv(2, class)
  return class
end

-- import runs a module for what it publishes rather than for a return value.
function import(name)
  return require(name)
end

-- Only an instrumented console build has the coverage hooks.
coveragec = nil

function ImplementClass(name, baseName)
  local class = _BuildClass(name, baseName)
  setfenv(2, class)
  return class
end

function BeStrict(target)
  local meta = getmetatable(target)
  if meta == nil then
    meta = {}
    setmetatable(target, meta)
  end
  meta.__strict = true
  return target
end

function IsBeingStrict(target)
  local meta = getmetatable(target)
  return meta ~= nil and meta.__strict == true
end

function Delegate(object, method)
  if type(method) == "string" then
    return function(...) return object[method](object, ...) end
  end
  return function(...) return method(object, ...) end
end

function Curry(fn, ...)
  local bound = { ... }
  return function(...)
    local args = {}
    for i = 1, #bound do args[i] = bound[i] end
    local extra = { ... }
    for i = 1, #extra do args[#bound + i] = extra[i] end
    return fn(unpack(args))
  end
end

function CreateCallbackWrapper(fn)
  return function(...) return fn(...) end
end

function throw(err)
  error(err, 2)
end

function SafeCall(fn, ...)
  if fn == nil then return nil end
  local ok, result = pcall(fn, ...)
  if not ok then return nil, result end
  return result
end

function Cancel()
end

function Join(...)
  return table.concat({ ... })
end

function CheckParameter()
end
)LUA";

LuaHost* HostOf(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, kHostKey);
  auto* host = static_cast<LuaHost*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return host;
}

// Every element the scripts touch arrives as the handle XuiBase keeps in
// `_handle`, so each binding starts by turning that back into an element.
Element* ArgElement(lua_State* L, int index) {
  LuaHost* host = HostOf(L);
  if (!host) {
    return nullptr;
  }
  const auto handle = static_cast<uint32_t>(luaL_optinteger(L, index, 0));
  return host->ElementFromHandle(handle);
}

Element* CheckElement(lua_State* L, int index) {
  Element* element = ArgElement(L, index);
  if (!element) {
    luaL_error(L, "invalid object handle");
  }
  return element;
}

Element* FindChildById(Element* parent, const std::string_view id) {
  for (Element& child : parent->children) {
    if (child.id == id) {
      return &child;
    }
  }
  return nullptr;
}

Element* FindDescendantById(Element* parent, const std::string_view id) {
  for (Element& child : parent->children) {
    if (child.id == id) {
      return &child;
    }
    if (Element* found = FindDescendantById(&child, id)) {
      return found;
    }
  }
  return nullptr;
}

Element* ParentOf(Element* root, Element* target) {
  for (Element& child : root->children) {
    if (&child == target) {
      return root;
    }
    if (Element* found = ParentOf(&child, target)) {
      return found;
    }
  }
  return nullptr;
}

int PushHandle(lua_State* L, Element* element) {
  if (!element) {
    lua_pushnil(L);
    return 1;
  }
  LuaHost* host = HostOf(L);
  lua_pushinteger(L, host ? host->HandleForElement(element) : 0);
  return 1;
}

// ---------------------------------------------------------------------------
// Xbox.XuiBaseImpl
// ---------------------------------------------------------------------------

int Base_IsValidObject(lua_State* L) {
  lua_pushboolean(L, ArgElement(L, 1) != nullptr);
  return 1;
}

int Base_GetClassName(lua_State* L) {
  Element* element = CheckElement(L, 1);
  lua_pushstring(L, element->class_name.c_str());
  return 1;
}

int Base_GetObjectClass(lua_State* L) { return Base_GetClassName(L); }

int Base_GetBaseClass(lua_State* L) {
  Element* element = CheckElement(L, 1);
  const std::vector<const XuiClass*> chain = ClassChain(element->class_name);
  // The chain is base first, so the class just below this one is its base.
  if (chain.size() >= 2) {
    lua_pushstring(L, chain[chain.size() - 2]->name);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

int Base_ClassDerivesFrom(lua_State* L) {
  Element* element = CheckElement(L, 1);
  const char* wanted = luaL_checkstring(L, 2);
  bool derives = false;
  for (const XuiClass* level : ClassChain(element->class_name)) {
    if (std::strcmp(level->name, wanted) == 0) {
      derives = true;
      break;
    }
  }
  lua_pushboolean(L, derives);
  return 1;
}

// The scenes are authored, not constructed, so an object is created by naming
// one that already exists rather than by allocating a new node.
int Base_CreateObject(lua_State* L) {
  LuaHost* host = HostOf(L);
  const char* id = luaL_optstring(L, 2, nullptr);
  Element* root = host ? host->scene_root() : nullptr;
  if (!root || !id) {
    lua_pushnil(L);
    return 1;
  }
  return PushHandle(L, FindDescendantById(root, id));
}

int Base_DestroyObject(lua_State* L) {
  // Nothing owns an authored element outside its scene; dropping the handle
  // is all a script can really do to it.
  (void)L;
  return 0;
}

int Base_ObjectFromHandle(lua_State* L) {
  lua_pushvalue(L, 1);
  return 1;
}

int Base_ReadFile(lua_State* L) {
  LuaHost* host = HostOf(L);
  const char* name = luaL_checkstring(L, 1);
  const std::vector<uint8_t> data =
      host ? host->FindScript(name) : std::vector<uint8_t>();
  if (data.empty()) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, reinterpret_cast<const char*>(data.data()), data.size());
  return 1;
}

int Base_GetProperty(lua_State* L) {
  Element* element = CheckElement(L, 1);
  const char* name = luaL_checkstring(L, 2);
  if (!element->source) {
    lua_pushnil(L);
    return 1;
  }
  const Value* value = Property(*element->source, name);
  if (!value) {
    lua_pushnil(L);
    return 1;
  }
  switch (value->type) {
    case PropertyType::kBool:
      lua_pushboolean(L, value->boolean);
      break;
    case PropertyType::kInteger:
    case PropertyType::kUnsigned:
      lua_pushinteger(L, static_cast<lua_Integer>(value->integer));
      break;
    case PropertyType::kFloat:
      lua_pushnumber(L, value->number);
      break;
    case PropertyType::kColor:
      lua_pushinteger(L, static_cast<lua_Integer>(value->color));
      break;
    case PropertyType::kString:
      lua_pushstring(L, value->string.c_str());
      break;
    case PropertyType::kVector:
    case PropertyType::kQuaternion:
      lua_pushnumber(L, value->vector.value[0]);
      lua_pushnumber(L, value->vector.value[1]);
      lua_pushnumber(L, value->vector.value[2]);
      return 3;
    default:
      lua_pushnil(L);
      break;
  }
  return 1;
}

// The authored scene is read-only, so a property set lands on the live value
// the drawer reads. The ones without a live counterpart are the scene's own
// description of itself and cannot change.
int Base_SetProperty(lua_State* L) {
  Element* element = CheckElement(L, 1);
  const std::string name = luaL_checkstring(L, 2);
  if (name == "Opacity") {
    element->opacity = static_cast<float>(luaL_checknumber(L, 3));
  } else if (name == "Show") {
    element->visible = lua_toboolean(L, 3) != 0;
  } else if (name == "Width") {
    element->rect.width = static_cast<float>(luaL_checknumber(L, 3));
  } else if (name == "Height") {
    element->rect.height = static_cast<float>(luaL_checknumber(L, 3));
  } else if (name == "Position") {
    element->rect.x = static_cast<float>(luaL_checknumber(L, 3));
    element->rect.y = static_cast<float>(luaL_optnumber(L, 4, 0.0));
  } else {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_pushboolean(L, 1);
  return 1;
}

int Base_SetTimer(lua_State* L) {
  LuaHost* host = HostOf(L);
  if (!host) {
    lua_pushnil(L);
    return 1;
  }
  const auto interval = static_cast<float>(luaL_checknumber(L, 1)) / 1000.0f;
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_pushvalue(L, 2);
  LuaHost::Timer timer;
  timer.id = host->next_timer_id_++;
  timer.interval = interval;
  timer.remaining = interval;
  timer.repeating = lua_toboolean(L, 3) != 0;
  timer.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  host->timers_.push_back(timer);
  lua_pushinteger(L, timer.id);
  return 1;
}

int Base_KillTimer(lua_State* L) {
  LuaHost* host = HostOf(L);
  const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
  if (host) {
    for (LuaHost::Timer& timer : host->timers_) {
      if (timer.id == id) {
        timer.dead = true;
      }
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Xbox.XuiElementImpl
// ---------------------------------------------------------------------------

int Element_GetId(lua_State* L) {
  lua_pushstring(L, CheckElement(L, 1)->id.c_str());
  return 1;
}

int Element_GetChildById(lua_State* L) {
  Element* element = CheckElement(L, 1);
  return PushHandle(L, FindChildById(element, luaL_checkstring(L, 2)));
}

int Element_GetDescendantById(lua_State* L) {
  Element* element = CheckElement(L, 1);
  return PushHandle(L, FindDescendantById(element, luaL_checkstring(L, 2)));
}

int Element_GetFirstChild(lua_State* L) {
  Element* element = CheckElement(L, 1);
  return PushHandle(
      L, element->children.empty() ? nullptr : &element->children[0]);
}

int Element_GetLastChild(lua_State* L) {
  Element* element = CheckElement(L, 1);
  return PushHandle(L, element->children.empty()
                           ? nullptr
                           : &element->children[element->children.size() - 1]);
}

int Element_GetParent(lua_State* L) {
  LuaHost* host = HostOf(L);
  Element* element = CheckElement(L, 1);
  Element* root = host ? host->scene_root() : nullptr;
  return PushHandle(L, root ? ParentOf(root, element) : nullptr);
}

int Element_GetSibling(lua_State* L, int step) {
  LuaHost* host = HostOf(L);
  Element* element = CheckElement(L, 1);
  Element* root = host ? host->scene_root() : nullptr;
  Element* parent = root ? ParentOf(root, element) : nullptr;
  if (!parent) {
    lua_pushnil(L);
    return 1;
  }
  for (size_t i = 0; i < parent->children.size(); ++i) {
    if (&parent->children[i] != element) {
      continue;
    }
    const auto at = static_cast<int64_t>(i) + step;
    if (at < 0 || at >= static_cast<int64_t>(parent->children.size())) {
      break;
    }
    return PushHandle(L, &parent->children[static_cast<size_t>(at)]);
  }
  lua_pushnil(L);
  return 1;
}

int Element_GetNext(lua_State* L) { return Element_GetSibling(L, 1); }
int Element_GetPrevious(lua_State* L) { return Element_GetSibling(L, -1); }

int Element_IsDescendant(lua_State* L) {
  Element* element = CheckElement(L, 1);
  Element* other = CheckElement(L, 2);
  bool found = false;
  std::function<void(Element*)> walk = [&](Element* node) {
    for (Element& child : node->children) {
      if (&child == other) {
        found = true;
        return;
      }
      walk(&child);
    }
  };
  walk(element);
  lua_pushboolean(L, found);
  return 1;
}

int Element_GetWidth(lua_State* L) {
  lua_pushnumber(L, CheckElement(L, 1)->rect.width);
  return 1;
}

int Element_GetHeight(lua_State* L) {
  lua_pushnumber(L, CheckElement(L, 1)->rect.height);
  return 1;
}

int Element_SetWidth(lua_State* L) {
  CheckElement(L, 1)->rect.width = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}

int Element_SetHeight(lua_State* L) {
  CheckElement(L, 1)->rect.height = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}

int Element_GetPosition(lua_State* L) {
  Element* element = CheckElement(L, 1);
  lua_pushnumber(L, element->rect.x);
  lua_pushnumber(L, element->rect.y);
  lua_pushnumber(L, 0.0);
  return 3;
}

// A script moves an element by the offset it was authored with, so the whole
// subtree has to come along the way it does during layout.
int Element_SetPosition(lua_State* L) {
  Element* element = CheckElement(L, 1);
  const auto x = static_cast<float>(luaL_checknumber(L, 2));
  const auto y = static_cast<float>(luaL_checknumber(L, 3));
  const float dx = x - element->rect.x;
  const float dy = y - element->rect.y;
  std::function<void(Element&)> move = [&](Element& node) {
    node.rect.x += dx;
    node.rect.y += dy;
    for (Element& child : node.children) {
      move(child);
    }
  };
  move(*element);
  return 0;
}

int Element_GetScale(lua_State* L) {
  Element* element = CheckElement(L, 1);
  lua_pushnumber(L, element->scale_x);
  lua_pushnumber(L, element->scale_y);
  lua_pushnumber(L, 1.0);
  return 3;
}

// Scale is absolute, not cumulative: setting it twice to 2 leaves the element
// twice its laid-out size, not four times.
int Element_SetScale(lua_State* L) {
  Element* element = CheckElement(L, 1);
  const auto sx = static_cast<float>(luaL_checknumber(L, 2));
  const auto sy = static_cast<float>(luaL_optnumber(L, 3, sx));
  const float relative_x =
      element->scale_x != 0.0f ? sx / element->scale_x : sx;
  const float relative_y =
      element->scale_y != 0.0f ? sy / element->scale_y : sy;
  const float pivot_x = element->rect.x + element->pivot_x;
  const float pivot_y = element->rect.y + element->pivot_y;
  element->rect.width *= relative_x;
  element->rect.height *= relative_y;
  element->rect.x = pivot_x - element->pivot_x * relative_x;
  element->rect.y = pivot_y - element->pivot_y * relative_y;
  element->scale_x = sx;
  element->scale_y = sy;
  return 0;
}

int Element_GetPivot(lua_State* L) {
  Element* element = CheckElement(L, 1);
  lua_pushnumber(L, element->pivot_x);
  lua_pushnumber(L, element->pivot_y);
  lua_pushnumber(L, 0.0);
  return 3;
}

int Element_SetPivot(lua_State* L) {
  Element* element = CheckElement(L, 1);
  element->pivot_x = static_cast<float>(luaL_checknumber(L, 2));
  element->pivot_y = static_cast<float>(luaL_checknumber(L, 3));
  return 0;
}

int Element_GetOpacity(lua_State* L) {
  lua_pushnumber(L, CheckElement(L, 1)->opacity);
  return 1;
}

int Element_SetOpacity(lua_State* L) {
  CheckElement(L, 1)->opacity = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}

int Element_IsShown(lua_State* L) {
  lua_pushboolean(L, CheckElement(L, 1)->visible);
  return 1;
}

int Element_SetShow(lua_State* L) {
  CheckElement(L, 1)->visible = lua_toboolean(L, 2) != 0;
  return 0;
}

// Scenes turn things in the plane only, so of the three angles a script can
// give, the one about z is the one that has any effect.
int Element_SetRotation(lua_State* L) {
  Element* element = CheckElement(L, 1);
  const auto z = static_cast<float>(luaL_optnumber(L, 4, 0.0));
  element->rotation = z;
  return 0;
}

int Element_GetRotation(lua_State* L) {
  Element* element = CheckElement(L, 1);
  lua_pushnumber(L, 0.0);
  lua_pushnumber(L, 0.0);
  lua_pushnumber(L, element->rotation);
  return 3;
}

// The one place a script reaches the timeline machinery directly.
int Element_PlayTimeline(lua_State* L) {
  LuaHost* host = HostOf(L);
  (void)CheckElement(L, 1);
  const char* name = luaL_checkstring(L, 2);
  TimelinePlayer* player = host ? host->player() : nullptr;
  const bool played = player && player->Play(name, lua_toboolean(L, 3) != 0);
  lua_pushboolean(L, played);
  return 1;
}

int Element_FindNamedFrame(lua_State* L) {
  LuaHost* host = HostOf(L);
  (void)CheckElement(L, 1);
  const char* name = luaL_checkstring(L, 2);
  TimelinePlayer* player = host ? host->player() : nullptr;
  if (!player) {
    lua_pushnil(L);
    return 1;
  }
  const StateSpan span = player->states().Find(name);
  if (!span.valid) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, span.first);
  lua_pushinteger(L, span.last);
  return 2;
}

int Element_SetFocus(lua_State* L) {
  LuaHost* host = HostOf(L);
  Element* element = CheckElement(L, 1);
  lua_pushboolean(L, host && host->SetFocus(element));
  return 1;
}

int Element_GetFocus(lua_State* L) {
  LuaHost* host = HostOf(L);
  (void)CheckElement(L, 1);
  return PushHandle(L, host ? host->focus() : nullptr);
}

int Element_InitFocus(lua_State* L) {
  LuaHost* host = HostOf(L);
  (void)CheckElement(L, 1);
  lua_pushboolean(L, host && host->InitFocus());
  return 1;
}

int Element_TreeHasFocus(lua_State* L) {
  LuaHost* host = HostOf(L);
  Element* element = CheckElement(L, 1);
  lua_pushboolean(L, host && host->TreeHasFocus(element));
  return 1;
}

int Element_EnableInput(lua_State* L) {
  LuaHost* host = HostOf(L);
  Element* element = CheckElement(L, 1);
  if (host) {
    host->SetInputEnabled(element, lua_toboolean(L, 2) != 0);
  }
  return 0;
}

int Element_InputEnabled(lua_State* L) {
  LuaHost* host = HostOf(L);
  Element* element = CheckElement(L, 1);
  lua_pushboolean(L, host && host->InputEnabled(element));
  return 1;
}

// The tree these act on is the one the layout built, so a child is moved
// within its own parent rather than reparented across scenes.
int Element_Unlink(lua_State* L) {
  LuaHost* host = HostOf(L);
  Element* element = CheckElement(L, 1);
  Element* root = host ? host->scene_root() : nullptr;
  Element* parent = root ? ParentOf(root, element) : nullptr;
  if (!parent) {
    return 0;
  }
  // Unlinking hides it: erasing from the vector would move every sibling and
  // invalidate the handles the scripts are holding.
  element->visible = false;
  if (host && host->focus() == element) {
    host->SetFocus(nullptr);
  }
  return 0;
}

int Element_AddChild(lua_State* L) {
  Element* parent = CheckElement(L, 1);
  Element* child = CheckElement(L, 2);
  // Both already live in the tree; what a script means by adding one is that
  // it should be shown under this parent.
  child->visible = true;
  (void)parent;
  return 0;
}

int Element_SetVisualPrefix(lua_State* L) {
  LuaHost* host = HostOf(L);
  Element* element = CheckElement(L, 1);
  const char* prefix = luaL_optstring(L, 2, "");
  lua_pushboolean(L, host && host->SetVisualPrefix(element, prefix));
  return 1;
}

int Element_GetVisualPrefix(lua_State* L) {
  LuaHost* host = HostOf(L);
  Element* element = CheckElement(L, 1);
  lua_pushstring(L, host ? host->VisualPrefix(element).c_str() : "");
  return 1;
}

int Element_Nop(lua_State* L) {
  (void)L;
  return 0;
}

int Element_False(lua_State* L) {
  lua_pushboolean(L, 0);
  return 1;
}

// ---------------------------------------------------------------------------
// Xbox.XuiSceneImpl
// ---------------------------------------------------------------------------

// Create names a scene the host has loaded; the scene that is already up is
// what a script gets when it names nothing.
int Scene_Create(lua_State* L) {
  LuaHost* host = HostOf(L);
  if (!host) {
    lua_pushnil(L);
    return 1;
  }
  const char* name = luaL_optstring(L, 1, nullptr);
  if (name && host->NavigateForward(name)) {
    return PushHandle(L, host->scene_root());
  }
  return PushHandle(L, host->scene_root());
}

int Scene_NavigateForward(lua_State* L) {
  LuaHost* host = HostOf(L);
  const char* name = luaL_optstring(L, 1, nullptr);
  lua_pushboolean(L, host && name && host->NavigateForward(name));
  return 1;
}

int Scene_NavigateBack(lua_State* L) {
  LuaHost* host = HostOf(L);
  lua_pushboolean(L, host && host->NavigateBack());
  return 1;
}

int Scene_NavigateFirst(lua_State* L) {
  LuaHost* host = HostOf(L);
  const char* name = luaL_optstring(L, 1, nullptr);
  lua_pushboolean(L, host && name && host->NavigateFirst(name));
  return 1;
}

int Scene_GetBackScene(lua_State* L) {
  LuaHost* host = HostOf(L);
  return PushHandle(L, host ? host->back_scene() : nullptr);
}

int Scene_SetBackScene(lua_State* L) {
  LuaHost* host = HostOf(L);
  if (host) {
    host->SetBackScene(ArgElement(L, 1));
  }
  return 0;
}

// A transition is a named span on the scene itself, so the four the class
// library asks for are simply four spans to play.
int PlayNamed(lua_State* L, const char* state) {
  LuaHost* host = HostOf(L);
  TimelinePlayer* player = host ? host->PlayerFor(host->scene_root()) : nullptr;
  lua_pushboolean(L, player && player->Play(state));
  return 1;
}

int Scene_PlayToTransition(lua_State* L) { return PlayNamed(L, "TransTo"); }
int Scene_PlayFromTransition(lua_State* L) { return PlayNamed(L, "FadeIn"); }
int Scene_PlayBackToTransition(lua_State* L) {
  return PlayNamed(L, "TransBack");
}
int Scene_PlayBackFromTransition(lua_State* L) {
  return PlayNamed(L, "FadeOut");
}

int Scene_InterruptTransitions(lua_State* L) {
  LuaHost* host = HostOf(L);
  if (TimelinePlayer* player =
          host ? host->PlayerFor(host->scene_root()) : nullptr) {
    player->Stop();
  }
  return 0;
}

int Scene_Nop(lua_State* L) {
  (void)L;
  return 0;
}

// ---------------------------------------------------------------------------
// Xbox.System, Xbox.Text, Xbox.Scheduler
// ---------------------------------------------------------------------------

int System_GetTickCount(lua_State* L) {
  lua_pushnumber(
      L, static_cast<lua_Number>(xe::Clock::QueryHostSystemTime() / 10000ull));
  return 1;
}

int System_GetTickDiff(lua_State* L) {
  const lua_Number from = luaL_checknumber(L, 1);
  const lua_Number to = luaL_optnumber(L, 2, 0.0);
  lua_pushnumber(L, to != 0.0
                        ? to - from
                        : static_cast<lua_Number>(
                              xe::Clock::QueryHostSystemTime() / 10000ull) -
                              from);
  return 1;
}

int System_GetSystemTime(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(xe::Clock::QueryHostSystemTime()));
  return 1;
}

int System_GetCommonString(lua_State* L) {
  LuaHost* host = HostOf(L);
  const char* key = luaL_checkstring(L, 1);
  const std::string text = host ? host->LookupString(key) : std::string();
  if (text.empty()) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, text.c_str());
  }
  return 1;
}

int System_IsUIActive(lua_State* L) {
  lua_pushboolean(L, 1);
  return 1;
}

int System_False(lua_State* L) {
  lua_pushboolean(L, 0);
  return 1;
}

int System_Nil(lua_State* L) {
  lua_pushnil(L);
  return 1;
}

int Text_Trim(lua_State* L) {
  std::string text = luaL_checkstring(L, 1);
  const size_t first = text.find_first_not_of(" \t\r\n");
  const size_t last = text.find_last_not_of(" \t\r\n");
  if (first == std::string::npos) {
    lua_pushstring(L, "");
  } else {
    lua_pushstring(L, text.substr(first, last - first + 1).c_str());
  }
  return 1;
}

int Text_Replace(lua_State* L) {
  std::string text = luaL_checkstring(L, 1);
  const std::string from = luaL_checkstring(L, 2);
  const std::string to = luaL_checkstring(L, 3);
  if (!from.empty()) {
    size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
      text.replace(at, from.size(), to);
      at += to.size();
    }
  }
  lua_pushstring(L, text.c_str());
  return 1;
}

// The scheduler runs a function later; a zero delay is the next tick.
int Scheduler_Start(lua_State* L) {
  LuaHost* host = HostOf(L);
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_pushvalue(L, 1);
  if (!host) {
    lua_pop(L, 1);
    lua_pushnil(L);
    return 1;
  }
  LuaHost::Timer timer;
  timer.id = host->next_timer_id_++;
  timer.interval = static_cast<float>(luaL_optnumber(L, 2, 0.0)) / 1000.0f;
  timer.remaining = timer.interval;
  timer.repeating = false;
  timer.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  host->timers_.push_back(timer);
  lua_pushinteger(L, timer.id);
  return 1;
}

int StringTable_GetString(lua_State* L) {
  LuaHost* host = HostOf(L);
  const char* key = luaL_checkstring(L, 1);
  const std::string text = host ? host->LookupString(key) : std::string();
  lua_pushstring(L, text.c_str());
  return 1;
}

struct Binding {
  const char* name;
  lua_CFunction fn;
};

const Binding kBaseImpl[] = {
    {"IsValidObject", Base_IsValidObject},
    {"GetClassName", Base_GetClassName},
    {"GetObjectClass", Base_GetObjectClass},
    {"GetBaseClass", Base_GetBaseClass},
    {"ClassDerivesFrom", Base_ClassDerivesFrom},
    {"CreateObject", Base_CreateObject},
    {"DestroyObject", Base_DestroyObject},
    {"LoadObject", Base_CreateObject},
    {"LoadVisual", Base_CreateObject},
    {"_GetObject", Base_ObjectFromHandle},
    {"_ObjectFromHandle", Base_ObjectFromHandle},
    {"ReadFile", Base_ReadFile},
    {"GetProperty", Base_GetProperty},
    {"SetProperty", Base_SetProperty},
    {"SetTimer", Base_SetTimer},
    {"KillTimer", Base_KillTimer},
    {nullptr, nullptr},
};

const Binding kElementImpl[] = {
    {"GetId", Element_GetId},
    {"GetChildById", Element_GetChildById},
    {"GetDescendantById", Element_GetDescendantById},
    {"GetFirstChild", Element_GetFirstChild},
    {"GetLastChild", Element_GetLastChild},
    {"GetParent", Element_GetParent},
    {"GetNext", Element_GetNext},
    {"GetPrevious", Element_GetPrevious},
    {"IsDescendant", Element_IsDescendant},
    {"GetWidth", Element_GetWidth},
    {"GetHeight", Element_GetHeight},
    {"SetWidth", Element_SetWidth},
    {"SetHeight", Element_SetHeight},
    {"GetPosition", Element_GetPosition},
    {"SetPosition", Element_SetPosition},
    {"GetScale", Element_GetScale},
    {"SetScale", Element_SetScale},
    {"GetPivot", Element_GetPivot},
    {"SetPivot", Element_SetPivot},
    {"GetOpacity", Element_GetOpacity},
    {"SetOpacity", Element_SetOpacity},
    {"IsShown", Element_IsShown},
    {"SetShow", Element_SetShow},
    {"SetRotation", Element_SetRotation},
    {"GetRotation", Element_GetRotation},
    {"PlayTimeline", Element_PlayTimeline},
    {"FindNamedFrame", Element_FindNamedFrame},
    {"SetFocus", Element_SetFocus},
    {"GetFocus", Element_GetFocus},
    {"InitFocus", Element_InitFocus},
    {"TreeHasFocus", Element_TreeHasFocus},
    {"EnableInput", Element_EnableInput},
    {"InputEnabled", Element_InputEnabled},
    {"AddChild", Element_AddChild},
    {"InsertChild", Element_AddChild},
    {"Unlink", Element_Unlink},
    {"SetVisualPrefix", Element_SetVisualPrefix},
    {"GetVisualPrefix", Element_GetVisualPrefix},
    {"DisallowRecursiveTimelineControl", Element_Nop},
    {nullptr, nullptr},
};

const Binding kSceneImpl[] = {
    {"Create", Scene_Create},
    {"NavigateBack", Scene_NavigateBack},
    {"NavigateFirst", Scene_NavigateFirst},
    {"NavigateFirstEx", Scene_NavigateFirst},
    {"NavigateForward", Scene_NavigateForward},
    {"GetBackScene", Scene_GetBackScene},
    {"SetBackScene", Scene_SetBackScene},
    // Whether a transition waits on another scene changes nothing while the
    // scenes are drawn one at a time.
    {"EnableTransitionDependency", Scene_Nop},
    {"PlayToTransition", Scene_PlayToTransition},
    {"PlayFromTransition", Scene_PlayFromTransition},
    {"PlayBackToTransition", Scene_PlayBackToTransition},
    {"PlayBackFromTransition", Scene_PlayBackFromTransition},
    {"InterruptTransitions", Scene_InterruptTransitions},
    {nullptr, nullptr},
};

const Binding kSystem[] = {
    {"GetTickCount", System_GetTickCount},
    {"GetTickDiff", System_GetTickDiff},
    {"GetSystemTime", System_GetSystemTime},
    {"GetCommonString", System_GetCommonString},
    {"IsUIActive", System_IsUIActive},
    // Live, demands and hive values belong to services that are not here; a
    // script that asks gets a clear no rather than a hang.
    {"IsConnectedToLive", System_False},
    {"GetLiveHiveValue", System_Nil},
    {"Demand", System_Nil},
    {"BeginDemand", System_Nil},
    {"CompleteDemand", System_Nil},
    {"GetCurrentDemand", System_Nil},
    {"GetErrorStringFromService", System_Nil},
    {"ShowMessageBox", System_Nil},
    {nullptr, nullptr},
};

const Binding kText[] = {
    {"Trim", Text_Trim},
    {"Replace", Text_Replace},
    {nullptr, nullptr},
};

const Binding kScheduler[] = {
    {"Start", Scheduler_Start},
    {nullptr, nullptr},
};

const Binding kStringTableImpl[] = {
    {"GetString", StringTable_GetString},
    {"GetText", StringTable_GetString},
    {nullptr, nullptr},
};

// Xbox.Dash.GameLibrary - the My Games tile.
//
// Shapes taken from dash.xex, not invented. Marketplace.GameLibrary:Load does
//
//   while self.Engine == false do
//     self.Engine = Xbox.Dash.GameLibrary.LoadGameLibrary()
//     if self.Engine == false then Sleep(0) end
//   end
//
// so LoadGameLibrary may answer immediately, and this one does.
// Marketplace.GameLibraryContentEnum._ReloadHelper then does
//
//   LibEnum = engine:CreateEnumerator(self.Filter)
//   for i = 1, LibEnum:GetTitleCount() do
//     title = LibEnum:GetTitleInfo(i)          -- ONE BASED
//     if title == false then return end
//     ...
//
// and Marketplace.GameLibrary:ConvertToLibraryItem reads Name, TitleId,
// ContentType, Category, IsPurchased, ImagePath, DiscMediaId, FileName,
// LatestTime, IsInPlayHistory and BasicAchievementInfo off each one. The rest
// of the properties below are the remainder of what luatitleinfo.cpp declares,
// so a script reaching for one of them gets a value rather than nothing.
void PushTitleInfo(lua_State* L, const GameLibraryItem& item) {
  lua_newtable(L);
  const auto set_string = [&](const char* key, const std::string& value) {
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, key);
  };
  const auto set_number = [&](const char* key, double value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
  };
  const auto set_bool = [&](const char* key, bool value) {
    lua_pushboolean(L, value ? 1 : 0);
    lua_setfield(L, -2, key);
  };

  set_string("Name", item.name);
  // A NUMBER: ConvertToLibraryItem runs it through tonumber, which an eight
  // hex digit string would fail.
  set_number("TitleId", double(item.title_id));
  set_number("ContentType", double(item.content_type));
  set_number("Category", double(item.category));
  set_string("FileName", item.path);
  set_string("DiscMediaId", item.media_id);
  // FILETIME, the unit the scripts sort on.
  set_number("LatestTime", double(item.latest_time));
  set_bool("IsInPlayHistory", item.in_play_history);
  // Nothing here is bought, rented or trialled: everything is local and
  // whole, so the licence questions all answer the same way.
  set_bool("IsPurchased", true);
  set_bool("IsLoaded", true);
  set_bool("IsInstallable", false);
  set_bool("IsDeletable", false);
  set_bool("IsRecentlyDownloaded", false);
  set_bool("IsArcadeTrial", false);
  set_bool("IsGameDemo", false);
  set_bool("IsNuiGame", false);
  set_bool("IsCommunityCreator", false);
  // An XNA title IS a community game, which is what the dashboard calls the
  // indie channel. Nothing here is a trial, so it is the full version.
  set_bool("IsCommunityGame", item.is_xna);
  set_bool("IsFullCommunityGame", item.is_xna);
  set_bool("IsTrialCommunityGame", false);
  set_bool("ShowRatingInfo", false);
  set_string("PCRatingDescription", "");
  set_string("LeaderboardName", "");
  set_number("OfferId", 0.0);
  // "titleicon://XXXXXXXX" when the title has an icon, which the scene
  // drawer's art source answers out of played.db, or empty when it has none.
  set_string("ImagePath", item.image_path);
  set_string("SlotImage", item.image_path);
  set_number("AchievementsEarned", double(item.achievements_earned));
  set_number("AchievementsPossible", double(item.achievements_possible));
  set_number("GamerScoreEarned", double(item.gamerscore_earned));
  set_number("GamerScorePossible", double(item.gamerscore_possible));

  // ConvertToLibraryItem reads the achievement line off this, not off the
  // title, and only when the title is in the play history.
  lua_newtable(L);
  set_number("AchievementsEarned", double(item.achievements_earned));
  set_number("AchievementsPossible", double(item.achievements_possible));
  set_number("GamerScoreEarned", double(item.gamerscore_earned));
  set_number("GamerScorePossible", double(item.gamerscore_possible));
  lua_setfield(L, -2, "BasicAchievementInfo");

  // title:Launch(). The path is an upvalue rather than read back off the
  // table, so a script that overwrote FileName cannot redirect it.
  lua_pushlstring(L, item.path.data(), item.path.size());
  lua_pushcclosure(
      L,
      [](lua_State* S) {
        const char* path = lua_tostring(S, lua_upvalueindex(1));
        lua_pushboolean(S, path && LaunchGameLibraryItem(path) ? 1 : 0);
        return 1;
      },
      1);
  lua_setfield(L, -2, "Launch");

  // The rest of what luatitleinfo.cpp declares. Nothing here can be bought,
  // installed or rated, so these answer rather than being absent - a script
  // calling one gets a clear no instead of an error.
  static const char* const kNoMethods[] = {
      "InstallToHDD",       "GetPCRatingImage",   "GetImageInfo",
      "GetLeaderboardInfo", "GetAchievementInfo",
  };
  for (const char* name : kNoMethods) {
    lua_pushcfunction(L, [](lua_State* S) {
      lua_pushboolean(S, 0);
      return 1;
    });
    lua_setfield(L, -2, name);
  }
}

// The list the enumerator was built over, kept as an upvalue so the whole
// library is read once per enumerator rather than once per row.
int GameLibraryEnum_GetTitleCount(lua_State* L) {
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_pushnumber(L, double(lua_objlen(L, -1)));
  return 1;
}

int GameLibraryEnum_GetTitleInfo(lua_State* L) {
  // One based, and FALSE past the end - _ReloadHelper stops on false.
  const int index = int(luaL_optnumber(L, 2, 0.0));
  lua_pushvalue(L, lua_upvalueindex(1));
  if (index < 1 || size_t(index) > lua_objlen(L, -1)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_rawgeti(L, -1, index);
  return 1;
}

int GameLibrary_CreateEnumerator(lua_State* L) {
  // The filter states NuiTitlesOnly, LicenseType, TitleId, SortOrder,
  // Category, ContentType and PackageType. Nothing here is Nui and everything
  // holds a full local licence, and the order is already newest first, so
  // those three are ignored; the rest select.
  //
  // PackageType is what separates the hub's two tiles. DataSet.Library does
  //
  //   _IsAppsList = (Filter.PackageType == 2) or (Filter.PackageType == 3)
  //
  // so 2 and 3 ask for My Apps and anything else asks for My Games.
  //
  // SortOrder is the dashboard's own: DataSet.LibrarySort builds "SortOrder=1"
  // for its default entry and "SortOrder=5" for the calendar one, and
  // DataSet.Library falls back to 1 when a filter states none. Both mean most
  // recently played here, which is the order the library is already built in.
  double want_title_id = 0.0;
  double want_content_type = 0.0;
  double want_category = 0.0;
  double package_type = 0.0;
  double sort_order = 0.0;
  if (lua_istable(L, 2)) {
    const auto number = [&](const char* key) {
      lua_getfield(L, 2, key);
      const double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
      lua_pop(L, 1);
      return value;
    };
    want_title_id = number("TitleId");
    want_content_type = number("ContentType");
    want_category = number("Category");
    package_type = number("PackageType");
    sort_order = number("SortOrder");
  }
  const bool apps_only = package_type == 2.0 || package_type == 3.0;

  const std::vector<GameLibraryItem> items =
      BuildGameLibrary(GameLibrarySortFor(uint32_t(sort_order)));
  lua_newtable(L);
  int written = 0;
  for (const GameLibraryItem& item : items) {
    if (item.is_app() != apps_only) {
      continue;
    }
    if (want_title_id != 0.0 && double(item.title_id) != want_title_id) {
      continue;
    }
    if (want_content_type != 0.0 &&
        double(item.content_type) != want_content_type) {
      continue;
    }
    if (want_category != 0.0 && double(item.category) != want_category) {
      continue;
    }
    PushTitleInfo(L, item);
    lua_rawseti(L, -2, ++written);
  }

  // The enumerator, closing over that list.
  lua_newtable(L);
  lua_pushvalue(L, -2);
  lua_pushcclosure(L, GameLibraryEnum_GetTitleCount, 1);
  lua_setfield(L, -2, "GetTitleCount");
  lua_pushvalue(L, -2);
  lua_pushcclosure(L, GameLibraryEnum_GetTitleInfo, 1);
  lua_setfield(L, -2, "GetTitleInfo");
  lua_remove(L, -2);
  return 1;
}

int GameLibrary_Load(lua_State* L) {
  // The engine. Nothing is read here - CreateEnumerator builds the list, so a
  // library that changes between enumerations is picked up without the
  // scripts having to be told.
  lua_newtable(L);
  lua_pushcfunction(L, GameLibrary_CreateEnumerator);
  lua_setfield(L, -2, "CreateEnumerator");
  return 1;
}

const Binding kGameLibrary[] = {
    {"LoadGameLibrary", GameLibrary_Load},
    {nullptr, nullptr},
};

// Stores the value on the top of the stack at a dotted path through _G,
// creating the tables along the way. The scripts reach a native module both
// by requiring it and by naming it - Xbox.Content - so it has to be in both
// places.
void SetGlobalPath(lua_State* L, const char* path) {
  const int value = lua_gettop(L);
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  std::string remaining(path);
  size_t at = 0;
  while (true) {
    const size_t dot = remaining.find('.', at);
    const std::string part = remaining.substr(at, dot - at);
    if (dot == std::string::npos) {
      lua_pushvalue(L, value);
      lua_setfield(L, -2, part.c_str());
      break;
    }
    lua_getfield(L, -1, part.c_str());
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      lua_newtable(L);
      lua_pushvalue(L, -1);
      lua_setfield(L, -3, part.c_str());
    }
    lua_remove(L, -2);
    at = dot + 1;
  }
  lua_pop(L, 1);
}

void PreloadTable(lua_State* L, const char* module_name,
                  const Binding* bindings) {
  lua_newtable(L);
  for (const Binding* binding = bindings; binding->name; ++binding) {
    lua_pushcfunction(L, binding->fn);
    lua_setfield(L, -2, binding->name);
  }

  // package.preload[name] = function() return <table> end
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "preload");
  lua_pushvalue(L, -3);
  lua_pushcclosure(
      L,
      [](lua_State* S) {
        lua_pushvalue(S, lua_upvalueindex(1));
        return 1;
      },
      1);
  lua_setfield(L, -2, module_name);
  lua_pop(L, 2);

  SetGlobalPath(L, module_name);
  lua_pop(L, 1);
}

// A service this host does not provide. Reading anything off it gives another
// one of the same, and calling it answers nothing, so a script can walk as
// deep into an absent service as it likes and still run. Without that, the
// half of the dashboard that talks to Live takes the whole scene down with
// it.
int AbsentServiceCall(lua_State* L) {
  const int count = lua_gettop(L);
  for (int i = 0; i < count; ++i) {
    lua_pushnil(L);
  }
  return count ? count : 1;
}

void PushAbsentService(lua_State* L);

int AbsentServiceIndex(lua_State* L) {
  PushAbsentService(L);
  return 1;
}

void PushAbsentService(lua_State* L) {
  lua_newtable(L);
  lua_newtable(L);
  lua_pushcfunction(L, AbsentServiceIndex);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, AbsentServiceCall);
  lua_setfield(L, -2, "__call");
  lua_setmetatable(L, -2);
}

void PreloadAbsentService(lua_State* L, const char* module_name) {
  PushAbsentService(L);

  lua_getglobal(L, "package");
  lua_getfield(L, -1, "preload");
  lua_pushvalue(L, -3);
  lua_pushcclosure(
      L,
      [](lua_State* S) {
        lua_pushvalue(S, lua_upvalueindex(1));
        return 1;
      },
      1);
  lua_setfield(L, -2, module_name);
  lua_pop(L, 2);

  SetGlobalPath(L, module_name);
  lua_pop(L, 1);
}

// The script half of a class lives in the module's package as a lower-case
// file: `Xbox.XuiElement` is `xuielement.lub`.
int ScriptLoader(lua_State* L) {
  LuaHost* host = HostOf(L);
  const char* name = luaL_checkstring(L, 1);
  const std::vector<uint8_t> chunk =
      host ? host->FindScript(name) : std::vector<uint8_t>();
  if (chunk.empty()) {
    lua_pushfstring(L, "\n\tno script '%s' in any loaded package", name);
    return 1;
  }
  const std::string label = std::string("@") + name;
  if (luaL_loadbuffer(L, reinterpret_cast<const char*>(chunk.data()),
                      chunk.size(), label.c_str()) != 0) {
    return lua_error(L);
  }
  return 1;
}

}  // namespace

LuaHost::LuaHost() = default;

LuaHost::~LuaHost() { Close(); }

void LuaHost::AddScriptPackage(Package* package) {
  if (package) {
    packages_.push_back(package);
  }
}

void LuaHost::SetSceneRoot(const Scene* scene, Element* root,
                           const std::string_view name) {
  scene_ = scene;
  scene_root_ = root;
  focus_ = nullptr;
  players_.clear();
  player_ = root ? PlayerFor(root) : nullptr;
  input_disabled_.clear();
  handles_.clear();
  handle_of_.clear();
  // Handle 0 is the invalid one the scripts test against.
  handles_.push_back(nullptr);
  if (!name.empty()) {
    RegisterScene(name, scene, root);
  }
}

TimelinePlayer* LuaHost::PlayerFor(Element* element) {
  if (!element || !element->source || !scene_) {
    return nullptr;
  }
  const auto found = players_.find(element);
  if (found != players_.end()) {
    return found->second.get();
  }
  auto player = std::make_unique<TimelinePlayer>(scene_, element->source);
  if (player->states().empty()) {
    // Nothing to play; remember that so the tree is not walked again.
    players_.emplace(element, nullptr);
    return nullptr;
  }
  TimelinePlayer* raw = player.get();
  players_.emplace(element, std::move(player));
  return raw;
}

TimelinePlayer* LuaHost::AttachPlayer(Element* element, const Scene* scene,
                                      const Node* node) {
  if (!element || !scene || !node) {
    return nullptr;
  }
  auto player = std::make_unique<TimelinePlayer>(scene, node);
  if (player->states().empty()) {
    return nullptr;
  }
  TimelinePlayer* raw = player.get();
  players_[element] = std::move(player);
  return raw;
}

// ---------------------------------------------------------------------------
// Focus
// ---------------------------------------------------------------------------

bool LuaHost::IsFocusable(Element* element) const {
  if (!element || !element->visible || !element->source) {
    return false;
  }
  const auto disabled = input_disabled_.find(element);
  if (disabled != input_disabled_.end() && disabled->second) {
    return false;
  }
  // Only a control takes focus; a figure or an image is decoration.
  for (const XuiClass* level : ClassChain(element->class_name)) {
    if (std::strcmp(level->name, "XuiControl") == 0) {
      return true;
    }
  }
  return false;
}

void LuaHost::CollectFocusable(Element* root,
                               std::vector<Element*>* out) const {
  if (!root || !root->visible) {
    return;
  }
  if (IsFocusable(root)) {
    out->push_back(root);
  }
  for (Element& child : root->children) {
    CollectFocusable(&child, out);
  }
}

bool LuaHost::ForceFocus(Element* element) { return SetFocus(element); }

bool LuaHost::SetFocus(Element* element) {
  if (element == focus_) {
    return element != nullptr;
  }
  if (Element* previous = focus_) {
    focus_ = nullptr;
    if (TimelinePlayer* player = PlayerFor(previous)) {
      // KillFocus is the authored way back to Normal; scenes that have no
      // such span simply return to Normal outright.
      if (!player->Play(kStateKillFocus)) {
        player->Play(kStateNormal);
      }
    }
    CallClassHandler(previous, "OnKillFocus");
  }
  focus_ = element;
  if (!element) {
    return false;
  }
  if (TimelinePlayer* player = PlayerFor(element)) {
    player->Play(kStateFocus);
  }
  CallClassHandler(element, "OnSetFocus");
  return true;
}

bool LuaHost::InitFocus() {
  std::vector<Element*> candidates;
  CollectFocusable(scene_root_, &candidates);
  if (candidates.empty()) {
    return false;
  }
  return SetFocus(candidates.front());
}

bool LuaHost::MoveFocus(int dx, int dy) {
  if (!dx && !dy) {
    return false;
  }
  std::vector<Element*> candidates;
  CollectFocusable(scene_root_, &candidates);
  if (candidates.empty()) {
    return false;
  }
  if (!focus_) {
    return SetFocus(candidates.front());
  }

  const float from_x = focus_->rect.x + focus_->rect.width * 0.5f;
  const float from_y = focus_->rect.y + focus_->rect.height * 0.5f;

  // The nearest control that genuinely lies that way: the step along the
  // direction has to beat the drift across it, or a control barely off to one
  // side would win over the one directly ahead.
  Element* best = nullptr;
  float best_cost = 0.0f;
  for (Element* candidate : candidates) {
    if (candidate == focus_) {
      continue;
    }
    const float to_x = candidate->rect.x + candidate->rect.width * 0.5f;
    const float to_y = candidate->rect.y + candidate->rect.height * 0.5f;
    const float along = (to_x - from_x) * dx + (to_y - from_y) * dy;
    if (along <= 0.0f) {
      continue;
    }
    const float across = std::abs((to_x - from_x) * dy - (to_y - from_y) * dx);
    if (across > along) {
      continue;
    }
    const float cost = along + across * 2.0f;
    if (!best || cost < best_cost) {
      best = candidate;
      best_cost = cost;
    }
  }
  if (!best) {
    return false;
  }
  return SetFocus(best);
}

bool LuaHost::TreeHasFocus(Element* root) const {
  if (!root || !focus_) {
    return false;
  }
  if (root == focus_) {
    return true;
  }
  for (Element& child : root->children) {
    if (TreeHasFocus(&child)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void LuaHost::SetInputEnabled(Element* element, bool enabled) {
  if (!element) {
    return;
  }
  input_disabled_[element] = !enabled;
  if (!enabled && focus_ == element) {
    SetFocus(nullptr);
  }
}

bool LuaHost::InputEnabled(Element* element) const {
  const auto found = input_disabled_.find(element);
  return found == input_disabled_.end() || !found->second;
}

bool LuaHost::HandleInput(Button button) {
  switch (button) {
    case Button::kUp:
      return MoveFocus(0, -1);
    case Button::kDown:
      return MoveFocus(0, 1);
    case Button::kLeft:
      return MoveFocus(-1, 0);
    case Button::kRight:
      return MoveFocus(1, 0);
    case Button::kAccept: {
      if (!focus_) {
        return false;
      }
      // The press is an animation as much as an event: the control plays its
      // Press span and then settles back into Focus.
      if (TimelinePlayer* player = PlayerFor(focus_)) {
        player->Play(kStatePress);
      }
      CallClassHandler(focus_, "OnPress");
      return true;
    }
    case Button::kCancel:
      if (focus_ && CallClassHandler(focus_, "OnCancel")) {
        return true;
      }
      return NavigateBack();
  }
  return false;
}

bool LuaHost::CallClassHandler(Element* element, const char* method) {
  if (!state_ || !element) {
    return false;
  }
  // _xuiToLua is where ImplementXuiClass files every class it declares, keyed
  // by the XUI class name the scene uses.
  lua_getglobal(state_, "_xuiToLua");
  if (!lua_istable(state_, -1)) {
    lua_pop(state_, 1);
    return false;
  }
  lua_getfield(state_, -1, element->class_name.c_str());
  if (!lua_istable(state_, -1)) {
    lua_pop(state_, 2);
    return false;
  }
  lua_getfield(state_, -1, method);
  if (!lua_isfunction(state_, -1)) {
    lua_pop(state_, 3);
    return false;
  }
  lua_pushinteger(state_, HandleForElement(element));
  if (lua_pcall(state_, 1, 0, 0) != 0) {
    XELOGW("xui lua: {} on {} raised {}", method, element->class_name,
           lua_tostring(state_, -1) ? lua_tostring(state_, -1) : "an error");
    lua_pop(state_, 1);
    lua_pop(state_, 2);
    return false;
  }
  lua_pop(state_, 2);
  return true;
}

// ---------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------

void LuaHost::RegisterScene(const std::string_view name, const Scene* scene,
                            Element* root) {
  if (name.empty() || !root) {
    return;
  }
  SceneEntry entry;
  entry.name.assign(name);
  entry.scene = scene;
  entry.root = root;
  known_scenes_[entry.name] = entry;
}

bool LuaHost::NavigateForward(const std::string_view name) {
  const auto found = known_scenes_.find(std::string(name));
  if (found == known_scenes_.end()) {
    XELOGW("xui lua: no scene registered as {}", name);
    return false;
  }
  // The scene being left plays itself out, and becomes what back returns to.
  if (scene_root_) {
    if (TimelinePlayer* player = PlayerFor(scene_root_)) {
      player->Play("FadeOut");
    }
    SceneEntry current;
    current.scene = scene_;
    current.root = scene_root_;
    scene_stack_.push_back(current);
    back_scene_ = scene_root_;
  }
  SetSceneRoot(found->second.scene, found->second.root, found->second.name);
  if (TimelinePlayer* player = PlayerFor(scene_root_)) {
    player->Play("FadeIn");
  }
  InitFocus();
  return true;
}

bool LuaHost::NavigateBack() {
  if (scene_stack_.empty()) {
    return false;
  }
  const SceneEntry previous = scene_stack_.back();
  scene_stack_.pop_back();
  if (scene_root_) {
    if (TimelinePlayer* player = PlayerFor(scene_root_)) {
      player->Play("FadeOut");
    }
  }
  SetSceneRoot(previous.scene, previous.root, previous.name);
  back_scene_ = scene_stack_.empty() ? nullptr : scene_stack_.back().root;
  if (TimelinePlayer* player = PlayerFor(scene_root_)) {
    player->Play("FadeIn");
  }
  InitFocus();
  return true;
}

bool LuaHost::NavigateFirst(const std::string_view name) {
  scene_stack_.clear();
  back_scene_ = nullptr;
  const auto found = known_scenes_.find(std::string(name));
  if (found == known_scenes_.end()) {
    return false;
  }
  SetSceneRoot(found->second.scene, found->second.root, found->second.name);
  if (TimelinePlayer* player = PlayerFor(scene_root_)) {
    player->Play("FadeIn");
  }
  InitFocus();
  return true;
}

Element* LuaHost::back_scene() const { return back_scene_; }

void LuaHost::SetBackScene(Element* root) { back_scene_ = root; }

std::vector<uint8_t> LuaHost::FindScript(
    const std::string_view module_name) const {
  // The dashboard's own modules are files named exactly as they are required,
  // dots and capitals and all - `App.Main` is App.Main.lub. Only the XUI class
  // library is renamed: `Xbox.XuiBase` is xuibase.lub.
  std::vector<std::string> candidates;
  candidates.push_back(std::string(module_name) + ".lub");
  std::string leaf(module_name);
  if (leaf.compare(0, std::strlen(kModulePrefix), kModulePrefix) == 0) {
    leaf = leaf.substr(std::strlen(kModulePrefix));
    candidates.push_back(leaf + ".lub");
  }
  candidates.push_back(xe::utf8::lower_ascii(leaf) + ".lub");

  for (const std::string& file : candidates) {
    for (Package* package : packages_) {
      if (const PackageEntry* entry = package->Find(file)) {
        return std::vector<uint8_t>(entry->data, entry->data + entry->size);
      }
    }
  }
  return {};
}

// A kind-0 .xus is pairs of NUL-terminated strings with the TEXT FIRST and
// its IDS_* key second, which is the order that catches people out.
void LuaHost::AddStringTable(const std::string_view file) {
  const std::string name(file);
  const PackageEntry* entry = nullptr;
  for (Package* package : packages_) {
    if ((entry = package->Find(name)) != nullptr) {
      break;
    }
  }
  if (!entry || entry->size < kStringTableHeader) {
    return;
  }
  const char* at =
      reinterpret_cast<const char*>(entry->data) + kStringTableHeader;
  const char* end = reinterpret_cast<const char*>(entry->data) + entry->size;
  while (at < end) {
    const std::string text(at);
    at += text.size() + 1;
    if (at >= end) {
      break;
    }
    const std::string key(at);
    at += key.size() + 1;
    if (!key.empty()) {
      strings_[key] = text;
    }
  }
}

std::string LuaHost::LookupString(const std::string_view key) const {
  const auto found = strings_.find(std::string(key));
  return found == strings_.end() ? std::string() : found->second;
}

bool LuaHost::SetVisualPrefix(Element* element, const std::string_view prefix) {
  if (!element || !skin_ || !skin_->loaded()) {
    return false;
  }
  // The control names the visual it wants; the prefix asks the skin for the
  // same one under another name, and the control keeps what it had when the
  // skin has no such entry.
  std::string base = element->source
                         ? StringProperty(*element->source, "Visual")
                         : std::string();
  if (base.empty()) {
    base = element->class_name;
  }
  const std::string wanted = std::string(prefix) + base;
  const Node* visual = skin_->Find(wanted);
  if (!visual) {
    return false;
  }
  element->visual = visual;
  visual_prefix_[element].assign(prefix);
  return true;
}

std::string LuaHost::VisualPrefix(Element* element) const {
  const auto found = visual_prefix_.find(element);
  return found == visual_prefix_.end() ? std::string() : found->second;
}

bool LuaHost::RaiseEvent(Element* element, const std::string_view name) {
  // The dashboard's convention: a handler for the event Foo is the method
  // OnFoo on the control's class.
  const std::string method = "On" + std::string(name);
  return CallClassHandler(element, method.c_str());
}

Element* LuaHost::ElementFromHandle(uint32_t handle) const {
  return handle && handle < handles_.size() ? handles_[handle] : nullptr;
}

uint32_t LuaHost::HandleForElement(Element* element) {
  if (!element) {
    return 0;
  }
  const auto found = handle_of_.find(element);
  if (found != handle_of_.end()) {
    return found->second;
  }
  const auto handle = static_cast<uint32_t>(handles_.size());
  handles_.push_back(element);
  handle_of_.emplace(element, handle);
  return handle;
}

void LuaHost::InstallLoader() {
  lua_getglobal(state_, "package");
  lua_getfield(state_, -1, "loaders");
  // Ahead of the file system loader, which cannot see inside a package.
  const int count = static_cast<int>(lua_objlen(state_, -1));
  for (int i = count; i >= 1; --i) {
    lua_rawgeti(state_, -1, i);
    lua_rawseti(state_, -2, i + 1);
  }
  lua_pushcfunction(state_, ScriptLoader);
  lua_rawseti(state_, -2, 1);
  lua_pop(state_, 2);
}

void LuaHost::RegisterNatives() {
  PreloadTable(state_, "Xbox.XuiBaseImpl", kBaseImpl);
  PreloadTable(state_, "Xbox.XuiElementImpl", kElementImpl);
  PreloadTable(state_, "Xbox.XuiSceneImpl", kSceneImpl);
  PreloadTable(state_, "Xbox.System", kSystem);
  PreloadTable(state_, "Xbox.Text", kText);
  PreloadTable(state_, "Xbox.Scheduler", kScheduler);
  PreloadTable(state_, "Xbox.XuiStringTableImpl", kStringTableImpl);
  // Every other class's native half is the element surface until its own
  // behaviour is bound; the class library only needs the table to exist.
  static const char* kSharedImpls[] = {
      "Xbox.XuiControlImpl",        "Xbox.XuiFigureImpl",
      "Xbox.XuiImageImpl",          "Xbox.XuiTextImpl",
      "Xbox.XuiEditImpl",           "Xbox.XuiListImpl",
      "Xbox.XuiCheckboxImpl",       "Xbox.XuiNavButtonImpl",
      "Xbox.XuiProgressBarImpl",    "Xbox.XuiRadioGroupImpl",
      "Xbox.XuiSliderImpl",         "Xbox.XuiSoundImpl",
      "Xbox.XuiTabSceneImpl",       "Xbox.XuiVideoImpl",
      "Xbox.XuiHubSceneImpl",       "Xbox.XuiColumnSceneImpl",
      "Xbox.XuiRowSceneImpl",       "Xbox.XuiAuraControlImpl",
      "Xbox.XuiHorizonControlImpl", "Xbox.XuiHtmlControlImpl",
      "Xbox.XuiTwistImpl",          "Xbox.XuiAvatarImpl",
  };
  for (const char* name : kSharedImpls) {
    PreloadTable(state_, name, kElementImpl);
  }

  // Everything the dashboard talks to that is not the UI. A scene reaches for
  // these whether or not there is anything behind them, so they exist and
  // answer nothing.
  static const char* kAbsentServices[] = {
      "Xbox.App",          "Xbox.AppUtils",    "Xbox.Avatar",
      "Xbox.Content",      "Xbox.Dash",        "Xbox.Dash.Nui",
      "Xbox.Dashboard",    "Xbox.DashEvents",  "Xbox.Events",
      "Xbox.Friends",      "Xbox.Marketplace", "Xbox.MPJson",
      "Xbox.MPXml",        "Xbox.Net",         "Xbox.Notification",
      "Xbox.PamDash",      "Xbox.Profile",     "Xbox.ProfileImpl",
      "Xbox.SystemEvents", "Xbox.User",        "Xbox.UserImpl",
      "Xbox.UserPins",     "Xbox.Vui",         "Xbox.WebService",
      "Xbox.Xml",          "Xbox.XmlParser",   "Xbox.XSTSV3Token",
      "Xbox.XuiDataCache", "Xbox.XuiEvents",   "Xbox.ConsoleImpl",
  };
  for (const char* name : kAbsentServices) {
    PreloadAbsentService(state_, name);
  }

  // After the absent services, so this lands ON Xbox.Dash rather than being
  // replaced by the stub that stands in for the rest of it.
  PreloadTable(state_, "Xbox.Dash.GameLibrary", kGameLibrary);
}

bool LuaHost::Open() {
  Close();
  state_ = luaL_newstate();
  if (!state_) {
    last_error_ = "could not create a Lua state";
    return false;
  }
  luaL_openlibs(state_);

  lua_pushlightuserdata(state_, this);
  lua_setfield(state_, LUA_REGISTRYINDEX, kHostKey);

  InstallLoader();
  RegisterNatives();

  if (luaL_dostring(state_, kBootstrap) != 0) {
    last_error_ = lua_tostring(state_, -1) ? lua_tostring(state_, -1)
                                           : "the bootstrap raised an error";
    XELOGE("xui lua: bootstrap failed: {}", last_error_);
    lua_pop(state_, 1);
    Close();
    return false;
  }

  if (handles_.empty()) {
    handles_.push_back(nullptr);
  }
  return true;
}

void LuaHost::Close() {
  if (!state_) {
    return;
  }
  lua_close(state_);
  state_ = nullptr;
  timers_.clear();
}

bool LuaHost::RunModule(const std::string_view name) {
  if (!state_) {
    last_error_ = "no Lua state";
    return false;
  }
  const std::vector<uint8_t> chunk = FindScript(name);
  if (chunk.empty()) {
    last_error_ = "no script named " + std::string(name);
    return false;
  }
  const std::string label = std::string("@") + std::string(name);
  if (luaL_loadbuffer(state_, reinterpret_cast<const char*>(chunk.data()),
                      chunk.size(), label.c_str()) != 0) {
    last_error_ = lua_tostring(state_, -1) ? lua_tostring(state_, -1)
                                           : "could not load the chunk";
    lua_pop(state_, 1);
    XELOGE("xui lua: {}", last_error_);
    return false;
  }
  if (lua_pcall(state_, 0, 0, 0) != 0) {
    last_error_ = lua_tostring(state_, -1) ? lua_tostring(state_, -1)
                                           : "the chunk raised an error";
    lua_pop(state_, 1);
    XELOGE("xui lua: {} stopped at {}", name, last_error_);
    return false;
  }
  XELOGI("xui lua: {} ran", name);
  return true;
}

uint32_t LuaHost::Tick(float seconds) {
  // Every playhead moves whether or not a script is waiting on anything: a
  // control that is mid-Focus has to keep animating.
  for (auto& entry : players_) {
    if (entry.second) {
      entry.second->Advance(seconds);
    }
  }
  // A control whose Press has finished settles back onto whether it is still
  // the focused one.
  if (focus_) {
    if (TimelinePlayer* player = PlayerFor(focus_)) {
      if (!player->playing() && player->state() == kStatePress) {
        player->Play(kStateFocus);
      }
    }
  }
  if (!state_ || timers_.empty()) {
    return 0;
  }
  uint32_t fired = 0;
  for (size_t i = 0; i < timers_.size(); ++i) {
    Timer& timer = timers_[i];
    if (timer.dead) {
      continue;
    }
    timer.remaining -= seconds;
    if (timer.remaining > 0.0f) {
      continue;
    }
    lua_rawgeti(state_, LUA_REGISTRYINDEX, timer.callback_ref);
    if (lua_pcall(state_, 0, 0, 0) != 0) {
      XELOGW("xui lua: timer {} raised {}", timer.id,
             lua_tostring(state_, -1) ? lua_tostring(state_, -1) : "an error");
      lua_pop(state_, 1);
      timer.dead = true;
    }
    ++fired;
    if (timer.repeating && !timer.dead) {
      timer.remaining += timer.interval > 0.0f ? timer.interval : 1.0f;
    } else {
      timer.dead = true;
    }
  }
  for (Timer& timer : timers_) {
    if (timer.dead && timer.callback_ref) {
      luaL_unref(state_, LUA_REGISTRYINDEX, timer.callback_ref);
      timer.callback_ref = 0;
    }
  }
  timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                               [](const Timer& timer) { return timer.dead; }),
                timers_.end());
  return fired;
}

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
