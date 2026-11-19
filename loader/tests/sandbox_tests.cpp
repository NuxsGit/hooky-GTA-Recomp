#include "courses.h"
#include "paths.h"
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
const Course kTrack{"Airport Practice", {{-3200, 1500, 2400}, {12000, 1500, 2400}}};

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
  auto pose = wire::Pose(kTrack.points.front());
  pose.insert(pose.begin(), hello.back());  // final byte of Hello and a second packet in one recv
  CHECK(framer.Push(pose));
  auto frame = framer.Pop();
  wire::Invite decoded{};
  std::string name;
  CHECK(frame && wire::ReadHello(*frame, decoded, name) && wire::MatchInvite(*invite, decoded) && name == "Driver_2");
  std::optional<Point> position;
  frame = framer.Pop();
  CHECK(frame && wire::ReadPose(*frame, position) && position == kTrack.points.front());
  CHECK(!framer.Pop() && !framer.failed());
  auto wrong_version = hello;
  ++wrong_version[2];
  CHECK(framer.Push(wrong_version) && !framer.Pop() && framer.failed());
  wire::Framing oversized;
  const uint8_t huge[] = {0xff, 0xff};
  CHECK(oversized.Push(huge) && !oversized.Pop() && oversized.failed());
  CHECK(wire::Pose(Point{0, 0, INT32_MIN}).empty());

  Room room("Host");
  const uint32_t friend_id = room.Add("Driver_2");
  CHECK(friend_id == 2 && room.SetCourse(kTrack));
  CHECK(!room.Start(0));
  room.Pose(1, kTrack.points[1], 100);
  room.Pose(friend_id, kTrack.points[0], 100);
  CHECK(room.Start(100));
  CHECK(room.View(1, 100).race == Race::Countdown);
  CHECK(!room.SetCourse({"Other track", {}}));
  room.Pose(friend_id, kTrack.points[0], 3099);  // not until green
  CHECK(Find(room.View(1, 3099), friend_id)->passed == 0);
  room.Tick(3100);
  room.Pose(1, kTrack.points[1], 3100);  // can't skip checkpoint 1
  CHECK(Find(room.View(1, 3100), 1)->passed == 0);
  room.Pose(friend_id, kTrack.points[0], 3100);
  room.Pose(friend_id, kTrack.points[1], 5300);
  room.Pose(1, kTrack.points[0], 5500);
  room.Pose(1, kTrack.points[1], 8100);
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
  room.Pose(1, kTrack.points[0], expired);
  CHECK(room.Start(expired));
  room.Tick(expired + kCountdownMs + kRaceLimitMs);
  CHECK(room.View(1, expired + kCountdownMs + kRaceLimitMs).race == Race::Finished);
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
  auto edited = kTrack;
  edited.points.pop_back();
  CHECK(SaveCourse(path, edited) && LoadCourse(path, kTrack.name) == edited);
  std::ofstream(path / "course-Airport Practice.gtcourse", std::ios::app) << "not a checkpoint";
  CHECK(!LoadCourse(path, kTrack.name));
  std::ofstream(path / "course-Airport Practice.gtcourse", std::ios::binary) << std::string(3000, 'a');
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
  CHECK(!client.SetCourse({"Cannot edit host map", {}}) && !client.StartRace());
  const auto id = client.View().world.self;
  CHECK(id > 1 && client.View().world.course == kTrack);
  CHECK(friend2.Join("127.0.0.1", listening.port, "Friend Two", listening.invite));
  CHECK(Until([&] { return friend2.View().link == Link::Joined; }));
  const auto id2 = friend2.View().world.self;
  CHECK(id2 > 1 && id2 != id);
  CHECK(Until([&] {
    host.Publish(kTrack.points[0]);
    client.Publish(kTrack.points[0]);
    friend2.Publish(kTrack.points[0]);
    const auto remote = client.View().world;
    return Find(remote, id2) && Find(remote, id2)->pose == kTrack.points[0] &&
        Find(remote, 1)->pose == kTrack.points[0] && Find(remote, id)->pose == kTrack.points[0];
  }));
  CHECK(host.StartRace());
  CHECK(Until([&] {
    host.Publish(kTrack.points[0]);
    client.Publish(kTrack.points[0]);
    friend2.Publish(kTrack.points[0]);
    const auto remote = client.View().world;
    return remote.race == Race::Running && Find(remote, id)->passed == 1 && Find(remote, id2)->passed == 1;
  }, 4500));
  CHECK(Until([&] {
    host.Publish(kTrack.points[1]);
    client.Publish(kTrack.points[1]);
    friend2.Publish(kTrack.points[1]);
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
  host.Stop();
}

int main() {
  ProtocolAndRoom();
  Disk();
  Loopback();
  std::cout << "Asset-free sandbox and TCP loopback checks passed\n";
}
