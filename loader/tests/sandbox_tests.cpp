#include "courses.h"
#include "paths.h"
#include "scene.h"
#include "session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::cerr << "Failed at " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

namespace fs = std::filesystem;
using namespace gtav::sandbox;
const Course kTrack{"Airport Practice", {{-3200, 1500, 2400}, {12000, 1500, 2400}},
    {{{-3500, 1500, 2350}, 900, PropKind::Cone}, {{12000, 1500, 2400}, 2700, PropKind::Ramp}}};
Presence At(Point p) { return {p, 0, Vehicle::Foot}; }

const Player* Find(const World& w, uint32_t id) {
  for (const auto& p : w.players) if (p.id == id) return &p;
  return nullptr;
}

template<class F> bool Until(F condition, int64_t ms = 3500) {
  const auto until = NowMs() + ms;
  do {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (NowMs() < until);
  return false;
}

void ProtocolAndRoom() {
  CHECK(!Point::FromMeters(std::numeric_limits<float>::infinity(), 0, 0));
  CHECK(!Point::FromMeters(std::numeric_limits<float>::quiet_NaN(), 0, 0));
  CHECK(Point::FromMeters(1.25f, -2.5f, 14.f) == (Point{125, -250, 1400}));
  CHECK(!ValidName("../saVe"));
  CHECK(kTrack.Valid());
  CHECK(!(Course{"too close", {{0, 0, 0}, {100, 0, 0}}}).Valid());
  auto invite = wire::ParseInvite("0123456789ABCDEF0123456789aBcDeF");
  CHECK(invite && wire::FormatInvite(*invite) == "0123456789abcdef0123456789abcdef");
  CHECK(!wire::ParseInvite("not hex"));
  const auto hello = wire::Hello(*invite, "Driver_2");
  wire::Framing framer;
  for (size_t i = 0; i + 1 < hello.size(); ++i) {
    CHECK(framer.Push(std::span(&hello[i], size_t(1))));
    CHECK(!framer.Pop() && !framer.failed());
  }
  auto pose = wire::Pose(At(kTrack.points.front()));
  pose.insert(pose.begin(), hello.back());  // final byte of Hello and a second packet in one recv
  CHECK(framer.Push(pose));
  auto frame = framer.Pop();
  wire::Invite decoded{};
  std::string name;
  CHECK(frame && wire::ReadHello(*frame, decoded, name) && wire::MatchInvite(*invite, decoded) && name == "Driver_2");
  std::optional<Presence> position;
  frame = framer.Pop();
  CHECK(frame && wire::ReadPose(*frame, position) && position == At(kTrack.points.front()));
  CHECK(!framer.Pop() && !framer.failed());
  auto wrong_version = hello;
  ++wrong_version[2];
  CHECK(framer.Push(wrong_version) && !framer.Pop() && framer.failed());
  wire::Framing oversized;
  const uint8_t huge[] = {0xff, 0xff};
  CHECK(oversized.Push(huge) && !oversized.Pop() && oversized.failed());
  CHECK(wire::Pose(At({0, 0, INT32_MIN})).empty());

  Room room("Host");
  const uint32_t friend_id = room.Add("Driver_2");
  CHECK(friend_id == 2 && room.SetCourse(kTrack));
  CHECK(!room.Start(0));
  room.Pose(1, At(kTrack.points[1]), 100);
  room.Pose(friend_id, At(kTrack.points[0]), 100);
  CHECK(room.Start(100));
  CHECK(room.View(1, 100).race == Race::Countdown);
  CHECK(!room.SetCourse({"Other track", {}}));
  room.Pose(friend_id, At(kTrack.points[0]), 3099);  // not until green
  CHECK(Find(room.View(1, 3099), friend_id)->Passed() == 0);
  room.Tick(3100);
  room.Pose(1, At(kTrack.points[1]), 3100);  // can't skip checkpoint 1
  CHECK(Find(room.View(1, 3100), 1)->Passed() == 0);
  room.Pose(friend_id, At(kTrack.points[0]), 3100);
  room.Pose(friend_id, At(kTrack.points[1]), 5300);
  room.Pose(1, At(kTrack.points[0]), 5500);
  room.Pose(1, At(kTrack.points[1]), 8100);
  room.Tick(8100);
  auto view = room.View(friend_id, 8100);
  CHECK(view.race == Race::Finished && !Find(view, friend_id)->pose);
  CHECK(Find(view, friend_id)->finish_ms == 2200 && Find(view, 1)->finish_ms == 5000);
  wire::Framing snapshot;
  CHECK(snapshot.Push(wire::Snapshot(view)));
  frame = snapshot.Pop();
  auto replay = frame ? wire::ReadSnapshot(*frame) : std::nullopt;
  CHECK(replay && replay->self == friend_id && replay->course == kTrack);
  CHECK(frame && !wire::ReadPose(*frame, position));  // a client may not send an authoritative snapshot
  frame->payload.push_back(0);
  CHECK(!wire::ReadSnapshot(*frame));
  CHECK(room.SetCourse(kTrack));
  room.Remove(friend_id);
  for (unsigned i = 1; i < kMaxPlayers; ++i) CHECK(room.Add("Another"));
  CHECK(!room.Add("Full"));
  constexpr int64_t expired = 8100 + kPoseLifetimeMs + 1;
  CHECK(!room.Start(expired));
  CHECK(!room.View(1, expired).players.front().pose);
  room.Pose(1, At(kTrack.points[0]), expired);
  CHECK(room.Start(expired));
  room.Tick(expired + kCountdownMs + kRaceLimitMs);
  CHECK(room.View(1, expired + kCountdownMs + kRaceLimitMs).race == Race::Finished);
}

void HuntAndLayoutCodec() {
  auto hunt = kTrack;
  hunt.game = Minigame::Hunt;
  Room room("Host");
  CHECK(room.SetCourse(hunt));
  room.Pose(1, At(hunt.points[1]), 0);
  CHECK(room.Start(0));
  const auto spectator = room.Add("Late");
  room.Tick(kCountdownMs);
  room.Pose(spectator, At(hunt.points[1]), kCountdownMs);
  CHECK(!Find(room.View(spectator, kCountdownMs), spectator)->racing);
  room.Pose(1, At(hunt.points[1]), kCountdownMs);
  const auto started = room.View(1, kCountdownMs);
  CHECK(started.players[0].checkpoints == 2 && started.players[0].Passed() == 1);
  CHECK(NextCheckpoint(hunt, started.players[0]) == 0);
  CHECK(!room.SetCourse(kTrack));
  wire::Framing framed;
  CHECK(framed.Push(wire::Snapshot(started)));
  auto frame = framed.Pop();
  auto decoded = frame ? wire::ReadSnapshot(*frame) : std::nullopt;
  CHECK(decoded && decoded->course == hunt && decoded->players[0].checkpoints == 2);
  auto wrong = started;
  wrong.course.game = Minigame::Race;  // ordered race cannot advance point 2 first
  CHECK(framed.Push(wire::Snapshot(wrong)) && !wire::ReadSnapshot(*framed.Pop()));
  wrong = started;
  wrong.players[0].checkpoints |= 1 << 24;
  CHECK(framed.Push(wire::Snapshot(wrong)) && !wire::ReadSnapshot(*framed.Pop()));
  room.Pose(1, At(hunt.points[0]), kCountdownMs + 500);
  room.Tick(kCountdownMs + 500);
  CHECK(room.View(1, kCountdownMs + 500).race == Race::Finished);
  CHECK(room.View(1, kCountdownMs + 500).players[0].finish_ms == 500);

  Room large(std::string(20, 'H'));
  Course full{std::string(32, 'R'), {}, {}, Minigame::Hunt};
  for (unsigned i = 0; i < kMaxCheckpoints; ++i) full.points.push_back({int32_t(i) * 1500, 0, 0});
  full.props.resize(kMaxProps, Prop{{10, -200, 100}, 100, PropKind::Crate});
  CHECK(large.SetCourse(full));
  for (unsigned i = 1; i < kMaxPlayers; ++i) CHECK(large.Add(std::string(20, 'X')) == i + 1);
  for (unsigned i = 1; i <= kMaxPlayers; ++i) large.Pose(i, Presence{{}, 3599, Vehicle::Bmx}, 0);
  const auto bytes = wire::Snapshot(large.View(1, 0));
  CHECK(!bytes.empty() && bytes.size() <= wire::kMaxPacket && framed.Push(bytes));
  frame = framed.Pop();
  decoded = frame ? wire::ReadSnapshot(*frame) : std::nullopt;
  CHECK(decoded && decoded->course == full && decoded->players.size() == kMaxPlayers);
  for (size_t i = 0; i < frame->payload.size(); ++i) {
    auto cut = *frame;
    cut.payload.resize(i);
    CHECK(!wire::ReadSnapshot(cut));
  }
  const auto first_kind = 4 + 1 + 4 + 1 + full.name.size() + 1 + 12 * full.points.size() + 1 + 1 + 14;
  frame->payload.at(first_kind) = uint8_t(PropKind::Count);
  CHECK(!wire::ReadSnapshot(*frame));
  full.props.push_back({});
  CHECK(!full.Valid());

  const auto pose = wire::Pose(Presence{{}, 3420, Vehicle::Sultan});
  CHECK(framed.Push(pose));
  frame = framed.Pop();
  std::optional<Presence> observation;
  CHECK(frame && wire::ReadPose(*frame, observation) && observation == (Presence{{}, 3420, Vehicle::Sultan}));
  frame->payload.back() = uint8_t(Vehicle::Count);
  CHECK(!wire::ReadPose(*frame, observation));
  CHECK(wire::Pose(Presence{{}, kFullTurn, Vehicle::Foot}).empty());
}

void Scene() {
  CHECK(HeadingFromDegrees(-90.f) == 2700 && HeadingFromDegrees(359.99f) == 0);
  const Presence start{{-200, 100, 125}, 900, Vehicle::Foot};
  CHECK(PlaceProp(start, PropKind::Barrier, -180, -50) == (Prop{{-500, 100, 75}, 2700, PropKind::Barrier}));
  CHECK(!PlaceProp(start, PropKind::Barrier, -181, -50));
  CHECK(!PlaceProp(start, PropKind::Barrier, 0, 201));
  CHECK(!PlaceProp({{}, 0, Vehicle::Blista}, PropKind::Cone, 0, 0));
  CHECK(!PlaceProp({{0, kWorldLimitCm, 0}, 0, Vehicle::Foot}, PropKind::Cone, 0, 0));

  Room room("Host");
  const auto other = room.Add("Friend");
  CHECK(room.SetCourse(kTrack));
  const auto local = At(kTrack.points[0]);
  room.Pose(1, local, 1);
  room.Pose(other, Presence{{-3100, 1500, 2450}, 1250, Vehicle::Adder}, 1);
  auto world = room.View(1, 1);
  auto targets = SceneTargets(world, local);
  CHECK(targets.size() == 2);  // near Adder and cone; ramp at 152 m and self are excluded
  CHECK(targets[0] == (Actor{other, Visual::Vehicle, 2, {-3100, 1500, 2450}, 1250}));
  CHECK(targets[1].kind == Visual::Prop && targets[1].key != other);
  CHECK(SceneTargets(world, std::nullopt).empty());
  room.Pose(other, Presence{kTrack.points[0], 100, Vehicle::Other}, 10);
  CHECK(SceneTargets(room.View(1, 10), local)[0].kind == Visual::Ped);
  CHECK(SceneTargets(room.View(1, 10 + kPoseLifetimeMs + 1), local).size() == 1);
  room.Remove(other);
  CHECK(SceneTargets(room.View(1, 20), local).size() == 1);
  CHECK(room.SetCourse({}));
  CHECK(SceneTargets(room.View(1, 20), local).empty());
}

void Disk() {
  const auto root = fs::temp_directory_path() / ("gtav-workshop-fixture-" + std::to_string(NowMs()));
  WorkspaceError error{};
  CHECK(!PrepareWorkspace(root, error) && error == WorkspaceError::MissingInstall);
  CHECK(!fs::exists(root));
  const auto disc = root / "0000000000000000/545408A7/00000002";
  const auto campaign = root / "4444444444444444/545408A7/00000001";
  fs::create_directories(disc);
  fs::create_directories(campaign);
  std::ofstream(campaign / "story-save") << "preserve";
  std::ofstream(disc / "disc-data") << "read only";
  std::error_code ec;
  fs::create_directory_symlink(campaign, root / "sandbox", ec);
  if (!ec) {
    CHECK(!PrepareWorkspace(root, error) && error == WorkspaceError::SeparateFolderRequired);
    CHECK(!fs::exists(campaign / "profile"));
    fs::remove(root / "sandbox");
  }
  const auto workspace = PrepareWorkspace(root, error);
  CHECK(workspace && error == WorkspaceError::None && fs::equivalent(root, workspace->common_content_root));
  CHECK(fs::is_empty(workspace->profile_root));
  CHECK(!fs::equivalent(workspace->profile_root, campaign));
  const auto path = workspace->course_root;
  CHECK(SaveCourse(path, kTrack));
  CHECK(LoadCourse(path, kTrack.name) == kTrack);
  CHECK(!LoadCourse(path, "../campaign"));
  const auto layout_file = path / "course-Airport Practice.gtcourse";
  std::ofstream(layout_file, std::ios::binary) << "GTAV-COURSE-1\nAirport Practice\n2\n-3200 1500 2400\n12000 1500 2400\n";
  auto legacy = kTrack;
  legacy.props.clear();
  CHECK(LoadCourse(path, kTrack.name) == legacy);
  std::ofstream(layout_file, std::ios::binary) << "GTAV-COURSE-2\nAirport Practice\n0\n1 1\n0 3600 0 0 0\n";
  CHECK(!LoadCourse(path, kTrack.name));
  std::ofstream(layout_file, std::ios::binary) << "GTAV-COURSE-2\nAirport Practice\n0\n1 25\n";
  CHECK(!LoadCourse(path, kTrack.name));
  auto edited = kTrack;
  edited.points.pop_back();
  edited.game = Minigame::Hunt;
  CHECK(SaveCourse(path, edited) && LoadCourse(path, kTrack.name) == edited);
  std::ofstream(path / "course-Airport Practice.gtcourse", std::ios::app) << "not a checkpoint";
  CHECK(!LoadCourse(path, kTrack.name));
  std::ofstream(path / "course-Airport Practice.gtcourse", std::ios::binary) << std::string(5000, 'a');
  CHECK(!LoadCourse(path, kTrack.name));
  std::string untouched;
  CHECK(std::getline(std::ifstream(campaign / "story-save"), untouched) && untouched == "preserve");
  CHECK(std::getline(std::ifstream(disc / "disc-data"), untouched) && untouched == "read only");
  fs::remove_all(root);
}

void Loopback() {
  Session host, client, friend2, rejected, occupied;
  CHECK(host.SetCourse(kTrack));
  CHECK(host.Host(0, "Host", true));
  CHECK(Until([&] { return host.View().link == Link::Hosting; }));
  const auto listening = host.View();
  CHECK(listening.port && listening.invite.size() == 32);
  CHECK(occupied.Host(listening.port, "Second host", true));
  CHECK(Until([&] { return occupied.View().link == Link::Failed; }));
  auto wrong = listening.invite;
  wrong[0] = wrong[0] == 'f' ? 'e' : 'f';
  CHECK(rejected.Join("127.0.0.1", listening.port, "Wrong invite", wrong));
  CHECK(Until([&] { return rejected.View().link == Link::Failed; }));
  CHECK(host.View().world.players.size() == 1);

  CHECK(client.Join("127.0.0.1", listening.port, "Friend", listening.invite));
  CHECK(Until([&] { return client.View().link == Link::Joined; }));
  CHECK(client.LinkState() == Link::Joined && !client.View().Owner());
  CHECK(!client.SetCourse({"Cannot edit host map", {}}) && !client.StartRace());
#ifdef __linux__
  CHECK(Until([&] { return client.View().rtt_ms.has_value(); }));
  CHECK(*client.View().rtt_ms < 1000);
#endif
  const auto id = client.View().world.self;
  CHECK(id > 1 && client.View().world.course == kTrack);
  CHECK(friend2.Join("127.0.0.1", listening.port, "Friend Two", listening.invite));
  CHECK(Until([&] { return friend2.View().link == Link::Joined; }));
  const auto id2 = friend2.View().world.self;
  CHECK(id2 > 1 && id2 != id);
  CHECK(Until([&] {
    host.Publish(At(kTrack.points[0]));
    client.Publish(At(kTrack.points[0]));
    friend2.Publish(At(kTrack.points[0]));
    const auto remote = client.View().world;
    return Find(remote, id2) && Find(remote, id2)->pose == At(kTrack.points[0]) &&
        Find(remote, 1)->pose == At(kTrack.points[0]) && Find(remote, id)->pose == At(kTrack.points[0]);
  }));
  auto shared = kTrack;
  shared.props.pop_back();
  CHECK(host.SetCourse(shared));
  CHECK(Until([&] { return client.View().world.course == shared; }));
  CHECK(host.SetCourse(kTrack));
  CHECK(Until([&] { return client.View().world.course == kTrack; }));
  CHECK(host.StartRace());
  CHECK(Until([&] {
    host.Publish(At(kTrack.points[0]));
    client.Publish(At(kTrack.points[0]));
    friend2.Publish(At(kTrack.points[0]));
    const auto remote = client.View().world;
    return remote.race == Race::Running && Find(remote, id)->Passed() == 1 && Find(remote, id2)->Passed() == 1;
  }, 4500));
  CHECK(Until([&] {
    host.Publish(At(kTrack.points[1]));
    client.Publish(At(kTrack.points[1]));
    friend2.Publish(At(kTrack.points[1]));
    const auto remote = client.View().world;
    return remote.race == Race::Finished && Find(remote, id)->finish_ms > 0 && Find(remote, id2)->finish_ms > 0;
  }));

  CHECK(Until([&] { return !Find(client.View().world, id2)->pose; }));
  friend2.Stop();
  client.Stop();
  CHECK(Until([&] { return host.View().world.players.size() == 1; }));
  CHECK(client.Join("127.0.0.1", listening.port, "Friend", listening.invite));
  CHECK(Until([&] { return client.View().link == Link::Joined; }));
  CHECK(client.View().world.self != id);
  std::array<Session, kMaxPlayers - 2> others;
  for (auto& guest : others) {
    CHECK(guest.Join("127.0.0.1", listening.port, "Friend", listening.invite));
    CHECK(Until([&] { return guest.View().link == Link::Joined; }));
  }
  CHECK(host.View().world.players.size() == kMaxPlayers);
  CHECK(rejected.Join("127.0.0.1", listening.port, "Room full", listening.invite));
  CHECK(Until([&] { return rejected.View().link == Link::Failed; }));
  CHECK(host.View().world.players.size() == kMaxPlayers);
  host.Stop();
  CHECK(Until([&] { return client.View().link == Link::Failed; }));
  CHECK(host.Host(listening.port, "Host", true));
  CHECK(Until([&] { return host.View().link == Link::Hosting; }));
  CHECK(host.View().invite != listening.invite);
  CHECK(host.View().world.course == kTrack);
  // Cancel a newly scheduled join (before or during the connection), then start another
  // worker on the same object. Stop must join the old worker before clearing state.
  Session cancelled;
  CHECK(cancelled.Join("127.0.0.1", listening.port, "Cancel", host.View().invite));
  cancelled.Stop();
  CHECK(cancelled.LinkState() == Link::Idle && !cancelled.View().rtt_ms);
  CHECK(cancelled.Host(0, "Restart", true));
  CHECK(Until([&] { return cancelled.LinkState() == Link::Hosting; }));
  host.Stop();
  // The still-hosting fixture is also closed by the destructor, without resetting a room.
}

int main() {
  ProtocolAndRoom();
  HuntAndLayoutCodec();
  Scene();
  Disk();
  Loopback();
  std::cout << "Asset-free sandbox and TCP loopback checks passed\n";
}
