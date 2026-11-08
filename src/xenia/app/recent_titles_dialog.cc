/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/recent_titles_dialog.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "xenia/app/emulator_window.h"
#include "xenia/base/filesystem.h"
#include "xenia/emulator.h"
#include "xenia/ui/imgui_guide_skin.h"
#include "xenia/ui/ui_focus_manager.h"

namespace xe {
namespace app {

namespace {

constexpr char kFocusName[] = "RecentTitlesDialog";

// Rows on the panel at once; a longer list scrolls under the selection.
constexpr size_t kVisibleRows = 7;

namespace skin = ui::guide_skin;

// A face button in the legend: the coloured disc with its letter. Same as the
// guide's, so the two read as one piece.
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

// Text cut to fit `max_width`, with an ellipsis when it had to be cut.
std::string FitText(ImFont* font, float size, const std::string& text,
                    float max_width) {
  if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= max_width) {
    return text;
  }
  std::string cut = text;
  while (!cut.empty()) {
    // Whole UTF-8 characters only: drop continuation bytes with their lead.
    while (!cut.empty() && (static_cast<uint8_t>(cut.back()) & 0xC0) == 0x80) {
      cut.pop_back();
    }
    if (!cut.empty()) {
      cut.pop_back();
    }
    const std::string candidate = cut + "...";
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, candidate.c_str()).x <=
        max_width) {
      return candidate;
    }
  }
  return "...";
}

// The display name a title sorts and shows by.
std::string NameOf(const RecentTitleEntry& entry) {
  return entry.title_name.empty() ? xe::path_to_utf8(entry.path_to_file)
                                  : entry.title_name;
}

// Case-insensitive A to Z on the ASCII letters, which is what decides the
// order of nearly every game name; the raw bytes break ties.
bool NameLess(const std::string& a, const std::string& b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const int ca = std::tolower(static_cast<unsigned char>(a[i]));
    const int cb = std::tolower(static_cast<unsigned char>(b[i]));
    if (ca != cb) {
      return ca < cb;
    }
  }
  return a.size() < b.size();
}

}  // namespace

// The last sort picked, so the list opens the way it was left this session.
static int last_sort = 0;

RecentTitlesDialog::RecentTitlesDialog(ui::ImGuiDrawer* imgui_drawer,
                                       EmulatorWindow* emulator_window,
                                       Mode mode)
    : ui::ImGuiDialog(imgui_drawer),
      emulator_window_(emulator_window),
      mode_(mode),
      entries_(emulator_window->recently_launched_titles()),
      sort_(static_cast<Sort>(last_sort)) {
  ApplySort();
  selected_ = 0;
  first_row_ = 0;
}

const char* RecentTitlesDialog::SortName(Sort sort) {
  switch (sort) {
    case Sort::kRecent:
      return "Recent";
    case Sort::kRecentDescending:
      return "Recent (Descending)";
    case Sort::kAlphabetical:
      return "Alphabetical";
    case Sort::kAlphabeticalDescending:
      return "Alphabetical (Descending)";
  }
  return "";
}

void RecentTitlesDialog::ApplySort() {
  std::filesystem::path keep;
  if (selected_ < entries_.size()) {
    keep = entries_[selected_].path_to_file;
  }

  // The window's list is most recent first; every sort starts from it, so ties
  // (same time, same name) keep that order.
  entries_ = emulator_window_->recently_launched_titles();
  switch (sort_) {
    case Sort::kRecent:
      break;
    case Sort::kRecentDescending:
      std::reverse(entries_.begin(), entries_.end());
      break;
    case Sort::kAlphabetical:
    case Sort::kAlphabeticalDescending: {
      const bool descending = sort_ == Sort::kAlphabeticalDescending;
      std::stable_sort(
          entries_.begin(), entries_.end(),
          [descending](const RecentTitleEntry& a, const RecentTitleEntry& b) {
            const std::string na = NameOf(a);
            const std::string nb = NameOf(b);
            return descending ? NameLess(nb, na) : NameLess(na, nb);
          });
      break;
    }
  }

  selected_ = 0;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].path_to_file == keep) {
      selected_ = i;
      break;
    }
  }
}

