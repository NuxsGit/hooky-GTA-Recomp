#pragma once

#include "protocol.h"

#include <mutex>
#include <thread>

namespace gtav::sandbox {

enum class Link { Idle, Opening, Hosting, Connecting, Joined, Failed };
struct SessionView {
  Link link = Link::Idle;
  uint16_t port = 0;
  std::string invite;
  std::string error;
  World world;
  std::optional<uint32_t> rtt_ms{};  // kernel TCP estimate, not a v2 packet field
  bool Owner() const { return link == Link::Idle || link == Link::Hosting || link == Link::Failed; }
};

// No dependency on the SDK or game. Only this worker thread owns sockets. The game passes
// observations and local user actions through bounded state; no packet executes a guest native.
class Session {
 public:
  Session() = default;
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Port 0 is for loopback tests only. Startup/stop serialize across UI and shutdown.
  bool Host(uint16_t port, std::string name, bool loopback_only = false);
  bool Join(std::string ipv4, uint16_t port, std::string name, std::string_view invite);
  void Stop();
  SessionView View() const;
  Link LinkState() const;
  void Publish(std::optional<Presence> point);
  bool SetCourse(Course course);
  bool StartRace();
  void ResetRace();

 private:
  void RunHost(std::stop_token stop, uint16_t port, bool loopback_only);
  void RunClient(std::stop_token stop, std::string ipv4, uint16_t port, std::string name, wire::Invite invite);
  void Fail(std::string error);
  bool Available() const;
  void ResetRoom(std::string name);
  void StopLocked();

  std::mutex lifecycle_;
  mutable std::mutex mutex_;
  Link link_ = Link::Idle;
  uint16_t port_ = 0;
  std::string invite_, error_;
  Course local_course_;
  Room room_{"Player"};
  World remote_;
  std::optional<Presence> local_pose_;
  std::optional<uint32_t> rtt_ms_;
  int64_t last_pose_ms_ = 0, last_snapshot_ms_ = 0;
  std::jthread worker_;  // joined before the fields a worker uses are destroyed
};
}  // namespace gtav::sandbox
