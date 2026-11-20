// Startup renderer policy and allocation math, independent of the GPU and the settings UI.
#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace gtav::native {

inline constexpr uint32_t kFullScenePitch = 1280;
inline constexpr uint32_t kMaxTargetRows = 4096;  // includes the stacked sun shadow cascades
inline constexpr float kUltraSceneScale = 1.5f;
inline constexpr float kMinSceneScale = 0.25f;  // N100 Ultra: 25% scene for Intel N100 / 8GB low-power
inline constexpr float kPotatoSceneScale = 0.5f;
inline constexpr float kExtraPotatoSceneScale = 0.333333f;
inline constexpr float kUltraPotatoSceneScale = 0.25f;  // Cyberpunk-style ultra-low: 25% for N100
inline constexpr uint32_t kMaxFrameSlots = 2;
inline constexpr uint64_t kMiB = 1ull << 20;

inline uint32_t ScaledTexels(uint32_t n, float scale) {
  return scale == 1.0f ? n : std::max(1u, uint32_t(std::lround(n * scale)));
}

inline float SceneScale(double requested, float fallback, bool desktop, uint64_t max_texture_size) {
  const float scale = std::isfinite(requested)
      ? float(std::clamp(requested, double(kMinSceneScale), desktop ? double(kUltraSceneScale) : 1.0)) : fallback;
  // Scale is fixed for the whole run (including warmed pipelines). Don't mix different non-unit
  // scales if a tall target grows; fall back to native size on adapters with a small image limit.
  return scale > 1.0f && ScaledTexels(kMaxTargetRows, scale) > max_texture_size ? 1.0f : scale;
}

constexpr float TargetScale(float scene_scale, uint32_t pitch, uint32_t msaa) {
  // Keep the existing downscale range. Supersample the 1280-wide single-sample passes, including
  // full-size forward water shading. The 320/640 water aliases, wave maps and MSAA stay at 1:1.
  return !msaa && (scene_scale > 1.0f ? pitch == kFullScenePitch : pitch >= kFullScenePitch) ? scene_scale : 1.0f;
}

// Occlusion samples are counted by the GPU in physical pixels. Use the actual draw scale, not
// the global setting (e.g. the water pre-pass is unscaled). If a query spans mixed targets, use
// the smallest scale to avoid treating a visible object as occluded.
struct QueryCoverage {
  float scale = 0;
  void Draw(float target_scale) { scale = scale ? std::min(scale, target_scale) : target_scale; }
  uint64_t GuestSamples(uint64_t samples) const {
    if (!samples || !scale || scale == 1.0f) return samples;
    return std::max<uint64_t>(1, uint64_t(double(samples) / (scale * scale)));
  }
};

struct GraphicsMemoryPolicy {
  uint32_t frame_slots = kMaxFrameSlots;
  uint32_t budget_interval = 60, min_idle = 120;
  int32_t texture_budget_mib = 1024, buffer_budget_mib = 0;
  uint64_t upload_chunk = 64 * kMiB;
  bool lean = false;
  bool extra_potato = false;  // ultra-lean: GTA IV faces on 3x rien hardware
  bool ultra_potato = false;  // N100 Cyberpunk ultra: 25% scene, 64/16 MiB, 4 MiB chunk, max LOD cull
};

constexpr GraphicsMemoryPolicy MakeGraphicsMemoryPolicy(bool lean, bool extra_potato, bool ultra_potato, uint64_t default_upload_chunk) {
  // Soft budgets for evictable caches only; active textures, resolves and guest memory are not capped.
  // N100 Ultra: even leaner than Extra Potato for 8GB / UHD - saccades-free on N100.
  if (ultra_potato) return {1, 8, 15, 64, 16, 4 * kMiB, true, true, true};
  if (extra_potato) return {1, 10, 20, 96, 32, 8 * kMiB, true, true, false};
  if (lean) return {1, 15, 30, 192, 64, 16 * kMiB, true, false, false};
  GraphicsMemoryPolicy policy;
  policy.upload_chunk = default_upload_chunk;
  return policy;
}

constexpr GraphicsMemoryPolicy MakeGraphicsMemoryPolicy(bool lean, bool extra_potato, uint64_t default_upload_chunk) {
  return MakeGraphicsMemoryPolicy(lean, extra_potato, false, default_upload_chunk);
}
constexpr GraphicsMemoryPolicy MakeGraphicsMemoryPolicy(bool lean, uint64_t default_upload_chunk) {
  return MakeGraphicsMemoryPolicy(lean, false, false, default_upload_chunk);
}

constexpr uint32_t NextFrameSlot(uint32_t slot, const GraphicsMemoryPolicy& policy) {
  return (slot + 1) % policy.frame_slots;
}

// Live cache payload/request sizes, not driver allocations. Retired objects may still exist
// until a fence, but don't count towards these evictable working sets.
struct CacheUsage {
  uint64_t bytes = 0, target = 0;
  constexpr bool OverTarget() const { return target && bytes > target; }
  constexpr bool Trimmed() const { return bytes <= target * 4 / 5; }
  constexpr void Replace(uint64_t before, uint64_t after) {
    assert(before <= bytes);
    bytes = bytes - before + after;
  }
};

struct GraphicsMemoryStats {
  CacheUsage decoded_textures, converted_buffers;
  uint64_t upload_reserved = 0, upload_reclaimed = 0;
};

inline constexpr uint32_t kUploadIdleFrames = 90;  // completed submissions of the same slot

// Never truncate an allocation to the nominal chunk size. Large textures can exceed it.
// Alignments here are powers of two and are supplied by the renderer, not the guest.
constexpr uint64_t AlignUpload(uint64_t offset, uint64_t alignment) {
  return (offset + alignment - 1) & ~(alignment - 1);
}
constexpr bool FitsUpload(uint64_t capacity, uint64_t offset, uint64_t bytes) {
  return offset <= capacity && bytes <= capacity - offset;
}
constexpr uint64_t UploadCapacity(uint64_t nominal, uint64_t bytes) {
  return std::max(nominal, bytes);
}

// Only strengthen trilinear, repeating material fetches with a real mip chain. Resolve views
// (notably water masks, packed depth and R32F waves), LUTs and intentional point/LOD fetches
// must not acquire a new filter just because the visual quality setting changed.
// 0 follows the existing preset (8x minimum in Ultra); 1 removes only the boost.
// Explicit values are a minimum on eligible textures, never a downgrade of a guest sampler.
constexpr uint32_t MaterialFilterFloor(int32_t option, bool preset_boost) {
  switch (option) {
    case 1: case 2: case 4: case 8: case 16: return uint32_t(option);
    default: return preset_boost ? 8u : 1u;
  }
}

constexpr uint32_t MaterialAnisotropy(uint32_t floor, uint32_t material_levels, bool repeat_uv, bool trilinear,
                                      bool mip_range, uint32_t guest, uint32_t cap) {
  const uint32_t requested = material_levels > 1 && repeat_uv && trilinear && mip_range
      ? std::max(floor, guest) : guest;
  return std::min(requested, cap);
}

}  // namespace gtav::native
