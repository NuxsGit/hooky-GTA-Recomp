#include "scene.h"

namespace gtav::sandbox {
std::vector<Actor> SceneTargets(const World& world, std::optional<Presence> local) {
  std::vector<Actor> actors;
  if (!local || !local->Valid() || !world.course.Valid() || world.players.size() > kMaxPlayers) return actors;
  constexpr int64_t radius = 150 * 100;
  auto nearby = [&](Point p) { return DistanceSquared(p, local->position) <= radius * radius; };
  actors.reserve(world.players.size() + world.course.props.size());
  for (const auto& p : world.players) {
    if (!p.id || p.id == world.self || !p.pose || !p.pose->Valid() || !nearby(p.pose->position)) continue;
    const auto vehicle = p.pose->vehicle;
    const bool in_replica = vehicle > Vehicle::Foot && vehicle < Vehicle::Other;
    actors.push_back({p.id, in_replica ? Visual::Vehicle : Visual::Ped,
        in_replica ? unsigned(vehicle) - 1 : 0, p.pose->position, p.pose->heading});
  }
  for (size_t i = 0; i < world.course.props.size(); ++i) {
    const auto& p = world.course.props[i];
    if (nearby(p.position)) actors.push_back({0x100000000ull + i, Visual::Prop, unsigned(p.kind), p.position, p.heading});
  }
  return actors;
}
}  // namespace gtav::sandbox
