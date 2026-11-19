#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gtav::sandbox {

inline constexpr unsigned kMaxPlayers = 8, kMaxCheckpoints = 24, kMaxProps = 24;
inline constexpr uint16_t kDefaultPort = 30127, kFullTurn = 3600;
inline constexpr int64_t kPoseLifetimeMs = 1'500, kCountdownMs = 3'000, kRaceLimitMs = 20 * 60 * 1'000;
inline constexpr int32_t kWorldLimitCm = 2'000'000;

bool ValidName(std::string_view name, size_t limit = 20);
int64_t NowMs();

// Coordinates are signed centimeters, headings are tenths of a degree. No raw guest pointers or
// model hashes cross the wire or become part of a map file.
struct Point {
  int32_t x = 0, y = 0, z = 0;
  bool Valid() const;
  bool operator==(const Point&) const = default;
  static std::optional<Point> FromMeters(float x, float y, float z);
};
int64_t DistanceSquared(Point a, Point b);
uint16_t HeadingFromDegrees(float degrees);

enum class Vehicle : uint8_t { Foot, Blista, Sultan, Adder, Bati, Bmx, Other, Count };
enum class PropKind : uint8_t { Cone, Barrier, Crate, Ramp, Count };
enum class Minigame : uint8_t { Race, Hunt, Count };

struct Presence {
  Point position;
  uint16_t heading = 0;
  Vehicle vehicle = Vehicle::Foot;
  bool Valid() const;
  bool operator==(const Presence&) const = default;
};

struct Prop {
  Point position;
  uint16_t heading = 0;
  PropKind kind = PropKind::Cone;
  bool Valid() const;
  bool operator==(const Prop&) const = default;
};
std::optional<Prop> PlaceProp(Presence origin, PropKind kind, int rotation, int height_cm);

struct Course {
  std::string name = "Untitled";
  std::vector<Point> points{};
  std::vector<Prop> props{};
  Minigame game = Minigame::Race;
  bool Valid() const;
  uint32_t AllPoints() const { return (uint32_t{1} << points.size()) - 1; }  // validated, <= 24
  bool operator==(const Course&) const = default;
};

enum class Race : uint8_t { Editing, Countdown, Running, Finished };
struct Player {
  uint32_t id = 0;
  std::string name;
  std::optional<Presence> pose;
  bool racing = false;
  uint32_t checkpoints = 0;
  uint32_t finish_ms = 0;
  unsigned Passed() const { return std::popcount(checkpoints); }
};

struct World {
  uint32_t self = 0;
  Course course;
  Race race = Race::Editing;
  uint32_t clock_ms = 0;  // countdown remaining or elapsed host milliseconds
  std::vector<Player> players;
};

std::optional<size_t> NextCheckpoint(const Course& course, const Player& player);

// Host-owned room, used under the session lock. Networking never dispatches a game native.
class Room {
 public:
  explicit Room(std::string owner);
  uint32_t Add(std::string name);
  void Remove(uint32_t id);
  void Pose(uint32_t id, std::optional<Presence> p, int64_t now);
  bool SetCourse(Course course);
  bool Start(int64_t now);
  void Reset();
  void Tick(int64_t now);
  World View(uint32_t self, int64_t now) const;

 private:
  struct Entry { Player player; int64_t last_pose = 0; };
  std::vector<Entry> players_;
  uint32_t next_id_ = 1;
  Course course_;
  Race race_ = Race::Editing;
  int64_t start_at_ = 0, end_at_ = 0;
};
}  // namespace gtav::sandbox
