#include "CheatController.h"

#include <iterator>

namespace gtav::workshop {
namespace {
constexpr uint32_t Flag(Effect e) { return 1u << unsigned(e); }
bool FrameEffect(Effect e) { return e == Effect::Jump || e == Effect::Stamina || e == Effect::Wanted; }
}
bool CheatController::Toggle(PlayerId id, Effect e, bool on) {
  if (!id || e >= Effect::Count) return false;
  auto& bits = players_[id].want.active;
  if (on && e == Effect::Noclip) bits &= ~Flag(Effect::Fly);
  if (on && e == Effect::Fly) bits &= ~Flag(Effect::Noclip);
  bits = on ? bits | Flag(e) : bits & ~Flag(e);
  return true;
}
bool CheatController::Speed(PlayerId id, unsigned speed) {
  if (!id || (speed != 1 && speed != 3 && speed != 8 && speed != 15)) return false;
  players_[id].want.speed = speed;
  return true;
}
CheatState CheatController::State(PlayerId id) const {
  auto it = players_.find(id);
  return it != players_.end() ? it->second.want : CheatState{};
}
void CheatController::Reset(PlayerId id) {
  if (auto it = players_.find(id); it != players_.end()) it->second.want = {};
}
void CheatController::Tick(WorkshopGame& game, PlayerId id, Entity ped, bool allowed, Motion motion) {
  auto it = players_.find(id);
  if (it == players_.end()) return;
  auto& track = it->second;
  if (!allowed || (track.ped && ped != track.ped)) track.want = {};
  const bool same = track.ped == ped;
  for (unsigned i = 0; i < unsigned(Effect::Count); ++i) {
    const auto effect = Effect(i);
    if (!(track.applied & Flag(effect)) || (same && track.want.Has(effect))) continue;
    // Sprint is a player setting rather than part of the old ped. It still needs to return
    // to 1.0 after a model switch/despawn, even if the old entity is no longer in the pool.
    if ((effect != Effect::Sprint && !game.Exists(track.ped)) || game.SetEffect(effect, track.ped, false))
      track.applied &= ~Flag(effect);
  }
  // Don't drop ownership of a modified handle if a reversal fails temporarily.
  if (track.applied & ~track.want.active) return;  // retry cleanup before enabling a conflicting effect
  track.ped = ped;
  if (!allowed || !ped || !track.want.active || !game.Exists(ped)) return;
  const auto caps = game.Features();
  for (unsigned i = 0; i < unsigned(Effect::Count); ++i) {
    const auto effect = Effect(i);
    if (!track.want.Has(effect)) continue;
    const bool frame = FrameEffect(effect);
    if (!caps.Has(effect) || ((frame || !(track.applied & Flag(effect))) && !game.SetEffect(effect, ped, true))) {
      Toggle(id, effect, false);
      continue;
    }
    if (!frame) track.applied |= Flag(effect);
    if ((effect == Effect::Fly || effect == Effect::Noclip) && !game.Fly(effect, ped, motion, float(track.want.speed)))
      Toggle(id, effect, false);
  }
}
void CheatController::Forget(WorkshopGame& game, PlayerId id) {
  auto it = players_.find(id);
  if (it == players_.end()) return;
  Tick(game, id, {}, false, {});
  if (!it->second.applied) players_.erase(it);
  else it->second.retiring = true;
}
void CheatController::CleanupRetired(WorkshopGame& game) {
  for (auto it = players_.begin(); it != players_.end();) {
    const auto next = std::next(it);
    if (it->second.retiring) Forget(game, it->first);
    it = next;
  }
}
bool CheatController::NeedsTick() const {
  return std::any_of(players_.begin(), players_.end(), [](const auto& p) {
    return p.second.want.active || p.second.applied;
  });
}
}  // namespace gtav::workshop
