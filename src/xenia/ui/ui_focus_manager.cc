/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ui_focus_manager.h"

namespace xe {
namespace ui {

UIFocusManager::UIFocusManager() {}

UIFocusManager::~UIFocusManager() {}

void UIFocusManager::UpdateInput(
    bool back_pressed, bool b_pressed, bool a_pressed, bool start_pressed,
    bool x_pressed, bool y_pressed, bool lb_pressed, bool rb_pressed,
    bool dpad_up, bool dpad_down, bool dpad_left, bool dpad_right,
    bool lstick_up, bool lstick_down, bool lstick_left, bool lstick_right) {
  // Detect releases (was pressed, now not pressed)
  current_input_.a_released = prev_a_ && !a_pressed;
  current_input_.b_released = prev_b_ && !b_pressed;
  current_input_.x_released = prev_x_ && !x_pressed;
  current_input_.y_released = prev_y_ && !y_pressed;
  current_input_.back_released = prev_back_ && !back_pressed;
  current_input_.start_released = prev_start_ && !start_pressed;

  // Store current pressed state
  current_input_.a_pressed = a_pressed;
  current_input_.b_pressed = b_pressed;
  current_input_.x_pressed = x_pressed;
  current_input_.y_pressed = y_pressed;
  current_input_.back_pressed = back_pressed;
  current_input_.start_pressed = start_pressed;
  current_input_.lb_pressed = lb_pressed;
  current_input_.rb_pressed = rb_pressed;

  // D-pad: detect new presses (not held)
  current_input_.dpad_up_pressed = dpad_up && !prev_dpad_up_;
  current_input_.dpad_down_pressed = dpad_down && !prev_dpad_down_;
  current_input_.dpad_left_pressed = dpad_left && !prev_dpad_left_;
  current_input_.dpad_right_pressed = dpad_right && !prev_dpad_right_;

  // Left stick: detect new presses (not held)
  current_input_.lstick_up_pressed = lstick_up && !prev_lstick_up_;
  current_input_.lstick_down_pressed = lstick_down && !prev_lstick_down_;
  current_input_.lstick_left_pressed = lstick_left && !prev_lstick_left_;
  current_input_.lstick_right_pressed = lstick_right && !prev_lstick_right_;

  // Save for next frame
  prev_a_ = a_pressed;
  prev_b_ = b_pressed;
  prev_x_ = x_pressed;
  prev_y_ = y_pressed;
  prev_back_ = back_pressed;
  prev_start_ = start_pressed;
  prev_lb_ = lb_pressed;
  prev_rb_ = rb_pressed;
  prev_dpad_up_ = dpad_up;
  prev_dpad_down_ = dpad_down;
  prev_dpad_left_ = dpad_left;
  prev_dpad_right_ = dpad_right;
  prev_lstick_up_ = lstick_up;
  prev_lstick_down_ = lstick_down;
  prev_lstick_left_ = lstick_left;
  prev_lstick_right_ = lstick_right;
}

void UIFocusManager::UISetFocus(const std::string& name) {
  if (name.empty()) return;

  // If already exists, just rebuild focus path
  auto it = nodes_.find(name);
  if (it != nodes_.end()) {
    RebuildFocusPath();
    return;
  }

  // Start input cooldown - block input for 500ms
  focus_change_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();

  // If there's already a focus tree, this new dialog becomes a child of the
  // deepest focused dialog This allows game dialogs to open on top of user
  // dialogs
  if (!focus_path_.empty()) {
    const std::string& current_focus = focus_path_.back();
    UIChildFocus(current_focus, name);
    return;
  }

  // No existing focus - create new root node
  FocusNode node;
  node.name = name;
  node.parent = "";  // Root has no parent
  node.child = "";
  nodes_[name] = node;

  RebuildFocusPath();
}

void UIFocusManager::UIChildFocus(const std::string& parent,
                                  const std::string& child) {
  if (parent.empty() || child.empty()) return;

  // Start input cooldown - block input for 500ms
  focus_change_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();

  // Parent must exist
  auto parent_it = nodes_.find(parent);
  if (parent_it == nodes_.end()) {
    // Parent doesn't exist - create it as root first
    UISetFocus(parent);
    parent_it = nodes_.find(parent);
    if (parent_it == nodes_.end()) return;
  }

  // If child already exists with same parent, just rebuild
  auto child_it = nodes_.find(child);
  if (child_it != nodes_.end()) {
    if (child_it->second.parent == parent) {
      // Already set up correctly
      parent_it->second.child = child;
      RebuildFocusPath();
      return;
    }
    // Child exists with different parent - remove old first
    UIDropFocus(child);
  }

  // Create child node
  FocusNode node;
  node.name = child;
  node.parent = parent;
  node.child = "";
  nodes_[child] = node;

  // Link parent to child
  parent_it->second.child = child;

  RebuildFocusPath();
}

void UIFocusManager::UIDropFocus(const std::string& name) {
  if (name.empty()) return;

  auto it = nodes_.find(name);
  if (it == nodes_.end()) return;

  // Recursively drop all children first
  if (!it->second.child.empty()) {
    UIDropFocus(it->second.child);
  }

  // Clear parent's child reference
  if (!it->second.parent.empty()) {
    auto parent_it = nodes_.find(it->second.parent);
    if (parent_it != nodes_.end() && parent_it->second.child == name) {
      parent_it->second.child = "";
    }
  }

  // Remove this node
  nodes_.erase(it);

  RebuildFocusPath();
}

bool UIFocusManager::IsFocused(const std::string& name) const {
  if (focus_path_.empty()) return false;
  return focus_path_.back() == name;
}

bool UIFocusManager::IsInputCoolingDown() const {
  if (focus_change_time_ == 0) return false;

  uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();

  return (now - focus_change_time_) < kInputCooldownMs;
}

const UIInput& UIFocusManager::GetInput(const std::string& name) const {
  // Block all input during cooldown period after focus change
  if (IsInputCoolingDown()) {
    return kNoInput;
  }

  if (IsFocused(name)) {
    return current_input_;
  }
  return kNoInput;
}

const std::string& UIFocusManager::GetFocusedDialog() const {
  if (focus_path_.empty()) {
    return empty_string_;
  }
  return focus_path_.back();
}

bool UIFocusManager::HasAnyFocus() const { return !focus_path_.empty(); }

const std::string& UIFocusManager::GetParent(const std::string& name) const {
  auto it = nodes_.find(name);
  if (it == nodes_.end()) {
    return empty_string_;
  }
  return it->second.parent;
}

void UIFocusManager::RebuildFocusPath() {
  focus_path_.clear();

  // Find a root node (node with no parent)
  std::string current;
  for (const auto& pair : nodes_) {
    if (pair.second.parent.empty()) {
      current = pair.first;
      break;
    }
  }

  // Walk down from root to deepest child
  while (!current.empty()) {
    focus_path_.push_back(current);
    auto it = nodes_.find(current);
    if (it == nodes_.end()) break;
    current = it->second.child;
  }
}

}  // namespace ui
}  // namespace xe
