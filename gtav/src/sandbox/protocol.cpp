#include "protocol.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace gtav::sandbox::wire {
namespace {
struct Writer {
  Bytes bytes{0, 0, kVersion, 0};
  void U8(uint8_t val) { bytes.push_back(val); }
  void U32(uint32_t val) {
    for (int i = 24; i >= 0; i -= 8) U8(uint8_t(val >> i));
  }
  void Text(std::string_view text) {
    U8(uint8_t(text.size()));
    bytes.insert(bytes.end(), text.begin(), text.end());
  }
  void Coord(Point p) { U32(uint32_t(p.x)); U32(uint32_t(p.y)); U32(uint32_t(p.z)); }
  void OptionalPoint(std::optional<Point> p) { U8(p.has_value()); if (p) Coord(*p); }
  Bytes Finish(Kind kind) {
    if (bytes.size() > kMaxPacket) return {};
    const auto size = bytes.size() - 2;
    bytes[0] = uint8_t(size >> 8);
    bytes[1] = uint8_t(size);
    bytes[3] = uint8_t(kind);
    return std::move(bytes);
  }
};

struct Reader {
  std::span<const uint8_t> bytes;
  bool ok = true;
  uint8_t U8() {
    if (bytes.empty()) { ok = false; return 0; }
    const uint8_t val = bytes.front();
    bytes = bytes.subspan(1);
    return val;
  }
  uint32_t U32() {
    uint32_t val = 0;
    for (int i = 0; i < 4; ++i) val = (val << 8) | U8();
    return val;
  }
  bool Bool() {
    const auto val = U8();
    if (val > 1) ok = false;
    return val != 0;
  }
  std::string Name(size_t limit) {
    const size_t size = U8();
    if (!size || size > limit || size > bytes.size()) { ok = false; return {}; }
    std::string name(bytes.begin(), bytes.begin() + size);
    bytes = bytes.subspan(size);
    if (!ValidName(name, limit)) ok = false;
    return name;
  }
  Point Coord() {
    Point point{std::bit_cast<int32_t>(U32()), std::bit_cast<int32_t>(U32()), std::bit_cast<int32_t>(U32())};
    if (!point.Valid()) ok = false;
    return point;
  }
  std::optional<Point> OptionalPoint() { return Bool() ? std::optional(Coord()) : std::nullopt; }
  bool Done() const { return ok && bytes.empty(); }
};
}  // namespace

bool Framing::Push(std::span<const uint8_t> bytes) {
  if (failed_ || bytes.size() > 4 * kMaxPacket - bytes_.size()) { failed_ = true; return false; }
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  return true;
}

std::optional<Frame> Framing::Pop() {
  if (failed_ || bytes_.size() < 2) return std::nullopt;
  const size_t size = (size_t(bytes_[0]) << 8) | bytes_[1];
  if (size < 2 || size > kMaxPacket - 2) { failed_ = true; return std::nullopt; }
  if (bytes_.size() < size + 2) return std::nullopt;
  if (bytes_[2] != kVersion || bytes_[3] < uint8_t(Kind::Hello) || bytes_[3] > uint8_t(Kind::World)) {
    failed_ = true;
    return std::nullopt;
  }
  auto end = bytes_.begin() + size + 2;
  Frame frame{Kind(bytes_[3]), Bytes(bytes_.begin() + 4, end)};
  bytes_.erase(bytes_.begin(), end);
  return frame;
}

Bytes Hello(const Invite& invite, std::string_view name) {
  if (!ValidName(name)) return {};
  Writer w;
  w.bytes.insert(w.bytes.end(), invite.begin(), invite.end());
  w.Text(name);
  return w.Finish(Kind::Hello);
}

bool ReadHello(const Frame& f, Invite& invite, std::string& name) {
  if (f.kind != Kind::Hello) return false;
  Reader r{f.payload};
  for (auto& value : invite) value = r.U8();
  name = r.Name(20);
  return r.Done();
}

