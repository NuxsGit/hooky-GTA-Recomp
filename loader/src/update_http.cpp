#include "update.h"

#include <charconv>
#include <chrono>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

#include <array>
#include <cwchar>
#include "windows_update.h"
#include "win_process.h"
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
bool FetchRelease(UpdateChannel channel, int* status, std::string* json, std::string* error) {
  // WinHTTP uses the system certificate store and needs no add-on Windows binaries.
  *error = kNetworkError;
  const auto until = Clock::now() + kTimeout;
  const auto endpoint = ReleaseApi(channel);
  const std::wstring url(endpoint.begin(), endpoint.end());
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

namespace {
struct AssetRequest {
  InternetHandle connection{}, request{};

  DWORD Get(HINTERNET session, const std::wstring& host, const std::wstring& path) {
    connection.handle = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection.handle) return 0;
    request.handle = WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE | WINHTTP_FLAG_ESCAPE_DISABLE);
    if (!request.handle) return 0;
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSendRequest(request.handle, L"Accept: application/octet-stream\r\n", DWORD(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.handle, nullptr)) return 0;
    DWORD code = 0, size = sizeof(code);
    return WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX) ? code : 0;
  }
};

}  // namespace

bool ParseWindowsAssetRedirect(std::wstring_view url, std::wstring& host, std::wstring& path) {
  // Recognize only the GitHub release CDN's signed HTTPS URLs, never an arbitrary URL.
  if (url.size() >= 8192) return false;
  if (!url.starts_with(L"https://") || url.find(L'#') != url.npos ||
      url.find_first_of(L"\\") != url.npos) return false;
  for (const auto c : url) if (c <= L' ' || c > L'~') return false;
  URL_COMPONENTS parts{sizeof(parts)};
  parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = parts.dwUrlPathLength =
      parts.dwExtraInfoLength = DWORD(-1);
  const std::wstring terminated(url);
  if (!WinHttpCrackUrl(terminated.c_str(), static_cast<DWORD>(terminated.size()), 0, &parts) ||
      parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
      parts.dwUserNameLength || parts.dwPasswordLength || !parts.dwHostNameLength || !parts.dwUrlPathLength) return false;
  host.assign(parts.lpszHostName, parts.dwHostNameLength);
  if (host != L"release-assets.githubusercontent.com" && host != L"objects.githubusercontent.com") return false;
  path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  return path.starts_with(L'/');
}

bool DownloadWindowsAsset(const UpdateAsset& asset, const std::filesystem::path& output,
                          InstallUi& ui, std::wstring* error) {
  const auto path = WindowsAssetPath(asset);
  if (path.empty() || ui.Cancelled()) { *error = L"No verified Windows update selected, or cancelled."; return false; }
  ui.Stage(L"Downloading the verified release from GitHub...");
  *error = L"Could not download the official ZIP from GitHub. Try again or use a local ZIP.";
  const auto deadline = Clock::now() + std::chrono::minutes(10);
  InternetHandle session{WinHttpOpen(L"Hooky-GTA-Recomp-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.handle || !WinHttpSetTimeouts(session.handle, 5000, 5000, 10000, 10000)) return false;
  AssetRequest origin, cdn;
  auto code = origin.Get(session.handle, L"github.com", std::wstring(path.begin(), path.end()));
  auto request = origin.request.handle;
  if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
    std::wstring target, resource;
    std::array<wchar_t, 8192> location{};
    DWORD bytes = sizeof(location);
    if (ui.Cancelled() || !WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
          location.data(), &bytes, WINHTTP_NO_HEADER_INDEX) || bytes >= sizeof(location) ||
        !ParseWindowsAssetRedirect(location.data(), target, resource)) return false;
    code = cdn.Get(session.handle, target, resource);
    request = cdn.request.handle;
  }
  if (code != 200) return false;

  win::Handle file(CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) { *error = L"Could not create the temporary update ZIP."; return false; }
  uint64_t total = 0;
  std::array<char, 64 * 1024> buffer;
  while (!ui.Cancelled() && Clock::now() < deadline) {
    DWORD read = 0, written = 0;
    if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) return false;
    if (read == 0 && total == asset.size) { error->clear(); return true; }
    if (read == 0 || read > asset.size - total) { *error = L"Downloaded ZIP size does not match GitHub metadata."; return false; }
    if (!WriteFile(file.Get(), buffer.data(), read, &written, nullptr) || written != read) {
      *error = L"Could not write the update ZIP. Check free disk space.";
      return false;
    }
    total += read;
    ui.Progress(.65 * double(total) / double(asset.size));
  }
  *error = ui.Cancelled() ? L"Cancelled." : L"Timed out downloading the official ZIP.";
  return false;
}

#else
bool FetchRelease(UpdateChannel channel, int* status, std::string* json, std::string* error) {
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
                        "--", ReleaseApi(channel).data(), nullptr};
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
