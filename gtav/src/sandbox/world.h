#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gtav::sandbox {

inline constexpr unsigned kMaxPlayers = 8, kMaxCheckpoints = 24;
inline constexpr uint16_t kDefaultPort = 30127;
inline constexpr int64_t kPoseLifetimeMs = 1'500, kCountdownMs = 3'000, kRaceLimitMs = 20 * 60 * 1'000;
inline constexpr int32_t kWorldLimitCm = 2'000'000;

bool ValidName(std::string_view name, size_t limit = 20);
int64_t NowMs();

// Protocol and disk coordinates use integer centimeters, not host structs or guest pointers.
struct Point {
  int32_t x = 0, y = 0, z = 0;
  bool Valid() const;
  bool operator==(const Point&) const = default;
  static std::optional<Point> FromMeters(float x, float y, float z);
};
int64_t DistanceSquared(Point a, Point b);

struct Course {
  std::string name = "Untitled";
  std::vector<Point> points;
  bool Valid() const;
  bool operator==(const Course&) const = default;
};

enum class Race : uint8_t { Editing, Countdown, Running, Finished };
struct Player {
  uint32_t id = 0;
  std::string name;
  std::optional<Point> pose;
  bool racing = false;
  uint8_t passed = 0;
  uint32_t finish_ms = 0;  // zero until this participant completes the race
};

struct World {
  uint32_t self = 0;
  Course course;
  Race race = Race::Editing;
  uint32_t clock_ms = 0;  // countdown remaining or elapsed host milliseconds
  std::vector<Player> players;
};

// Host-owned room. Called under the session lock, never from a socket callback into the game.
class Room {
 public:
  explicit Room(std::string owner);
  uint32_t Add(std::string name);
  void Remove(uint32_t id);
  void Pose(uint32_t id, std::optional<Point> p, int64_t now);
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
