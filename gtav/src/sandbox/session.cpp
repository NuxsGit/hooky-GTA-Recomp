#include "session.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <fstream>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <system_error>
#include <utility>

namespace gtav::sandbox {
namespace {
constexpr int64_t kUpdateMs = 100, kHandshakeMs = 4'000, kIdleMs = 6'000;
constexpr size_t kPeerSlots = kMaxPlayers + 3;
constexpr unsigned kPacketsPerSecond = 36;
constexpr std::string_view kWhitespace = " \t\r\n";

std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(kWhitespace);
  return first == text.npos ? std::string_view{} : text.substr(first, text.find_last_not_of(kWhitespace) - first + 1);
}

bool Decimal(std::string_view text, uint32_t& value) {
  if (text.empty()) return false;
  const auto* end = text.data() + text.size();
  const auto [next, error] = std::from_chars(text.data(), end, value);
  return error == std::errc{} && next == end;
}

bool Unicast(uint32_t ipv4) {
  const auto first = ipv4 >> 24;
  return first > 0 && first < 224;  // exclude unspecified, multicast, limited broadcast and reserved addresses
}

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

std::optional<uint32_t> MeasureRtt(Sock sock) {
#if defined(_WIN32) && defined(NTDDI_WIN10_RS2) && NTDDI_VERSION >= NTDDI_WIN10_RS2
  TCP_INFO_v0 info{};
  ULONG version = 0;
  DWORD bytes = 0;
  if (WSAIoctl(sock, SIO_TCP_INFO, &version, sizeof(version), &info, sizeof(info), &bytes, nullptr, nullptr) == 0 &&
      bytes >= offsetof(TCP_INFO_v0, RttUs) + sizeof(info.RttUs) && info.RttUs)
    return uint32_t((uint64_t(info.RttUs) + 500) / 1000);
#elif defined(__linux__) && defined(TCP_INFO)
  tcp_info info{};
  SockLen bytes = sizeof(info);
  if (getsockopt(sock, IPPROTO_TCP, TCP_INFO, &info, &bytes) == 0 &&
      bytes >= offsetof(tcp_info, tcpi_rtt) + sizeof(info.tcpi_rtt) && info.tcpi_rtt)
    return uint32_t((uint64_t(info.tcpi_rtt) + 500) / 1000);
#elif defined(__APPLE__) && defined(TCP_CONNECTION_INFO)
  tcp_connection_info info{};
  SockLen bytes = sizeof(info);
  if (getsockopt(sock, IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &bytes) == 0 &&
      bytes >= offsetof(tcp_connection_info, tcpi_srtt) + sizeof(info.tcpi_srtt)) return info.tcpi_srtt;
#else
  (void)sock;
#endif
  return std::nullopt;
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

// Snapshot the usable local interfaces when hosting starts. This never sends traffic or
// guesses the address of a router; the host chooses the interface their friends can reach.
std::vector<Endpoint> LocalEndpoints(uint16_t port, bool loopback_only) {
  if (loopback_only) return {{INADDR_LOOPBACK, port}};
  std::vector<Endpoint> endpoints;
  const auto add = [&](const sockaddr* socket_address) {
    if (!socket_address || socket_address->sa_family != AF_INET) return;
    const uint32_t ipv4 = ntohl(reinterpret_cast<const sockaddr_in*>(socket_address)->sin_addr.s_addr);
    if (Unicast(ipv4) && (ipv4 >> 24) != 127) endpoints.push_back({ipv4, port});
  };
#ifdef _WIN32
  constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                          GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
  ULONG size = 15'000;
  for (int retry = 0; retry < 2 && size <= (1u << 20); ++retry) {
    std::vector<uint8_t> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    const ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) continue;
    if (result != NO_ERROR) break;
    for (const auto* nic = adapters; nic; nic = nic->Next) {
      if (nic->OperStatus != IfOperStatusUp) continue;
      for (const auto* address = nic->FirstUnicastAddress; address; address = address->Next) {
        if (address->DadState == IpDadStatePreferred && address->Address.iSockaddrLength >= int(sizeof(sockaddr_in)))
          add(address->Address.lpSockaddr);
      }
    }
    break;
  }
#else
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) == 0) {
    for (const auto* nic = interfaces; nic; nic = nic->ifa_next)
      if (nic->ifa_flags & IFF_UP) add(nic->ifa_addr);
    freeifaddrs(interfaces);
  }
