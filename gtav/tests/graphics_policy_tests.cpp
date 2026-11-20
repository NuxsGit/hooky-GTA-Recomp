// Asset-free checks of the policy shared by the renderer and the desktop presets.
#include "graphics_fixture.h"
#include "native/graphics_policy.h"
#include "native/presentation.h"
#include "native/resolve_policy.h"

#include <limits>

using namespace gtav::native;

int main() {
  constexpr uint64_t limit = 8192;
  CHECK(SceneScale(1.5, 1, true, limit) == 1.5f);
  CHECK(SceneScale(2.0, 1, true, limit) == 1.5f);
  CHECK(SceneScale(1.5, 1, true, 4096) == 1.0f);  // shrunken max VkImage2D, not a bad allocation
  CHECK(SceneScale(1.5, 0.75f, false, limit) == 1.0f);
  CHECK(SceneScale(0.25, 1, true, limit) == kMinSceneScale);
  CHECK(SceneScale(0.333333, 1, true, limit) == kExtraPotatoSceneScale);
  CHECK(SceneScale(std::numeric_limits<double>::quiet_NaN(), 0.75f, false, limit) == 0.75f);
  for (const float scale : {0.5f, 0.75f, 1.0f, 1.5f}) {
    CHECK(TargetScale(scale, 320, 2) == 1.0f);   // water pre-pass 4x MSAA
    CHECK(TargetScale(scale, 640, 0) == 1.0f);   // its 1x alias, including depth
    CHECK(TargetScale(scale, 128, 0) == 1.0f);   // wave simulation
    CHECK(TargetScale(scale, 1280, 2) == 1.0f);  // don't replace guest MSAA
    CHECK(TargetScale(scale, 1280, 0) == scale);
  }
  auto shape = [](float scale) { return Extent{ScaledTexels(1280, scale), ScaledTexels(720, scale)}; };
  CHECK(shape(1.5f) == Extent{1920, 1080} && shape(0.5f) == Extent{640, 360});
  {
    auto e = shape(kExtraPotatoSceneScale);
    CHECK(e.width < 640 && e.height == 240);
  }
  // Full-width colour and depth resolves share host texels with their source, including tiled
  // destination points. Both scales are applied to this guest rect only once.
  const ResolveRegion guest{0, 60, 1280, 360, 0, 360};
  const auto large = ScaleResolveRegion(guest, 1.5f, 1920, 1080);
  const auto small = ScaleResolveRegion(guest, 0.5f, 640, 360);
  CHECK(large.top == 90 && large.right == 1920 && large.bottom == 540 && large.dest_y == 540);
  CHECK(small.top == 30 && small.right == 640 && small.bottom == 180 && small.dest_y == 180);

  QueryCoverage coverage;
  coverage.Draw(1.5f);
  CHECK(coverage.GuestSamples(225) == 100 && coverage.GuestSamples(0) == 0);
  CHECK(coverage.GuestSamples(1) == 1);  // a visible sample must not get rounded away
  coverage.Draw(1.0f);
  CHECK(coverage.GuestSamples(225) == 225);

  constexpr auto standard = MakeGraphicsMemoryPolicy(false, 64 * kMiB);
  constexpr auto potato = MakeGraphicsMemoryPolicy(true, 64 * kMiB);
  constexpr auto extra = MakeGraphicsMemoryPolicy(true, true, 64 * kMiB);
  CHECK(standard.frame_slots == 2 && NextFrameSlot(0, standard) == 1 && NextFrameSlot(1, standard) == 0);
  CHECK(potato.frame_slots == 1 && NextFrameSlot(0, potato) == 0 && potato.lean);
  CHECK(extra.frame_slots == 1 && extra.extra_potato && extra.lean);
  CHECK(potato.budget_interval < standard.budget_interval && potato.min_idle < standard.min_idle);
  CHECK(extra.budget_interval < potato.budget_interval && extra.min_idle < potato.min_idle);
  CHECK(potato.texture_budget_mib == 192 && potato.buffer_budget_mib == 64 && standard.buffer_budget_mib == 0);
  CHECK(extra.texture_budget_mib == 96 && extra.buffer_budget_mib == 32);
  CHECK(potato.upload_chunk == 16 * kMiB && standard.upload_chunk == 64 * kMiB);
  CHECK(extra.upload_chunk == 8 * kMiB);
  CHECK(MakeGraphicsMemoryPolicy(false, 32 * kMiB).upload_chunk == 32 * kMiB);
  // Cache accounting observes replacements and removals, rather than repeatedly polling the
  // heap. An unlimited buffer budget is not a 0 MiB allowance.
  CacheUsage usage{0, uint64_t(potato.texture_budget_mib) * kMiB};
  usage.Replace(0, 180 * kMiB);
  usage.Replace(0, 12 * kMiB);
  CHECK(!usage.OverTarget());
  usage.Replace(12 * kMiB, 25 * kMiB);
  CHECK(usage.bytes == 205 * kMiB && usage.OverTarget());
  usage.Replace(25 * kMiB, 0);
  CHECK(!usage.OverTarget() && !usage.Trimmed());
  usage.Replace(180 * kMiB, 153 * kMiB);
  CHECK(usage.Trimmed());
  usage.target = 0;
  CHECK(!usage.OverTarget());
  const uint64_t cap = potato.upload_chunk;
  CHECK(AlignUpload(13, 512) == 512 && AlignUpload(512, 512) == 512);
  CHECK(FitsUpload(cap, cap - 256, 256) && !FitsUpload(cap, cap - 256, 257));
  CHECK(!FitsUpload(cap, cap + 1, 0));
  const uint64_t large_upload = 37 * kMiB + 16;
  CHECK(UploadCapacity(cap, 4096) == cap);
  CHECK(!FitsUpload(cap, 0, large_upload) && FitsUpload(UploadCapacity(cap, large_upload), 0, large_upload));
  CHECK(OutputPreset(ParseOutputMode(-1, potato.lean)) == Extent{960, 540});
  CHECK(OutputPreset(ParseOutputMode(-1, standard.lean)) == Extent{1280, 720});
  CHECK(OutputPreset(ParseOutputMode(1440, potato.lean)) == Extent{2560, 1440});
  CHECK(!UseBorderless(OutputMode::k540p, {960, 540}));

  CHECK(MaterialFilterFloor(0, false) == 1 && MaterialFilterFloor(0, true) == 8);
  CHECK(MaterialFilterFloor(1, true) == 1 && MaterialFilterFloor(2, true) == 2);
  CHECK(MaterialFilterFloor(4, false) == 4 && MaterialFilterFloor(16, false) == 16);
  CHECK(MaterialFilterFloor(-1, true) == 8 && MaterialFilterFloor(17, false) == 1);
  CHECK(MaterialAnisotropy(16, 8, true, true, true, 1, 16) == 16);
  CHECK(MaterialAnisotropy(16, 8, true, true, true, 1, 1) == 1);  // unsupported adapter
  CHECK(MaterialAnisotropy(16, 8, true, true, true, 1, 2) == 2);  // Potato or explicit cap
  CHECK(MaterialAnisotropy(4, 8, true, true, true, 16, 16) == 16);  // guest filtering preserved
  CHECK(MaterialAnisotropy(8, 8, true, true, true, 2, 16) == 8);
  CHECK(MaterialAnisotropy(8, 8, true, true, true, 16, 16) == 16);
  CHECK(MaterialAnisotropy(8, 8, true, true, true, 2, 4) == 4);
  CHECK(MaterialAnisotropy(1, 8, true, true, true, 2, 16) == 2);
  CHECK(MaterialAnisotropy(8, 0, true, true, true, 1, 16) == 1);  // resolve / R32F / depth
  CHECK(MaterialAnisotropy(8, 1, true, true, true, 1, 16) == 1);  // no mip chain
  CHECK(MaterialAnisotropy(8, 8, false, true, true, 1, 16) == 1); // clamp/border/lookup
  CHECK(MaterialAnisotropy(8, 8, true, false, true, 1, 16) == 1); // nearest
  CHECK(MaterialAnisotropy(8, 8, true, true, false, 1, 16) == 1); // basemap or fixed LOD
  std::puts("Graphics policy: scale, water alias isolation, resolve regions, GPU slots, upload sizes and texture filtering passed");
}
