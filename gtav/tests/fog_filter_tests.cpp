#include "native/fog_filter.h"

#include <array>
#include <cstdlib>
#include <cstdio>

using namespace gtav::native;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

int main() {
  FogFilter f;
  std::array<uint8_t, kRegShadowSize> regs{};
  // RB_COLOR_MASK = 0xF (all channels), RB_DEPTHCONTROL bits etc, MODECONTROL 4 (color)
  auto set = [&](uint32_t mask, uint32_t dc, uint32_t mode) {
    xenos_regs::Write(regs.data(), xenos_regs::RB_COLOR_MASK, mask);
    xenos_regs::Write(regs.data(), xenos_regs::RB_DEPTHCONTROL, dc);
    xenos_regs::Write(regs.data(), xenos_regs::RB_MODECONTROL, mode);
    xenos_regs::Write(regs.data(), xenos_regs::RB_STENCILREFMASK, 0);
  };
  // Reduced disabled by default.
  Check(!f.Skip(kTu27FogStripPs, true, regs.data(), false), "off by default");
  Check(!f.BeginFrame(false), "no change when still off");
  Check(f.BeginFrame(true), "enables reduced");
  Check(!f.BeginFrame(true), "no change when still reduced");
  // Now reduced enabled.
  set(0xF, 0, 4);
  Check(!f.Skip(0x1234, true, regs.data(), false), "hash mismatch");
  Check(!f.Skip(kTu27FogStripPs, false, regs.data(), false), "not drawable");
  set(0xF, 0, 4);
  Check(f.Skip(kTu27FogStripPs, true, regs.data(), false), "suppressed exact strip");
  Check(f.Info().suppressed == 1, "suppressed count");
  // Protected: depth write
  set(0xF, 4, 4);
  Check(!f.Skip(kTu27FogStripPs, true, regs.data(), false), "depth write protected");
  Check(f.Info().protected_draws == 1, "protected count depth");
  // Protected: stencil writes
  xenos_regs::Write(regs.data(), xenos_regs::RB_STENCILREFMASK, 0xFF0000);
  xenos_regs::Write(regs.data(), xenos_regs::RB_DEPTHCONTROL, 1 | (7 << 11));
  set(0xF, 1 | (7 << 11), 4);
  // need stencil ref mask still set? set overwrites, so re-set after
  xenos_regs::Write(regs.data(), xenos_regs::RB_STENCILREFMASK, 0xFF0000);
  Check(!f.Skip(kTu27FogStripPs, true, regs.data(), false), "stencil write protected");
  // Protected: MRT mask bits beyond 0xF
  set(0x1F, 0, 4);
  Check(!f.Skip(kTu27FogStripPs, true, regs.data(), false), "MRT mask protected");
  // Protected: no channels
  set(0, 0, 4);
  Check(!f.Skip(kTu27FogStripPs, true, regs.data(), false), "no channels protected");
  // Protected: wrong mode
  set(0xF, 0, 2);
  Check(!f.Skip(kTu27FogStripPs, true, regs.data(), false), "wrong mode protected");
  // Protected: active query
  set(0xF, 0, 4);
  Check(!f.Skip(kTu27FogStripPs, true, regs.data(), true), "query protected");
  Check(f.Info().protected_draws == 6, "protected count final");
  // Live toggle revert.
  Check(f.BeginFrame(false), "disable reduces");
  Check(f.Info().suppressed == 0 && f.Info().protected_draws == 0 && !f.Info().reduced, "cleared on disable");
  set(0xF, 0, 4);
  Check(!f.Skip(kTu27FogStripPs, true, regs.data(), false), "no skip when off again");
  // Re-enable clears counts.
  f.BeginFrame(true);
  set(0xF, 0, 4);
  Check(f.Skip(kTu27FogStripPs, true, regs.data(), false) && f.Info().suppressed == 1, "re-enabled works");
  std::puts("fog: exact strip suppression with protection checks passed");
  return 0;
}
