#include "session.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <fstream>
#endif

#include <algorithm>
#include <chrono>
#include <utility>

namespace gtav::sandbox {
namespace {
constexpr int64_t kUpdateMs = 100, kHandshakeMs = 4'000, kIdleMs = 6'000;
constexpr size_t kPeerSlots = kMaxPlayers + 3;
constexpr unsigned kPacketsPerSecond = 36;
#ifdef _WIN32
using Sock = SOCKET;
constexpr Sock kInvalid = INVALID_SOCKET;
using SockLen = int;
int SocketError() { return WSAGetLastError(); }
bool WouldBlock(int error) { return error == WSAEWOULDBLOCK; }
bool InProgress(int error) { return WouldBlock(error) || error == WSAEINPROGRESS; }
void Close(Sock sock) { closesocket(sock); }
constexpr int kSendFlags = 0;
#else
using Sock = int;
constexpr Sock kInvalid = -1;
using SockLen = socklen_t;
int SocketError() { return errno; }
bool WouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR; }
bool InProgress(int error) { return WouldBlock(error) || error == EINPROGRESS; }
void Close(Sock sock) { close(sock); }
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // macOS uses SO_NOSIGPIPE per socket
#endif
#endif

struct Sockets {
#ifdef _WIN32
  WSADATA data{};
  const bool ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  ~Sockets() { if (ready) WSACleanup(); }
#else
  const bool ready = true;
#endif
};

class Socket {
 public:
  explicit Socket(Sock socket = kInvalid) : value_(socket) {}
  Socket(Socket&& other) noexcept : value_(std::exchange(other.value_, kInvalid)) {}
  Socket& operator=(Socket&& other) noexcept {
    if (value_ != kInvalid) Close(value_);
    value_ = std::exchange(other.value_, kInvalid);
    return *this;
  }
  ~Socket() { if (value_ != kInvalid) Close(value_); }
  Sock Get() const { return value_; }
  explicit operator bool() const { return value_ != kInvalid; }
 private:
  Sock value_;
};

bool Nonblocking(Sock socket) {
#ifdef _WIN32
  u_long on = 1;
  return ioctlsocket(socket, FIONBIO, &on) == 0;
#else
  const int flags = fcntl(socket, F_GETFL);
  if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0 || fcntl(socket, F_SETFD, FD_CLOEXEC) != 0)
    return false;
#ifdef SO_NOSIGPIPE
  const int on = 1;
  return setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0;
#else
  return true;
#endif
#endif
}

