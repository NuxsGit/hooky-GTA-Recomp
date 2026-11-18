#include "update.h"

#include <charconv>
#include <chrono>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
extern char** environ;
#endif

namespace loader::detail {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kTimeout = std::chrono::seconds(15);
constexpr char kNetworkError[] = "Could not contact GitHub. Check your connection and try again.";
#ifdef _WIN32
struct InternetHandle {
  HINTERNET handle;
  ~InternetHandle() { if (handle) WinHttpCloseHandle(handle); }
};
#endif
}  // namespace

#ifdef _WIN32
bool FetchRelease(int* status, std::string* json, std::string* error) {
  // WinHTTP uses the system certificate store and needs no add-on Windows binaries.
  *error = kNetworkError;
  const auto until = Clock::now() + kTimeout;
  const std::wstring url(kReleaseApi.begin(), kReleaseApi.end());
  const auto path = url.find(L'/', 8);  // fixed HTTPS URL, no user or server-controlled parts
  const std::wstring host = url.substr(8, path - 8);
  InternetHandle session{WinHttpOpen(L"Hooky-GTA-Recomp-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.handle || !WinHttpSetTimeouts(session.handle, 5000, 5000, 5000, 5000)) return false;
  InternetHandle connection{WinHttpConnect(session.handle, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
  if (!connection.handle) return false;
  InternetHandle request{WinHttpOpenRequest(connection.handle, L"GET", url.c_str() + path, nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE)};
  if (!request.handle) return false;
  DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
  if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
    return false;
  if (!WinHttpSendRequest(request.handle,
                          L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
                          DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.handle, nullptr)) return false;
  DWORD code = 0, size = sizeof(code);
  if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX))
    return false;
  *status = static_cast<int>(code);
  if (code != 200) return true;
  char buffer[8192];
  while (Clock::now() < until) {
    DWORD n = 0;
    if (!WinHttpReadData(request.handle, buffer, sizeof(buffer), &n)) return false;
    if (n == 0) return true;
    if (json->size() + n > kMaxReleaseBytes) {
      *error = "The GitHub response is too large.";
      return false;
    }
    json->append(buffer, n);
  }
  return false;
}
#else
bool FetchRelease(int* status, std::string* json, std::string* error) {
  // The launcher links no extra Linux libraries. Use system curl (built in on macOS), with no
  // shell, no .curlrc and no redirects. Bounded pipe also limits bodies without Content-Length.
  *error = kNetworkError;
  int fd[2];
#ifdef __APPLE__
  if (pipe(fd) != 0) return false;
  bool cloexec = fcntl(fd[0], F_SETFD, FD_CLOEXEC) != -1 && fcntl(fd[1], F_SETFD, FD_CLOEXEC) != -1;
  if (!cloexec) { close(fd[0]); close(fd[1]); return false; }
  const char* curl = "/usr/bin/curl";
#else
  if (pipe2(fd, O_CLOEXEC) != 0) return false;
  const char* curl = "curl";
#endif
  const auto until = Clock::now() + kTimeout;
  const char* args[] = {curl, "-q", "--silent", "--proto", "=https", "--connect-timeout", "5",
                        "--max-time", "12", "--user-agent", "Hooky-GTA-Recomp-Launcher",
                        "--header", "Accept: application/vnd.github+json", "--header",
                        "X-GitHub-Api-Version: 2022-11-28", "--write-out", "\n%{http_code}",
                        "--", kReleaseApi.data(), nullptr};
  posix_spawn_file_actions_t actions;
  int rc = posix_spawn_file_actions_init(&actions);
  pid_t pid = -1;
  if (!rc) {
    rc = posix_spawn_file_actions_addclose(&actions, fd[0]);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
    if (!rc && fd[1] != STDOUT_FILENO) rc = posix_spawn_file_actions_addclose(&actions, fd[1]);
    if (!rc) rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (!rc) rc = posix_spawnp(&pid, curl, &actions, nullptr, const_cast<char* const*>(args), environ);
    posix_spawn_file_actions_destroy(&actions);
  }
  close(fd[1]);
  if (rc) {
    close(fd[0]);
    if (rc == ENOENT) *error = "Update checks need curl. Install it or visit the project's Releases page.";
    return false;
  }
  std::string body;
  char buffer[8192];
  bool complete = false;
  for (;;) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
    if (remaining <= 0) break;
    pollfd input{fd[0], POLLIN, 0};
    const int available = poll(&input, 1, static_cast<int>(remaining));
    if (available < 0 && errno == EINTR) continue;
    if (available <= 0) break;
    const ssize_t n = read(fd[0], buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { complete = n == 0; break; }
    if (body.size() + static_cast<size_t>(n) > kMaxReleaseBytes + 4) {
      *error = "The GitHub response is too large.";
      break;
    }
    body.append(buffer, static_cast<size_t>(n));
  }
  close(fd[0]);
  if (!complete) kill(pid, SIGKILL);
  int exit_status = 0;
  pid_t waited;
  do { waited = waitpid(pid, &exit_status, 0); } while (waited == -1 && errno == EINTR);
  if (!complete || waited != pid || !WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0 ||
      body.size() < 4 || body[body.size() - 4] != '\n') return false;
  const char* end = body.data() + body.size();
  const auto [ptr, ec] = std::from_chars(end - 3, end, *status);
  if (ptr != end || ec != std::errc{} || *status < 100 || *status > 599) return false;
  body.resize(body.size() - 4);
  *json = std::move(body);
  return true;
}
#endif

}  // namespace loader::detail