RecentTitlesDialog::~RecentTitlesDialog() {
  if (opened_ && !finished_) {
    imgui_drawer()->GetFocusManager()->UIDropFocus(kFocusName);
  }
  // The icons go back to the drawer, not out with this object: ImGui still has
  // Image commands holding these pointers, and they are not rendered until
  // later in the same paint that deleted this dialog.
  for (auto& [key, icon] : icons_) {
    imgui_drawer()->ReleaseTextureDeferred(std::move(icon));
  }
  icons_.clear();

  if (closed_callback_) {
    closed_callback_();
  }
}

ui::ImmediateTexture* RecentTitlesDialog::IconFor(
    const RecentTitleEntry& entry) {
  const std::string key = xe::path_to_utf8(entry.path_to_file);
  const auto found = icons_.find(key);
  if (found != icons_.end()) {
    return found->second.get();
  }
  if (entry.icon.empty()) {
    // Remembered as absent, so a title without art is not decoded every frame.
    icons_[key] = nullptr;
    return nullptr;
  }
  icons_[key] = imgui_drawer()->LoadImGuiIcon(entry.icon);
  return icons_[key].get();
}

void RecentTitlesDialog::Finish(bool picked) {
  if (finished_) {
    return;
  }
  finished_ = true;
  imgui_drawer()->GetFocusManager()->UIDropFocus(kFocusName);

  const bool launch = picked && selected_ < entries_.size();
  std::filesystem::path chosen;
  uint32_t chosen_title_id = 0;
  if (launch) {
    chosen = entries_[selected_].path_to_file;
    chosen_title_id = entries_[selected_].title_id;
  }
  EmulatorWindow* window = emulator_window_;
  const Mode mode = mode_;
  Close();
  if (!launch) {
    return;
  }
  // Never from inside the frame: launching tears down and rebuilds the
  // graphics system, and opening the update selector puts up another dialog
  // while this one is still drawing. DEFERRED, not CallInUIThread: that one
  // runs the body INLINE when already on the UI thread, which OnDraw always is.
  window->app_context().CallInUIThreadDeferred(
      [window, mode, chosen, chosen_title_id]() {
        if (mode == Mode::kLaunch) {
          window->RunTitle(chosen);
        } else {
          window->OpenTitleUpdateSelector(chosen, chosen_title_id);
        }
      });
}

