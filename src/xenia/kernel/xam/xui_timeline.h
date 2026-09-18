/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_TIMELINE_H_
#define XENIA_KERNEL_XAM_XUI_TIMELINE_H_

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xam/xam_ui_new.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

class Scene;

// The console runs its UI timelines on a fixed 30 frame clock, which is what
// every authored frame number in a scene counts in.
constexpr float kTimelineFramesPerSecond = 30.0f;

// The state names every control in the 17559 skin is authored with. A state is
// a span of timeline frames: the NAME block gives "Focus" at its first frame
// and "EndFocus" at its last, so playing a state means running from one to the
// other.
constexpr const char* kStateNormal = "Normal";
constexpr const char* kStateFocus = "Focus";
constexpr const char* kStatePress = "Press";
constexpr const char* kStateKillFocus = "KillFocus";
constexpr const char* kStateNormalDisable = "NormalDisable";
constexpr const char* kStateFocusDisable = "FocusDisable";
constexpr const char* kStatePressDisable = "PressDisable";

struct StateSpan {
  uint32_t first = 0;
  uint32_t last = 0;
  // The state the closing record points at. The skin uses this to hold a
  // control alive: Focus ends at its last frame and points back at FocusLoop,
  // so the tail of the span repeats for as long as the control is focused.
  std::string next;
  bool valid = false;
};

// The named spans of one object's timelines. The NAME records arrive as a flat
// run of opening and closing records, so this pairs them back up. Scenes are
// not consistent about which form they use - the skin writes EndFocus, the
// editor's own scenes write FadeInEnd - so both are understood.
class StateTable {
 public:
  void Build(const std::vector<TimelineState>& states);

  StateSpan Find(const std::string_view name) const;
  bool empty() const { return spans_.empty(); }

 private:
  std::map<std::string, StateSpan> spans_;
};

// The animated properties of one object. A timeline drives these on top of the
// values the scene authored, so an element that no timeline touches keeps
// exactly what it was built with.
struct AnimatedProperties {
  bool has_position = false;
  Vector position;
  bool has_scale = false;
  Vector scale;
  bool has_opacity = false;
  float opacity = 1.0f;
  bool has_width = false;
  float width = 0.0f;
  bool has_height = false;
  float height = 0.0f;
  bool has_rotation = false;
  // Degrees about z; a scene never turns anything out of the plane.
  float rotation = 0.0f;
  bool has_show = false;
  bool show = true;
  bool has_color_factor = false;
  uint32_t color_factor = 0xFFFFFFFF;
  // Brush colours a timeline drives, by the property path it drives them
  // through ("Fill.FillColor"). The drawer asks for these by name.
  std::map<std::string, uint32_t> colors;

  bool any() const {
    return has_position || has_scale || has_opacity || has_width ||
           has_height || has_rotation || has_show || has_color_factor ||
           !colors.empty();
  }
};

// Plays one object's timelines. The console drives a control's appearance
// entirely through these: focusing a button does not recolour it directly, it
// plays the button's "Focus" span.
class TimelinePlayer {
 public:
  TimelinePlayer(const Scene* scene, const Node* node);

  // Starts a named state. Returns false when the object does not have one,
  // which is normal - most elements are not controls.
  bool Play(const std::string_view name, bool loop = false);
  void Stop();

  void Advance(float seconds);

  bool playing() const { return playing_; }
  const std::string& state() const { return state_; }
  float frame() const { return frame_; }

  // The properties of a named target under the playhead. The target is a
  // child's Id, or empty for the object the timelines belong to.
  const AnimatedProperties* Properties(const std::string_view target) const;

  const StateTable& states() const { return states_; }

 private:
  void Sample();
  void ApplyTrack(const Timeline& timeline, const Track& track,
                  AnimatedProperties* out) const;

  const Scene* scene_ = nullptr;
  const Node* node_ = nullptr;
  StateTable states_;
  std::string state_;
  StateSpan span_;
  float frame_ = 0.0f;
  bool playing_ = false;
  bool loop_ = false;
  std::map<std::string, AnimatedProperties> sampled_;
};

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
