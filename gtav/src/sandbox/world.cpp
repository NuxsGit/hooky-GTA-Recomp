#include "world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace gtav::sandbox {
namespace {
constexpr int64_t kCheckpointRadiusCm = 600;
constexpr int64_t kMinSpacingCm = 1500;
}

bool ValidName(std::string_view name, size_t limit) {
  if (name.empty() || name.size() > limit || name.front() == ' ' || name.back() == ' ') return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == ' ' || c == '-' || c == '_';
  });
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Point::Valid() const {
  return x >= -kWorldLimitCm && x <= kWorldLimitCm && y >= -kWorldLimitCm && y <= kWorldLimitCm &&
         z >= -kWorldLimitCm && z <= kWorldLimitCm;
}

std::optional<Point> Point::FromMeters(float x, float y, float z) {
  constexpr float bound = kWorldLimitCm / 100.f;
  if (!(std::abs(x) <= bound && std::abs(y) <= bound && std::abs(z) <= bound)) return std::nullopt;
  return Point{int32_t(std::round(x * 100)), int32_t(std::round(y * 100)), int32_t(std::round(z * 100))};
}

int64_t DistanceSquared(Point a, Point b) {
  const int64_t x = int64_t(a.x) - b.x, y = int64_t(a.y) - b.y, z = int64_t(a.z) - b.z;
  return x * x + y * y + z * z;  // points have already passed Valid()
}

bool Course::Valid() const {
  if (!ValidName(name, 32) || points.size() > kMaxCheckpoints) return false;
  for (size_t i = 0; i < points.size(); ++i) {
    if (!points[i].Valid() || (i && DistanceSquared(points[i - 1], points[i]) < kMinSpacingCm * kMinSpacingCm))
      return false;
  }
  return true;
}

Room::Room(std::string owner) { Add(std::move(owner)); }

uint32_t Room::Add(std::string name) {
  if (players_.size() == kMaxPlayers || !ValidName(name) || next_id_ == 0) return 0;
  const uint32_t id = next_id_++;
  players_.push_back({{id, std::move(name), std::nullopt, false, 0, 0}, 0});
  return id;
}

void Room::Remove(uint32_t id) {
  if (id != 1) std::erase_if(players_, [id](const Entry& e) { return e.player.id == id; });
}

void Room::Pose(uint32_t id, std::optional<Point> p, int64_t now) {
  if (p && !p->Valid()) return;
  for (auto& entry : players_) {
    auto& player = entry.player;
    if (player.id != id) continue;
    player.pose = p;
    entry.last_pose = now;
    if (race_ == Race::Running && now >= start_at_ && now - start_at_ < kRaceLimitMs &&
        p && player.racing && player.passed < course_.points.size() &&
        DistanceSquared(*p, course_.points[player.passed]) <= kCheckpointRadiusCm * kCheckpointRadiusCm) {
      if (++player.passed == course_.points.size())
        player.finish_ms = uint32_t(std::clamp(now - start_at_, int64_t{1}, kRaceLimitMs));
    }
    return;
  }
}

bool Room::SetCourse(Course course) {
  if (race_ == Race::Countdown || race_ == Race::Running || !course.Valid()) return false;
  Reset();
  course_ = std::move(course);
  return true;
}

bool Room::Start(int64_t now) {
  if (race_ == Race::Countdown || race_ == Race::Running || course_.points.size() < 2) return false;
  const auto ready = [now](const Entry& entry) {
    return entry.player.pose && now - entry.last_pose <= kPoseLifetimeMs;
  };
  if (std::none_of(players_.begin(), players_.end(), ready)) return false;
  Reset();
  for (auto& entry : players_) entry.player.racing = ready(entry);
  start_at_ = now + kCountdownMs;
  race_ = Race::Countdown;
  return true;
}

void Room::Reset() {
  race_ = Race::Editing;
  for (auto& entry : players_) {
    entry.player.racing = false;
    entry.player.passed = 0;
    entry.player.finish_ms = 0;
  }
}

void Room::Tick(int64_t now) {
  if (race_ == Race::Countdown && now >= start_at_) race_ = Race::Running;
  if (race_ == Race::Running && (now - start_at_ >= kRaceLimitMs ||
      std::none_of(players_.begin(), players_.end(), [](const Entry& p) { return p.player.racing && !p.player.finish_ms; }))) {
    race_ = Race::Finished;
    end_at_ = now;
  }
}

World Room::View(uint32_t self, int64_t now) const {
  World result{self, course_, race_, 0, {}};
  if (race_ == Race::Countdown) result.clock_ms = uint32_t(std::clamp(start_at_ - now, int64_t{0}, kCountdownMs));
  if (race_ == Race::Running || race_ == Race::Finished) {
    const auto clock = (race_ == Race::Running ? now : end_at_) - start_at_;
    result.clock_ms = uint32_t(std::clamp(clock, int64_t{0}, kRaceLimitMs));
  }
  for (const auto& entry : players_) {
    result.players.push_back(entry.player);
    result.clock_ms = std::max(result.clock_ms, entry.player.finish_ms);
    if (now - entry.last_pose > kPoseLifetimeMs) result.players.back().pose.reset();
  }
  return result;
}
}  // namespace gtav::sandbox
