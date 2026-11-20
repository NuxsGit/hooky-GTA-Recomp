#pragma once

#include "../core/WorkshopGame.h"

namespace gtav::tu27 { class NativeCall; }

namespace gtav::workshop {
struct FlightState { Entity ped; Vec3 velocity; };
// One adapter per safe script tick. Only the smoothed flight velocity survives to the next
// frame; it owns no guest pointer or reference to the native invocation scratch.
class Tu27Workshop final : public WorkshopGame {
 public:
  Tu27Workshop(tu27::NativeCall&, Entity local, bool extended, FlightState&, float dt);
  Capabilities Features() const override { return caps_; }
  bool RequestModel(const SpawnSpec&) override;
  Load PollModel(uint32_t) override;
  Entity Create(const SpawnSpec&) override;
  bool Exists(Entity) override;
  bool Destroy(Entity) override;
  void ReleaseModel(uint32_t) override;
  std::optional<Pose> Locate(Entity) override;
  std::optional<Ray> Camera(Lens) override;
  std::optional<float> Ground(Vec3) override;
  bool MoveGhost(Entity, Pose) override;
  bool Anchor(Entity, bool) override;
  bool Perform(Tool, Entity, uint32_t) override;
  bool SetEffect(Effect, Entity, bool) override;
  bool Fly(Effect, Entity, Motion, float) override;
  std::optional<Entity> TravelEntity(Entity, bool) override;
  std::optional<Vec3> Waypoint() override;
  std::optional<Vec3> StreamGround(Vec3, unsigned) override;
  bool Relocate(Entity, Vec3) override;
  void WorldRule(Rule, bool) override;
  void SuppressFire(Entity) override;
  std::optional<unsigned> Hour() override;
 private:
  std::optional<bool> Check(Entity);
  std::optional<unsigned> PlayerIndex(Entity, bool reset = false);
  std::optional<float> GroundProbe(Vec3);
  tu27::NativeCall& native_;
  Entity local_;
  bool extended_;
  FlightState& flight_;
  float dt_;
  Capabilities caps_;
};
}  // namespace gtav::workshop
