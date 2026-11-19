// Host-independent choices for replaying a guest D3D Resolve.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gtav::native {

constexpr uint32_t kResolveClearColor = 0x100;
constexpr uint32_t kResolveClearDepthStencil = 0x200;

struct ResolveOperations {
  uint32_t source;
  bool copy_depth, clear_color, clear_depth;
};

constexpr ResolveOperations DecodeResolve(uint32_t flags) {
  const uint32_t source = flags & 7;
  const bool depth = source >= 4;
  return {source, depth, !depth && (flags & kResolveClearColor) != 0, (flags & kResolveClearDepthStencil) != 0};
}

struct ResolveRegion {
  uint32_t left = 0, top = 0, right = 0, bottom = 0;
  uint32_t dest_x = 0, dest_y = 0;
  constexpr bool Empty() const { return left >= right || top >= bottom; }
  bool operator==(const ResolveRegion&) const = default;
};

constexpr ResolveRegion ClipResolveDestination(ResolveRegion region, uint32_t width, uint32_t height) {
  if (region.Empty() || region.dest_x >= width || region.dest_y >= height) return {};
  region.right = region.left + std::min(region.right - region.left, width - region.dest_x);
  region.bottom = region.top + std::min(region.bottom - region.top, height - region.dest_y);
  return region;
}

// Coordinates are guest pixels; pass the destination mip's dimensions, not the base level's.
constexpr ResolveRegion ResolveCopyRegion(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                                          const uint32_t* rect, const uint32_t* point) {
  const uint32_t x = rect ? rect[0] : 0, y = rect ? rect[1] : 0;
  return ClipResolveDestination({x, y, std::min(src_w, rect ? rect[2] : dst_w),
                                 std::min(src_h, rect ? rect[3] : dst_h), point ? point[0] : x, point ? point[1] : y},
                                dst_w, dst_h);
}

// A fractional scale can round the source and destination origins in opposite directions.
// Clip again against the actual host mip extent before drawing or clearing anything.
inline ResolveRegion ScaleResolveRegion(const ResolveRegion& region, float scale, uint32_t dst_w, uint32_t dst_h) {
  if (scale == 1.0f) return ClipResolveDestination(region, dst_w, dst_h);
  auto sc = [scale](uint32_t x) { return uint32_t(std::lround(x * scale)); };
  return ClipResolveDestination({sc(region.left), sc(region.top), sc(region.right), sc(region.bottom),
                                 sc(region.dest_x), sc(region.dest_y)}, dst_w, dst_h);
}

struct ResolveReplay {
  bool copied = false, cleared_depth = false;
  float applied_z = 0.0f;
  uint32_t applied_stencil = 0;
};

}  // namespace gtav::native
