// gtav launcher - first-run install: the player's own discs and title update in, a playable
// folder out. Nothing retail ships with the launcher; everything below data\ and gtav_game.dll is
// made here from the player's files.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace loader {

struct InstallInputs {
  std::wstring disc1;  // Disc 1 (Install): ISO image or extracted folder
  std::wstring disc2;  // Disc 2 (Play): ISO image or extracted folder
  std::wstring tu;     // Title Update 27: the STFS package file or an extracted folder
};

// Callbacks into the window; called from the install thread.
class InstallUi {
 public:
  virtual ~InstallUi() = default;
  virtual void Stage(const std::wstring& text) = 0;   // one line: what is happening now
  virtual void Log(const std::wstring& text) = 0;     // appended to the log box and install.log
  virtual void Progress(double fraction) = 0;         // 0..1 for the whole install
  virtual bool Cancelled() = 0;
  virtual bool Ask(const std::wstring& question) = 0;  // yes/no, blocks the install thread
};

// Game data only (iPhone setup screen): Disc 2 files and TU27 into home/data/{game,update}, the
// Disc 1 install partitions into home/data/user, each checked against `expected` (expected.txt).
bool RunDataInstall(const std::filesystem::path& home, const std::filesystem::path& expected,
                    const InstallInputs& in, InstallUi& ui, std::wstring* error);
bool IsDataInstalled(const std::filesystem::path& home);
// iPhone, on any desktop system: from the installed game, <home>/iphone/GTAV.ipa (the package's iOS
// shell with gtav_game.dylib built for iOS and the shaders translated for it; no game files: the
// phone installs those from the player's discs). macOS: root is the package, home the game folder;
// Windows / Linux: both the installed launcher folder.
bool RunIphoneBuild(const std::filesystem::path& root, const std::filesystem::path& home, InstallUi& ui,
                    std::wstring* error);
// The Android build, from any desktop system: libgtav_game.so (aarch64 Android, against the APK's
// libgtav_fast.so), the shaders for Adreno and expected.txt in <home>/android/gtav-android-pack.zip.
// The APK (no game code) imports the pack and installs the game files from the discs itself.
bool RunAndroidBuild(const std::filesystem::path& root, const std::filesystem::path& home, InstallUi& ui,
                     std::wstring* error);

// root is the release's runtime and tools; home is the player's data/ and game library.
// On Windows/Linux they are both the installed folder (not the extracted download). GetInstallStatus just checks the file inventory and
// the local build stamp. RunRebuild checks the data again and builds from it without the discs.
enum class InstallStatus { NeedsSources, NeedsRebuild, Ready };
InstallStatus GetInstallStatus(const std::filesystem::path& root, const std::filesystem::path& home);
const wchar_t* InstallStatusText(InstallStatus status);
bool RunRebuild(const std::filesystem::path& root, const std::filesystem::path& home,
                InstallUi& ui, std::wstring* error);
bool RunInstall(const std::filesystem::path& root, const InstallInputs& in, InstallUi& ui,
                std::wstring* error);
// macOS: root is the read-only package inside the app, home the game folder everything made goes
// to (gtav_game.dylib, data/).
bool RunInstall(const std::filesystem::path& root, const std::filesystem::path& home,
                const InstallInputs& in, InstallUi& ui, std::wstring* error);
// extra: more game arguments after "--". On Linux the launcher becomes the game.
#ifdef __APPLE__
// Copy bundled runtime files only when changed, then launch with the same held game lock.
bool LaunchMacGame(const std::filesystem::path& package, const std::filesystem::path& home,
                   std::wstring* error, const std::vector<std::wstring>& extra = {});
#else
bool LaunchGame(const std::filesystem::path& root, std::wstring* error,
                const std::vector<std::wstring>& extra = {});
#endif
#ifdef _WIN32
namespace win { class Handle; }
// The dashboard can track the host while the existing inherited install lock protects its files.
bool LaunchWindowsGame(const std::filesystem::path& root, std::wstring* error,
                       const std::vector<std::wstring>& extra, win::Handle* process);
#endif
// Checks that can fail before anything is touched (CPU, runtime DLLs, bundled tools).
bool Preflight(const std::filesystem::path& root, std::wstring* error);
#ifdef __APPLE__
// The game folder can be created and is on a case-insensitive volume.
bool CheckGameFolder(const std::filesystem::path& home, std::wstring* error);
#endif

}  // namespace loader
