/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/guide_dialog.h"

#include <algorithm>
#include <cfloat>
#include <ctime>
#include <utility>

#include "xenia/app/emulator_window.h"
#include "xenia/base/platform.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/ui/imgui_guide_skin.h"
#include "xenia/ui/ui_focus_manager.h"

namespace xe {
namespace app {

namespace {

constexpr char kFocusName[] = "GuideDialog";

// Rows on the panel at once; a longer blade scrolls under the selection.
constexpr size_t kVisibleRows = 7;

namespace skin = ui::guide_skin;

// Text running top to bottom, as on a blade. Drawn level and then turned a
// quarter clockwise about its start, so `top_center` is where the first glyph
// lands. The fine clip rect is unbounded because the level text is only an
// intermediate - culling it against the window would drop glyphs that end up
// inside the blade once turned.
void AddVerticalText(ImDrawList* draw_list, ImFont* font, float size,
                     ImVec2 top_center, ImU32 color, const char* text) {
  const ImVec4 unbounded(-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX);
  const ImVec2 pivot(top_center.x + size * 0.5f, top_center.y);
  const int first_vertex = draw_list->VtxBuffer.Size;
  draw_list->AddText(font, size, pivot, color, text, nullptr, 0.0f, &unbounded);
  for (int i = first_vertex; i < draw_list->VtxBuffer.Size; ++i) {
    ImVec2& pos = draw_list->VtxBuffer[i].pos;
    const float dx = pos.x - pivot.x;
    const float dy = pos.y - pivot.y;
    pos = ImVec2(pivot.x - dy, pivot.y + dx);
  }
}

std::string ClockText() {
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
#if XE_PLATFORM_WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char text[16] = {};
  std::strftime(text, sizeof(text), "%I:%M %p", &local);
  // "09:05 AM" reads "9:05 AM" on the console.
  return text[0] == '0' ? std::string(text + 1) : std::string(text);
}

// A face button in the legend: the coloured disc with its letter.
void AddFaceButton(ImDrawList* draw_list, ImFont* font, float size,
                   ImVec2 center, ImU32 color, const char* letter) {
  const float radius = size * 0.62f;
  draw_list->AddCircleFilled(center, radius, color, 24);
  draw_list->AddCircle(center, radius, IM_COL32(255, 255, 255, 160), 24, 1.0f);
  const ImVec2 letter_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, letter);
  draw_list->AddText(
      font, size,
      ImVec2(center.x - letter_size.x * 0.5f, center.y - letter_size.y * 0.5f),
      IM_COL32(255, 255, 255, 255), letter);
}

}  // namespace

GuideDialog::GuideDialog(ui::ImGuiDrawer* imgui_drawer,
                         EmulatorWindow* emulator_window,
                         std::vector<GuideBlade> blades, size_t open_blade,
                         std::function<void()> leave_game)
    : ui::ImGuiDialog(imgui_drawer),
      emulator_window_(emulator_window),
      blades_(std::move(blades)),
      leave_game_(std::move(leave_game)) {
  if (!blades_.empty()) {
    blade_ = std::min(open_blade, blades_.size() - 1);
  }
}

GuideDialog::~GuideDialog() {
  // Back to the drawer rather than out with this object: the paint that
  // deleted this dialog still has Image commands holding the pointer.
  if (gamerpic_) {
    imgui_drawer()->ReleaseTextureDeferred(std::move(gamerpic_));
  }
  if (closed_callback_) {
    closed_callback_();
  }
}

ui::ImmediateTexture* GuideDialog::Gamerpic() {
  if (gamerpic_loaded_) {
    return gamerpic_.get();
  }
  gamerpic_loaded_ = true;
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state || !kernel_state->xam_state()) {
    return nullptr;
  }
  auto* profile_manager = kernel_state->xam_state()->profile_manager();
  if (!profile_manager) {
    return nullptr;
  }
  auto* profile = profile_manager->GetProfile(static_cast<uint8_t>(0));
  if (!profile) {
    return nullptr;
  }
  const auto icon = profile->GetProfileIcon(kernel::xam::XTileType::kGamerTile);
  if (!icon.empty()) {
    gamerpic_ = imgui_drawer()->LoadImGuiIcon(icon);
  }
  return gamerpic_.get();
}

