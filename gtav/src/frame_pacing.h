// Compatibility helpers shared by the TU27 pacing hook and asset-free tests.
#pragma once

#include <cstdint>
#include <optional>

namespace gtav {

struct PacingValues {
  int32_t interval;
  uint8_t audio_timeout;
};

// Restore only values we still own: the guest may update either field while uncapped.
class GuestPacing {
 public:
  PacingValues Update(PacingValues live, bool stock, bool unsync) {
    if (stock) {
      if (interval_ && live.interval == 0) live.interval = *interval_;
      interval_.reset();
    } else {
      if (!interval_ || live.interval != 0) interval_ = live.interval;
      live.interval = 0;
    }
    if (stock || !unsync) {
      if (audio_ && live.audio_timeout == 1) live.audio_timeout = *audio_;
      audio_.reset();
    } else {
      if (!audio_ || live.audio_timeout != 1) audio_ = live.audio_timeout;
      live.audio_timeout = 1;
    }
    return live;
  }

 private:
  std::optional<int32_t> interval_;
  std::optional<uint8_t> audio_;
};

// A repeated notification (even every frame) must not pin the cap forever. Normal
// exit restores the selected mode immediately; timeout changes pacing only, not script state.
struct FailurePacing {
  int state = 0;
  int64_t until_ms = 0;
  bool Notify(int next, int64_t now_ms) {
    if (next == state) return false;
    if (!next) until_ms = 0;
    else if (!state) until_ms = now_ms + 20'000;
    state = next;
    return true;
  }
  bool Active(int64_t now_ms) const { return state != 0 && now_ms < until_ms; }
};

void NoteFailureEffect(int state);
bool InFailureEffect();

}  // namespace gtav
