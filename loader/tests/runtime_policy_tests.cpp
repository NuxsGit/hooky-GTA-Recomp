#include "debug_control.h"
#include "frame_pacing.h"

#include <cstdlib>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << "Failed: " #x " (line " << __LINE__ << ")\n"; std::exit(1); } } while (false)

int main() {
  gtav::GuestPacing pacing;
  auto live = pacing.Update({2, 0}, true, true);
  CHECK(live.interval == 2 && !live.audio_timeout);
  live = pacing.Update(live, false, true);
  CHECK(live.interval == 0 && live.audio_timeout == 1);
  live = pacing.Update(live, false, true);
  CHECK(live.interval == 0 && live.audio_timeout == 1);
  live = pacing.Update(live, true, true);
  CHECK(live.interval == 2 && live.audio_timeout == 0);
  live = pacing.Update({3, 1}, false, true);
  live = pacing.Update(live, false, false);
  CHECK(live.interval == 0 && live.audio_timeout == 1);  // the game had already timed out
  live = pacing.Update({4, 0}, true, true);
  CHECK(live.interval == 4 && !live.audio_timeout);  // preserve the guest's more recent update
  live = pacing.Update({0, 0}, false, true);
  live = pacing.Update(live, false, false);
  CHECK(live.interval == 0 && live.audio_timeout == 0);

  gtav::FailurePacing fail;
  CHECK(!fail.Active(0));
  CHECK(fail.Notify(2, 100) && fail.Active(20'099));
  CHECK(!fail.Notify(2, 10'000) && !fail.Active(20'100));
  CHECK(fail.Notify(3, 20'200) && !fail.Active(20'200));
  CHECK(fail.Notify(0, 21'000) && !fail.Active(21'000));
  CHECK(fail.Notify(2, 22'000) && fail.Active(22'001));
  CHECK(fail.Notify(0, 22'003) && !fail.Active(22'004));

  using namespace gtav::debug;
  CHECK(TogglePanel(Panel::Closed, Panel::Debug) == Panel::Debug);
  CHECK(TogglePanel(Panel::Debug, Panel::Debug) == Panel::Closed);
  CHECK(TogglePanel(Panel::Debug, Panel::Settings) == Panel::Settings);
  CHECK(TogglePanel(Panel::Settings, Panel::Debug) == Panel::Debug);
  Controls controls;
  controls.Arm(true);
  CHECK(!controls.Queue(Action::Give, 10));
  controls.SetOpen(true);
  CHECK(!controls.Queue(Action::Teleport, 10));
  controls.Arm(true);
  CHECK(controls.Queue(Action::Give, 20));
  CHECK(!controls.Queue(Action::Fly, 21));
  controls.SetKey(Key::Forward, true);
  auto input = controls.Consume(30);
  CHECK(input.armed && input.action == Action::Give && input.Axis(Key::Forward, Key::Back) == 1);
  CHECK(controls.StillArmed(input.epoch));
  CHECK(!controls.Consume(31).action);
  CHECK(controls.Queue(Action::Teleport, 35));
  CHECK(!controls.Consume(2036).action);  // stale UI command never executes when loading finishes
  CHECK(controls.Queue(Action::Fly, 2040));
  CHECK(controls.Queue(Action::Stop, 2041));
  CHECK(controls.Consume(2042).action == Action::Stop);
  CHECK(controls.Queue(Action::Freecam, 2100));
  controls.SetOpen(false);
  controls.SetOpen(true);
  const auto reopened = controls.Consume(2110);
  CHECK(!controls.StillArmed(input.epoch));
  CHECK(!reopened.armed && !reopened.action && !reopened.keys && reopened.epoch != input.epoch);
  controls.Arm(true);
  CHECK(controls.Queue(Action::Give, 2120));
  controls.Reset();
  CHECK(!controls.Consume(2121).action && !controls.Armed());
}
