// Discover stable releases. Windows can verify and install the official downloaded ZIP.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace loader {

inline constexpr std::string_view kReleasePage = "https://github.com/NuxsGit/hooky-GTA-Recomp/releases";
inline constexpr std::string_view kReleaseApi = "https://api.github.com/repos/NuxsGit/hooky-GTA-Recomp/releases/latest";
inline constexpr size_t kMaxReleaseBytes = 1 << 20;

inline constexpr char kLauncherVersion[] = GTAV_LAUNCHER_VERSION;

inline constexpr std::string_view kWindowsPackageStem = "gtav-recomp-windows-x64";
inline constexpr uint64_t kMaxUpdateArchiveBytes = 512u << 20;

struct UpdateAsset {
  std::string version;  // without v, already checked to be strictly newer
  std::string sha256;   // GitHub's uploaded asset digest, lowercase hex
  uint64_t size = 0;
  explicit operator bool() const { return !version.empty() && !sha256.empty() && size != 0; }
};

enum class UpdateStatus { Error, NoRelease, Current, NoPackage, Available };
struct UpdateResult {
  UpdateStatus status = UpdateStatus::Error;
  std::string message;
  std::string url;  // empty, or a link constructed from kReleasePage and a validated stable tag
  UpdateAsset asset{};  // absent for older releases without verifiable asset metadata
};

// Blocking; call on a worker in a GUI. Never reads or writes the game folder.
UpdateResult CheckForUpdate();
// Pure policy/parser for trying archived responses in tests without a network connection.
UpdateResult EvaluateRelease(int status, std::string_view json, std::string_view current,
                             std::string_view package);

namespace detail {
bool FetchRelease(int* status, std::string* json, std::string* error);
}

}  // namespace loader
