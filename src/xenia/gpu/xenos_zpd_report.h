/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_XENOS_ZPD_REPORT_H_
#define XENIA_GPU_XENOS_ZPD_REPORT_H_

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "xenia/base/byte_order.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

// Guest-memory helpers for EVENT_WRITE_ZPD reports. Each 0x40-byte slot holds
// an END record at the base and a BEGIN record at +0x20; the guest reads
// (end - begin) of the passing samples. Fields are le<uint32_t>, so logical
// values are read/written directly and the D3D END sentinel is matched against
// byte_swap(0xFFFFFEED).
struct XenosZPDReport {
  static constexpr uint32_t kRecordSizeBytes = 0x20;
  static constexpr uint32_t kRecordAlignMask = ~(kRecordSizeBytes - 1);

  static constexpr uint32_t kSlotSizeBytes = 0x40;
  static constexpr uint32_t kSlotAlignMask = ~(kSlotSizeBytes - 1);

  static constexpr uint32_t GetRecordBase(uint32_t address) {
    return address & kRecordAlignMask;
  }
  static constexpr uint32_t GetSlotBase(uint32_t address) {
    return address & kSlotAlignMask;
  }
  // END record is at the slot base, BEGIN record one record above it.
  static constexpr uint32_t GetEndRecordBase(uint32_t address) {
    return GetSlotBase(address);
  }
  static constexpr uint32_t GetBeginRecordBase(uint32_t address) {
    return GetSlotBase(address) + kRecordSizeBytes;
  }

  static constexpr bool IsBeginRecord(uint32_t address) {
    uint32_t record = GetRecordBase(address);
    return record != 0 && record == GetBeginRecordBase(record);
  }
  static constexpr bool IsEndRecord(uint32_t address) {
    uint32_t record = GetRecordBase(address);
    return record != 0 && record == GetEndRecordBase(record);
  }

  // Only ZPass_A / ZFail_A are inspected; the B fields carry title-specific
  // values not understood well enough to act on.
  static bool HasPendingSentinel(
      const xenos::xe_gpu_depth_sample_counts* report) {
    const uint32_t kSentinel = xe::byte_swap(0xFFFFFEEDu);
    return report->ZPass_A == kSentinel || report->ZFail_A == kSentinel;
  }

  // Host queries only yield a passing count; Total/ZPass A mirror it, rest 0.
  static void WriteSampleCount(xenos::xe_gpu_depth_sample_counts* report,
                               uint32_t sample_count, bool saturate = true) {
    if (saturate) {
      sample_count = SaturateSampleCount(sample_count);
    }
    report->Total_A = sample_count;
    report->Total_B = 0;
    report->ZFail_A = 0;
    report->ZFail_B = 0;
    report->ZPass_A = sample_count;
    report->ZPass_B = 0;
    report->StencilFail_A = 0;
    report->StencilFail_B = 0;
  }

  // Compresses counts above the knee by an exponent from
  // query_occlusion_saturation (1.0 = pass-through) to tame flicker from titles
  // driving exposure/lens-flare straight off raw occlusion counts.
  static uint32_t SaturateSampleCount(uint32_t sample_count) {
    double saturation = std::clamp(
        static_cast<double>(cvars::query_occlusion_saturation), 0.0, 1.0);
    if (sample_count == 0 || saturation >= 1.0) {
      return sample_count;
    }
    if (saturation <= 0.0) {
      // Never report fully occluded for a count that actually passed samples.
      return 1;
    }
    constexpr double kKnee = 32.0;
    if (static_cast<double>(sample_count) <= kKnee) {
      return sample_count;
    }
    const double exponent = std::clamp(saturation, 0.05, 1.0);
    const double compressed =
        kKnee + std::pow(static_cast<double>(sample_count) - kKnee, exponent);
    return static_cast<uint32_t>(compressed + 0.5);
  }

  // Fake QueryBatch counter that walks the range from the lower threshold,
  // wrapping at the top. Returns the base when range faking is disabled.
  static uint32_t QueryBatchFakeSamples(uint32_t& counter) {
    int32_t lower = cvars::query_occlusion_sample_lower_threshold;
    uint32_t base = lower > 0 ? static_cast<uint32_t>(lower) : 0;
    uint32_t range =
        static_cast<uint32_t>(cvars::query_occlusion_querybatch_range);
    if (range == 0) {
      return base;
    }
    if (counter < base || counter - base >= range) {
      counter = base;
    }
    return counter++;
  }

  // Writes a begin/end pair so (end - begin) is the saturated delta. The begin
  // record is rewritten only when a fresh BEGIN snapshot is available.
  static void WriteReportDelta(xenos::xe_gpu_depth_sample_counts* begin_report,
                               xenos::xe_gpu_depth_sample_counts* end_report,
                               uint32_t begin_value, uint32_t delta_value,
                               bool write_begin_report) {
    delta_value = SaturateSampleCount(delta_value);
    uint32_t end_value = begin_value + delta_value;
    if (write_begin_report && begin_report && begin_report != end_report) {
      WriteSampleCount(begin_report, begin_value, false);
    }
    WriteSampleCount(end_report, end_value, false);
  }
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_XENOS_ZPD_REPORT_H_