Bytes Pose(std::optional<Point> point) {
  if (point && !point->Valid()) return {};
  Writer w;
  w.OptionalPoint(point);
  return w.Finish(Kind::Pose);
}

bool ReadPose(const Frame& f, std::optional<Point>& point) {
  if (f.kind != Kind::Pose) return false;
  Reader r{f.payload};
  point = r.OptionalPoint();
  return r.Done();
}

Bytes Snapshot(const World& world) {
  if (!world.course.Valid() || world.players.empty() || world.players.size() > kMaxPlayers) return {};
  Writer w;
  w.U32(world.self);
  w.U8(uint8_t(world.race));
  w.U32(world.clock_ms);
  w.Text(world.course.name);
  w.U8(uint8_t(world.course.points.size()));
  for (auto p : world.course.points) w.Coord(p);
  w.U8(uint8_t(world.players.size()));
  for (const auto& player : world.players) {
    if (!ValidName(player.name) || (player.pose && !player.pose->Valid())) return {};
    w.U32(player.id);
    w.Text(player.name);
    w.OptionalPoint(player.pose);
    w.U8(player.racing);
    w.U8(player.passed);
    w.U32(player.finish_ms);
  }
  return w.Finish(Kind::World);
}

std::optional<World> ReadSnapshot(const Frame& f) {
  if (f.kind != Kind::World) return std::nullopt;
  Reader r{f.payload};
  World w;
  w.self = r.U32();
  const auto race = r.U8();
  w.clock_ms = r.U32();
  if (race > uint8_t(Race::Finished) || !w.self || w.clock_ms > kRaceLimitMs ||
      (race == uint8_t(Race::Countdown) && w.clock_ms > kCountdownMs)) return std::nullopt;
  w.race = Race(race);
  w.course.name = r.Name(32);
  const auto points = r.U8();
  if (points > kMaxCheckpoints || (w.race != Race::Editing && points < 2)) return std::nullopt;
  for (unsigned i = 0; i < points; ++i) w.course.points.push_back(r.Coord());
  if (!w.course.Valid()) return std::nullopt;
  const auto players = r.U8();
  if (!players || players > kMaxPlayers) return std::nullopt;
  bool self = false, owner = false;
  for (unsigned i = 0; i < players; ++i) {
    Player p{r.U32(), r.Name(20), r.OptionalPoint(), r.Bool(), r.U8(), r.U32()};
    if (!r.ok || !p.id || p.passed > points || (p.finish_ms && (p.passed != points || p.finish_ms > w.clock_ms)) ||
        (!p.racing && (p.passed || p.finish_ms)) || (w.race == Race::Editing && p.racing) ||
        std::any_of(w.players.begin(), w.players.end(), [&](const Player& old) { return old.id == p.id; })) return std::nullopt;
    self |= p.id == w.self;
    owner |= p.id == 1;
    w.players.push_back(std::move(p));
  }
  return self && owner && r.Done() ? std::optional(std::move(w)) : std::nullopt;
}

bool MatchInvite(const Invite& expected, const Invite& received) {
  unsigned mismatch = 0;
  for (size_t i = 0; i < expected.size(); ++i) mismatch |= unsigned(expected[i] ^ received[i]);
  return mismatch == 0;
}

std::string FormatInvite(const Invite& invite) {
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result;
  result.reserve(2 * invite.size());
  for (auto byte : invite) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}

std::optional<Invite> ParseInvite(std::string_view code) {
  Invite invite{};
  if (code.size() != 2 * invite.size()) return std::nullopt;
  const auto digit = [](char c) -> int {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < invite.size(); ++i) {
    const int hi = digit(code[2 * i]), lo = digit(code[2 * i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    invite[i] = uint8_t(hi * 16 + lo);
  }
  return invite;
}
}  // namespace gtav::sandbox::wire
