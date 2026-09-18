/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_timeline.h"

#include <algorithm>
#include <cmath>

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

namespace {

// A closing record repeats its state's name with "End" on one side or the
// other; which side is a per-scene habit, not a rule.
constexpr std::string_view kEnd = "End";

// Returns the state a closing record closes, or an empty string when the
// record opens one.
std::string ClosedState(const std::string& name) {
  if (name.size() <= kEnd.size()) {
    return std::string();
  }
  if (name.compare(0, kEnd.size(), kEnd) == 0) {
    return name.substr(kEnd.size());
  }
  if (name.compare(name.size() - kEnd.size(), kEnd.size(), kEnd) == 0) {
    return name.substr(0, name.size() - kEnd.size());
  }
  return std::string();
}

float Mix(float from, float to, float t) { return from + (to - from) * t; }

uint32_t MixColor(uint32_t from, uint32_t to, float t) {
  uint32_t out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const float a = static_cast<float>((from >> shift) & 0xFF);
    const float b = static_cast<float>((to >> shift) & 0xFF);
    const auto mixed =
        static_cast<uint32_t>(std::clamp(std::lround(Mix(a, b, t)), 0L, 255L));
    out |= mixed << shift;
  }
  return out;
}

Vector MixVector(const Vector& from, const Vector& to, float t) {
  Vector out;
  for (size_t i = 0; i < 3; ++i) {
    out.value[i] = Mix(from.value[i], to.value[i], t);
  }
  return out;
}

// The ease bytes shape the curve between this key and the next; without a
// decoded meaning for each one, a smooth step matches the console's feel far
// better than a straight ramp and is what every eased key is doing.
float Shape(uint8_t interpolation, float t) {
  switch (interpolation) {
    case kInterpolateHold:
      return 0.0f;
    case kInterpolateLinear:
      return t;
    case kInterpolateEase:
      return t * t * (3.0f - 2.0f * t);
    default:
      return t;
  }
}

}  // namespace

void StateTable::Build(const std::vector<TimelineState>& states) {
  spans_.clear();
  for (const TimelineState& state : states) {
    if (state.name.empty()) {
      continue;
    }
    const std::string closed = ClosedState(state.name);
    // An opening record whose name also parses as a closing one is still an
    // opening record unless that state is already open.
    const bool closing =
        !closed.empty() && spans_.count(closed) != 0 && !spans_[closed].valid;
    if (closing) {
      StateSpan& span = spans_[closed];
      span.last = std::max(span.last, state.frame);
      span.next = state.target;
      span.valid = true;
      continue;
    }
    StateSpan& span = spans_[state.name];
    if (!span.valid) {
      span.first = state.frame;
      span.last = state.frame;
    }
  }
  // A state named only once is a single frame, not an error.
  for (auto& entry : spans_) {
    if (!entry.second.valid) {
      entry.second.valid = true;
    }
    if (entry.second.last < entry.second.first) {
      entry.second.last = entry.second.first;
    }
  }
}

StateSpan StateTable::Find(const std::string_view name) const {
  const auto found = spans_.find(std::string(name));
  return found == spans_.end() ? StateSpan() : found->second;
}

TimelinePlayer::TimelinePlayer(const Scene* scene, const Node* node)
    : scene_(scene), node_(node) {
  if (node_) {
    states_.Build(node_->states);
  }
}

bool TimelinePlayer::Play(const std::string_view name, bool loop) {
  const StateSpan span = states_.Find(name);
  if (!span.valid) {
    return false;
  }
  state_.assign(name);
  span_ = span;
  frame_ = static_cast<float>(span.first);
  loop_ = loop;
  playing_ = span.last > span.first;
  Sample();
  return true;
}

void TimelinePlayer::Stop() { playing_ = false; }

