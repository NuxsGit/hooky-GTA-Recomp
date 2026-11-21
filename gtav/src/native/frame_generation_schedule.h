#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace gtav::native::framegen {

struct SourceFrame {
  uint64_t guest_frame;
  uint64_t history_epoch;  // Change across cuts, resize, pause and resource recreation.
  bool eligible;           // Fresh, complete scene and validated interpolation inputs.
};

struct Insertion {
  std::chrono::nanoseconds offset{};
  float phase{};  // Interpolate between the older (0) and newer (1) source images.
};

struct Schedule {
  std::array<Insertion, 7> insertions{};
  std::size_t count{};
};

// Relative timing within a delayed presentation interval after both source images
// are available. The caller must present the real endpoints separately. This is
// only a cadence plan: it neither interpolates pixels nor submits a presentation.
constexpr Schedule Plan(unsigned multiplier, SourceFrame older, SourceFrame newer,
                        std::chrono::nanoseconds interval,
                        std::chrono::nanoseconds minimum_present_spacing) {
  if (multiplier != 2 && multiplier != 4 && multiplier != 6 && multiplier != 8) return {};
  if (!older.eligible || !newer.eligible || older.history_epoch != newer.history_epoch ||
      newer.guest_frame <= older.guest_frame || newer.guest_frame - older.guest_frame != 1 ||
      minimum_present_spacing.count() <= 0 || interval / multiplier < minimum_present_spacing) return {};
  Schedule result;
  result.count = multiplier - 1;
  const auto step = interval / multiplier;
  const auto remainder = interval % multiplier;
  for (unsigned i = 1; i < multiplier; ++i)
    result.insertions[i - 1] = {step * i + remainder * i / multiplier, float(i) / float(multiplier)};
  return result;
}

}  // namespace gtav::native::framegen
