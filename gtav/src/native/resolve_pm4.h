// Evidence from a single D3D Resolve's immediate PM4 writes. This is not a command processor:
// anything that requires inspecting another buffer, inherited copy state or a predicate is unknown.
#pragma once

#include "resolve_policy.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace gtav::native {

enum class ResolveScan { kNotRequested, kUnavailable, kNoCopy, kUnverified, kDirectCopy };

struct ResolvePackets {
  ResolveScan scan = ResolveScan::kNotRequested;
  uint32_t control = 0;
  std::optional<uint32_t> surface, depth, depth_clear;
};

struct ResolveProbe {
  ResolvePackets packets;
  uint32_t surface_after = 0, depth_after = 0;
  bool same_depth_surface = false;
};

constexpr char const* ResolveScanName(ResolveScan scan) {
  switch (scan) {
    case ResolveScan::kNotRequested: return "off";
    case ResolveScan::kUnavailable: return "unavailable";
    case ResolveScan::kNoCopy: return "no-copy";
    case ResolveScan::kUnverified: return "unverified";
    case ResolveScan::kDirectCopy: return "direct";
  }
  return "unknown";
}

inline ResolvePackets ScanResolvePackets(std::span<const uint8_t> bytes) {
  ResolvePackets unverified;
  unverified.scan = ResolveScan::kUnverified;
  if (bytes.size() % 4 != 0) return unverified;
  auto be = [&](size_t i) {
    const uint8_t* p = bytes.data() + i * 4;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  };
  ResolvePackets state, result;
  result.scan = ResolveScan::kNoCopy;
  std::optional<uint32_t> mode, control;
  auto write = [&](uint32_t reg, uint32_t value) {
    switch (reg) {
      case 0x2000: state.surface = value; break;     // RB_SURFACE_INFO
      case 0x2002: state.depth = value; break;       // RB_DEPTH_INFO
      case 0x2208: mode = value; break;              // RB_MODECONTROL
      case 0x2318: control = value; break;           // RB_COPY_CONTROL
      case 0x231D: state.depth_clear = value; break; // RB_DEPTH_CLEAR
    }
  };
  const size_t words = bytes.size() / 4;
  for (size_t i = 0; i < words;) {
    const uint32_t header = be(i++), type = header >> 30;
    if (!header || type == 2) continue;  // padding
    const uint32_t count = type == 1 ? 2 : ((header >> 16) & 0x3FFF) + 1;
    if (count > words - i) return unverified;
    if (type == 0) {
      const uint32_t reg = header & 0x7FFF;
      for (uint32_t n = 0; n < count; ++n) write(reg + ((header & 0x8000) ? 0 : n), be(i + n));
    } else if (type == 3) {
      if (header & 1) return unverified;  // predicated, e.g. a tile of a tiled frame
      switch ((header >> 8) & 0x7F) {
        case 0x2D:  // SET_CONSTANT, bank 4 = registers relative to 0x2000
        case 0x55:  // SET_CONSTANT2, absolute register index
        case 0x56: { // SET_SHADER_CONSTANTS, same layout
          const uint32_t base = be(i);
          const bool relative = ((header >> 8) & 0x7F) == 0x2D;
          if (relative && ((base >> 16) & 0xFF) != 4) break;
          const uint32_t reg = relative ? 0x2000 + (base & 0x7FF) : base & 0xFFFF;
          for (uint32_t n = 1; n < count; ++n) write(reg + n - 1, be(i + n));
          break;
        }
        case 0x22:  // DRAW_INDX (has a viz-query condition word before the initiator)
          if (count < 2 || be(i) != 0) return unverified;
          [[fallthrough]];
        case 0x36:  // DRAW_INDX_2
          if (result.scan == ResolveScan::kDirectCopy || !control || !mode || (*mode & 7) != 6)
            return unverified;
          result = state;  // use the state at the copy, not writes restoring it after the draw
          result.scan = ResolveScan::kDirectCopy;
          result.control = *control;
          break;
        case 0x10: case 0x26: case 0x3C: case 0x46: case 0x58: case 0x59:
          break;  // NOP, waits, events (no writes to these registers)
        default:
          return unverified;
      }
    }  // Type 1 can only write registers below 0x800.
    i += count;
  }
  return result;
}

struct ResolveDepthClear {
  float z;
  uint32_t stencil;
};

inline float ResolveDepthValue(uint32_t packed, bool float24) {
  const uint32_t d = packed >> 8;
  if (!float24) return float(d) / 16777215.0f;
  const uint32_t exponent = d >> 20;
  const uint32_t mantissa = (d & 0xFFFFF) | (exponent ? 0x100000 : 0);
  // This replay stores guest depth without the emulator's [0,2)->[0,1) remap.
  return std::ldexp(float(mantissa), int(exponent ? exponent : 1) - 35);
}

// Verify TU27's API flag against the emitted copy control. Don't guess the stack slot of the
// stencil argument or apply a temporary/tiled depth target to the logical full-size surfaces.
inline std::optional<ResolveDepthClear> VerifiedResolveClear(uint32_t flags, uint32_t surface, uint32_t depth,
                                                             const ResolveProbe& probe) {
  const auto& p = probe.packets;
  constexpr uint32_t source_and_clear = 7 | kResolveClearDepthStencil;
  if ((flags & kResolveClearDepthStencil) == 0 || p.scan != ResolveScan::kDirectCopy || !p.depth_clear ||
      ((p.control ^ flags) & source_and_clear) != 0 || ((p.control >> 20) & 3) >= 2 ||
      (surface & 0x3FFF) == 0 || ((surface >> 16) & 3) != 0 || !probe.same_depth_surface ||
      surface != probe.surface_after || depth != probe.depth_after ||
      p.surface.value_or(surface) != surface || p.depth.value_or(depth) != depth)
    return std::nullopt;
  const float z = ResolveDepthValue(*p.depth_clear, (depth & (1u << 16)) != 0);
  if (z > 1.0f) return std::nullopt;
  return ResolveDepthClear{z, *p.depth_clear & 0xFF};
}

inline std::optional<ResolveDepthClear> ReplayResolveDepthClear(uint32_t flags, float z, uint32_t surface,
                                                                 uint32_t depth, const ResolveProbe& probe,
                                                                 bool experiment) {
  if (experiment)
    if (auto verified = VerifiedResolveClear(flags, surface, depth, probe)) return verified;
  // Preserve the old clear for supported depth copies when the experiment is off or inconclusive.
  const auto ops = DecodeResolve(flags);
  if (ops.copy_depth && ops.clear_depth && z >= 0.0f && z <= 1.0f) return ResolveDepthClear{z, 0};
  return std::nullopt;
}

}  // namespace gtav::native
