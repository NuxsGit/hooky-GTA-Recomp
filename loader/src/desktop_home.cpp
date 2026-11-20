#include "desktop_home.h"

#include "setup_lock.h"
#include "sources.h"

#ifdef _WIN32
#include "windows_package_layout.h"
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace loader {
namespace {

enum class Kind { Missing, File, Directory, Unsafe };

Kind Inspect(const fs::path& path) {
#ifdef _WIN32
  const DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    const DWORD reason = GetLastError();
    return reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PATH_NOT_FOUND ? Kind::Missing : Kind::Unsafe;
  }
  if (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return Kind::Unsafe;
  return attrs & FILE_ATTRIBUTE_DIRECTORY ? Kind::Directory : Kind::File;
#else
  std::error_code ec;
  const auto type = fs::symlink_status(path, ec).type();
  if (type == fs::file_type::not_found) return Kind::Missing;
  if (!ec && type == fs::file_type::regular) return Kind::File;
  if (!ec && type == fs::file_type::directory) return Kind::Directory;
  return Kind::Unsafe;
#endif
}

bool Error(std::wstring* error, std::wstring text) {
  *error = std::move(text);
  return false;
}

bool Within(const fs::path& path, const fs::path& parent) {
  // Resolve existing parents as well, so an alias or a case-insensitive spelling does not
  // permit staging a copy of a folder inside itself.
  std::error_code ec;
  auto p = fs::weakly_canonical(path, ec);
  if (ec) return true;
  const auto base = fs::weakly_canonical(parent, ec);
  if (ec) return true;
  for (;;) {
    if (p == base || SameFolder(p, base)) return true;
#ifdef _WIN32
    if (CompareStringOrdinal(p.c_str(), -1, base.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
#endif
    auto up = p.parent_path();
    if (up.empty() || up == p) return false;
    p = std::move(up);
  }
}

constexpr const char* kMutable[] = {
    "data/game", "data/update", "data/user", "data/build", "data/build-iphone", "data/build-android",
    "data/spirv", "data/shader_cache", "data/config/graphics.ini", "data/launcher.ini", "data/campaign_coop.ini", "data/installed.txt",
    "data/install_state.txt",
    "data/data_installed.txt", "data/verified-assets.txt", "data/launcher-updates.txt",
#ifdef _WIN32
    "gtav_game.dll",
#elif defined(__APPLE__)
    "gtav_game.dylib",
#else
    "gtav_game.so",
#endif
    "gtav_settings.ini", "gtav.toml", "args.txt", "gamecontrollerdb.txt", "shader_cache", "spirv",
    "iphone/GTAV.ipa", "android/gtav-android-pack.zip",
};

// These are local files selected by the user, not an archive to unpack. Copy only the requested
// paths. In particular never import links, locks, dumps, or an old update's rollback tree.
class Stage {
 public:
  explicit Stage(const std::function<bool()>& cancelled) : cancelled_(cancelled) {}
  ~Stage() {
    if (!path.empty()) {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  }

  bool Copy(const fs::path& from, const fs::path& to, bool required, std::wstring* error) const {
    if (Cancelled()) return Error(error, L"Cancelled. The original installation was not changed.");
    std::error_code ec;
    switch (Inspect(from)) {
      case Kind::Missing:
        if (!required) return true;
        break;
      case Kind::File:
        fs::create_directories(to.parent_path(), ec);
        if (!ec && fs::copy_file(from, to, ec)) {
          // Preserve timestamps of verified assets (and file permissions via copy_file).
          const auto timestamp = fs::last_write_time(from, ec);
          if (!ec) fs::last_write_time(to, timestamp, ec);
          return true;
        }
        break;
      case Kind::Directory:
        fs::create_directories(to, ec);
        for (auto i = fs::directory_iterator(from, ec); !ec && i != fs::directory_iterator(); i.increment(ec))
          if (!Copy(i->path(), to / i->path().filename(), true, error)) return false;
        if (!ec) return true;
        break;
      case Kind::Unsafe:
        return Error(error, L"Cannot import links or special files: " + PathWide(from));
    }
    return Error(error, L"Could not copy " + PathWide(from) + L" to " + PathWide(to) +
                            (ec ? L": " + Widen(ec.message()) : L". Check the package and free space."));
  }

  bool Cancelled() const { return cancelled_ && cancelled_(); }

  fs::path path;

 private:
  const std::function<bool()>& cancelled_;
};

bool EmptyOrMissing(const fs::path& home) {
  const auto kind = Inspect(home);
  std::error_code ec;
  return kind == Kind::Missing || (kind == Kind::Directory && fs::is_empty(home, ec) && !ec);
}

// Publish the complete folder at once. An existing installation (even a partial one with a
// save) is never merged or replaced. On any failure the caller can retry with the same source.
bool Initialize(const fs::path& package, const fs::path& legacy, const fs::path& home,
                const std::vector<fs::path>& shipped, const std::function<bool()>& cancelled,
                std::wstring* error) {
  if (!home.is_absolute() || home.filename().empty() || home == home.root_path() ||
      (!package.empty() && (Within(home, package) || Within(package, home))) ||
      (!legacy.empty() && (!legacy.is_absolute() || Within(home, legacy) || Within(legacy, home))))
    return Error(error, L"Choose a separate, absolute game folder, not inside your source folder.");
  const auto parent = home.parent_path();
  std::error_code ec;
  fs::create_directories(parent, ec);
  SetupLock publish, source_lock;
  if (ec || !publish.Acquire(parent / (L"." + PathWide(home.filename()) + L".install.lock")))
    return Error(error, L"Could not prepare " + PathWide(home) + L". Check permissions and close other setup windows.");
  if (!EmptyOrMissing(home))
    return Error(error, L"This game folder is not empty. Nothing was overwritten: " + PathWide(home));
#ifdef __APPLE__
  if (pathconf(parent.c_str(), _PC_CASE_SENSITIVE) == 1)
    return Error(error, L"Use a case-insensitive volume for the game folder (standard APFS or Mac OS Extended).");
#endif
#ifdef _WIN32
  SetupLock source_dashboard;
#endif

  if (!legacy.empty()) {
    if (Inspect(legacy) != Kind::Directory || !HasLocalState(legacy))
      return Error(error, L"No existing local game data found in " + PathWide(legacy));
    if (Inspect(legacy / "data") == Kind::Unsafe || Inspect(legacy / "data") == Kind::File)
      return Error(error, L"The original data folder is not a regular directory.");
    fs::create_directory(legacy / "data", ec);
    if (ec || !source_lock.Acquire(legacy / "data/setup.lock") ||
        Inspect(legacy / ".gtav-update/pending.txt") != Kind::Missing)
      return Error(error, L"Close the original game and launcher and finish any pending update before importing.");
#ifdef _WIN32
    if (!source_dashboard.Acquire(legacy / "data/launcher-ui.lock"))
      return Error(error, L"Close the original launcher before importing.");
#endif
    if (Inspect(legacy / "iphone") == Kind::Unsafe || Inspect(legacy / "android") == Kind::Unsafe)
      return Error(error, L"Cannot import linked phone folders.");
  }

  // Never reuse or delete a stage from an interrupted process. Only this new directory belongs
  // to this invocation, including on a case-insensitive filesystem.
#ifdef _WIN32
  const auto pid = GetCurrentProcessId();
#else
  const auto pid = getpid();
#endif
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto staging = parent / (".gtav-incoming-" + std::to_string(pid) + "-" + std::to_string(tick));
  if (!fs::create_directory(staging, ec))
    return Error(error, L"Could not create a staging folder beside " + PathWide(home));
  Stage stage(cancelled);
  stage.path = staging;
  for (const auto& rel : shipped)
    if (!stage.Copy(package / rel, staging / rel, true, error)) return false;
  if (!legacy.empty()) {
    for (const auto* rel : kMutable)
      if (!stage.Copy(legacy / rel, staging / rel, false, error)) return false;
  }
  if (stage.Cancelled()) return Error(error, L"Cancelled. The original installation was not changed.");
  if (!EmptyOrMissing(home)) return Error(error, L"The destination was changed while preparing the game. Nothing was replaced.");
  // Windows cannot rename a directory onto an empty directory. remove() is deliberately NOT
  // remove_all(): if anything has appeared there, publishing fails without touching it.
  if (Inspect(home) == Kind::Directory && !fs::remove(home, ec))
    return Error(error, L"Could not publish into " + PathWide(home));
#ifdef _WIN32
  if (!MoveFileExW(staging.c_str(), home.c_str(), MOVEFILE_WRITE_THROUGH))
    return Error(error, L"Could not publish the game folder: " + PathWide(home));
#else
  fs::rename(staging, home, ec);
  if (ec) return Error(error, L"Could not publish the game folder: " + Widen(ec.message()));
#endif
  stage.path.clear();
  return true;
}

#ifndef __APPLE__
bool LooksLikeRelease(const fs::path& folder) {
  const auto marker = folder / "gtav_release.txt";
  std::error_code ec;
  if (Inspect(marker) != Kind::File || fs::file_size(marker, ec) > 160 || ec ||
      Inspect(folder / kDesktopLauncher) != Kind::File) return false;
  std::ifstream in(marker);
  std::string line;
  return std::getline(in, line) && line.starts_with("gtav-recomp release folder ") &&
#ifdef _WIN32
      line.ends_with(" (windows-x64)");
#else
      line.ends_with(" (linux-x64)");
#endif
}
#endif

}  // namespace

fs::path DefaultGameHome() {
  fs::path profile;
#ifdef _WIN32
  wchar_t path[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, SHGFP_TYPE_CURRENT, path))) profile = path;
#else
  if (const char* home = std::getenv("HOME")) profile = home;
  if (!profile.is_absolute()) {
    if (const auto* user = getpwuid(getuid())) profile = user->pw_dir;
  }
#endif
  return profile.is_absolute() ? profile / "Games/GTAV Recomp" : fs::path();
}

bool HasLocalState(const fs::path& home) {
  for (const auto* rel : kMutable)
    if (Inspect(home / rel) != Kind::Missing) return true;
  return false;
}

bool SameFolder(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  return fs::equivalent(a, b, ec) && !ec;
}

#ifndef __APPLE__
bool OpenDesktopHome(const fs::path& package, const DesktopPaths& options, fs::path* result,
                     std::wstring* error, const std::function<bool()>& cancelled) {
  error->clear();
  *result = package;
  if (options.portable && (!options.home.empty() || !options.migrate_from.empty()))
    return Error(error, L"Use --portable by itself, or use --home and optionally --migrate-from.");
  if (options.portable) return true;
  if (options.home.empty() && options.migrate_from.empty() &&
      (HasLocalState(package) || !LooksLikeRelease(package))) return true;

  const auto home = options.home.empty() ? DefaultGameHome() : options.home;
  if (home.empty() || !home.is_absolute()) return Error(error, L"Choose an absolute game folder with --home.");
  if (SameFolder(package, home) && options.migrate_from.empty()) return true;
  if (options.migrate_from.empty() && LooksLikeRelease(home)) {
    *result = home;
    return true;
  }
  if (!LooksLikeRelease(package)) return Error(error, L"Use a complete desktop release to prepare the game folder.");

  std::vector<fs::path> shipped;
#ifdef _WIN32
  for (const auto* name : windows_package::kOwned) shipped.emplace_back(name);
#else
  for (const auto* name : {"tools", "LICENSES", kDesktopLauncher, "gtav_fast", "librexruntime.so",
                           "librexgpu-xenos.so", "libTracyClient.so", "rexglue", "README.txt", "gtav_release.txt"})
    shipped.emplace_back(name);
#endif
  // Optional phone shells from the distribution, not the local packs made with them.
  if (Inspect(package / "ios") != Kind::Missing) shipped.emplace_back("ios");
  if (Inspect(package / "android/GTAV.apk") != Kind::Missing) {
    if (Inspect(package / "android") != Kind::Directory) return Error(error, L"The Android shell folder is unsafe.");
    for (const auto* rel : {"GTAV.apk", "link", "interface", "include", "LICENSES"}) shipped.push_back(fs::path("android") / rel);
  }
  if (!Initialize(package, options.migrate_from, home, shipped, cancelled, error)) return false;
  *result = home;
  return true;
}
#else
bool ImportMacHome(const fs::path& from, const fs::path& home, std::wstring* error,
                   const std::function<bool()>& cancelled) {
  error->clear();
  if (!from.is_absolute()) return Error(error, L"Use an absolute source path with --migrate-from.");
  return Initialize({}, from, home, {}, cancelled, error);
}
#endif

}  // namespace loader
