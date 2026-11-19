#pragma once

#include "world.h"

#include <array>

namespace gtav::sandbox {

inline constexpr std::array kPropModels = {"prop_roadcone02a", "prop_barrier_work05", "prop_box_wood02a_pu", "prop_mp_ramp_01"};
inline constexpr std::array kPropNames = {"Road cone", "Work barrier", "Wood crate", "Stunt ramp"};
inline constexpr std::array kVehicleModels = {"blista", "sultan", "adder", "bati", "bmx"};
inline constexpr std::array kVehicleNames = {"Blista", "Sultan", "Adder", "Bati 801", "BMX"};
static_assert(kPropModels.size() == size_t(PropKind::Count) && kPropModels.size() == kPropNames.size());
static_assert(kVehicleModels.size() + 1 == size_t(Vehicle::Other) && kVehicleModels.size() == kVehicleNames.size());

enum class Visual : uint8_t { Prop, Ped, Vehicle };
struct Actor {
  uint64_t key;  // never a guest entity or pointer
  Visual kind;
  unsigned model; // local palette index, not a native or model hash
  Point position;
  uint16_t heading;
  bool operator==(const Actor&) const = default;
};

// Only nearby actors should be streamed into the local world. Marks outside the streaming radius
// remain available via the map. Bad/missing local free-roam observations request no actors.
std::vector<Actor> SceneTargets(const World& world, std::optional<Presence> local);
}  // namespace gtav::sandbox
