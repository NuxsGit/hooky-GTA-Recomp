#pragma once

#include "world.h"

#include <array>
#include <span>

namespace gtav::sandbox::wire {

// TCP packets: uint16 body size (network order), protocol version, kind, fieldwise body.
// The server accepts only Hello and Pose. It never reads files or executes guest calls.
using Bytes = std::vector<uint8_t>;
using Invite = std::array<uint8_t, 16>;
inline constexpr uint8_t kVersion = 1;
inline constexpr size_t kMaxPacket = 1024;

enum class Kind : uint8_t { Hello = 1, Pose, World };
struct Frame { Kind kind; Bytes payload; };

class Framing {
 public:
  bool Push(std::span<const uint8_t> bytes);
  // Empty without failed() means the next packet is incomplete.
  std::optional<Frame> Pop();
  bool failed() const { return failed_; }
 private:
  Bytes bytes_;
  bool failed_ = false;
};

Bytes Hello(const Invite& invite, std::string_view name);
bool ReadHello(const Frame& f, Invite& invite, std::string& name);
Bytes Pose(std::optional<Point> point);
bool ReadPose(const Frame& f, std::optional<Point>& point);
Bytes Snapshot(const World& world);
std::optional<World> ReadSnapshot(const Frame& f);

bool MatchInvite(const Invite& expected, const Invite& received);
std::string FormatInvite(const Invite& invite);
std::optional<Invite> ParseInvite(std::string_view code);
}  // namespace gtav::sandbox::wire
