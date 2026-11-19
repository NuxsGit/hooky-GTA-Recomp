#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace gtav::debug {

enum class Panel { Closed, Settings, Debug, Workshop };
constexpr Panel TogglePanel(Panel shown, Panel requested) {
  return shown == requested ? Panel::Closed : requested;
}

enum class Action { Give, Teleport, Fly, Freecam, Stop };
enum class Key : unsigned { Forward, Left, Back, Right, Down, Up, LookUp, LookLeft, LookDown, LookRight };

// Native-render and window threads can request *named* actions. Only the guest script thread
// consumes them. No addresses, arbitrary native names, player IDs, or persistent cheats.
class Controls {
 public:
  struct Input {
    bool armed = false;
    uint64_t epoch = 0;
    unsigned keys = 0;
    std::optional<Action> action;
    int Axis(Key positive, Key negative) const { return Held(positive) - Held(negative); }
   private:
    int Held(Key key) const { return (keys >> unsigned(key)) & 1; }
  };

  bool Open() const { return open_.load(std::memory_order_relaxed); }
  void SetOpen(bool open) {
    std::lock_guard lock(mutex_);
    ResetLocked();
    open_.store(open, std::memory_order_relaxed);
  }
  void Reset() {
    std::lock_guard lock(mutex_);
    ResetLocked();
  }
  void Arm(bool armed) {
    std::lock_guard lock(mutex_);
    ResetLocked();
    armed_ = Open() && armed;
  }
  bool Armed() const {
    std::lock_guard lock(mutex_);
    return armed_;
  }
  bool StillArmed(uint64_t epoch) const {
    std::lock_guard lock(mutex_);
    return armed_ && epoch_ == epoch;
  }
  void SetKey(Key key, bool down) {
    std::lock_guard lock(mutex_);
    if (!armed_) return;
    const unsigned bit = 1u << unsigned(key);
    keys_ = down ? keys_ | bit : keys_ & ~bit;
  }
  bool Queue(Action action, int64_t now_ms) {
    std::lock_guard lock(mutex_);
    if (!armed_ || (pending_ && now_ms <= deadline_ && action != Action::Stop)) return false;
    pending_ = action;
    deadline_ = now_ms + 2'000;
    return true;
  }
  Input Consume(int64_t now_ms) {
    std::lock_guard lock(mutex_);
    auto action = std::exchange(pending_, std::nullopt);
    return {armed_, epoch_, keys_, now_ms <= deadline_ ? action : std::nullopt};
  }

 private:
  void ResetLocked() {
    ++epoch_;
    armed_ = false;
    keys_ = 0;
    pending_.reset();
  }
  std::atomic<bool> open_{false};
  mutable std::mutex mutex_;
  bool armed_ = false;
  uint64_t epoch_ = 0;
  unsigned keys_ = 0;
  std::optional<Action> pending_;
  int64_t deadline_ = 0;
};

}  // namespace gtav::debug