void TimelinePlayer::Advance(float seconds) {
  if (!playing_) {
    return;
  }
  frame_ += seconds * kTimelineFramesPerSecond;
  const auto last = static_cast<float>(span_.last);
  if (frame_ >= last) {
    // A closing record that names another state is a loop point, not a
    // transition: the playhead goes back to it and the tail keeps running,
    // which is how a focused control stays alive instead of freezing on its
    // final frame.
    if (!span_.next.empty()) {
      const StateSpan target = states_.Find(span_.next);
      if (target.valid && target.first < span_.last) {
        frame_ = static_cast<float>(target.first);
        Sample();
        return;
      }
    }
    if (loop_) {
      const float length = last - static_cast<float>(span_.first);
      frame_ =
          length > 0.0f
              ? static_cast<float>(span_.first) +
                    std::fmod(frame_ - static_cast<float>(span_.first), length)
              : last;
    } else {
      frame_ = last;
      playing_ = false;
    }
  }
  Sample();
}

const AnimatedProperties* TimelinePlayer::Properties(
    const std::string_view target) const {
  const auto found = sampled_.find(std::string(target));
  return found == sampled_.end() ? nullptr : &found->second;
}

void TimelinePlayer::Sample() {
  sampled_.clear();
  if (!node_ || !scene_) {
    return;
  }
  for (const Timeline& timeline : node_->timelines) {
    if (timeline.keys.empty()) {
      continue;
    }
    AnimatedProperties& out = sampled_[timeline.target];
    for (const Track& track : timeline.tracks) {
      ApplyTrack(timeline, track, &out);
    }
  }
}

void TimelinePlayer::ApplyTrack(const Timeline& timeline, const Track& track,
                                AnimatedProperties* out) const {
  if (track.property_name.empty() ||
      track.values.size() != timeline.keys.size()) {
    return;
  }

  // Find the pair of keys the playhead sits between. Before the first key and
  // after the last one the track simply holds that key's value.
  size_t index = 0;
  while (index + 1 < timeline.keys.size() &&
         static_cast<float>(timeline.keys[index + 1].frame) <= frame_) {
    ++index;
  }
  const Key& from = timeline.keys[index];
  const bool has_next = index + 1 < timeline.keys.size();
  const Key& to = has_next ? timeline.keys[index + 1] : from;

  float t = 0.0f;
  if (has_next && to.frame > from.frame) {
    t = (frame_ - static_cast<float>(from.frame)) /
        static_cast<float>(to.frame - from.frame);
    t = std::clamp(t, 0.0f, 1.0f);
    t = Shape(from.interpolation, t);
  }

  const uint32_t a = track.values[index];
  const uint32_t b = has_next ? track.values[index + 1] : a;
  const std::string& name = track.property_name;

  switch (track.property_type) {
    case PropertyType::kFloat: {
      const float value = Mix(scene_->FloatAt(a), scene_->FloatAt(b), t);
      if (name == "Opacity") {
        out->has_opacity = true;
        out->opacity = value;
      } else if (name == "Width") {
        out->has_width = true;
        out->width = value;
      } else if (name == "Height") {
        out->has_height = true;
        out->height = value;
      }
      break;
    }
    case PropertyType::kVector: {
      const Vector value =
          MixVector(scene_->VectorAt(a), scene_->VectorAt(b), t);
      if (name == "Position") {
        out->has_position = true;
        out->position = value;
      } else if (name == "Scale") {
        out->has_scale = true;
        out->scale = value;
      }
      break;
    }
    case PropertyType::kColor: {
      const uint32_t value =
          MixColor(scene_->ColorAt(a), scene_->ColorAt(b), t);
      if (name == "ColorFactor") {
        out->has_color_factor = true;
        out->color_factor = value;
      } else {
        out->colors[name] = value;
      }
      break;
    }
    case PropertyType::kQuaternion: {
      if (name == "Rotation") {
        const float from = Scene::RotationDegrees(scene_->QuaternionAt(a));
        const float to = Scene::RotationDegrees(scene_->QuaternionAt(b));
        out->has_rotation = true;
        out->rotation = Mix(from, to, t);
      }
      break;
    }
    case PropertyType::kBool: {
      if (name == "Show") {
        out->has_show = true;
        out->show = (t < 1.0f ? a : b) != 0;
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