void RecentTitlesDialog::OnDraw(ImGuiIO& io) {
  auto* focus_manager = imgui_drawer()->GetFocusManager();
  if (!opened_) {
    focus_manager->UISetFocus(kFocusName);
    opened_ = true;
  }
  if (finished_) {
    return;
  }
  const ui::UIInput& input = focus_manager->XamInputFocus(kFocusName);

  const std::vector<RecentTitleEntry>& titles = entries_;
  const size_t count = titles.size();
  if (count) {
    selected_ = std::min(selected_, count - 1);
  }

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("##nexia_recent_titles", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* draw_list = ImGui::GetWindowDrawList();

  ImFont* font = ImGui::GetFont();
  const float font_size = ImGui::GetFontSize();
  ImFont* row_font = imgui_drawer()->GetTitleFont();
  const float row_font_size = row_font->FontSize;

  // Sized like the guide, centred, with the button legend underneath.
  const float width =
      std::clamp(io.DisplaySize.x * 0.56f, 520.0f,
                 std::max(520.0f, std::min(io.DisplaySize.x - 16.0f, 1000.0f)));
  const float header_height = std::max(width * 0.075f, row_font_size * 2.0f);
  const float row_height =
      std::max(width * 0.085f, row_font_size + font_size * 1.6f);
  const float body_height = row_height * kVisibleRows;
  const float footer_height = font_size * 1.8f;
  const float legend_height = font_size * 2.6f;
  const float total_height =
      header_height + body_height + footer_height + legend_height;
  const ImVec2 origin((io.DisplaySize.x - width) * 0.5f,
                      std::max(0.0f, (io.DisplaySize.y - total_height) * 0.5f));
  const float left = origin.x;
  const float right = origin.x + width;
  const float body_top = origin.y + header_height;
  const float body_bottom = body_top + body_height;
  const float footer_bottom = body_bottom + footer_height;

  draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, skin::kShade);

  // Header: the slate bar with the dialog's name and the count.
  draw_list->AddRectFilled(origin, ImVec2(right, body_top), skin::kBlade);
  draw_list->AddLine(ImVec2(left, body_top - 1.0f),
                     ImVec2(right, body_top - 1.0f), skin::kBladeEdge, 2.0f);
  const float inset = row_font_size * 0.7f;
  const char* title =
      mode_ == Mode::kLaunch ? "Open Recent" : "Open Recent with Title Update";
  const float title_size = row_font_size * 1.1f;
  draw_list->AddText(
      row_font, title_size,
      ImVec2(left + inset, origin.y + (header_height - title_size) * 0.5f),
      skin::kHeaderText, title);
  if (count) {
    const std::string tally = std::string(SortName(sort_)) + "   " +
                              std::to_string(count) +
                              (count == 1 ? " title" : " titles");
    const ImVec2 tally_size =
        font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, tally.c_str());
    draw_list->AddText(font, font_size,
                       ImVec2(right - inset - tally_size.x,
                              origin.y + (header_height - font_size) * 0.5f),
                       skin::kBladeText, tally.c_str());
  }

  // Body and footer share the guide's panel.
  draw_list->AddRectFilledMultiColor(
      ImVec2(left, body_top), ImVec2(right, footer_bottom), skin::kPanelTop,
      skin::kPanelTop, skin::kPanelBottom, skin::kPanelBottom);

  // Keep the selection on screen (d-pad, keys)...
  if (selected_ < first_row_) {
    first_row_ = selected_;
  } else if (selected_ >= first_row_ + kVisibleRows) {
    first_row_ = selected_ - (kVisibleRows - 1);
  }
  const size_t max_first = count > kVisibleRows ? count - kVisibleRows : 0;
  first_row_ = std::min(first_row_, max_first);

  // ...and let the wheel and the scrollbar move the window, the selection
  // following into it, so the next frame's rule above leaves it alone.
  bool scrolled = false;
  if (ImGui::IsMouseHoveringRect(ImVec2(left, body_top),
                                 ImVec2(right, body_bottom)) &&
      io.MouseWheel != 0.0f) {
    const int steps = std::max(1, static_cast<int>(std::abs(io.MouseWheel)));
    for (int i = 0; i < steps; ++i) {
      if (io.MouseWheel > 0.0f && first_row_ > 0) {
        --first_row_;
      } else if (io.MouseWheel < 0.0f && first_row_ < max_first) {
        ++first_row_;
      }
    }
    scrolled = true;
  }

  // The scrollbar, down the right of the rows, whenever there is more than a
  // screenful. Drag the thumb, or click the track to jump there.
  const bool has_scrollbar = count > kVisibleRows;
  const float scrollbar_width = has_scrollbar ? font_size * 0.9f : 0.0f;
  const float rows_right = right - scrollbar_width;
  float thumb_height = 0.0f;
  bool thumb_active = false;
  bool thumb_hovered = false;
  if (has_scrollbar) {
    thumb_height =
        std::max(body_height * static_cast<float>(kVisibleRows) / float(count),
                 font_size * 1.5f);
    const float travel = body_height - thumb_height;
    auto thumb_top_for = [&](size_t first) {
      return body_top + travel * static_cast<float>(first) / float(max_first);
    };
    ImGui::SetCursorScreenPos(ImVec2(rows_right, body_top));
    ImGui::InvisibleButton("##scrollbar", ImVec2(scrollbar_width, body_height));
    thumb_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) {
      const float thumb_top = thumb_top_for(first_row_);
      const float mouse_y = io.MousePos.y;
      drag_offset_ =
          (mouse_y >= thumb_top && mouse_y <= thumb_top + thumb_height)
              ? mouse_y - thumb_top
              : thumb_height * 0.5f;
    }
    if (ImGui::IsItemActive()) {
      thumb_active = true;
      const float fraction =
          travel > 0.0f
              ? std::clamp((io.MousePos.y - drag_offset_ - body_top) / travel,
                           0.0f, 1.0f)
              : 0.0f;
      first_row_ = static_cast<size_t>(
          std::lround(fraction * static_cast<float>(max_first)));
      scrolled = true;
    }
  }
  if (scrolled && count) {
    selected_ = std::clamp(selected_, first_row_,
                           std::min(count - 1, first_row_ + kVisibleRows - 1));
  }

  if (has_scrollbar) {
    const float travel = body_height - thumb_height;
    const float thumb_top =
        body_top + travel * static_cast<float>(first_row_) / float(max_first);
    draw_list->AddRectFilled(ImVec2(rows_right, body_top),
                             ImVec2(right, body_bottom),
                             IM_COL32(190, 194, 199, 255));
    draw_list->AddRectFilled(
        ImVec2(rows_right + 2.0f, thumb_top + 2.0f),
        ImVec2(right - 2.0f, thumb_top + thumb_height - 2.0f),
        thumb_active || thumb_hovered ? skin::kSelected : skin::kBlade);
  }

  bool picked = false;
  if (!count) {
    const char* empty = "Nothing has been played yet.";
    draw_list->AddText(
        row_font, row_font_size,
        ImVec2(left + inset, body_top + (row_height - row_font_size) * 0.5f),
        skin::kText, empty);
  }
  const float icon_size = row_height - row_font_size * 0.6f;
  for (size_t row = 0; row < kVisibleRows; ++row) {
    const size_t index = first_row_ + row;
    if (index >= count) {
      break;
    }
    const RecentTitleEntry& entry = titles[index];
    const float top = body_top + row_height * static_cast<float>(row);
    const ImVec2 row_min(left, top);
    const ImVec2 row_max(rows_right, top + row_height);
    const bool selected = index == selected_;
    if (selected) {
      draw_list->AddRectFilled(row_min, row_max, skin::kSelected);
    }
    draw_list->AddLine(ImVec2(left, row_max.y - 1.0f),
                       ImVec2(rows_right, row_max.y - 1.0f), skin::kRowLine,
                       1.0f);

    const ImVec2 icon_min(left + inset, top + (row_height - icon_size) * 0.5f);
    const ImVec2 icon_max(icon_min.x + icon_size, icon_min.y + icon_size);
    if (ui::ImmediateTexture* icon = IconFor(entry)) {
      draw_list->AddImage(reinterpret_cast<ImTextureID>(icon), icon_min,
                          icon_max);
    } else {
      draw_list->AddRectFilled(icon_min, icon_max, skin::kBlade);
    }

    const float text_left = icon_max.x + inset;
    const float text_width = rows_right - inset - text_left;
    const float text_block = row_font_size + font_size * 1.1f;
    const float name_y = top + (row_height - text_block) * 0.5f;
    const std::string name = NameOf(entry);
    draw_list->AddText(
        row_font, row_font_size, ImVec2(text_left, name_y),
        selected ? skin::kSelectedText : skin::kText,
        FitText(row_font, row_font_size, name, text_width).c_str());
    char id_text[32];
    if (entry.title_id) {
      std::snprintf(id_text, sizeof(id_text), "%08X", entry.title_id);
    } else {
      std::snprintf(id_text, sizeof(id_text), "unknown title id");
    }
    draw_list->AddText(
        font, font_size,
        ImVec2(text_left, name_y + row_font_size + font_size * 0.1f),
        selected ? IM_COL32(222, 240, 214, 255) : skin::kActiveBladeText,
        id_text);

    ImGui::SetCursorScreenPos(row_min);
    ImGui::PushID(static_cast<int>(index));
    if (ImGui::InvisibleButton("##row",
                               ImVec2(rows_right - left, row_height))) {
      selected_ = index;
      picked = true;
    }
    // Follows the mouse only while it moves, so a pointer left resting over
    // the panel does not fight the d-pad - and not while the scrollbar is
    // being dragged, which the selection is already following.
    if (!thumb_active && ImGui::IsItemHovered() &&
        (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
      selected_ = index;
    }
    ImGui::PopID();
  }

  // Footer: where the selected title lives, which used to be a tooltip.
  if (count) {
    const std::string path = xe::path_to_utf8(titles[selected_].path_to_file);
    draw_list->AddText(
        font, font_size,
        ImVec2(left + inset, body_bottom + (footer_height - font_size) * 0.5f),
        skin::kActiveBladeText,
        FitText(font, font_size, path, width - inset * 2.0f).c_str());
  }
  draw_list->AddLine(ImVec2(left, body_bottom), ImVec2(right, body_bottom),
                     skin::kRowLine, 1.0f);

  // Legend. Each entry is also a button, so it works with a mouse.
  struct LegendEntry {
    ImU32 color;
    const char* letter;
    const char* label;
    int id;
  };
  std::vector<LegendEntry> legend;
  if (count) {
    legend.push_back({skin::kButtonA, "A",
                      mode_ == Mode::kLaunch ? "Play" : "Choose Update", 0});
  }
  if (count) {
    legend.push_back({skin::kButtonX, "X", "Remove", 4});
  }
  if (count > 1) {
    legend.push_back({skin::kButtonY, "Y", "Sort", 3});
  }
  legend.push_back({skin::kButtonB, "B", "Back", 1});
  float legend_width = 0.0f;
  const float disc = font_size * 1.3f;
  const float gap = font_size * 1.2f;
  for (const LegendEntry& entry : legend) {
    legend_width +=
        disc + font_size * 0.4f +
        font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, entry.label).x + gap;
  }
  legend_width -= gap;
  float legend_x = left + (width - legend_width) * 0.5f;
  const float legend_center_y = footer_bottom + legend_height * 0.5f;
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
    ImGui::PushID(2000000 + entry.id);
    if (ImGui::InvisibleButton("##legend", ImVec2(entry_width, disc))) {
      legend_pressed = entry.id;
    }
    ImGui::PopID();
    legend_x += entry_width + gap;
  }

  // A click anywhere off the panel backs out.
  const bool clicked_outside =
      ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      !ImGui::IsAnyItemHovered() &&
      !ImGui::IsMouseHoveringRect(origin, ImVec2(right, footer_bottom));

  ImGui::End();

  // Pad through the focus manager, keyboard through ImGui. Up/down wrap, as
  // the guide's do; LB/RB and page keys move a screenful.
  if (count) {
    if (input.NavUp() || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
      selected_ = selected_ == 0 ? count - 1 : selected_ - 1;
    }
    if (input.NavDown() || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
      selected_ = (selected_ + 1) % count;
    }
    if (input.lb_pressed || ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
      selected_ = selected_ > kVisibleRows ? selected_ - kVisibleRows : 0;
    }
    if (input.rb_pressed || ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
      selected_ = std::min(selected_ + kVisibleRows, count - 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
      selected_ = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End)) {
      selected_ = count - 1;
    }
  }

  // Y rotates the sort: Recent, Recent (Descending), Alphabetical,
  // Alphabetical (Descending), back to Recent. The selected title stays
  // selected wherever it lands.
  if (count > 1 && (input.y_released || legend_pressed == 3 ||
                    ImGui::IsKeyPressed(ImGuiKey_Y, false))) {
    sort_ = static_cast<Sort>((static_cast<int>(sort_) + 1) % 4);
    last_sort = static_cast<int>(sort_);
    ApplySort();
  }

  // X removes the selected title from both recent lists and My Games; the
  // cursor stays on the row, which is now the next title.
  if (count && selected_ < count &&
      (input.x_released || legend_pressed == 4 ||
       ImGui::IsKeyPressed(ImGuiKey_Delete, false))) {
    const size_t row = selected_;
    const RecentTitleEntry removed = entries_[row];
    emulator_window_->RemoveRecentTitle(removed.title_id, removed.path_to_file);
    ApplySort();
    selected_ = entries_.empty() ? 0 : std::min(row, entries_.size() - 1);
    return;
  }

  if (picked) {
    Finish(true);
    return;
  }
  const bool activate = input.Activated() || legend_pressed == 0 ||
                        ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
  if (activate && count) {
    Finish(true);
    return;
  }
  if (input.ShouldClose() || legend_pressed == 1 || clicked_outside ||
      ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    Finish(false);
  }
}

}  // namespace app
}  // namespace xe
