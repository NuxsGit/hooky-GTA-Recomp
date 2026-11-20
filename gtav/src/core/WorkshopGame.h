#pragma once

#include "EntitySpawnQueue.h"
#include "GamemodeManager.h"

namespace gtav::workshop {

enum class Effect : uint8_t { Noclip, Fly, God, Stamina, Jump, Sprint, Wanted, InfiniteAmmo, Count };
// Sent by the authority, never by a direct call from a window or network handler.
enum class Tool : uint8_t {
  Heal, Damage, Revive, Weapon, Warp, Repair, Clean, Clock, PauseClock, Weather, Gravity, Count
};
enum class Feature : uint8_t {
  Prop, Vehicle, Ped, Ghost, Anchor, Ground, Teleport, Waypoint,
  Traffic, Peds, Police, PvP, FallDamage, ClockRead, Count
};
struct Capabilities {
  uint32_t features{}, effects{}, tools{};
  bool Has(Feature f) const { return f < Feature::Count && (features & (1u << unsigned(f))); }
  bool Has(Effect e) const { return e < Effect::Count && (effects & (1u << unsigned(e))); }
  bool Has(Tool t) const { return t < Tool::Count && (tools & (1u << unsigned(t))); }
};
inline Feature ForKind(Kind kind) {
  switch (kind) {
    case Kind::Ped: return Feature::Ped;
    case Kind::Vehicle: return Feature::Vehicle;
    case Kind::Prop: return Feature::Prop;
  }
  return Feature::Count;
}
inline Feature ForRule(Rule r) {
  switch (r) {
    case Rule::Traffic: return Feature::Traffic;
    case Rule::Peds: return Feature::Peds;
    case Rule::Police: return Feature::Police;
    case Rule::PvP: return Feature::PvP;
    case Rule::FallDamage: return Feature::FallDamage;
    default: return Feature::Count;
  }
}
struct Lens { float x{}, y{}, aspect = 16.f / 9.f; };  // -1..1 in the game viewport
// A transport can back this interface with host entities instead of PPC handles. By default no
// optional operation succeeds. Return only results actually supported by the adapter.
class WorkshopGame : public SpawnBackend {
 public:
  virtual Capabilities Features() const = 0;
  virtual std::optional<Pose> Locate(Entity) = 0;
  virtual std::optional<Ray> Camera(Lens) { return {}; }
  virtual std::optional<Vec3> RayHit(const Ray&, float) { return {}; }
  virtual std::optional<float> Ground(Vec3) { return {}; }
  virtual bool MoveGhost(Entity, Pose) { return false; }
  virtual bool Anchor(Entity, bool) { return false; }
  virtual bool Perform(Tool, Entity, uint32_t = 0) { return false; }
  virtual bool SetEffect(Effect, Entity, bool) { return false; }
  virtual bool Fly(Effect, Entity, Motion, float) { return false; }
  virtual std::optional<Entity> TravelEntity(Entity, bool) { return {}; }
  virtual std::optional<Vec3> Waypoint() { return {}; }
  virtual std::optional<Vec3> StreamGround(Vec3, unsigned) { return {}; }
  virtual bool Relocate(Entity, Vec3) { return false; }
  virtual void WorldRule(Rule, bool) {}
  virtual void SuppressFire(Entity) {}
  virtual std::optional<unsigned> Hour() { return {}; }
};
}  // namespace gtav::workshop
