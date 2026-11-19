// GitHub release discovery and the Windows verified installer. The two repository names
// are pinned by the build, not read from a response, local settings, or a download URL.
#pragma once

#include "update_version.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#ifndef GTAV_STABLE_REPO
#define GTAV_STABLE_REPO "NuxsGit/hooky-GTA-Recomp"
#endif
#ifndef GTAV_NIGHTLY_REPO
#define GTAV_NIGHTLY_REPO GTAV_STABLE_REPO
#endif

namespace loader {

inline constexpr std::string_view kReleasePage = "https://github.com/" GTAV_STABLE_REPO "/releases";
inline constexpr std::string_view kNightlyReleasePage = "https://github.com/" GTAV_NIGHTLY_REPO "/releases";
inline constexpr std::string_view ReleasePage(UpdateChannel channel) {
  return channel == UpdateChannel::Nightly ? kNightlyReleasePage : kReleasePage;
}
inline constexpr std::string_view ReleaseRepository(UpdateChannel channel) {
  return channel == UpdateChannel::Nightly ? GTAV_NIGHTLY_REPO : GTAV_STABLE_REPO;
}
inline constexpr size_t kMaxReleaseBytes = 1 << 20;
inline constexpr char kLauncherVersion[] = GTAV_LAUNCHER_VERSION;
inline constexpr std::string_view kWindowsPackageStem = "gtav-recomp-windows-x64";
inline constexpr std::string_view kWindowsPackageName = "gtav-recomp-windows-x64.zip";
inline constexpr uint64_t kMaxUpdateArchiveBytes = 512u << 20;

struct UpdateAsset {
  std::string version;  // without v, already compared against the installed launcher
  std::string sha256;   // GitHub's uploaded asset digest, lowercase hex
  uint64_t size = 0;
  UpdateChannel channel = UpdateChannel::Stable;
  explicit operator bool() const {
    const auto build = ParseBuildVersion(version);
    return build && build->Channel() == channel && size && size <= kMaxUpdateArchiveBytes &&
           sha256.size() == 64 && sha256.find_first_not_of("0123456789abcdef") == sha256.npos;
  }
};

enum class UpdateStatus { Error, NoRelease, Current, NoPackage, Available, OlderChannel };
struct UpdateResult {
  UpdateStatus status = UpdateStatus::Error;
  std::string message;
  std::string url;  // constructed solely from the selected repository and a validated tag
  UpdateAsset asset{};
  std::string latest{};  // without v, may be unverified or not the requested platform
};

// Blocking; call on a worker in a GUI. Never reads or writes the game folder.
UpdateResult CheckForUpdate(UpdateChannel channel = UpdateChannel::Stable);
UpdateResult EvaluateRelease(int status, std::string_view json, std::string_view current,
                             std::string_view package, UpdateChannel channel = UpdateChannel::Stable);

// Exact path on github.com for the selected, verified Windows asset (never a server-supplied URL).
std::string WindowsAssetPath(const UpdateAsset& asset);

struct UpdatePreferences {
  UpdateChannel channel = UpdateChannel::Stable;
  bool automatic = true;
  bool operator==(const UpdatePreferences&) const = default;
};
std::optional<UpdatePreferences> ParseUpdatePreferences(std::string_view text);
std::string SerializeUpdatePreferences(UpdatePreferences prefs);

namespace detail {
bool FetchRelease(UpdateChannel channel, int* status, std::string* json, std::string* error);
inline constexpr std::string_view ReleaseApi(UpdateChannel channel) {
  return channel == UpdateChannel::Nightly
      ? "https://api.github.com/repos/" GTAV_NIGHTLY_REPO "/releases?per_page=20"
      : "https://api.github.com/repos/" GTAV_STABLE_REPO "/releases/latest";
}
}  // namespace detail

}  // namespace loader
