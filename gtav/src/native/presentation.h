// Game output sizing, independent of the window system and Vulkan.
#pragma once

#include "graphics_policy.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

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
enum class OutputMode : int32_t { kDesktop = 0, k540p = 540, k720p = 720, k1080p = 1080, k1440p = 1440, k2160p = 2160 };

constexpr OutputMode ParseOutputMode(int32_t value, bool lean = false) {
  switch (value) {
    case -1: return lean ? OutputMode::k540p : OutputMode::k720p;  // Auto
    case 0: return OutputMode::kDesktop;
    case 540: return OutputMode::k540p;
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
  return mode == OutputMode::kDesktop || (int32_t(mode) > 720 && OutputPreset(mode) == monitor);
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

// EASU uses SV_Position in target pixels. It can write straight to the swap-chain
// image only when the game viewport begins at (0,0) and needs no further resize.
enum class Fsr1Route { Bypass, DirectEasu, EasuThenLinear, DirectRcas, RcasThenLinear };

constexpr Fsr1Route PlanFsr1(Extent fsr_size, const OutputRect& rect, bool sharpen) {
  if (fsr_size.Empty()) return Fsr1Route::Bypass;
  if (sharpen) return fsr_size == rect.size ? Fsr1Route::DirectRcas : Fsr1Route::RcasThenLinear;
  if (!rect.x && !rect.y && fsr_size == rect.size) return Fsr1Route::DirectEasu;
  return Fsr1Route::EasuThenLinear;
}

enum class UpscaleStatus { kLinear, kFsr1, kNoUpscale, kUnavailable };
enum class AntiLagStatus { Unsupported, Off, Fallback, Ready, Pacing };
enum class ContactRayStatus { Off, NoCompatiblePass, NoAlignedDepth, Recorded, Unavailable };

struct FogInfo {
  bool reduced = false;
  uint32_t suppressed = 0, protected_draws = 0;  // last replay, not a GPU/presentation result
};

struct RealismInfo {
  uint32_t material_floor = 1, anisotropy_cap = 1;  // effective boost and cap for this run
  ContactRayStatus contact_rays = ContactRayStatus::Off;
  FogInfo fog;
};

constexpr AntiLagStatus DriverPacing(bool supported, bool requested, bool enabled, bool called) {
  if (!supported) return AntiLagStatus::Unsupported;
  if (!requested) return AntiLagStatus::Off;
  if (!enabled) return AntiLagStatus::Fallback;
  return called ? AntiLagStatus::Pacing : AntiLagStatus::Ready;
}

struct PipelineInfo {
  double wait_ms = 0;           // CPU wait for background compiles in the last replay
  uint32_t pending = 0;         // on-demand jobs not yet collected, including completed ones
  uint32_t warmed = 0;          // cached pipelines reused since launch
};

struct PresentationInfo {
  Extent input;          // Actual front buffer including the game's HUD, not the monitor size.
  Extent fsr_output;     // Empty when EASU/RCAS weren't run.
  UpscaleStatus status = UpscaleStatus::kLinear;
  bool fsr_sharpened = false;
  bool vsync = true;     // Created Vulkan swap-chain mode, not the requested cvar.
  std::string_view adapter;
  OutputMode boot_output = OutputMode::k720p;
  float scene_scale = 1.0f;
  uint32_t queue_limit = 2;
  AntiLagStatus anti_lag = AntiLagStatus::Unsupported;
  uint32_t gpu_slots = 2;
  bool lean_memory = false;
  std::optional<GraphicsMemoryStats> memory;  // sampled when a host overlay is open
  RealismInfo realism;
  std::optional<PipelineInfo> pipelines;
};

}  // namespace gtav::native
