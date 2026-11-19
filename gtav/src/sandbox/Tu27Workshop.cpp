#include "Tu27Workshop.h"

#include "../native/tu27_call.h"
#include "BuildManager.h"

#include <bit>
#include <numbers>

namespace gtav::workshop {
namespace {
// Xbox 360 TU27 handler addresses. Check both tools/scripttools/{fn2name,handlers}.txt and
// tools/game_dll/codegen.partition.json. These are not PC native IDs or arbitrary input.
enum N : uint32_t {
  kCam = 0x82C767A0, kRotation = 0x82C76808, kFov = 0x82C76860, kGameplayCam = 0x82C76B80,
  kExists = 0x82C7B318, kDead = 0x82C9B7D0, kCoords = 0x82C9ADD8, kHeading = 0x82C9AF18, kType = 0x82C9B4E8,
  kModelOnDisc = 0x82CF0B50, kVehicleModel = 0x82CF0C48, kRequest = 0x82CF0CC0,
  kLoaded = 0x82CF0A90, kRelease = 0x82CF0AE0, kObject = 0x82D1E4B0, kPed = 0x82CE60F0,
  kCar = 0x82D5CBC8, kDelete = 0x82C9BC50, kCoordsNoOffset = 0x82C9C000, kSetHeading = 0x82C9C068,
  kCollision = 0x82C9BFA8, kDynamic = 0x82C9C048, kHasGravity = 0x82C9C2B0, kAlpha = 0x82C9C320,
  kBlockPed = 0x82D1CD70, kStand = 0x82D2FD90, kClearTask = 0x82D3D400, kDoors = 0x82D4E510,
  kInvincible = 0x82C9C088, kVelocity = 0x82C9C2A0, kHealth = 0x82C9C078, kArmor = 0x82D0C3C8,
  kClearBlood = 0x82D0F9F0, kClearDamage = 0x82D57710, kRevive = 0x82D0F7E8, kResurrect = 0x82D0F7F8,
  kId = 0x82CE9A20, kPlayerPed = 0x82CE9CA8, kStamina = 0x82CE9580, kJump = 0x82C898F8,
  kSprint = 0x82CEA418, kClearWanted = 0x82CE8B18, kNoFire = 0x82CEA5D0, kInfinite = 0x82D575D0,
  kGive = 0x82D575F0, kWeaponValid = 0x82D32E88, kMaxAmmo = 0x82D57820,
  kInCar = 0x82D0BF30, kPedCar = 0x82D0C510, kSeat = 0x82D4EC90, kPassengers = 0x82D4E2D8, kWarp = 0x82D0C3D8,
  kEngine = 0x82D4FD50, kRepair = 0x82D503C8, kDirt = 0x82D4FCB8,
  kWaypoint = 0x82C82520, kBlipFirst = 0x82C81768, kBlipExists = 0x82C824D8, kBlipPos = 0x82C81A20,
  kGround = 0x82C87798, kCollisionAt = 0x82CF0D10,
  kClock = 0x82C77D10, kPauseClock = 0x82C77D60, kHours = 0x82C77DE0, kWeather = 0x82C871C0, kWeatherClear = 0x82C6DB48,
  kGravityLevel = 0x82C88D38, kTraffic = 0x82D315C0, kTrafficRandom = 0x82D315F8,
  kParked = 0x82D31618, kPeds = 0x82CE6270, kPedsScenario = 0x82CE6280, kDispatch = 0x82C89080,
};
uint32_t F(float f) { return std::bit_cast<uint32_t>(f); }
constexpr float kDeg = std::numbers::pi_v<float> / 180.f;
Vec3 V(tu27::Vec v) { return {v.x, v.y, v.z}; }
constexpr uint32_t Driver = UINT32_MAX;
}
Tu27Workshop::Tu27Workshop(tu27::NativeCall& call, Entity local, bool extended, FlightState& flight, float dt)
    : native_(call), local_(local), extended_(extended), flight_(flight), dt_(std::clamp(dt, .0f, .05f)) {
  const auto feature = [&](Feature f, std::initializer_list<uint32_t> keys, bool available = true) {
    if (available && call.Supports(keys)) caps_.features |= 1u << unsigned(f);
  };
  const auto effect = [&](Effect e, std::initializer_list<uint32_t> keys, bool available = true) {
    if (available && call.Supports(keys)) caps_.effects |= 1u << unsigned(e);
  };
  const auto tool = [&](Tool t, std::initializer_list<uint32_t> keys, bool available = true) {
    if (available && call.Supports(keys)) caps_.tools |= 1u << unsigned(t);
  };
  const bool common = call.Supports({kExists, kType, kDelete, kModelOnDisc, kRequest, kLoaded, kRelease, kSetHeading});
  feature(Feature::Prop, {kObject}, common);
  feature(Feature::Ped, {kPed, kBlockPed}, common);
  feature(Feature::Vehicle, {kCar, kVehicleModel}, common);
  feature(Feature::Anchor, {kDynamic, kHasGravity, kVelocity, kStand, kClearTask});
  feature(Feature::Ghost, {kCoordsNoOffset, kCollision, kDynamic, kHasGravity, kAlpha, kDoors, kBlockPed}, common);
  feature(Feature::Ground, {kGround, kCollisionAt}, extended);
  feature(Feature::Teleport, {kCoordsNoOffset, kInCar, kPedCar, kSeat});
  feature(Feature::Waypoint, {kWaypoint, kBlipFirst, kBlipPos, kBlipExists}, caps_.Has(Feature::Teleport) && caps_.Has(Feature::Ground) && call.Supports({kCollisionAt}));
  feature(Feature::Traffic, {kTraffic, kTrafficRandom, kParked});
  feature(Feature::Peds, {kPeds, kPedsScenario});
  feature(Feature::Police, {kDispatch});
  feature(Feature::ClockRead, {kHours});
  effect(Effect::Noclip, {kCollision, kHasGravity, kCoordsNoOffset, kRotation, kInCar}, extended);
  effect(Effect::Fly, {kVelocity, kRotation, kInCar});
  effect(Effect::God, {kInvincible});
  effect(Effect::Stamina, {kId, kPlayerPed, kStamina});
  effect(Effect::Jump, {kId, kPlayerPed, kJump});
  effect(Effect::Sprint, {kId, kPlayerPed, kSprint});
  effect(Effect::Wanted, {kId, kPlayerPed, kClearWanted});
  effect(Effect::InfiniteAmmo, {kInfinite});
  tool(Tool::Heal, {kHealth, kArmor});
  tool(Tool::Damage, {kClearBlood, kClearDamage});
  tool(Tool::Revive, {kDead, kRevive, kResurrect, kHealth, kArmor});
  tool(Tool::Weapon, {kWeaponValid, kGive});
  tool(Tool::Warp, {kInCar, kSeat, kWarp, kEngine});
  tool(Tool::Repair, {kRepair, kDirt});
  tool(Tool::Clean, {kDirt});
  tool(Tool::Clock, {kClock});
  tool(Tool::PauseClock, {kPauseClock});
  tool(Tool::Weather, {kWeather, kWeatherClear}, extended);
  tool(Tool::Gravity, {kGravityLevel});
}

std::optional<bool> Tu27Workshop::Check(Entity e) {
  if (!e) return false;
  auto call = native_.CleanupCall();
  const bool present = call.Flag(kExists, {e.handle});
  if (!call.Okay()) return {};
  const bool match = present && call.Read(kType, {e.handle}) == uint32_t(e.kind);
  return call.Okay() ? std::optional(match) : std::nullopt;
}
bool Tu27Workshop::Exists(Entity e) { return Check(e).value_or(false); }
bool Tu27Workshop::Destroy(Entity e) {
  const auto present = Check(e);
  if (!present || !*present) return present.has_value();
  auto call = native_.CleanupCall();
  // Do not delete the quick-spawn car from under its driver/passengers (including Undo).
  if (e.kind == Kind::Vehicle && call.Supports({kSeat, kPassengers}) &&
      (call.Read(kSeat, {e.handle, Driver, 0}) || call.Read(kPassengers, {e.handle}) || !call.Okay())) return false;
  return call.Invoke(kDelete, {}, nullptr, e.handle) && Check(e) == false;
}
bool Tu27Workshop::RequestModel(const SpawnSpec& spec) {
  if (!caps_.Has(ForKind(spec.kind))) return false;
  const auto* known = FindModel(spec.model);
  if (!known || known->kind != spec.kind) return false;
  auto call = native_.CleanupCall();
  return call.Flag(kModelOnDisc, {spec.model}) &&
      (spec.kind != Kind::Vehicle || call.Flag(kVehicleModel, {spec.model})) && call.Invoke(kRequest, {spec.model});
}
Load Tu27Workshop::PollModel(uint32_t model) {
  auto call = native_.CleanupCall();
  const bool loaded = call.Flag(kLoaded, {model});
  return !call.Okay() ? Load::Rejected : loaded ? Load::Ready : Load::Waiting;
}
void Tu27Workshop::ReleaseModel(uint32_t model) { native_.CleanupCall().Invoke(kRelease, {model}); }
Entity Tu27Workshop::Create(const SpawnSpec& spec) {
  if (!spec.pose.Valid() || !caps_.Has(ForKind(spec.kind)) ||
      (spec.ghost ? !caps_.Has(Feature::Ghost) : spec.locked && !caps_.Has(Feature::Anchor))) return {};
  auto call = native_.CleanupCall();
  const auto [x, y, z] = spec.pose.at;
  const auto h = F(spec.pose.yaw);
  Entity e{0, spec.kind};
  switch (spec.kind) {
    case Kind::Ped: e.handle = call.Read(kPed, {4, spec.model, F(x), F(y), F(z), h, 0, 1}); break;
    case Kind::Prop: e.handle = call.Read(kObject, {spec.model, F(x), F(y), F(z), 0, 0, 0}); break;
    case Kind::Vehicle: e.handle = call.Read(kCar, {spec.model, F(x), F(y), F(z), h, 0, 1}); break;
  }
  if (!call.Okay() || !Exists(e)) return {};
  if (spec.kind == Kind::Ped) call.Invoke(kBlockPed, {e.handle, 1});
  if (spec.kind == Kind::Prop) call.Invoke(kSetHeading, {e.handle, h});
  if (spec.ghost) {
    if (spec.kind == Kind::Vehicle) call.Invoke(kDoors, {e.handle, 2});
    call.Invoke(kCollision, {e.handle, 0, 0});
    call.Invoke(kAlpha, {e.handle, 160, 0});
    call.Invoke(kDynamic, {e.handle, 0});
    call.Invoke(kHasGravity, {e.handle, 0});
  }
  if (call.Okay() && (spec.ghost || !spec.locked || Anchor(e, true))) return e;
  Destroy(e);
  return {};
}
std::optional<Pose> Tu27Workshop::Locate(Entity e) {
  if (!Exists(e)) return {};
  auto call = native_.CleanupCall();
  const auto at = V(call.Vector(kCoords, {e.handle, 0}));
  const auto yaw = call.Float(kHeading, {e.handle});
  if (!call.Okay() || !at.Valid() || !std::isfinite(yaw)) return {};
  return Pose{at, std::fmod(std::fmod(yaw, 360.f) + 360.f, 360.f)};
}
std::optional<Ray> Tu27Workshop::Camera(Lens lens) {
  if (std::abs(lens.x) > 1.f || std::abs(lens.y) > 1.f || !(lens.aspect >= .5f && lens.aspect <= 4.f)) return {};
  auto call = native_.CleanupCall();
  if (!call.Flag(kGameplayCam)) return {};
  const auto origin = V(call.Vector(kCam));
  const auto rot = V(call.Vector(kRotation, {2}));
  const auto fov = call.Float(kFov);
  if (!call.Okay() || !origin.Valid() || !rot.Valid(360.f) || !(fov > 5 && fov < 140)) return {};
  const float p = rot.x * kDeg, h = rot.z * kDeg;
  const float s = std::sin(h), c = std::cos(h), sp = std::sin(p), cp = std::cos(p);
  const Vec3 ahead{-s * cp, c * cp, sp}, right{c, s, 0}, up{s * sp, -c * sp, cp};
  const float half = std::tan(.5f * fov * kDeg);
  Vec3 dir = ahead + right * (lens.x * half * lens.aspect) + up * (lens.y * half);
  dir = dir * (1.f / std::sqrt(dir.Dot(dir)));
  return dir.Valid(1.f) ? std::optional(Ray{origin, dir}) : std::nullopt;
}
std::optional<float> Tu27Workshop::GroundProbe(Vec3 at) {
  if (!at.Valid() || !caps_.Has(Feature::Ground)) return {};
  auto call = native_.CleanupCall();
  // The TU27 call uses 4 words (XYZ + output); do not copy the PC ignore-water argument.
  auto z = call.ReadOutput(kGround, {F(at.x), F(at.y), F(at.z), 0}, 3);
  if (!z || !(std::abs(std::bit_cast<float>(*z)) <= 20000.f)) return {};
  return std::bit_cast<float>(*z);
}
std::optional<float> Tu27Workshop::Ground(Vec3 at) { return GroundProbe(at + Vec3{0, 0, 6.f}); }
bool Tu27Workshop::MoveGhost(Entity e, Pose pose) {
  return pose.Valid() && Relocate(e, pose.at) && native_.CleanupCall().Invoke(kSetHeading, {e.handle, F(pose.yaw)});
}
bool Tu27Workshop::Anchor(Entity e, bool on) {
  if (!caps_.Has(Feature::Anchor) || !Exists(e) || e == local_) return false;
  auto call = native_.CleanupCall();
  if (on) call.Invoke(kVelocity, {e.handle, 0, 0, 0});
  if (e.kind == Kind::Ped) {
    if (on) call.Invoke(kStand, {e.handle, UINT32_MAX});
    else call.Invoke(kClearTask, {e.handle});
  }
  return call.Invoke(kDynamic, {e.handle, !on}) && call.Invoke(kHasGravity, {e.handle, !on});
}
std::optional<unsigned> Tu27Workshop::PlayerIndex(Entity e, bool reset) {
  if (!reset && (!e || e != local_ || !Exists(e))) return {};
  auto call = native_.CleanupCall();
  const auto id = call.Read(kId);
  const auto ped = call.Read(kPlayerPed);
  return call.Okay() && id < 32 && ped && (reset || ped == e.handle) ? std::optional(id) : std::nullopt;
}
bool Tu27Workshop::SetEffect(Effect effect, Entity e, bool on) {
  if (effect != Effect::Sprint && !Exists(e)) return false;
  if (on && !caps_.Has(effect)) return false;
  auto call = native_.CleanupCall();
  switch (effect) {
    case Effect::Noclip:
      if (on && (e != local_ || call.Flag(kInCar, {e.handle, 0}) || !call.Okay())) return false;
      // On foot only, using the same completely-disable collision wrapper as Workshop ghosts.
      if (!call.Invoke(kCollision, {e.handle, !on, 0})) return false;
      if (call.Invoke(kHasGravity, {e.handle, !on})) return true;
      native_.CleanupCall().Invoke(kCollision, {e.handle, 1, 0});
      return false;
    case Effect::Fly:
      flight_ = {};
      return !on || (e == local_ && !call.Flag(kInCar, {e.handle, 0}) && call.Okay());
    case Effect::God: return call.Invoke(kInvincible, {e.handle, on});
    case Effect::InfiniteAmmo: return call.Invoke(kInfinite, {e.handle, on});
    default: break;
  }
  const auto id = PlayerIndex(e, effect == Effect::Sprint && !on);
  if (!id) return false;
  switch (effect) {
    case Effect::Sprint: return call.Invoke(kSprint, {*id, F(on ? 1.49f : 1.f)});
    case Effect::Stamina: return !on || call.Invoke(kStamina, {*id});
    case Effect::Jump: return !on || call.Invoke(kJump, {*id});
    case Effect::Wanted: return !on || call.Invoke(kClearWanted, {*id});
    default: return false;
  }
}
bool Tu27Workshop::Fly(Effect type, Entity e, Motion in, float speed) {
  auto call = native_.CleanupCall();
  if (!Exists(e) || e != local_ || call.Flag(kInCar, {e.handle, 0}) || !call.Flag(kGameplayCam)) return false;
  const auto rot = V(call.Vector(kRotation, {2}));
  if (!call.Okay() || !rot.Valid(360.f)) return false;
  if (flight_.ped != e) flight_ = {e, {}};
  const float p = rot.x * kDeg, h = rot.z * kDeg;
  const float s = std::sin(h), c = std::cos(h);
  Vec3 v = Vec3{-s * std::cos(p), c * std::cos(p), std::sin(p)} * in.forward + Vec3{c, s, 0} * in.right + Vec3{0, 0, in.up};
  const float len = std::sqrt(v.Dot(v));
  if (!std::isfinite(len) || !(speed >= 1 && speed <= 15)) return false;
  if (len > 1) v = v * (1 / len);
  if (type == Effect::Noclip) {
    const auto at = V(call.Vector(kCoords, {e.handle, 0}));
    if (!call.Okay() || !at.Valid()) return false;
    if (len == 0) return call.Invoke(kVelocity, {e.handle, 0, 0, 0});
    return Relocate(e, at + v * (4.f * speed * dt_));
  }
  // Retain existing collision and gravity. Ease towards the camera-relative velocity.
  const auto goal = v * (3.f * speed);  // distinct rates, bounded to 45 m/s
  const auto smooth = 1.f - std::exp(-9.f * dt_);
  flight_.velocity = flight_.velocity + (goal - flight_.velocity) * smooth;
  const auto [x, y, z] = flight_.velocity;
  return call.Invoke(kVelocity, {e.handle, F(x), F(y), F(z)});
}

bool Tu27Workshop::Perform(Tool tool, Entity e, uint32_t value) {
  if (!caps_.Has(tool)) return false;
  auto call = native_.CleanupCall();
  if (tool >= Tool::Clock) {
    // UI world commands supply the current local ped; runtime-owned cleanup uses no handle.
    if (e && (e != local_ || !Exists(e))) return false;
    constexpr std::string_view weathers[]{"EXTRASUNNY", "CLEAR", "CLOUDS", "RAIN", "THUNDER", "SNOW", "HALLOWEEN"};
    switch (tool) {
      case Tool::Clock: return value < 24 && call.Invoke(kClock, {value, 0, 0});
      case Tool::PauseClock: return call.Invoke(kPauseClock, {bool(value)});
      case Tool::Weather:
        return value == UINT32_MAX && !e ? call.Invoke(kWeatherClear) :
            value < std::size(weathers) && call.InvokeString(kWeather, weathers[value]);
      case Tool::Gravity: return call.Invoke(kGravityLevel, {bool(value)});
      default: return false;
    }
  }
  if (!Exists(e)) return false;
  if (tool == Tool::Repair || tool == Tool::Clean || tool == Tool::Warp) {
    if (e.kind != Kind::Vehicle) return false;
    if (tool == Tool::Warp) {
      if (local_.handle != value || !Exists(local_) || call.Flag(kInCar, {value, 0}) || call.Read(kSeat, {e.handle, Driver, 0}) != 0 ||
          !call.Okay()) return false;
      return call.Invoke(kWarp, {value, e.handle, Driver}) && call.Invoke(kEngine, {e.handle, 1, 1, 0});
    }
    return (tool != Tool::Repair || call.Invoke(kRepair, {e.handle})) && call.Invoke(kDirt, {e.handle, F(0.f)});
  }
  if (e.kind != Kind::Ped) return false;
  switch (tool) {
    case Tool::Revive:
      if (!call.Invoke(call.Flag(kDead, {e.handle, 0}) ? kResurrect : kRevive, {e.handle}) || !Exists(e)) return false;
      [[fallthrough]];
    case Tool::Heal: return call.Invoke(kHealth, {e.handle, 200}) && call.Invoke(kArmor, {e.handle, 100});
    case Tool::Damage: return call.Invoke(kClearBlood, {e.handle}) && call.Invoke(kClearDamage, {e.handle});
    case Tool::Weapon: {
      if (!KnownWeapon(value) || !call.Flag(kWeaponValid, {value}) || !call.Okay()) return false;
      unsigned ammo = 60;
      if (extended_ && call.Supports({kMaxAmmo})) {
        const auto max = call.ReadOutput(kMaxAmmo, {e.handle, value, 0}, 2);
        if (max && *max <= 100000) ammo = *max;
      }
      return native_.CleanupCall().Invoke(kGive, {e.handle, value, ammo, 0});
    }
    default: return false;
  }
}
std::optional<Entity> Tu27Workshop::TravelEntity(Entity e, bool driver) {
  if (!Exists(e) || e.kind != Kind::Ped) return {};
  auto call = native_.CleanupCall();
  const bool in = call.Flag(kInCar, {e.handle, 0});
  if (!call.Okay()) return {};
  if (!in) return e;
  if (!driver) return {};
  const Entity car{call.Read(kPedCar, {e.handle, 0}), Kind::Vehicle};
  if (!call.Okay() || !Exists(car) || call.Read(kSeat, {car.handle, Driver, 0}) != e.handle || !call.Okay()) return {};
  return car;
}
std::optional<Vec3> Tu27Workshop::Waypoint() {
  if (!caps_.Has(Feature::Waypoint)) return {};
  auto call = native_.CleanupCall();
  if (!call.Flag(kWaypoint)) return {};
  const auto blip = call.Read(kBlipFirst, {8});
  if (!blip || !call.Flag(kBlipExists, {blip}) || !call.Okay()) return {};
  const auto pos = V(call.Vector(kBlipPos, {blip}));
  return call.Okay() && pos.Valid() ? std::optional(pos) : std::nullopt;
}
std::optional<Vec3> Tu27Workshop::StreamGround(Vec3 at, unsigned probe) {
  if (!at.Valid() || !caps_.Has(Feature::Teleport) || !caps_.Has(Feature::Ground)) return {};
  constexpr float offsets[]{2.f, 8.f, 16.f, 35.f, 75.f, 150.f, 300.f, 800.f, 1200.f};
  const float z = at.z + offsets[probe % std::size(offsets)];
  if (probe % 3 == 0) native_.CleanupCall().Invoke(kCollisionAt, {F(at.x), F(at.y), F(z)});
  if (auto floor = GroundProbe({at.x, at.y, z}); floor && *floor <= z) return Vec3{at.x, at.y, *floor + .75f};
  return {};
}
bool Tu27Workshop::Relocate(Entity e, Vec3 at) {
  return at.Valid() && Exists(e) && native_.CleanupCall().Invoke(kCoordsNoOffset,
      {e.handle, F(at.x), F(at.y), F(at.z), 0, 0, 1});
}
void Tu27Workshop::WorldRule(Rule r, bool on) {
  auto call = native_.CleanupCall();
  const auto mult = F(on ? 1.f : 0.f);
  if (r == Rule::Traffic) {
    call.Invoke(kTraffic, {mult}); call.Invoke(kTrafficRandom, {mult}); call.Invoke(kParked, {mult});
  } else if (r == Rule::Peds) {
    call.Invoke(kPeds, {mult}); call.Invoke(kPedsScenario, {mult, mult});
  } else if (r == Rule::Police) {
    // Dispatch services 1/2/4 are police cars/helicopters/SWAT, not ambulance or fire services.
    for (const unsigned service : {1u, 2u, 4u, 6u, 7u, 8u, 9u, 10u, 12u, 13u}) call.Invoke(kDispatch, {service, on});
  }
}
void Tu27Workshop::SuppressFire(Entity e) {
  if (auto id = PlayerIndex(e); id && native_.Supports({kNoFire})) native_.CleanupCall().Invoke(kNoFire, {*id, 1});
}
std::optional<unsigned> Tu27Workshop::Hour() {
  auto call = native_.CleanupCall();
  const auto h = call.Read(kHours);
  return call.Okay() && h < 24 ? std::optional(h) : std::nullopt;
}
}  // namespace gtav::workshop