void GuideDialog::SelectBlade(size_t index) {
  if (index >= blades_.size() || index == blade_) {
    return;
  }
  blade_ = index;
  item_ = 0;
}

void GuideDialog::Finish(std::function<void()> action) {
  if (finished_) {
    return;
  }
  finished_ = true;
  imgui_drawer()->GetFocusManager()->UIDropFocus(kFocusName);
  Close();
  if (!action) {
    return;
  }
  // DEFERRED: CallInUIThread runs the body inline when already on the UI
  // thread, which a draw always is, and what the items do - open a dialog,
  // leave a game - must not happen in the middle of the guide's own frame.
  emulator_window_->app_context().CallInUIThreadDeferred(std::move(action));
}

void GuideDialog::OnDraw(ImGuiIO& io) {
  auto* focus_manager = imgui_drawer()->GetFocusManager();
  if (!opened_) {
    focus_manager->UISetFocus(kFocusName);
    opened_ = true;
  }
  if (blades_.empty()) {
    Finish(nullptr);
    return;
  }
  const ui::UIInput& input = focus_manager->XamInputFocus(kFocusName);

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("##nexia_guide", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* draw_list = ImGui::GetWindowDrawList();

  ImFont* font = ImGui::GetFont();
  const float font_size = ImGui::GetFontSize();
  ImFont* row_font = imgui_drawer()->GetTitleFont();
  const float row_font_size = row_font->FontSize;

  // The guide sits in the middle of the screen at roughly the share of it the
  // console's does, with the button legend underneath.
  const float width =
      std::clamp(io.DisplaySize.x * 0.62f, 520.0f,
                 std::max(520.0f, std::min(io.DisplaySize.x - 16.0f, 1100.0f)));
  const float header_height = std::max(width * 0.085f, row_font_size * 2.2f);
  const float row_height = std::max(width * 0.052f, row_font_size * 1.6f);
  const float body_height = row_height * kVisibleRows;
  const float legend_height = font_size * 2.6f;
  const float total_height = header_height + body_height + legend_height;
  const ImVec2 origin((io.DisplaySize.x - width) * 0.5f,
                      std::max(0.0f, (io.DisplaySize.y - total_height) * 0.5f));
  const float body_top = origin.y + header_height;
  const float body_bottom = body_top + body_height;

  draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, skin::kShade);

  // Blades: the ones before the open one stacked on the left, the open one's
  // own tab against its panel, the rest stacked on the right.
  const float blade_width = std::max(width * 0.058f, font_size * 2.0f);
  const float list_width =
      width - blade_width * static_cast<float>(blades_.size());
  float x = origin.x;
  float list_left = 0.0f;
  float title_left = origin.x;
  for (size_t i = 0; i < blades_.size(); ++i) {
    const bool open = i == blade_;
    const ImVec2 blade_min(x, body_top);
    const ImVec2 blade_max(x + blade_width, body_bottom);
    draw_list->AddRectFilled(blade_min, blade_max,
                             open ? skin::kActiveBlade : skin::kBlade);
    draw_list->AddLine(ImVec2(blade_max.x - 1.0f, body_top),
                       ImVec2(blade_max.x - 1.0f, body_bottom),
                       skin::kBladeEdge, 2.0f);
    AddVerticalText(
        draw_list, font, font_size * 1.15f,
        ImVec2(x + blade_width * 0.5f, body_top + blade_width * 0.35f),
        open ? skin::kActiveBladeText : skin::kBladeText,
        blades_[i].label.c_str());

    ImGui::SetCursorScreenPos(blade_min);
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::InvisibleButton("##blade", ImVec2(blade_width, body_height))) {
      SelectBlade(i);
    }
    ImGui::PopID();

    x += blade_width;
    if (open) {
      title_left = blade_min.x;
      list_left = x;
      x += list_width;
    }
  }
  const float list_right = list_left + list_width;

  // The open blade's panel.
  draw_list->AddRectFilledMultiColor(
      ImVec2(list_left, body_top), ImVec2(list_right, body_bottom),
      skin::kPanelTop, skin::kPanelTop, skin::kPanelBottom, skin::kPanelBottom);

  std::vector<GuideItem>& items = blades_[blade_].items;
  if (!items.empty()) {
    item_ = std::min(item_, items.size() - 1);
  }
  const size_t first_row =
      item_ >= kVisibleRows ? item_ - (kVisibleRows - 1) : 0;
  std::function<void()> chosen;
  bool picked = false;
  const float text_inset = row_font_size * 0.7f;
  for (size_t row = 0; row < kVisibleRows; ++row) {
    const size_t index = first_row + row;
    const float top = body_top + row_height * static_cast<float>(row);
    const ImVec2 row_min(list_left, top);
    const ImVec2 row_max(list_right, top + row_height);
    if (index >= items.size()) {
      continue;
    }
    const bool selected = index == item_;
    if (selected) {
      draw_list->AddRectFilled(row_min, row_max, skin::kSelected);
    }
    draw_list->AddLine(ImVec2(list_left, row_max.y - 1.0f),
                       ImVec2(list_right, row_max.y - 1.0f), skin::kRowLine,
                       1.0f);
    const float text_y = top + (row_height - row_font_size) * 0.5f;
    const ImU32 text_color = selected ? skin::kSelectedText : skin::kText;
    draw_list->AddText(row_font, row_font_size,
                       ImVec2(list_left + text_inset, text_y), text_color,
                       items[index].label.c_str());
    if (items[index].value) {
      const std::string value = items[index].value();
      if (!value.empty()) {
        const ImVec2 value_size = row_font->CalcTextSizeA(
            row_font_size, FLT_MAX, 0.0f, value.c_str());
        draw_list->AddText(
            row_font, row_font_size,
            ImVec2(list_right - text_inset - value_size.x, text_y), text_color,
            value.c_str());
      }
    }

    ImGui::SetCursorScreenPos(row_min);
    ImGui::PushID(static_cast<int>(1000 + index));
    if (ImGui::InvisibleButton("##row", ImVec2(list_width, row_height))) {
      item_ = index;
      chosen = items[index].action;
      picked = true;
    }
    // Follows the mouse only while it moves, so a pointer left resting over
    // the panel does not fight the d-pad.
    if (ImGui::IsItemHovered() &&
        (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
      item_ = index;
    }
    ImGui::PopID();
  }

  // Header: the guide's name over the open blade, the signed-in gamerpic in
  // the middle, the clock on the right.
  const float title_size = row_font_size * 1.1f;
  draw_list->AddText(row_font, title_size,
                     ImVec2(title_left + blade_width * 0.25f,
                            origin.y + (header_height - title_size) * 0.5f),
                     skin::kHeaderText, "Nexia Guide");
  const float picture_size = header_height * 0.86f;
  const ImVec2 picture_min(origin.x + (width - picture_size) * 0.5f,
                           body_top - picture_size - header_height * 0.06f);
  const ImVec2 picture_max(picture_min.x + picture_size,
                           picture_min.y + picture_size);
  draw_list->AddRectFilled(ImVec2(picture_min.x - 2.0f, picture_min.y - 2.0f),
                           ImVec2(picture_max.x + 2.0f, picture_max.y + 2.0f),
                           IM_COL32(255, 255, 255, 255));
  if (ui::ImmediateTexture* picture = Gamerpic()) {
    draw_list->AddImage(reinterpret_cast<ImTextureID>(picture), picture_min,
                        picture_max);
  } else {
    draw_list->AddRectFilled(picture_min, picture_max, skin::kBlade);
  }
  const std::string clock = ClockText();
  const ImVec2 clock_size =
      row_font->CalcTextSizeA(row_font_size, FLT_MAX, 0.0f, clock.c_str());
  draw_list->AddText(
      row_font, row_font_size,
      ImVec2(origin.x + width - clock_size.x - blade_width * 0.25f,
             origin.y + (header_height - row_font_size) * 0.5f),
      skin::kHeaderText, clock.c_str());

  // Legend. Each entry is also a button, so the guide works with a mouse.
  struct LegendEntry {
    ImU32 color;
    const char* letter;
    const char* label;
    int id;
  };
  std::vector<LegendEntry> legend = {
      {skin::kButtonA, "A", "Select", 0},
      {skin::kButtonB, "B", "Back", 1},
  };
  if (leave_game_) {
    legend.push_back({skin::kButtonY, "Y", "Leave Game", 3});
  }
  float legend_width = 0.0f;
  const float disc = font_size * 1.3f;
  const float gap = font_size * 1.2f;
  for (const LegendEntry& entry : legend) {
    legend_width +=
        disc + font_size * 0.4f +
        font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, entry.label).x + gap;
  }
  legend_width -= gap;
  float legend_x = list_left + (list_width - legend_width) * 0.5f;
  const float legend_center_y = body_bottom + legend_height * 0.5f;
  int legend_pressed = -1;
  for (const LegendEntry& entry : legend) {
    const float label_width =
        font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, entry.label).x;
    const float entry_width = disc + font_size * 0.4f + label_width;
    AddFaceButton(draw_list, font, font_size,
                  ImVec2(legend_x + disc * 0.5f, legend_center_y), entry.color,
                  entry.letter);
    draw_list->AddText(font, font_size,
                       ImVec2(legend_x + disc + font_size * 0.4f,
                              legend_center_y - font_size * 0.5f),
                       skin::kHeaderText, entry.label);
    ImGui::SetCursorScreenPos(ImVec2(legend_x, legend_center_y - disc * 0.5f));
    ImGui::PushID(2000 + entry.id);
    if (ImGui::InvisibleButton("##legend", ImVec2(entry_width, disc))) {
      legend_pressed = entry.id;
    }
    ImGui::PopID();
    legend_x += entry_width + gap;
  }

  // A click anywhere off the guide dismisses it.
  const bool clicked_outside =
      ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      !ImGui::IsAnyItemHovered() &&
      !ImGui::IsMouseHoveringRect(origin,
                                  ImVec2(origin.x + width, body_bottom));

  ImGui::End();

  // Pad through the focus manager, keyboard through ImGui.
  const size_t item_count = items.size();
  if (input.NavUp() || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
    if (item_count) {
      item_ = item_ == 0 ? item_count - 1 : item_ - 1;
    }
  }
  if (input.NavDown() || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
    if (item_count) {
      item_ = (item_ + 1) % item_count;
    }
  }
  if (input.NavLeft() || input.lb_pressed ||
      ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
    if (blade_ > 0) {
      SelectBlade(blade_ - 1);
    }
  }
  if (input.NavRight() || input.rb_pressed ||
      ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
    SelectBlade(blade_ + 1);
  }

  if (picked) {
    Finish(std::move(chosen));
    return;
  }
  const bool activate = input.Activated() || legend_pressed == 0 ||
                        ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
  if (activate && item_ < blades_[blade_].items.size()) {
    Finish(blades_[blade_].items[item_].action);
    return;
  }
  if (leave_game_ && (input.y_released || legend_pressed == 3)) {
    Finish(leave_game_);
    return;
  }
  if (input.ShouldClose() || legend_pressed == 1 || clicked_outside ||
      ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    Finish(nullptr);
  }
}

}  // namespace app
}  // namespace xe
