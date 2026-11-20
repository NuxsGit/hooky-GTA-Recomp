#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace gtav::native {

// One optional driver call per guest frame. A second guest poll never waits for the driver.
// The device owner publishes after initialization and stops/drains before unloading the device.
class InputPacingGate {
 public:
  void Enable() {
    auto expected = State::Unavailable;
    state_.compare_exchange_strong(expected, State::Ready, std::memory_order_release, std::memory_order_relaxed);
  }

  bool Ready() const { return state_.load(std::memory_order_acquire) == State::Ready; }

  template <typename Eligible, typename Pace>
  void TryPace(uint64_t frame, Eligible eligible, Pace pace) {
    if (!Ready()) return;
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock || !Ready() || (last_frame_ && frame <= *last_frame_) || !eligible()) return;
    last_frame_ = frame;
    pace();
  }

  void Stop() {
    state_.store(State::Stopped, std::memory_order_release);
    // Wait only at orderly shutdown, while the renderer can still submit/present.
    std::lock_guard drain(mutex_);
  }

 private:
  enum class State { Unavailable, Ready, Stopped };
  std::atomic<State> state_{State::Unavailable};
  std::mutex mutex_;
  std::optional<uint64_t> last_frame_;
};

}  // namespace gtav::native
