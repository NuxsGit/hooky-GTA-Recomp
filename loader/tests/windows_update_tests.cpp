#include "windows_update.h"
#include "win_process.h"
#include "sources.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace loader;

#define CHECK(x) do { if (!(x)) { std::wcerr << L"Failed at line " << __LINE__ << L": " << Widen(#x) << L" " << error << '\n'; throw std::runtime_error(#x); } } while (false)

namespace {
void Put(const fs::path& path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
}

std::string Text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

void Stage(const fs::path& path) {
  for (const auto* file : {
       "tools/game_dll/expected.txt", "tools/game_dll/codegen.partition.json", "tools/game_dll/gtav_manifest.toml",
       "tools/game_dll/fs/gtav_pch.h", "tools/game_dll/interface/gtav_game.def", "tools/llvm/bin/clang++.exe",
       "tools/llvm/bin/lld-link.exe", "LICENSES/fixture.txt", "gtav_launcher.exe", "gtav_fast.exe",
       "rexruntimerd.dll", "rexgpu-xenosrd.dll", "TracyClientrd.dll", "SDL3rd.dll", "rexgluerd.exe",
       "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll",
       "vcruntime140_1.dll", "README.txt"}) Put(path / file, "new runtime");
  Put(path / "tools/game_dll/build-id.txt", std::string(64, 'a') + '\n');
  Put(path / "gtav_release.txt", "gtav-recomp release folder 99.0.0 (windows-x64)\n");
}

class Ui final : public InstallUi {
 public:
  bool cancelled = false;
  bool Cancelled() override { return cancelled; }
  bool Ask(const std::wstring&) override { return false; }
  void Stage(const std::wstring&) override {}
  void Log(const std::wstring&) override {}
  void Progress(double) override {}
};
std::wstring Quote(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }

constexpr std::array kPreserved = {"data/user/save", "data/user/sandbox/profile/save",
    "data/user/sandbox/courses/course-Track.gtcourse", "data/game/original", "data/update/title-update",
    "data/build/obj/cache", "data/config/graphics.ini", "gtav_game.dll", "gtav.toml", "gtav_settings.ini", "args.txt",
    "gamecontrollerdb.txt", "shader_cache/kept", "android/export"};

struct TestProcess {
  win::Handle handle;
  ~TestProcess() {
    if (handle && WaitForSingleObject(handle.Get(), 0) == WAIT_TIMEOUT) {
      TerminateProcess(handle.Get(), 1);
      WaitForSingleObject(handle.Get(), 2000);
    }
  }
};

struct WindowQuery {
  const wchar_t* caption;
  const wchar_t* class_name;
  DWORD pid;
  fs::path exe;
  HWND result = nullptr;
  bool visible = false;
};

BOOL CALLBACK MatchWindow(HWND hwnd, LPARAM context) {
  auto& q = *reinterpret_cast<WindowQuery*>(context);
  if (q.visible && !IsWindowVisible(hwnd)) return TRUE;
  wchar_t text[256];
  if (q.caption && (!GetWindowTextW(hwnd, text, 256) || std::wstring_view(text) != q.caption)) return TRUE;
  if (q.class_name && (!GetClassNameW(hwnd, text, 256) || std::wstring_view(text) != q.class_name)) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid || (q.pid && pid != q.pid)) return TRUE;
  if (!q.exe.empty()) {
    win::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    std::wstring path(32768, L'\0');
    DWORD size = DWORD(path.size());
    if (!process || !QueryFullProcessImageNameW(process.Get(), 0, path.data(), &size)) return TRUE;
    path.resize(size);
    if (CompareStringOrdinal(q.exe.c_str(), -1, path.c_str(), -1, TRUE) != CSTR_EQUAL) return TRUE;
  }
  q.result = hwnd;
  return FALSE;
}

struct LabelQuery { std::wstring_view expected; HWND result = nullptr; };
BOOL CALLBACK MatchLabel(HWND h, LPARAM context) {
  auto& q = *reinterpret_cast<LabelQuery*>(context);
  wchar_t text[256]{};
  DWORD_PTR copied = 0;
  if (SendMessageTimeoutW(h, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(text), SMTO_ABORTIFHUNG, 100, &copied) &&
      copied && q.expected == text) {
    q.result = h;
    return FALSE;
  }
  return TRUE;
}
HWND HasLabel(HWND window, std::wstring_view label) {
  LabelQuery q{label};
  EnumChildWindows(window, MatchLabel, reinterpret_cast<LPARAM>(&q));
  return q.result;
}

