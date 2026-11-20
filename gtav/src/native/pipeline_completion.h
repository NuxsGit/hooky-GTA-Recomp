#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace gtav::native {

// One result, published from a compiler worker. A ready job stays a lock-free check on replay;
// a pending job sleeps until completion or the *remaining* shared frame budget expires.
class PipelineCompletion {
 public:
  void Signal() {
    {
      std::lock_guard lock(mutex_);
      ready_.store(true, std::memory_order_release);
    }
    cv_.notify_one();
  }

  bool Wait(double budget_ms, double& waited_ms) const {
    const auto ready = [&] { return ready_.load(std::memory_order_acquire); };
    if (ready()) return true;
    if (!(budget_ms > 0)) return false;
    const auto start = std::chrono::steady_clock::now();
    using Milliseconds = std::chrono::duration<double, std::milli>;
    std::unique_lock lock(mutex_);
    const bool done = cv_.wait_until(lock, start + Milliseconds(budget_ms), ready);
    waited_ms += Milliseconds(std::chrono::steady_clock::now() - start).count();
    return done;
  }

 private:
  std::atomic<bool> ready_{false};
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
};

}  // namespace gtav::native
