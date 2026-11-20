#pragma once

#include "WorkshopGame.h"

#include <map>

namespace gtav::workshop {
struct CheatState {
  uint32_t active{};
  unsigned speed = 1;
  bool Has(Effect e) const { return e < Effect::Count && (active & (1u << unsigned(e))); }
  float RunMultiplier() const { return Has(Effect::Sprint) ? 1.49f : 1.f; }
};
class CheatController {
 public:
  bool Toggle(PlayerId id, Effect effect, bool on);
  bool Speed(PlayerId id, unsigned speed);
  CheatState State(PlayerId id) const;
  // Clearing desired state doesn't touch natives. Tick reconciles reversals against the exact
  // previously modified entity. Frame-only flags stop being renewed immediately.
  void Reset(PlayerId id);
  void Tick(WorkshopGame&, PlayerId id, Entity ped, bool allowed, Motion input);
  void Forget(WorkshopGame&, PlayerId id);
  void CleanupRetired(WorkshopGame&);
  bool NeedsTick() const;
 private:
  struct Track { CheatState want; uint32_t applied{}; Entity ped; bool retiring{}; };
  std::map<PlayerId, Track> players_;
};
}  // namespace gtav::workshop
