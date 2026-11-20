// Opt-in removal of the TU27 fog strip identified in docs/HANDOFF_MAC_2026-10-29.md.
#pragma once

#include "presentation.h"
#include "xenos_regs.h"

namespace gtav::native {

inline constexpr uint64_t kTu27FogStripPs = 0x9DFE7F549A326A57ull;

class FogFilter {
 public:
  bool BeginFrame(bool reduced) {
    const bool changed = reduced != info_.reduced;
    info_ = {reduced};
    return changed;
  }

  bool Skip(uint64_t ps, bool drawable, const uint8_t* regs, bool in_query) {
    if (!info_.reduced || ps != kTu27FogStripPs || !drawable) return false;
    namespace x = xenos_regs;
    const uint32_t mask = x::Read(regs, x::RB_COLOR_MASK);
    const uint32_t dc = x::Read(regs, x::RB_DEPTHCONTROL);
    // A read-only depth/stencil test is OK, but never suppress a depth write, stencil
    // operation other than KEEP with writes enabled, MRT draw, or occlusion query sample.
    const bool stencil_writes = (dc & 1) &&
        (x::Read(regs, x::RB_STENCILREFMASK) & 0xFF0000) &&
        (((dc >> 11) & 0x1FF) || ((dc & 0x80) && ((dc >> 23) & 0x1FF)));
    if (in_query || (dc & 4) || stencil_writes || (mask & ~0xFu) || !(mask & 7) ||
        (x::Read(regs, x::RB_MODECONTROL) & 7) != 4) {
      ++info_.protected_draws;
      return false;
    }
    ++info_.suppressed;
    return true;
  }

  FogInfo Info() const { return info_; }

 private:
  FogInfo info_;
};

}  // namespace gtav::native