HWND WaitWindow(WindowQuery query, HANDLE process = nullptr, std::wstring_view label = {}) {
  const auto deadline = GetTickCount64() + 20'000;
  do {
    if (process && WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return nullptr;
    query.result = nullptr;
    EnumWindows(MatchWindow, reinterpret_cast<LPARAM>(&query));
    if (query.result && (label.empty() || HasLabel(query.result, label))) return query.result;
    Sleep(40);
  } while (GetTickCount64() < deadline);
  return nullptr;
}

void WaitExit(HANDLE process, DWORD timeout, DWORD expected = 0) {
  std::wstring error;
  CHECK(WaitForSingleObject(process, timeout) == WAIT_OBJECT_0);
  DWORD exit = 1;
  CHECK(GetExitCodeProcess(process, &exit));
  if (exit != expected) error = L"Process exit status: " + std::to_wstring(exit);
  CHECK(exit == expected);
}

// The previous launcher reads a fixture feed instead of GitHub; everything from discovery and
// auto-install scheduling through the game's lock, updater and restarted dashboard is real code.
void VerifyPackage(const fs::path& root, const fs::path& archive, const wchar_t* version, const wchar_t* hash,
                   const fs::path& candidate, const fs::path& old_launcher) {
  std::wstring error;
  const auto build = ParseBuildVersion(Narrow(version));
  CHECK(build);
  const bool nightly = build->Channel() == UpdateChannel::Nightly;
  CHECK(fs::create_directory(root));
  const auto launcher = root / "gtav_launcher.exe";
  CHECK(fs::copy_file(old_launcher, launcher));
  for (const auto* path : kPreserved) Put(root / path, "local content");
  Put(root / "tools/original.txt", "old tools");
  Put(root / "gtav_fast.exe", "previous host");
  Put(root / "gtav_release.txt", "previous package");
  const UpdatePreferences prefs{build->Channel(), true};
  CHECK(SaveWindowsUpdatePreferences(root, prefs));
  const auto feed = root / ".test-feed";
  std::string release = "{\"tag_name\":\"v" + Narrow(version) + "\",\"draft\":false,\"prerelease\":" + (nightly ? "true" : "false") +
      ",\"assets\":[{\"name\":\"" + std::string(kWindowsPackageName) + "\",\"state\":\"uploaded\",\"digest\":\"sha256:" + Narrow(hash) +
      "\",\"size\":" + std::to_string(fs::file_size(archive)) + "}]}";
  Put(feed / (nightly ? "nightly.json" : "stable.json"), nightly ? '[' + release + ']' : release);
  CHECK(fs::copy_file(archive, feed / "asset.zip"));
  DWORD old_pid = 0;
  TestProcess parent;
  {
    win::Handle game(win::OpenSetupLock(root / "data/setup.lock"));
    CHECK(game && win::Spawn(launcher, Quote(launcher), root, 0, &parent.handle));
    old_pid = GetProcessId(parent.handle.Get());
    CHECK(old_pid && WaitWindow({nullptr, L"GtavRecompDashboard", old_pid, launcher}, parent.handle.Get(),
        L"Mise \u00e0 jour pr\u00eate. Elle attend la fermeture du jeu ou de l\u2019installation en cours."));
    CHECK(!HasPendingWindowsUpdate(root) && !fs::exists(root / ".gtav-update/verified.zip"));
    CHECK(Text(root / "gtav_fast.exe") == "previous host");
  }
  WaitExit(parent.handle.Get(), 200'000);  // releasing the game's lock lets automatic installation proceed
  auto dashboard = WaitWindow({nullptr, L"GtavRecompDashboard", 0, launcher});
  if (!dashboard) error = Widen(Text(root / "data/logs/update.log"));
  CHECK(dashboard);
  DWORD pid = 0;
  GetWindowThreadProcessId(dashboard, &pid);
  TestProcess restarted;
  restarted.handle.Set(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid));
  CHECK(restarted.handle && pid != old_pid);
  CHECK(WaitWindow({nullptr, L"GtavRecompDashboard", pid, launcher}, restarted.handle.Get(),
                   L"Mise \u00e0 jour install\u00e9e. Choisis tes disques pour commencer."));
  CHECK(LoadWindowsUpdatePreferences(root) == prefs);
  // Exercise real controls in the restarted dashboard, without contacting unpopulated feeds.
  const auto updates = HasLabel(dashboard, L"Mises \u00e0 jour");
  CHECK(updates && IsWindowVisible(updates));
  CHECK(SendMessageTimeoutW(dashboard, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(updates), BN_CLICKED),
                            reinterpret_cast<LPARAM>(updates), SMTO_ABORTIFHUNG, 2'000, nullptr));
  const auto auto_button = HasLabel(dashboard, L"Automatique : activ\u00e9");
  const auto download = HasLabel(dashboard, L"Installer maintenant");
  CHECK(auto_button && IsWindowVisible(auto_button) && download && !IsWindowEnabled(download));
  CHECK(SendMessageTimeoutW(dashboard, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(auto_button), BN_CLICKED),
                            reinterpret_cast<LPARAM>(auto_button), SMTO_ABORTIFHUNG, 2'000, nullptr));
  CHECK(LoadWindowsUpdatePreferences(root) == (UpdatePreferences{prefs.channel, false}));
  const auto other = nightly ? UpdateChannel::Stable : UpdateChannel::Nightly;
  const auto channel = HasLabel(dashboard, nightly ? L"Stable   /   Recommand\u00e9" : L"Nightly   /   Exp\u00e9rimental");
  CHECK(channel && IsWindowVisible(channel));
  CHECK(PostMessageW(dashboard, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(channel), BN_CLICKED), reinterpret_cast<LPARAM>(channel)));
  if (!nightly) {
    const auto confirm = WaitWindow({L"Canal Nightly", L"#32770", pid, {}}, restarted.handle.Get());
    CHECK(confirm);
    CHECK(PostMessageW(confirm, WM_COMMAND, IDYES, 0));
  }
  CHECK(WaitWindow({nullptr, L"GtavRecompDashboard", pid, launcher}, restarted.handle.Get(),
                   L"Canal s\u00e9lectionn\u00e9. Lance une recherche pour voir les versions disponibles."));
  CHECK(LoadWindowsUpdatePreferences(root) == (UpdatePreferences{other, false}));
  CHECK(PostMessageW(dashboard, WM_CLOSE, 0, 0));
  WaitExit(restarted.handle.Get(), 10'000);
  CHECK(!HasPendingWindowsUpdate(root) && !fs::exists(root / ".gtav-update/stage"));
  CHECK(Text(root / "data/logs/update.log").find("Installed") != std::string::npos);
  CHECK(Text(root / "gtav_release.txt") == Text(candidate.parent_path() / "gtav_release.txt"));
  CHECK(Text(root / ".gtav-update/backup/tools/original.txt") == "old tools");
  CHECK(Text(root / ".gtav-update/backup/gtav_fast.exe") == "previous host");
  CHECK(Text(launcher) == Text(candidate));
  CHECK(Text(root / ".gtav-update/backup/gtav_launcher.exe") == Text(old_launcher));
  CHECK(fs::file_size(root / "gtav_fast.exe") == fs::file_size(candidate.parent_path() / "gtav_fast.exe"));
  for (const auto* path : kPreserved) CHECK(Text(root / path) == "local content");
  fs::remove_all(root);
  std::cout << "Automatic Windows update waited for game lock, verified exact ZIP and relaunched; data and settings preserved\n";
}

void VerifyHostSelector(const fs::path& root) {
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  std::wstring error;
  const auto host = root / "gtav_fast.exe";
  // The fixture DLL satisfies the imports, and traps any attempt to run a guest function. A missing
  // install must be recognized before the title runs. No GPU or original game material is needed.
  const auto workspace = root / "data/user/sandbox";
  const auto settings = root / "data/config/graphics.ini";
  const auto legacy = root / "gtav_settings.ini";
  const char* legacy_values = "gtav_native_output=720\ngtav_fps_limit=60\n";
  Put(legacy, legacy_values);
  enum class Case { SaveGraphics, ReloadGraphics, CliOverride, MissingInstall, UnicodeHome };
  for (auto test : {Case::SaveGraphics, Case::ReloadGraphics, Case::CliOverride, Case::MissingInstall, Case::UnicodeHome}) {
    const bool enter = test == Case::MissingInstall || test == Case::UnicodeHome;
    const bool have_partitions = test == Case::UnicodeHome;
    if (have_partitions) fs::create_directories(root / "data/user/0000000000000000/545408A7/00000002");
    TestProcess process;
    const auto log = root / "data/logs" / ("host-smoke-" + std::to_string(int(test)) + ".log");
    auto command = Quote(host) + L" --gtav_mode=menu --gtav_native=on --headless=true " +
        Quote(L"--user_data_root=" + (root / "data/user").wstring()) + L" " +
        Quote(L"--gtav_settings_path=" + settings.wstring()) + L" " + Quote(L"--log_file=" + log.wstring());
    if (test == Case::CliOverride) command += L" --gtav_native_output=1440";
    CHECK(win::Spawn(host, command, root, 0, &process.handle));
    const auto pid = GetProcessId(process.handle.Get());
    CHECK(pid);
    auto modes = WaitWindow({L"GTA V Recomp | Play", nullptr, pid, {}, nullptr, true}, process.handle.Get());
    if (!modes) error = Widen(Text(log));
    CHECK(modes);
    wchar_t class_name[64]{};
    CHECK(GetClassNameW(modes, class_name, 64) && std::wstring_view(class_name) != L"#32770");  // not the fallback
    auto key = [&](WPARAM vk) {
      CHECK(PostMessageW(modes, WM_KEYDOWN, vk, 0));  // virtual-key input, no hardware required
      CHECK(PostMessageW(modes, WM_KEYUP, vk, 0));
    };
    auto click = [&](int x, int y) {
      // Click the software hub's 1440x900 coordinate space. Unlike a posted F1 key, this
      // does not need the CI runner to grant the new process real keyboard focus.
      RECT rect{};
      CHECK(GetClientRect(modes, &rect) && rect.right > 0 && rect.bottom > 0);
      const float scale = std::min(rect.right / 1440.0f, rect.bottom / 900.0f);
      const auto point = MAKELPARAM((rect.right - int(1440 * scale)) / 2 + int(x * scale),
                                   (rect.bottom - int(900 * scale)) / 2 + int(y * scale));
      CHECK(PostMessageW(modes, WM_MOUSEMOVE, 0, point));
      CHECK(PostMessageW(modes, WM_LBUTTONDOWN, MK_LBUTTON, point));
      CHECK(PostMessageW(modes, WM_LBUTTONUP, 0, point));
    };
    if (test == Case::SaveGraphics || test == Case::CliOverride) {
      click(1265, 193);  // Graphismes tab.
      click(916, 310);   // 720p -> 1080p, or ignored when output is forced.
      if (test == Case::SaveGraphics) {
        click(916, 374);  // Scene scale 100 -> 87.5%.
        click(916, 434);  // Linear -> FSR 1.
      } else {
        click(588, 374);  // Change another value while output is forced.
      }
    }
    if (!enter) CHECK(PostMessageW(modes, WM_CLOSE, 0, 0));
    if (enter) {
      CHECK(PostMessageW(modes, WM_SETFOCUS, 0, 0));
      key(VK_LEFT);
      key(VK_DOWN);
      key(VK_RETURN);  // campaign co-op is locked, Enter must not leave the hub
      key(VK_RIGHT);
      key(VK_UP);
      key(VK_RETURN);  // Sandbox solo routes through Workshop's original validation
      // The final pass supplies only the installed-content directory, not the game or even
      // --game_data_root. The hub must mount the exact Workshop path (including the accented
      // name in the launcher fixture) and the SDK must then reject the missing game root.
      auto missing = WaitWindow({have_partitions ? L"ReXGlue" : L"GTA V Recomp | Workshop", L"#32770", pid, {}},
          process.handle.Get(), have_partitions ? L"--game_data_root was not provided." :
          L"Installed TU27 game data is missing. Install the game with the launcher first.");
      CHECK(missing);
      CHECK(PostMessageW(missing, WM_COMMAND, IDCANCEL, 0));
    }
    WaitExit(process.handle.Get(), 10'000, have_partitions ? 1 : 0);
    CHECK(Text(legacy) == legacy_values);
    const auto saved = Text(settings);
    const auto lines = Text(log);
    const auto graphics = lines.find("[graphics] preferences:");
    error = L"Pass " + std::to_wstring(int(test)) + L", file: " + Widen(saved.empty() ? "<empty or missing>" : saved);
    if (graphics != lines.npos) error += L"\n" + Widen(lines.substr(graphics, 1200));
    CHECK(saved.find("gtav_native_output=1080\n") != std::string::npos && saved.find("gtav_fps_limit=60\n") != std::string::npos);
    CHECK(saved.find("gtav_native_fsr1=true\n") != std::string::npos);
    CHECK(saved.find(test == Case::SaveGraphics || test == Case::ReloadGraphics ?
        "gtav_native_render_scale=0.875\n" : "gtav_native_render_scale=1\n") != std::string::npos);
    for (const auto& entry : fs::directory_iterator(settings.parent_path()))
      CHECK(!entry.path().filename().wstring().starts_with(L"graphics.ini.new-"));
    const auto* output = test == Case::SaveGraphics ? "=720" : test == Case::CliOverride ? "=1440 (launch override)" : "=1080";
    CHECK(lines.find(std::string("[graphics] startup gtav_native_output") + output) != std::string::npos);
    if (test != Case::SaveGraphics) CHECK(lines.find("[graphics] startup gtav_native_fsr1=true") != std::string::npos);
    CHECK(fs::is_directory(workspace / "profile") == have_partitions);
    CHECK(fs::is_directory(workspace / "courses") == have_partitions);
    const auto config = workspace / "profile/config";
    CHECK(fs::is_directory(config) == have_partitions);
    CHECK(!fs::exists(root / "data/user/config"));
    if (have_partitions) {
      for (const auto* name : {"permissions.json", "bans.json", "build_favorites.json", "presets.json"}) {
        CHECK(Text(config / name).find("\"version\": 1") != std::string::npos);
        CHECK(!fs::exists(config / (std::string(name) + ".new")));
      }
    }
  }
  std::cout << "Packaged host saved/reloaded graphics in the accented install, honored a CLI override, blocked co-op and flushed the isolated Workshop profile\n";
}
}  // namespace

int wmain(int argc, wchar_t** argv) try {
  std::wstring error;
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  if (argc == 8 && std::wstring_view(argv[1]) == L"--verify-package") {
    VerifyPackage(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]);
    return 0;
  }
  if (argc == 3 && std::wstring_view(argv[1]) == L"--verify-host-selector") {
    VerifyHostSelector(argv[2]);
    return 0;
  }
  CHECK(argc == 1);
  std::wstring host, resource;
  CHECK(detail::ParseWindowsAssetRedirect(L"https://release-assets.githubusercontent.com/path?sig=one%2Btwo", host, resource));
  CHECK(host == L"release-assets.githubusercontent.com" && resource == L"/path?sig=one%2Btwo");
  CHECK(detail::ParseWindowsAssetRedirect(L"https://objects.githubusercontent.com/asset?x=1", host, resource));
  for (const auto* bad : {L"http://release-assets.githubusercontent.com/x", L"https://release-assets.githubusercontent.com.evil.invalid/x",
                         L"https://evil.invalid@release-assets.githubusercontent.com/x", L"https://release-assets.githubusercontent.com:8443/x",
                         L"https://release-assets.githubusercontent.com/x#fragment", L"//release-assets.githubusercontent.com/x",
                         L"https://release-assets.githubusercontent.com/x y", L"https://api.github.com/x"})
    CHECK(!detail::ParseWindowsAssetRedirect(bad, host, resource));
  // Non-ASCII filename and paths with spaces. All fixtures are tiny, invented files.
  const auto root = fs::temp_directory_path() / (L"GTAV update \u00e9 fixture " + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(root, ec);
  CHECK(WindowsUpdateIdle(root));
  CHECK(SaveWindowsUpdatePreferences(root, {UpdateChannel::Nightly, false}));
  CHECK(LoadWindowsUpdatePreferences(root) == (UpdatePreferences{UpdateChannel::Nightly, false}));
  Put(root / "data/launcher-updates.txt", std::string(256, 'x'));
  CHECK(LoadWindowsUpdatePreferences(root) == (UpdatePreferences{UpdateChannel::Stable, false}));
  const auto work = root / ".gtav-update";
  const auto stage = work / "stage/gtav-recomp-windows-x64";
  for (const auto* path : kPreserved) Put(root / path, "local content");
  Put(root / "gtav_launcher.exe", "previous launcher");
  Put(root / "gtav_fast.exe", "previous host");
  Put(root / "tools/original.txt", "old tools");

  const auto archive = root / L"official \u00e9.zip";
  Put(archive, "not an archive");
  Ui ui;
  UpdateAsset asset{"99.0.0", std::string(64, 'b'), fs::file_size(archive)};
  CHECK(PrepareWindowsUpdate(root, archive, asset, ui, &error) == UpdatePreparation::Failed);
  CHECK(error.find(L"SHA-256 mismatch") != error.npos && !HasPendingWindowsUpdate(root));
  CHECK(Text(root / "gtav_fast.exe") == "previous host");
  {
    win::Handle lock(win::OpenSetupLock(root / "data/setup.lock"));
    CHECK(lock && !WindowsUpdateIdle(root) && PrepareWindowsUpdate(root, archive, asset, ui, &error) == UpdatePreparation::Deferred);
    CHECK(error.find(L"Close the game") != error.npos);
  }
  ui.cancelled = true;
  CHECK(PrepareWindowsUpdate(root, archive, asset, ui, &error) == UpdatePreparation::Failed && error == L"Cancelled.");
  ui.cancelled = false;

  // Wrong version / player-data contamination never reach the transaction.
  Stage(stage);
  CHECK(!detail::JournalUpdate(root, "99.0.1", &error));
  CHECK(!HasPendingWindowsUpdate(root));
  Put(stage / "data/user/foreign-save", "must not import");
  CHECK(!detail::JournalUpdate(root, "99.0.0", &error));
  fs::remove_all(stage / "data");
  Put(stage.parent_path() / L"unexpected.txt", "extra root entry");
  CHECK(!detail::JournalUpdate(root, "99.0.0", &error));
  fs::remove(stage.parent_path() / L"unexpected.txt");
  const auto kept = stage / "selected.zip";
  Put(kept, "not an archive");
  CHECK(PrepareWindowsUpdate(root, kept, asset, ui, &error) == UpdatePreparation::Failed);
  CHECK(Text(kept) == "not an archive");
  fs::remove(kept);

  // Deny rename of a later asset. Earlier paths must be restored, not left half updated.
  CHECK(detail::JournalUpdate(root, "99.0.0", &error));
  {
    win::Handle busy(CreateFileW((root / "gtav_fast.exe").c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    CHECK(busy);
    CHECK(!detail::ApplyUpdate(root, &error) && HasPendingWindowsUpdate(root));
    CHECK(Text(root / "gtav_fast.exe") == "previous host");
    CHECK(detail::RollbackUpdate(root, &error));
  }
  CHECK(!HasPendingWindowsUpdate(root) && Text(root / "tools/original.txt") == "old tools");
  CHECK(!fs::exists(root / "LICENSES"));  // a newly added path was also removed by rollback
  CHECK(Text(root / "gtav_launcher.exe") == "previous launcher");
  for (const auto* path : kPreserved) CHECK(Text(root / path) == "local content");

  // Simulate restart after a crash between moving old tools away and placing new ones.
  CHECK(detail::JournalUpdate(root, "99.0.0", &error));
  fs::rename(root / "tools", work / "backup/tools");
  CHECK(detail::RollbackUpdate(root, &error));
  CHECK(Text(root / "tools/original.txt") == "old tools");
  CHECK(detail::JournalUpdate(root, "99.0.0", &error));
  CHECK(detail::ApplyUpdate(root, &error));
  CHECK(!HasPendingWindowsUpdate(root));
  CHECK(Text(root / "gtav_launcher.exe") == "new runtime");
  CHECK(Text(work / "backup/gtav_launcher.exe") == "previous launcher");
  CHECK(Text(work / "backup/tools/original.txt") == "old tools");
  CHECK(Text(root / "tools/llvm/bin/clang++.exe") == "new runtime");
  for (const auto* path : kPreserved) CHECK(Text(root / path) == "local content");
  fs::remove_all(root, ec);
  CHECK(!ec);
  return 0;
}
catch (const std::exception& e) {
  std::cerr << "Update/launch check failed: " << e.what() << '\n';
  return 1;
}
