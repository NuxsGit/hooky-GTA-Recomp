// Game output sizing, independent of the window system and Vulkan.
#pragma once

#include <algorithm>
#include <cstdint>

namespace gtav::native {

struct Extent {
  uint32_t width = 0, height = 0;
  constexpr bool Empty() const { return !width || !height; }
  constexpr bool operator==(const Extent&) const = default;
};

struct OutputRect {
  uint32_t x = 0, y = 0;
  Extent size;
};

// Present settings intentionally don't change the guest's display registers or EDRAM layout.
enum class OutputMode : int32_t { kDesktop = 0, k720p = 720, k1080p = 1080, k1440p = 1440, k2160p = 2160 };

constexpr OutputMode ParseOutputMode(int32_t value) {
  switch (value) {
    case 0: return OutputMode::kDesktop;
    case 1080: return OutputMode::k1080p;
    case 1440: return OutputMode::k1440p;
    case 2160: return OutputMode::k2160p;
    default: return OutputMode::k720p;
  }
}

constexpr Extent OutputPreset(OutputMode mode) {
  const auto height = static_cast<uint32_t>(mode);
  return {height * 16 / 9, height};
}

// A matching monitor can show all preset pixels, without losing rows to the frame/taskbar.
constexpr bool UseBorderless(OutputMode mode, Extent monitor) {
  return mode == OutputMode::kDesktop || (mode != OutputMode::k720p && OutputPreset(mode) == monitor);
}

constexpr OutputRect FitAspect(Extent bounds, Extent aspect) {
  if (bounds.Empty() || aspect.Empty()) return {};
  Extent size = bounds;
  if (uint64_t(bounds.width) * aspect.height > uint64_t(bounds.height) * aspect.width) {
    size.width = std::max(1u, uint32_t(uint64_t(bounds.height) * aspect.width / aspect.height));
  } else {
    size.height = std::max(1u, uint32_t(uint64_t(bounds.width) * aspect.height / aspect.width));
  }
  return {(bounds.width - size.width) / 2, (bounds.height - size.height) / 2, size};
}

constexpr Extent WindowedClient(OutputMode mode, Extent available) {
  const Extent preset = OutputPreset(mode);
  return FitAspect({std::min(preset.width, available.width), std::min(preset.height, available.height)},
                   {16, 9}).size;
}

// FSR 1 is designed for up to 2x per axis. On a 4K display the Xbox's 720p front buffer takes
// EASU+RCAS to 1440p, then the existing linear blit fills the display (not a 4K guest render).
constexpr Extent Fsr1Extent(Extent input, Extent output) {
  if (input.Empty() || output.width <= input.width || output.height <= input.height) return {};
  if (output.width <= uint64_t(input.width) * 2 && output.height <= uint64_t(input.height) * 2) return output;
  // Keep the exact input aspect at 2x if possible, or fit it inside the available output pixels.
  const auto size = FitAspect({uint32_t(std::min<uint64_t>(output.width, uint64_t(input.width) * 2)),
                               uint32_t(std::min<uint64_t>(output.height, uint64_t(input.height) * 2))}, input).size;
  return size.width > input.width && size.height > input.height ? size : Extent{};
}

enum class UpscaleStatus { kLinear, kFsr1, kNoUpscale, kUnavailable };

struct PresentationInfo {
  Extent input;          // Actual front buffer including the game's HUD, not the monitor size.
  Extent fsr_output;     // Empty when EASU/RCAS weren't run.
  UpscaleStatus status = UpscaleStatus::kLinear;
};

}  // namespace gtav::native
