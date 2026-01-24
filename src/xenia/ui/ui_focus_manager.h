/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_UI_FOCUS_MANAGER_H_
#define XENIA_UI_UI_FOCUS_MANAGER_H_

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace xe {
namespace ui {

/**
 * UIFocusManager - Parent-child focus model for UI dialogs
 * =========================================================
 * 
 * OVERVIEW:
 * Manages focus for UI dialogs using a parent-child tree structure.
 * Only the deepest focused dialog receives input. When a parent closes,
 * all its children are automatically dropped from focus.
 * 
 * KEY CONCEPTS:
 * 
 * 1. FOCUS TREE (not stack)
 *    Each dialog can have children. Focus goes to the deepest child.
 *    Closing a parent automatically unfocuses all descendants.
 * 
 * 2. THREE OPERATIONS:
 *    - UISetFocus("Name")              - Root dialog takes focus
 *    - UIChildFocus("Parent", "Child") - Child takes focus under parent
 *    - UIDropFocus("Name")             - Dialog and all children lose focus
 * 
 * 3. AUTOMATIC CLEANUP
 *    When a parent is dropped, all children are dropped too.
 *    No manual cleanup needed for nested dialogs.
 * 
 * 
 * USAGE:
 * ======
 * 
 * Root dialog (e.g., ProfileMenu):
 *   // On open:
 *   focus_manager->UISetFocus("ProfileMenu");
 *   
 *   // On close:
 *   focus_manager->UIDropFocus("ProfileMenu");
 * 
 * Child dialog (e.g., CreateProfile opened from ProfileMenu):
 *   // On open:
 *   focus_manager->UIChildFocus("ProfileMenu", "CreateProfile");
 *   
 *   // On close:
 *   focus_manager->UIDropFocus("CreateProfile");
 * 
 * Grandchild (e.g., Keyboard opened from CreateProfile):
 *   // On open:
 *   focus_manager->UIChildFocus("CreateProfile", "Keyboard");
 *   
 *   // On close:
 *   focus_manager->UIDropFocus("Keyboard");
 *   // CreateProfile regains focus automatically
 * 
 * If CreateProfile closes while Keyboard is open:
 *   focus_manager->UIDropFocus("CreateProfile");
 *   // Keyboard is automatically dropped too
 *   // ProfileMenu regains focus
 */

// Gamepad input state for a focused dialog
struct UIInput {
  bool a_pressed = false;
  bool b_pressed = false;
  bool x_pressed = false;
  bool y_pressed = false;
  bool back_pressed = false;
  bool start_pressed = false;
  
  bool a_released = false;
  bool b_released = false;
  bool x_released = false;
  bool y_released = false;
  bool back_released = false;
  bool start_released = false;
  
  // Returns true if any action button is currently pressed
  bool AnyPressed() const {
    return a_pressed || b_pressed || x_pressed || y_pressed || back_pressed || start_pressed;
  }
  
  // Returns true if A was just released (use for button activation)
  bool Activated() const { return a_released; }
  
  // Returns true if Back was just released
  bool BackClose() const { return back_released; }
  
  // Returns true if B was just released
  bool BClose() const { return b_released; }
  
  // Returns true if Back OR B was just released (standard close behavior)
  bool ShouldClose() const { return back_released || b_released; }
};

// Empty input - returned when dialog doesn't have focus
inline constexpr UIInput kNoInput = {};

/**
 * Central manager for UI focus using parent-child relationships.
 */
class UIFocusManager {
 public:
  UIFocusManager();
  ~UIFocusManager();

  /**
   * Update raw gamepad input state.
   * Called once per frame from ImGuiDrawer.
   */
  void UpdateInput(bool back_pressed, bool b_pressed, 
                   bool a_pressed, bool start_pressed,
                   bool x_pressed = false, bool y_pressed = false);

  /**
   * Set focus to a root dialog (no parent).
   * If the dialog already exists, it becomes the new focus.
   */
  void UISetFocus(const std::string& name);
  
  /**
   * Set focus to a child dialog under a parent.
   * Parent must already be registered.
   * Child becomes the new focused dialog.
   */
  void UIChildFocus(const std::string& parent, const std::string& child);
  
  /**
   * Drop focus from a dialog and all its children.
   * Focus returns to the parent (if any).
   */
  void UIDropFocus(const std::string& name);
  
  /**
   * Check if a dialog currently has focus (is the deepest in the tree).
   */
  bool IsFocused(const std::string& name) const;
  
  /**
   * Get input for a dialog. Returns kNoInput if not focused.
   */
  const UIInput& GetInput(const std::string& name) const;
  
  /**
   * Convenience alias for GetInput().
   */
  const UIInput& XamInputFocus(const std::string& name) const {
    return GetInput(name);
  }
  
  /**
   * Get the currently focused dialog name.
   */
  const std::string& GetFocusedDialog() const;
  
  /**
   * Check if any dialog has focus.
   */
  bool HasAnyFocus() const;
  
  /**
   * Get the parent of a dialog (empty string if root or not found).
   */
  const std::string& GetParent(const std::string& name) const;

  // Legacy API - deprecated, use new API
  bool DrawerStackAdd(size_t tree_id, const std::string& name) {
    (void)tree_id;
    UISetFocus(name);
    return true;
  }
  bool DrawerStackPop(const std::string& name) {
    UIDropFocus(name);
    return true;
  }
  size_t GetStackDepth(size_t tree_id) const {
    (void)tree_id;
    return focus_path_.size();
  }

 private:
  struct FocusNode {
    std::string name;
    std::string parent;  // Empty for root
    std::string child;   // Current active child (empty if none)
  };
  
  // All registered dialogs
  std::unordered_map<std::string, FocusNode> nodes_;
  
  // Current focus path from root to deepest child
  std::vector<std::string> focus_path_;
  
  // Current input state
  UIInput current_input_;
  
  // Previous frame state for release detection
  bool prev_a_ = false;
  bool prev_b_ = false;
  bool prev_x_ = false;
  bool prev_y_ = false;
  bool prev_back_ = false;
  bool prev_start_ = false;
  
  // Input cooldown - block input for 500ms after focus changes
  static constexpr uint64_t kInputCooldownMs = 500;
  uint64_t focus_change_time_ = 0;  // Timestamp when focus last changed
  bool IsInputCoolingDown() const;
  
  // Empty string for returning references
  std::string empty_string_;
  
  // Helper: Rebuild focus path from root to deepest child
  void RebuildFocusPath();
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_UI_FOCUS_MANAGER_H_
