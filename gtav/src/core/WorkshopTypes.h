#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace gtav::workshop {
using PlayerId = uint32_t;
using PieceId = uint32_t;
inline constexpr PlayerId kLocalOwner = 1;

enum class Role : uint8_t { Player, Admin, Owner };
enum class Mode : uint8_t { Creative, Survival };
enum class Kind : uint8_t { Ped = 1, Vehicle = 2, Prop = 3 };
enum class Seat : uint8_t { Local, Simulated, Remote };

const char* Label(Role role);
const char* Label(Mode mode);
// Identities are supplied by the authority, not taken from display names or commands.
bool ValidIdentity(std::string_view text);
bool ValidText(std::string_view text, size_t limit);
std::string_view Trim(std::string_view text);

struct Vec3 {
  float x{}, y{}, z{};
  bool Valid(float bound = 20000.f) const {
    return std::abs(x) <= bound && std::abs(y) <= bound && std::abs(z) <= bound;
  }
  Vec3 operator+(Vec3 b) const { return {x + b.x, y + b.y, z + b.z}; }
  Vec3 operator-(Vec3 b) const { return {x - b.x, y - b.y, z - b.z}; }
  Vec3 operator*(float f) const { return {x * f, y * f, z * f}; }
  float Dot(Vec3 b) const { return x * b.x + y * b.y + z * b.z; }
  bool operator==(const Vec3&) const = default;
};
struct Pose {
  Vec3 at;
  float yaw{};
  bool Valid() const { return at.Valid() && yaw >= 0 && yaw < 360; }
  bool operator==(const Pose&) const = default;
};
struct Entity {
  uint32_t handle{};
  Kind kind = Kind::Ped;
  explicit operator bool() const { return handle != 0 && handle != UINT32_MAX; }
  bool operator==(const Entity&) const = default;
};
struct Ray { Vec3 origin, direction; };
struct Motion { float forward{}, right{}, up{}; };

constexpr uint32_t ModelHash(std::string_view name) {
  uint32_t h = 0;
  for (uint8_t c : name) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    h += c; h += h << 10; h ^= h >> 6;
  }
  h += h << 3; h ^= h >> 11;
  return h + (h << 15);
}
}  // namespace gtav::workshop
