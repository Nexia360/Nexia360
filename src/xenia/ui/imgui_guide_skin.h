/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_GUIDE_SKIN_H_
#define XENIA_UI_IMGUI_GUIDE_SKIN_H_

#include <iterator>

#include "third_party/imgui/imgui.h"

namespace xe {
namespace ui {

// The console guide's look: pale grey panels with dark text, slate blades, and
// the green bar on whatever is selected. The guide draws itself out of these,
// and every dialog is drawn inside PushGuideSkin so each piece the guide opens
// matches it.
namespace guide_skin {

inline constexpr ImU32 kPanelTop = IM_COL32(244, 245, 246, 250);
inline constexpr ImU32 kPanelBottom = IM_COL32(204, 207, 211, 250);
inline constexpr ImU32 kRowLine = IM_COL32(176, 180, 185, 255);
inline constexpr ImU32 kSelected = IM_COL32(46, 160, 20, 255);
inline constexpr ImU32 kSelectedText = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 kText = IM_COL32(38, 40, 44, 255);
inline constexpr ImU32 kBlade = IM_COL32(62, 76, 90, 250);
inline constexpr ImU32 kBladeEdge = IM_COL32(40, 50, 60, 255);
inline constexpr ImU32 kBladeText = IM_COL32(214, 220, 226, 255);
inline constexpr ImU32 kActiveBlade = IM_COL32(196, 199, 204, 250);
inline constexpr ImU32 kActiveBladeText = IM_COL32(80, 84, 90, 255);
inline constexpr ImU32 kHeaderText = IM_COL32(240, 242, 244, 255);
inline constexpr ImU32 kShade = IM_COL32(0, 0, 0, 120);

inline constexpr ImU32 kButtonA = IM_COL32(96, 176, 40, 255);
inline constexpr ImU32 kButtonB = IM_COL32(196, 40, 32, 255);
inline constexpr ImU32 kButtonX = IM_COL32(32, 96, 200, 255);
inline constexpr ImU32 kButtonY = IM_COL32(236, 160, 20, 255);

}  // namespace guide_skin

struct GuideSkinColor {
  ImGuiCol index;
  ImVec4 color;
};

inline constexpr GuideSkinColor kGuideSkinColors[] = {
    {ImGuiCol_Text, ImVec4(0.15f, 0.16f, 0.17f, 1.00f)},
    {ImGuiCol_TextDisabled, ImVec4(0.42f, 0.44f, 0.46f, 1.00f)},
    {ImGuiCol_WindowBg, ImVec4(0.88f, 0.89f, 0.90f, 0.98f)},
    {ImGuiCol_ChildBg, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
    {ImGuiCol_PopupBg, ImVec4(0.91f, 0.92f, 0.93f, 0.99f)},
    {ImGuiCol_Border, ImVec4(0.55f, 0.58f, 0.61f, 1.00f)},
    {ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f)},
    {ImGuiCol_FrameBg, ImVec4(0.97f, 0.97f, 0.98f, 1.00f)},
    {ImGuiCol_FrameBgHovered, ImVec4(0.84f, 0.93f, 0.80f, 1.00f)},
    {ImGuiCol_FrameBgActive, ImVec4(0.76f, 0.90f, 0.70f, 1.00f)},
    {ImGuiCol_TitleBg, ImVec4(0.74f, 0.76f, 0.79f, 1.00f)},
    {ImGuiCol_TitleBgActive, ImVec4(0.62f, 0.80f, 0.53f, 1.00f)},
    {ImGuiCol_TitleBgCollapsed, ImVec4(0.74f, 0.76f, 0.79f, 1.00f)},
    {ImGuiCol_MenuBarBg, ImVec4(0.80f, 0.82f, 0.84f, 1.00f)},
    {ImGuiCol_ScrollbarBg, ImVec4(0.80f, 0.82f, 0.84f, 1.00f)},
    {ImGuiCol_ScrollbarGrab, ImVec4(0.55f, 0.58f, 0.61f, 1.00f)},
    {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.18f, 0.63f, 0.08f, 1.00f)},
    {ImGuiCol_ScrollbarGrabActive, ImVec4(0.15f, 0.55f, 0.06f, 1.00f)},
    {ImGuiCol_CheckMark, ImVec4(0.12f, 0.50f, 0.05f, 1.00f)},
    {ImGuiCol_SliderGrab, ImVec4(0.18f, 0.63f, 0.08f, 1.00f)},
    {ImGuiCol_SliderGrabActive, ImVec4(0.15f, 0.55f, 0.06f, 1.00f)},
    {ImGuiCol_Button, ImVec4(0.80f, 0.82f, 0.84f, 1.00f)},
    {ImGuiCol_ButtonHovered, ImVec4(0.56f, 0.80f, 0.44f, 1.00f)},
    {ImGuiCol_ButtonActive, ImVec4(0.45f, 0.74f, 0.32f, 1.00f)},
    {ImGuiCol_Header, ImVec4(0.62f, 0.82f, 0.52f, 1.00f)},
    {ImGuiCol_HeaderHovered, ImVec4(0.56f, 0.80f, 0.44f, 1.00f)},
    {ImGuiCol_HeaderActive, ImVec4(0.45f, 0.74f, 0.32f, 1.00f)},
    {ImGuiCol_Separator, ImVec4(0.69f, 0.71f, 0.73f, 1.00f)},
    {ImGuiCol_SeparatorHovered, ImVec4(0.18f, 0.63f, 0.08f, 1.00f)},
    {ImGuiCol_SeparatorActive, ImVec4(0.15f, 0.55f, 0.06f, 1.00f)},
    {ImGuiCol_ResizeGrip, ImVec4(0.55f, 0.58f, 0.61f, 0.40f)},
    {ImGuiCol_ResizeGripHovered, ImVec4(0.18f, 0.63f, 0.08f, 0.70f)},
    {ImGuiCol_ResizeGripActive, ImVec4(0.15f, 0.55f, 0.06f, 0.90f)},
    {ImGuiCol_Tab, ImVec4(0.74f, 0.76f, 0.79f, 1.00f)},
    {ImGuiCol_TabHovered, ImVec4(0.56f, 0.80f, 0.44f, 1.00f)},
    {ImGuiCol_TabSelected, ImVec4(0.62f, 0.82f, 0.52f, 1.00f)},
    {ImGuiCol_TabDimmed, ImVec4(0.80f, 0.82f, 0.84f, 1.00f)},
    {ImGuiCol_TabDimmedSelected, ImVec4(0.70f, 0.80f, 0.66f, 1.00f)},
    {ImGuiCol_TableHeaderBg, ImVec4(0.78f, 0.80f, 0.83f, 1.00f)},
    {ImGuiCol_TableBorderStrong, ImVec4(0.55f, 0.58f, 0.61f, 1.00f)},
    {ImGuiCol_TableBorderLight, ImVec4(0.69f, 0.71f, 0.73f, 1.00f)},
    {ImGuiCol_TableRowBgAlt, ImVec4(0.00f, 0.00f, 0.00f, 0.04f)},
    {ImGuiCol_TextSelectedBg, ImVec4(0.18f, 0.63f, 0.08f, 0.35f)},
    {ImGuiCol_NavCursor, ImVec4(0.18f, 0.63f, 0.08f, 1.00f)},
    {ImGuiCol_ModalWindowDimBg, ImVec4(0.00f, 0.00f, 0.00f, 0.45f)},
};

inline constexpr int kGuideSkinStyleVars = 4;

inline void PushGuideSkin() {
  for (const GuideSkinColor& entry : kGuideSkinColors) {
    ImGui::PushStyleColor(entry.index, entry.color);
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
}

inline void PopGuideSkin() {
  ImGui::PopStyleVar(kGuideSkinStyleVars);
  ImGui::PopStyleColor(static_cast<int>(std::size(kGuideSkinColors)));
}

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IMGUI_GUIDE_SKIN_H_