#endif
  std::sort(endpoints.begin(), endpoints.end(), [](Endpoint a, Endpoint b) { return a.ipv4 < b.ipv4; });
  endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
  return endpoints;
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

bool FinishConnect(Sock socket, const std::atomic<bool>& stop_requested) {
#ifndef _WIN32
  if (socket >= FD_SETSIZE) return false;
#endif
  const auto until = NowMs() + kHandshakeMs;
  while (!stop_requested.load(std::memory_order_relaxed) && NowMs() < until) {
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

std::optional<Endpoint> ParseEndpoint(std::string_view text) {
  text = Trim(text);
  const auto colon = text.find(':');
  auto address = text.substr(0, colon);
  uint32_t port = kDefaultPort;
  if (colon != text.npos) {
    const auto digits = text.substr(colon + 1);
    if (digits.size() > 5 || !Decimal(digits, port) || port == 0 || port > 65535) return std::nullopt;
  }
  uint32_t ipv4 = 0;
  for (int i = 0; i < 4; ++i) {
    const auto dot = address.find('.');
    if ((dot == address.npos) != (i == 3)) return std::nullopt;
    const auto octet = address.substr(0, dot);
    uint32_t value;
    if (octet.size() > 3 || (octet.size() > 1 && octet.front() == '0') || !Decimal(octet, value) || value > 255)
      return std::nullopt;
    ipv4 = (ipv4 << 8) | value;
    if (i != 3) address.remove_prefix(dot + 1);
  }
  if (!Unicast(ipv4)) return std::nullopt;
  return Endpoint{ipv4, uint16_t(port)};
}

std::string FormatEndpoint(Endpoint endpoint) {
  const auto ip = endpoint.ipv4;
  return std::to_string(ip >> 24) + '.' + std::to_string((ip >> 16) & 255) + '.' +
         std::to_string((ip >> 8) & 255) + '.' + std::to_string(ip & 255) + ':' + std::to_string(endpoint.port);
}

std::string FormatInvitation(Endpoint endpoint, std::string_view invite) {
  return FormatEndpoint(endpoint) + ' ' + std::string(invite);
}

Session::~Session() {
  // Don't reconstruct UI state from a destructor.
  StopWorker();
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
  stop_requested_.store(false, std::memory_order_relaxed);
  try {
    worker_ = std::thread([this, port, loopback_only] { RunHost(port, loopback_only); });
  } catch (const std::system_error&) {
    Fail("Could not start the private session worker.");
    return false;
  }
  return true;
}

bool Session::Join(std::string_view address, std::string name, std::string_view code) {
  address = Trim(address);
  const auto separator = address.find_first_of(kWhitespace);
  if (separator != address.npos) {
    code = address.substr(separator);
    address = address.substr(0, separator);
  }
  const auto endpoint = ParseEndpoint(address);
  const auto invite = wire::ParseInvite(Trim(code));
  if (!endpoint || !invite || !ValidName(name)) return false;
  std::lock_guard lifecycle(lifecycle_);
  { std::lock_guard lock(mutex_); if (!Available()) return false; }
  StopLocked();
  {
    std::lock_guard lock(mutex_);
    link_ = Link::Connecting;
    port_ = endpoint->port;
  }
  stop_requested_.store(false, std::memory_order_relaxed);
  try {
    worker_ = std::thread([this, endpoint = *endpoint, name = std::move(name), invite = *invite] {
      RunClient(endpoint, name, invite);
    });
  } catch (const std::system_error&) {
    Fail("Could not start the private session worker.");
    return false;
  }
  return true;
}

void Session::Stop() {
  std::lock_guard lifecycle(lifecycle_);
  StopLocked();
}

void Session::StopWorker() {
  // The worker never takes lifecycle_. Don't join while holding mutex_.
  if (!worker_.joinable()) return;
  stop_requested_.store(true, std::memory_order_relaxed);
  worker_.join();
}

void Session::StopLocked() {
  StopWorker();
  std::lock_guard lock(mutex_);
  link_ = Link::Idle;
  port_ = 0;
  invite_.clear();
  host_addresses_.clear();
  error_.clear();
  remote_ = {};
  rtt_ms_.reset();
  last_snapshot_ms_ = 0;
  ResetRoom("Player");
}

void Session::Fail(std::string error) {
  std::lock_guard lock(mutex_);
  link_ = Link::Failed;
  port_ = 0;
  invite_.clear();
  host_addresses_.clear();
  error_ = std::move(error);
  remote_ = {};
  rtt_ms_.reset();
  last_snapshot_ms_ = 0;
  ResetRoom("Player");
}

Link Session::LinkState() const {
  std::lock_guard lock(mutex_);
  return link_;
}

SessionView Session::View() const {
  std::lock_guard lock(mutex_);
  const auto now = NowMs();
  if (link_ != Link::Joined) return {link_, port_, invite_, error_, room_.View(1, now), {}, host_addresses_};
  auto world = remote_;
  if (now - last_snapshot_ms_ > kPoseLifetimeMs)
    for (auto& p : world.players) p.pose.reset();
  return {link_, port_, {}, error_, std::move(world), rtt_ms_};
}

void Session::Publish(std::optional<Presence> point) {
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

void Session::RunHost(uint16_t port, bool loopback_only) {
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
  const auto bound_port = ntohs(addr.sin_port);
  auto addresses = LocalEndpoints(bound_port, loopback_only);
  {
    std::lock_guard lock(mutex_);
    link_ = Link::Hosting;
    port_ = bound_port;
    invite_ = wire::FormatInvite(invite);
    host_addresses_ = std::move(addresses);
  }
  std::vector<Peer> peers;
  int64_t snapshot_at = 0;
  while (!stop_requested_.load(std::memory_order_relaxed)) {
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
        std::optional<Presence> point;
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

void Session::RunClient(Endpoint endpoint, const std::string& name, wire::Invite invite) {
  Sockets sockets;
  Socket connection(sockets.ready ? socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) : kInvalid);
  const auto addr = Address(endpoint.port, endpoint.ipv4);
  if (!connection || !Nonblocking(connection.Get())) {
    Fail("Cannot initialize the client TCP socket.");
    return;
  }
  LowLatency(connection.Get());
  if (connect(connection.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 &&
      !(InProgress(SocketError()) && FinishConnect(connection.Get(), stop_requested_))) {
    if (!stop_requested_.load(std::memory_order_relaxed)) Fail("Cannot reach the host. Check address, TCP port and firewall/forwarding.");
    return;
  }
  const auto begin = NowMs();
  Pipe pipe(std::move(connection), begin);
  pipe.Queue(wire::Hello(invite, name), begin);
  uint32_t self = 0;
  int64_t update_at = 0, rtt_at = 0;
  while (!stop_requested_.load(std::memory_order_relaxed)) {
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
      if (!stop_requested_.load(std::memory_order_relaxed)) Fail(self ? "Disconnected from the host." :
          "The host did not accept this connection. Check invite, room capacity and game version.");
      return;
    }
    if (self && now >= rtt_at) {
      rtt_at = now + 1000;
      const auto rtt = MeasureRtt(pipe.socket.Get());
      std::lock_guard lock(mutex_);
      rtt_ms_ = rtt;
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
