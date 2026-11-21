#include "presentation.h"
#include "frame_generation_schedule.h"
#include "replay_shader.h"

#include <cstdlib>
#include <iostream>

using namespace gtav::native;

#define CHECK(...)                                                                     \
  do {                                                                                \
    if (!(__VA_ARGS__)) {                                                              \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n';           \
      std::exit(1);                                                                   \
    }                                                                                 \
  } while (false)

int main() {
  using namespace std::chrono_literals;
  using framegen::Plan;
  constexpr framegen::SourceFrame older{42, 1, true}, newer{43, 1, true};
  for (unsigned factor : {2u, 4u, 6u, 8u}) {
    const auto plan = Plan(factor, older, newer, 48ms + 7ns, 6ms);
    CHECK(plan.count == factor - 1);
    auto previous = 0ns;
    for (unsigned i = 1; i < factor; ++i) {
      const auto& insertion = plan.insertions[i - 1];
      CHECK(insertion.offset - previous >= 6ms);
      CHECK(insertion.offset == (48ms + 7ns) * i / factor);
      CHECK(insertion.phase == float(i) / float(factor));
      previous = insertion.offset;
    }
    CHECK(48ms + 7ns - previous >= 6ms);
  }
  CHECK(!Plan(3, older, newer, 48ms, 6ms).count);
  CHECK(!Plan(8, older, newer, 48ms - 1ns, 6ms).count);
  CHECK(!Plan(2, older, newer, 48ms, 0ns).count);
  CHECK(!Plan(2, older, older, 48ms, 6ms).count);
  CHECK(!Plan(2, older, {44, 1, true}, 48ms, 6ms).count);
  CHECK(!Plan(2, older, {43, 2, true}, 48ms, 6ms).count);
  CHECK(!Plan(2, older, {43, 1, false}, 48ms, 6ms).count);
  CHECK(OutputPreset(ParseOutputMode(1080)) == Extent{1920, 1080});
  CHECK(OutputPreset(ParseOutputMode(1440)) == Extent{2560, 1440});
  CHECK(OutputPreset(ParseOutputMode(2160)) == Extent{3840, 2160});
  CHECK(ParseOutputMode(0) == OutputMode::kDesktop);
  CHECK(ParseOutputMode(-1) == OutputMode::k720p);
  CHECK(ParseOutputMode(9999999) == OutputMode::k720p);
  CHECK(UseBorderless(OutputMode::k2160p, {3840, 2160}));
  CHECK(UseBorderless(OutputMode::k1080p, {1920, 1080}));
  CHECK(UseBorderless(OutputMode::k1440p, {2560, 1440}));
  CHECK(UseBorderless(OutputMode::kDesktop, {3440, 1440}));
  CHECK(!UseBorderless(OutputMode::k720p, {1280, 720}));
  CHECK(!UseBorderless(OutputMode::k2160p, {1920, 1080}));
  CHECK(WindowedClient(OutputMode::k1080p, {2400, 1200}) == Extent{1920, 1080});
  CHECK(WindowedClient(OutputMode::k2160p, {1920, 990}) == Extent{1760, 990});

  const auto wide = FitAspect({3440, 1440}, {16, 9});
  CHECK(wide.x == 440 && wide.y == 0 && wide.size == Extent{2560, 1440});
  const auto tall = FitAspect({1280, 1024}, {16, 9});
  CHECK(tall.x == 0 && tall.y == 152 && tall.size == Extent{1280, 720});
  const auto odd = FitAspect({1367, 768}, {16, 9});
  CHECK(odd.size == Extent{1365, 768} && odd.x == 1 && odd.y == 0);
  CHECK(FitAspect({}, {16, 9}).size.Empty());
  CHECK(FitAspect({1920, 1080}, {}).size.Empty());
  CHECK(FitAspect({1, 64}, {16, 9}).size == Extent{1, 1});

  CHECK(Fsr1Extent({1280, 720}, {1920, 1080}) == Extent{1920, 1080});
  CHECK(Fsr1Extent({1280, 720}, {2560, 1440}) == Extent{2560, 1440});
  CHECK(Fsr1Extent({1280, 720}, {3840, 2160}) == Extent{2560, 1440});
  CHECK(Fsr1Extent({960, 540}, {3840, 2160}) == Extent{1920, 1080});
  CHECK(Fsr1Extent({640, 360}, {1920, 1080}) == Extent{1280, 720});
  CHECK(Fsr1Extent({853, 480}, {2560, 1440}) == Extent{1706, 960});
  CHECK(Fsr1Extent({1280, 720}, {1280, 720}).Empty());
  CHECK(Fsr1Extent({1280, 720}, {1024, 576}).Empty());
  CHECK(Fsr1Extent({}, {3840, 2160}).Empty());

  CHECK(PlanFsr1({}, {0, 0, {1280, 720}}, false) == Fsr1Route::Bypass);
  CHECK(PlanFsr1({1920, 1080}, {0, 0, {1920, 1080}}, false) == Fsr1Route::DirectEasu);
  CHECK(PlanFsr1({1920, 1080}, {16, 0, {1920, 1080}}, false) == Fsr1Route::EasuThenLinear);
  CHECK(PlanFsr1({1920, 1080}, {0, 12, {1920, 1080}}, false) == Fsr1Route::EasuThenLinear);
  CHECK(PlanFsr1({1920, 1080}, {16, 0, {1920, 1080}}, true) == Fsr1Route::DirectRcas);
  CHECK(PlanFsr1({2560, 1440}, {0, 0, {3840, 2160}}, true) == Fsr1Route::RcasThenLinear);
  CHECK(PlanFsr1({2560, 1440}, {0, 0, {3840, 2160}}, false) == Fsr1Route::EasuThenLinear);

  // A header alone isn't hardware support. The feature needs both device enablement and a call
  // from the guest input path; repeated polls within a frame don't pace the GPU again.
  CHECK(DriverPacing(false, true, false, false) == AntiLagStatus::Unsupported);
  CHECK(DriverPacing(true, false, false, false) == AntiLagStatus::Off);
  CHECK(DriverPacing(true, true, false, false) == AntiLagStatus::Fallback);
  CHECK(DriverPacing(true, true, true, false) == AntiLagStatus::Ready);
  CHECK(DriverPacing(true, true, true, true) == AntiLagStatus::Pacing);
  // A genuine depth-only draw is not dropped. A color draw must not touch the depth buffer or
  // pipeline cache until its own fragment shader is available.
  CHECK(!SkipMissingPixelShader(0, false));
  CHECK(SkipMissingPixelShader(0x12345678, false));
  CHECK(!SkipMissingPixelShader(0x12345678, true));
}
