#pragma once

#include "protocol.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace gtav::sandbox {

// Numeric IPv4 in host byte order. No DNS, URL handling or network discovery is needed to join.
struct Endpoint {
  uint32_t ipv4 = 0;
  uint16_t port = kDefaultPort;
  bool operator==(const Endpoint&) const = default;
};
std::optional<Endpoint> ParseEndpoint(std::string_view text);
std::string FormatEndpoint(Endpoint endpoint);
std::string FormatInvitation(Endpoint endpoint, std::string_view invite);

enum class Link { Idle, Opening, Hosting, Connecting, Joined, Failed };
struct SessionView {
  Link link = Link::Idle;
  uint16_t port = 0;
  std::string invite;
  std::string error;
  World world;
  std::optional<uint32_t> rtt_ms{};  // kernel TCP estimate, not a v2 packet field
  std::vector<Endpoint> host_addresses{};  // successful listener only; local interfaces, not public discovery
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
  // An IPv4[:port] with a separate code, or the host's single pasted "IPv4:port code".
  // Inline codes take precedence. Invalid input is rejected before starting a socket worker.
  bool Join(std::string_view address_or_invitation, std::string name, std::string_view invite = {});
  void Stop();
  SessionView View() const;
  Link LinkState() const;
  void Publish(std::optional<Presence> point);
  bool SetCourse(Course course);
  bool StartRace();
  void ResetRace();

 private:
  void RunHost(uint16_t port, bool loopback_only);
  void RunClient(Endpoint endpoint, const std::string& name, wire::Invite invite);
  void Fail(std::string error);
  bool Available() const;
  void ResetRoom(std::string name);
  void StopLocked();
  void StopWorker();

  std::mutex lifecycle_;
  mutable std::mutex mutex_;
  Link link_ = Link::Idle;
  uint16_t port_ = 0;
  std::string invite_, error_;
  std::vector<Endpoint> host_addresses_;
  Course local_course_;
  Room room_{"Player"};
  World remote_;
  std::optional<Presence> local_pose_;
  std::optional<uint32_t> rtt_ms_;
  int64_t last_pose_ms_ = 0, last_snapshot_ms_ = 0;
  // Explicit stop/join also works with the libc++ shipped by older Xcode versions.
  std::atomic<bool> stop_requested_{false};
  std::thread worker_;  // joined before the fields a worker uses are destroyed
};
}  // namespace gtav::sandbox
