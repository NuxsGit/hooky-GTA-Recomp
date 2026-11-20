#include "input_pacing.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::abort(); } } while (false)

int main() {
  using namespace std::chrono_literals;
  using gtav::native::InputPacingGate;
  InputPacingGate gate;
  std::atomic<int> calls{0};
  auto active = [] { return true; };
  auto pace = [&] { ++calls; };
  gate.TryPace(0, active, pace);
  CHECK(calls == 0);  // no device, failed driver activation or off
  gate.Enable();
  gate.TryPace(0, [] { return false; }, pace);
  CHECK(calls == 0);  // focus/visibility or an overlay; don't claim the frame
  gate.TryPace(0, active, pace);
  gate.TryPace(0, active, pace);
  gate.TryPace(7, active, pace);
  gate.TryPace(6, active, pace);
  CHECK(calls == 2);

  std::promise<void> entered, release, stopped;
  auto started_call = entered.get_future();
  auto driver_return = release.get_future();
  auto safely_unload = stopped.get_future();
  std::thread guest([&] {
    gate.TryPace(8, active, [&] {
      entered.set_value();
      driver_return.wait();
      pace();
    });
  });
  CHECK(started_call.wait_for(2s) == std::future_status::ready);

  // Other polls can read their devices while the first guest is blocked in the driver.
  gate.TryPace(8, active, pace);
  gate.TryPace(9, active, pace);
  CHECK(calls == 2);
  std::thread shutdown([&] {
    gate.Stop();
    stopped.set_value();
  });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (gate.Ready() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  CHECK(!gate.Ready());
  CHECK(safely_unload.wait_for(0ms) == std::future_status::timeout);
  gate.TryPace(10, active, pace);
  release.set_value();
  CHECK(safely_unload.wait_for(2s) == std::future_status::ready);
  guest.join();
  shutdown.join();
  CHECK(calls == 3);

  gate.Enable();  // renderer initialization racing shutdown may not re-arm the gate
  gate.TryPace(11, active, pace);
  CHECK(!gate.Ready() && calls == 3);
}
