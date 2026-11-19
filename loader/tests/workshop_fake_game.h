#pragma once

#include "core/WorkshopCore.h"

#include <cstdlib>
#include <iostream>
#include <tuple>
#include <utility>

#define CHECK(x) do { if (!(x)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

namespace workshop_test {
using namespace gtav::workshop;
inline constexpr Entity kPed{1, Kind::Ped};

struct FakeGame final : WorkshopGame {
  struct Instance { Entity entity; Pose pose; bool anchor, ghost; };
  std::map<uint32_t, Instance> entities{{1, {kPed, {{0, 0, 1}, 0}, false, false}}};
  std::vector<SpawnSpec> made;
  std::vector<std::tuple<Tool, Entity, uint32_t>> tools;
  std::vector<std::tuple<Effect, Entity, bool>> changes;
  std::vector<Entity> moved;
  std::vector<Motion> flight;
  mutable std::function<void()> on_features;
  std::vector<std::pair<Rule, bool>> rules;
  std::optional<Entity> driver;
  unsigned requests{}, releases{}, creates{}, shots_suppressed{};
  bool loaded = true, ground_ready = true, no_pointers = false;
  uint32_t model_in_use{}, fail = 0, next = 10;
  Capabilities Features() const override {
    if (auto notify = std::exchange(on_features, {})) notify();
    auto features = (1u << unsigned(Feature::Count)) - 1u;
    if (no_pointers) features &= ~(1u << unsigned(Feature::Ground));
    return {features, (1u << unsigned(Effect::Count)) - 1, (1u << unsigned(Tool::Count)) - 1};
  }
  bool RequestModel(const SpawnSpec& spec) override {
    CHECK(!model_in_use);
    ++requests; model_in_use = spec.model; return true;
  }
  Load PollModel(uint32_t hash) override {
    CHECK(model_in_use == hash);
    return loaded ? Load::Ready : Load::Waiting;
  }
  Entity Create(const SpawnSpec& s) override {
    CHECK(model_in_use == s.model);
    ++creates;
    if (s.model == fail) return {};
    made.push_back(s);
    Entity e{next++, s.kind};
    entities[e.handle] = {e, s.pose, s.locked, s.ghost};
    return e;
  }
  bool Exists(Entity e) override {
    const auto it = entities.find(e.handle);
    return it != entities.end() && it->second.entity == e;
  }
  bool Destroy(Entity e) override { if (Exists(e)) entities.erase(e.handle); return true; }
  void ReleaseModel(uint32_t hash) override { CHECK(hash == model_in_use); ++releases; model_in_use = 0; }
  std::optional<Pose> Locate(Entity e) override { return Exists(e) ? std::optional(entities.at(e.handle).pose) : std::nullopt; }
  std::optional<Ray> Camera(Lens lens) override { return Ray{{lens.x, lens.y, 2}, {0, 1, 0}}; }
  std::optional<float> Ground(Vec3) override { return ground_ready ? std::optional(1.f) : std::nullopt; }
  bool MoveGhost(Entity e, Pose p) override {
    CHECK(Exists(e) && entities.at(e.handle).ghost);
    entities.at(e.handle).pose = p; return true;
  }
  bool Anchor(Entity e, bool on) override { CHECK(Exists(e)); entities.at(e.handle).anchor = on; return true; }
  bool SetEffect(Effect e, Entity entity, bool on) override {
    CHECK(Exists(entity) || (e == Effect::Sprint && !on));
    changes.emplace_back(e, entity, on); return true;
  }
  bool Fly(Effect, Entity e, Motion motion, float) override {
    CHECK(Exists(e)); flight.push_back(motion); return true;
  }
  bool Perform(Tool tool, Entity e, uint32_t v) override {
    CHECK(Exists(e) || ((!e) && (tool == Tool::PauseClock || tool == Tool::Gravity || tool == Tool::Weather)));
    tools.emplace_back(tool, e, v); return true;
  }
  std::optional<Entity> TravelEntity(Entity e, bool can_drive) override {
    if (!Exists(e)) return {};
    if (e == kPed && driver && can_drive) return driver;
    return e;
  }
  std::optional<Vec3> Waypoint() override { return Vec3{100, 200, 301}; }
  std::optional<Vec3> StreamGround(Vec3 at, unsigned probe) override {
    return probe >= 2 && ground_ready ? std::optional(at) : std::nullopt;
  }
  bool Relocate(Entity e, Vec3 at) override {
    CHECK(Exists(e)); moved.push_back(e); entities.at(e.handle).pose.at = at; return true;
  }
  void WorldRule(Rule r, bool enabled) override { rules.emplace_back(r, enabled); }
  void SuppressFire(Entity e) override { CHECK(Exists(e)); ++shots_suppressed; }
};
}  // namespace workshop_test
