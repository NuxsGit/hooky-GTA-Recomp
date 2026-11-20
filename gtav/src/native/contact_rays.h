// Conservative TU27 screen-space contact pass policy. No dependency on the GPU API.
#pragma once

#include "presentation.h"
#include "resolve_policy.h"

namespace gtav::native {

struct ContactSettings {
  uint32_t quality = 0;
  float strength = 0;
  constexpr bool Enabled() const { return quality != 0 && strength > 0; }
};

constexpr ContactSettings ContactOptions(int32_t quality, int32_t strength) {
  if (quality < 1 || quality > 3 || strength < 1 || strength > 100) return {};
  return {uint32_t(quality), float(strength) / 100.0f};
}

constexpr ContactRayStatus BeginContactStatus(ContactSettings options, bool pass_available) {
  if (!options.Enabled()) return ContactRayStatus::Off;
  return pass_available ? ContactRayStatus::NoCompatiblePass : ContactRayStatus::Unavailable;
}

constexpr std::string_view ContactStatusName(ContactRayStatus status) {
  switch (status) {
    case ContactRayStatus::Off: return "off";
    case ContactRayStatus::NoCompatiblePass: return "no_compatible_pass";
    case ContactRayStatus::NoAlignedDepth: return "no_aligned_depth";
    case ContactRayStatus::Recorded: return "recorded";
    case ContactRayStatus::Unavailable: return "unavailable";
  }
  return "unknown";
}

// TU27 main view in guest pixels, not the window or an inferred EDRAM target height.
inline constexpr Extent kContactScene{kFullScenePitch, 720};
inline constexpr Extent kContactHalfScene{kFullScenePitch / 2, 360};

// Narrow pass fingerprints from the TU27 capture notes (HANDOFF_LINUX_2026-10-27.md).
// Only the high dword was recorded there. Require the matching blend, draw shape and a
// same-frame depth input as well; don't apply to a guessed surface address or the front buffer.
enum class ContactMaskDraw { None, Initialize, Multiply };
constexpr ContactMaskDraw ClassifyContactMask(uint64_t ps, uint32_t primitive, uint32_t vertices,
                                              uint32_t mask, uint32_t blend) {
  const bool rectangle = (primitive == 8 && vertices == 3) ||
                         (primitive == 4 && (vertices == 3 || vertices == 6)) ||
                         ((primitive == 5 || primitive == 6 || primitive == 13) && vertices == 4);
  if (!rectangle || (mask & ~0xFu) || !(mask & 8u)) return ContactMaskDraw::None;
  const uint32_t alpha = (blend >> 16) & 0x1FFF;
  if (uint32_t(ps >> 32) == 0x487A191Fu && alpha == 1u)  // replace alpha: one, add, zero
    return ContactMaskDraw::Initialize;
  if (uint32_t(ps >> 32) == 0x5416149Fu && mask == 8u && alpha == (6u << 8))  // zero, add, src-alpha
    return ContactMaskDraw::Multiply;
  return ContactMaskDraw::None;
}

constexpr Extent ContactViewport(float x, float top, float width, float height) {
  if (x != 0 || top != 0) return {};
  if (width == kContactScene.width && height == kContactScene.height) return kContactScene;
  if (width == kContactHalfScene.width && height == kContactHalfScene.height) return kContactHalfScene;
  return {};
}

struct ContactMaskStamp {
  uint64_t frame = 0;
  uint32_t surface = 0;
  Extent size;
  constexpr bool Matches(uint64_t now, uint32_t object, Extent extent) const {
    return surface && frame == now && surface == object && size == extent;
  }
};

enum class ContactDepthOrder : uint32_t { Unknown, Forward, Reversed, Conflicting };

struct ContactDepthWriters {
  uint64_t frame = 0;
  ContactDepthOrder order = ContactDepthOrder::Unknown;
  uint32_t surface = 0, revision = 0;

  constexpr void Clear() {
    order = ContactDepthOrder::Unknown;
    surface = 0;
    ++revision;
  }

  // Only count *recorded* writes. Equal-depth passes don't establish a convention. A
  // masked/subview camera, special Z range or inconsistent comparisons aren't evidence.
  constexpr void Write(uint64_t now, uint32_t depth_control, bool full_view, uint32_t object) {
    if (!(depth_control & 4)) return;
    if (frame != now) *this = {now};
    ++revision;
    if (order == ContactDepthOrder::Conflicting) return;
    if (!object || (surface && surface != object)) {
      order = ContactDepthOrder::Conflicting;
      return;
    }
    surface = object;
    const uint32_t cmp = (depth_control >> 4) & 7;
    if (full_view && (depth_control & 2) && (cmp == 0 || cmp == 2)) return;  // NEVER / EQUAL
    const auto next = !full_view || !(depth_control & 2) ? ContactDepthOrder::Conflicting
        : cmp == 1 || cmp == 3 ? ContactDepthOrder::Forward
        : cmp == 4 || cmp == 6 ? ContactDepthOrder::Reversed : ContactDepthOrder::Conflicting;
    order = order == ContactDepthOrder::Unknown || order == next ? next : ContactDepthOrder::Conflicting;
  }

  constexpr ContactDepthOrder For(uint64_t now) const {
    return frame == now && order != ContactDepthOrder::Conflicting ? order : ContactDepthOrder::Unknown;
  }
};

// Each copy must preserve texel coordinates. Accept a whole depth copy or a contiguous
// top-to-bottom series of tiles from the same version of a source. Never join two different
// depth writes merely because the host reuses the same texture.
struct ContactDepthCoverage {
  uint64_t frame = 0;
  const void* source = nullptr;
  Extent size;
  ContactDepthOrder order = ContactDepthOrder::Unknown;
  uint32_t rows = 0, revision = 0;

  constexpr void Copy(uint64_t now, const void* src, Extent extent, ContactDepthOrder z, uint32_t rev,
                      const ResolveRegion& rect) {
    if (!src || extent.Empty() || (z != ContactDepthOrder::Forward && z != ContactDepthOrder::Reversed) ||
        rect.Empty() || rect.left != 0 || rect.dest_x != 0 || rect.right != extent.width ||
        rect.dest_y != rect.top || rect.bottom > extent.height) {
      *this = {};
      return;
    }
    if (rect.top == 0) *this = {now, src, extent, z, rect.bottom, rev};
    else if (frame == now && source == src && size == extent && order == z && revision == rev && rect.top == rows)
      rows = rect.bottom;
    else *this = {};
  }

  constexpr bool Ready(uint64_t now) const { return source && frame == now && rows == size.height; }
};

// Matches the native layout's existing 24-byte push range. Quality is interpreted on CPU;
// the shader gets a bounded sample budget and handles either device-depth convention.
struct ContactRayPush {
  uint32_t depth_index, width, height, quality;
  float strength;
  uint32_t reversed;
};
static_assert(sizeof(ContactRayPush) == 24);

}  // namespace gtav::native