void LowLatency(Sock socket) {
  const int on = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

void Pause() { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

bool RandomInvite(wire::Invite& invite) {
#ifdef _WIN32
  return BCryptGenRandom(nullptr, invite.data(), ULONG(invite.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
  std::ifstream urandom("/dev/urandom", std::ios::binary);
  return bool(urandom.read(reinterpret_cast<char*>(invite.data()), invite.size()));
#endif
}

sockaddr_in Address(uint16_t port, uint32_t ipv4) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(ipv4);
  return addr;
}

struct Pipe {
  explicit Pipe(Socket socket, int64_t now) : socket(std::move(socket)), last_rx(now), window(now) {}
  Socket socket;
  wire::Framing framing;
  wire::Bytes outgoing;
  size_t sent = 0;
  int64_t last_rx, window, send_by = 0;
  unsigned packets = 0;

  void Queue(wire::Bytes packet, int64_t now) {
    outgoing = std::move(packet);
    sent = 0;
    send_by = now + kIdleMs;
  }
  bool Send(int64_t now) {
    if (outgoing.empty()) return true;
    if (now >= send_by) return false;
    const auto n = send(socket.Get(), reinterpret_cast<const char*>(outgoing.data() + sent),
                        int(outgoing.size() - sent), kSendFlags);
    if (n <= 0) return n < 0 && WouldBlock(SocketError());
    sent += size_t(n);
    if (sent == outgoing.size()) outgoing.clear();
    return true;
  }

  template <typename Func>
  bool Read(int64_t now, Func receive) {
    std::array<uint8_t, wire::kMaxPacket> buffer{};
    const auto n = recv(socket.Get(), reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0);
    if (n == 0 || (n < 0 && !WouldBlock(SocketError()))) return false;
    if (n > 0 && !framing.Push(std::span(buffer.data(), size_t(n)))) return false;
    if (now - window >= 1000) { window = now; packets = 0; }
    while (const auto frame = framing.Pop()) {
      if (++packets > kPacketsPerSecond || !receive(*frame)) return false;
      last_rx = now;
    }
    return !framing.failed() && now - last_rx < kIdleMs;
  }
};

struct Peer {
  Pipe pipe;
  int64_t joined_by;
  uint32_t id = 0;
};

bool FinishConnect(Sock socket, std::stop_token stop) {
#ifndef _WIN32
  if (socket >= FD_SETSIZE) return false;
#endif
  const auto until = NowMs() + kHandshakeMs;
  while (!stop.stop_requested() && NowMs() < until) {
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(socket, &writable);
    FD_SET(socket, &failed);
    timeval timeout{0, 20'000};
    const int n = select(
#ifdef _WIN32
        0,
#else
        socket + 1,
#endif
        nullptr, &writable, &failed, &timeout);
    if (n == 0 || (n < 0 && WouldBlock(SocketError()))) continue;
    int error = 0;
    SockLen size = sizeof(error);
    return n > 0 && getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0 && !error;
  }
  return false;
}
}  // namespace

Session::~Session() {
  // Don't reconstruct UI state from a destructor.
  if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
}

bool Session::Available() const { return link_ == Link::Idle || link_ == Link::Failed; }

void Session::ResetRoom(std::string name) {
  room_ = Room(std::move(name));
  room_.SetCourse(local_course_);
  if (local_pose_ && NowMs() - last_pose_ms_ <= kPoseLifetimeMs) room_.Pose(1, local_pose_, last_pose_ms_);
}

bool Session::Host(uint16_t port, std::string name, bool loopback_only) {
  if ((!port && !loopback_only) || !ValidName(name)) return false;
  std::lock_guard lifecycle(lifecycle_);
  { std::lock_guard lock(mutex_); if (!Available()) return false; }
  StopLocked();
  {
    std::lock_guard lock(mutex_);
    link_ = Link::Opening;
    ResetRoom(std::move(name));
  }
  worker_ = std::jthread([this, port, loopback_only](std::stop_token stop) { RunHost(stop, port, loopback_only); });
  return true;
}

bool Session::Join(std::string ipv4, uint16_t port, std::string name, std::string_view code) {
  auto invite = wire::ParseInvite(code);
  if (!port || !invite || !ValidName(name) || ipv4.empty() || ipv4.size() > 15 ||
      ipv4.find_first_not_of("0123456789.") != ipv4.npos) return false;
  std::lock_guard lifecycle(lifecycle_);
  { std::lock_guard lock(mutex_); if (!Available()) return false; }
  StopLocked();
  {
    std::lock_guard lock(mutex_);
    link_ = Link::Connecting;
    port_ = port;
  }
  worker_ = std::jthread([this, ipv4 = std::move(ipv4), port, name = std::move(name), invite = *invite](std::stop_token stop) {
    RunClient(stop, ipv4, port, name, invite);
  });
  return true;
}

void Session::Stop() {
  std::lock_guard lifecycle(lifecycle_);
  StopLocked();
}

void Session::StopLocked() {
  if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
  std::lock_guard lock(mutex_);
  link_ = Link::Idle;
  port_ = 0;
  invite_.clear();
  error_.clear();
  remote_ = {};
  last_snapshot_ms_ = 0;
  ResetRoom("Player");
}

void Session::Fail(std::string error) {
  std::lock_guard lock(mutex_);
  link_ = Link::Failed;
  port_ = 0;
  invite_.clear();
  error_ = std::move(error);
  remote_ = {};
  last_snapshot_ms_ = 0;
  ResetRoom("Player");
}

SessionView Session::View() const {
  std::lock_guard lock(mutex_);
  const auto now = NowMs();
  if (link_ != Link::Joined) return {link_, port_, invite_, error_, room_.View(1, now)};
  auto world = remote_;
  if (now - last_snapshot_ms_ > kPoseLifetimeMs)
    for (auto& p : world.players) p.pose.reset();
  return {link_, port_, {}, error_, std::move(world)};
}

void Session::Publish(std::optional<Point> point) {
  if (point && !point->Valid()) return;
  const auto now = NowMs();
  std::lock_guard lock(mutex_);
  local_pose_ = point;
  last_pose_ms_ = now;
  if (link_ != Link::Joined && link_ != Link::Connecting) {
    room_.Tick(now);
    room_.Pose(1, point, now);
    room_.Tick(now);
  }
}

bool Session::SetCourse(Course course) {
  std::lock_guard lock(mutex_);
  if (!(link_ == Link::Hosting || Available()) || !room_.SetCourse(course)) return false;
  local_course_ = std::move(course);
  return true;
}

bool Session::StartRace() {
  std::lock_guard lock(mutex_);
  return (link_ == Link::Hosting || Available()) && room_.Start(NowMs());
}

void Session::ResetRace() {
  std::lock_guard lock(mutex_);
  if (link_ == Link::Hosting || Available()) room_.Reset();
}

void Session::RunHost(std::stop_token stop, uint16_t port, bool loopback_only) {
  Sockets sockets;
  Socket listener(sockets.ready ? socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) : kInvalid);
  wire::Invite invite{};
  if (!listener || !Nonblocking(listener.Get()) || !RandomInvite(invite)) {
    Fail("Cannot initialize the private listener or invite code.");
    return;
  }
#ifdef _WIN32
  const int exclusive = 1;
  if (setsockopt(listener.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                 sizeof(exclusive)) != 0) {
    Fail("Cannot reserve an exclusive listener.");
    return;
  }
#else
  const int reuse = 1;
  setsockopt(listener.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
  auto addr = Address(port, loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
  SockLen len = sizeof(addr);
  if (bind(listener.Get(), reinterpret_cast<sockaddr*>(&addr), len) != 0 || listen(listener.Get(), int(kPeerSlots)) != 0 ||
      getsockname(listener.Get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    Fail("Can't open that TCP port. Choose another port and check your local firewall.");
    return;
  }
  {
    std::lock_guard lock(mutex_);
    link_ = Link::Hosting;
    port_ = ntohs(addr.sin_port);
    invite_ = wire::FormatInvite(invite);
  }
  std::vector<Peer> peers;
  int64_t snapshot_at = 0;
  while (!stop.stop_requested()) {
    const auto now = NowMs();
    for (unsigned i = 0; i < 3; ++i) {
      Socket incoming(accept(listener.Get(), nullptr, nullptr));
      if (!incoming) break;
      if (peers.size() == kPeerSlots || !Nonblocking(incoming.Get())) continue;
#ifdef _WIN32
      // A stopped room has nothing to flush. Don't leave its exclusive listener port
      // associated with accepted sockets waiting to finish a TCP graceful close.
      const linger immediate{1, 0};
      if (setsockopt(incoming.Get(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&immediate),
                     sizeof(immediate)) != 0) continue;
#endif
      LowLatency(incoming.Get());
      peers.push_back({Pipe(std::move(incoming), now), now + kHandshakeMs});
    }
    {
      std::lock_guard lock(mutex_);
      room_.Tick(now);
    }
    for (auto it = peers.begin(); it != peers.end();) {
      auto& peer = *it;
      const bool alive = (peer.id || now < peer.joined_by) && peer.pipe.Read(now, [&](const wire::Frame& frame) {
        if (!peer.id) {
          wire::Invite code{};
          std::string name;
          if (!wire::ReadHello(frame, code, name) || !wire::MatchInvite(invite, code)) return false;
          std::lock_guard lock(mutex_);
          peer.id = room_.Add(std::move(name));
          if (!peer.id) return false;
          peer.pipe.Queue(wire::Snapshot(room_.View(peer.id, NowMs())), now);
          return true;
        }
        std::optional<Point> point;
        if (!wire::ReadPose(frame, point)) return false;
        std::lock_guard lock(mutex_);
        room_.Pose(peer.id, point, NowMs());
        return true;
      }) && peer.pipe.Send(now);
      if (!alive) {
        std::lock_guard lock(mutex_);
        room_.Remove(peer.id);
        it = peers.erase(it);
      } else ++it;
    }
    if (now >= snapshot_at) {
      snapshot_at = now + kUpdateMs;
      std::lock_guard lock(mutex_);
      room_.Tick(now);
      for (auto& peer : peers) {
        if (peer.id && peer.pipe.outgoing.empty()) peer.pipe.Queue(wire::Snapshot(room_.View(peer.id, NowMs())), now);
      }
    }
    Pause();
  }
}

void Session::RunClient(std::stop_token stop, std::string ipv4, uint16_t port, std::string name, wire::Invite invite) {
  Sockets sockets;
  Socket connection(sockets.ready ? socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) : kInvalid);
  auto addr = Address(port, 0);
  if (!connection || !Nonblocking(connection.Get()) || inet_pton(AF_INET, ipv4.c_str(), &addr.sin_addr) != 1 ||
      addr.sin_addr.s_addr == htonl(INADDR_ANY) || addr.sin_addr.s_addr == htonl(INADDR_BROADCAST)) {
    Fail("Enter a valid host IPv4 address and TCP port.");
    return;
  }
  LowLatency(connection.Get());
  if (connect(connection.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 &&
      !(InProgress(SocketError()) && FinishConnect(connection.Get(), stop))) {
    if (!stop.stop_requested()) Fail("Cannot reach the host. Check address, TCP port and firewall/forwarding.");
    return;
  }
  const auto begin = NowMs();
  Pipe pipe(std::move(connection), begin);
  pipe.Queue(wire::Hello(invite, name), begin);
  uint32_t self = 0;
  int64_t update_at = 0;
  while (!stop.stop_requested()) {
    const auto now = NowMs();
    if ((!self && now - begin >= kHandshakeMs) || !pipe.Read(now, [&](const wire::Frame& frame) {
          auto view = wire::ReadSnapshot(frame);
          if (!view || view->self == 1 || (self && self != view->self)) return false;
          self = view->self;
          std::lock_guard lock(mutex_);
          link_ = Link::Joined;
          last_snapshot_ms_ = now;
          remote_ = std::move(*view);
          return true;
        }) || !pipe.Send(now)) {
      if (!stop.stop_requested()) Fail(self ? "Disconnected from the host." :
          "The host did not accept this connection. Check invite, room capacity and game version.");
      return;
    }
    if (self && now >= update_at && pipe.outgoing.empty()) {
      update_at = now + kUpdateMs;
      std::lock_guard lock(mutex_);
      pipe.Queue(wire::Pose(now - last_pose_ms_ <= kPoseLifetimeMs ? local_pose_ : std::nullopt), now);
    }
    Pause();
  }
}
}  // namespace gtav::sandbox
