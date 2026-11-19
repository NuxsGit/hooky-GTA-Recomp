#include "windows_update.h"
#include "win_process.h"
#include "sources.h"

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
    "data/build/obj/cache", "gtav_game.dll", "gtav.toml", "gtav_settings.ini", "args.txt",
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
};

BOOL CALLBACK MatchWindow(HWND hwnd, LPARAM context) {
  auto& q = *reinterpret_cast<WindowQuery*>(context);
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

void WaitExit(HANDLE process, DWORD timeout) {
  std::wstring error;
  CHECK(WaitForSingleObject(process, timeout) == WAIT_OBJECT_0);
  DWORD exit = 1;
  CHECK(GetExitCodeProcess(process, &exit));
  if (exit) error = L"Process exit status: " + std::to_wstring(exit);
  CHECK(exit == 0);
}

// Real helper, real official-format ZIP, real restarted launcher. The previous install is disposable:
// its helper executable is bootstrapped from the candidate, not dependent on a past release URL.
void VerifyPackage(const fs::path& test, const fs::path& root, const fs::path& archive,
                   const wchar_t* version, const wchar_t* hash, const fs::path& candidate) {
  std::wstring error;
  CHECK(fs::create_directory(root));
  const auto launcher = root / "gtav_launcher.exe";
  CHECK(fs::copy_file(candidate, launcher));
  for (const auto* path : kPreserved) Put(root / path, "local content");
  Put(root / "tools/original.txt", "old tools");
  Put(root / "gtav_fast.exe", "previous host");
  Put(root / "gtav_release.txt", "previous package");
  TestProcess parent;
  CHECK(win::Spawn(test, Quote(test) + L" --prepare-package " + Quote(root) + L" " + Quote(archive) + L" " +
                       version + L" " + hash, root, 0, &parent.handle));
  WaitExit(parent.handle.Get(), 200'000);
  auto dashboard = WaitWindow({nullptr, L"GtavRecompDashboard", 0, launcher});
  if (!dashboard) error = Widen(Text(root / "data/logs/update.log"));
  CHECK(dashboard);
  DWORD pid = 0;
  GetWindowThreadProcessId(dashboard, &pid);
  TestProcess restarted;
  restarted.handle.Set(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid));
  CHECK(restarted.handle);
  CHECK(WaitWindow({nullptr, L"GtavRecompDashboard", pid, launcher}, restarted.handle.Get(),
                   L"Update installed. Select your discs to set up the game."));
  CHECK(PostMessageW(dashboard, WM_CLOSE, 0, 0));
  WaitExit(restarted.handle.Get(), 10'000);
  CHECK(!HasPendingWindowsUpdate(root) && !fs::exists(root / ".gtav-update/stage"));
  CHECK(Text(root / "data/logs/update.log").find("Installed") != std::string::npos);
  CHECK(Text(root / "gtav_release.txt") == Text(candidate.parent_path() / "gtav_release.txt"));
  CHECK(Text(root / ".gtav-update/backup/tools/original.txt") == "old tools");
  CHECK(Text(root / ".gtav-update/backup/gtav_fast.exe") == "previous host");
  CHECK(Text(launcher) == Text(candidate));
  CHECK(fs::file_size(root / "gtav_fast.exe") == fs::file_size(candidate.parent_path() / "gtav_fast.exe"));
  for (const auto* path : kPreserved) CHECK(Text(root / path) == "local content");
  fs::remove_all(root);
  std::cout << "Windows ZIP verified, applied and relaunched; player data preserved\n";
}

void VerifyHostSelector(const fs::path& root) {
  std::wstring error;
  const auto host = root / "gtav_fast.exe";
  // The fixture DLL satisfies the imports, and traps any attempt to run a guest function. A missing
  // install must be recognized before the title runs. No GPU or original game material is needed.
  for (bool enter : {false, true}) {
    TestProcess process;
    const auto command = Quote(host) + L" --gtav_mode=menu --gtav_native=on --headless=true " +
        Quote(L"--user_data_root=" + (root / "data/user").wstring()) + L" " +
        Quote(L"--log_file=" + (root / "data/logs/host-smoke.log").wstring());
    CHECK(win::Spawn(host, command, root, 0, &process.handle));
    const auto pid = GetProcessId(process.handle.Get());
    CHECK(pid);
    auto modes = WaitWindow({L"GTA V Recomp | Play", L"#32770", pid, {}}, process.handle.Get());
    if (!modes) error = Widen(Text(root / "data/logs/host-smoke.log"));
    CHECK(modes);
    const auto solo = HasLabel(modes, L"Sandbox solo");
    CHECK(solo && HasLabel(modes, L"Private multiplayer") && HasLabel(modes, L"Campaign") && HasLabel(modes, L"Quit"));
    DWORD_PTR def = 0;
    CHECK(SendMessageTimeoutW(modes, DM_GETDEFID, 0, 0, SMTO_ABORTIFHUNG, 1000, &def));
    CHECK(HIWORD(def) == DC_HASDEFID && LOWORD(def) == GetDlgCtrlID(solo));
    CHECK(PostMessageW(modes, WM_COMMAND, enter ? IDOK : IDCANCEL, 0));
    if (enter) {
      auto missing = WaitWindow({L"GTA V Recomp | Workshop", L"#32770", pid, {}}, process.handle.Get());
      CHECK(missing && HasLabel(missing, L"Installed TU27 game data is missing. Install the game with the launcher first."));
      CHECK(PostMessageW(missing, WM_COMMAND, IDCANCEL, 0));
    }
    WaitExit(process.handle.Get(), 10'000);
    CHECK(!fs::exists(root / "data/user/sandbox"));
  }
  std::cout << "Packaged host reached the mode selector and exited cleanly without a retail installation\n";
}
}  // namespace

int wmain(int argc, wchar_t** argv) try {
  std::wstring error;
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  if (argc == 7 && std::wstring_view(argv[1]) == L"--verify-package") {
    VerifyPackage(fs::absolute(argv[0]), argv[2], argv[3], argv[4], argv[5], argv[6]);
    return 0;
  }
  if (argc == 6 && std::wstring_view(argv[1]) == L"--prepare-package") {
    const fs::path root = argv[2], archive = argv[3];
    Ui ui;
    CHECK(PrepareWindowsUpdate(root, archive, {Narrow(argv[4]), Narrow(argv[5]), fs::file_size(archive)}, ui, &error));
    // The helper must wait for this parent and hold the game/setup lock across that handoff.
    CHECK(HasPendingWindowsUpdate(root) && Text(root / "gtav_fast.exe") == "previous host");
    win::Handle lock(win::OpenSetupLock(root / "data/setup.lock"));
    CHECK(!lock);
    return 0;
  }
  if (argc == 3 && std::wstring_view(argv[1]) == L"--verify-host-selector") {
    VerifyHostSelector(argv[2]);
    return 0;
  }
  CHECK(argc == 1);
  // Non-ASCII filename and paths with spaces. All fixtures are tiny, invented files.
  const auto root = fs::temp_directory_path() / (L"GTAV update \u00e9 fixture " + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(root, ec);
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
  CHECK(!PrepareWindowsUpdate(root, archive, asset, ui, &error));
  CHECK(error.find(L"SHA-256 mismatch") != error.npos && !HasPendingWindowsUpdate(root));
  CHECK(Text(root / "gtav_fast.exe") == "previous host");
  {
    win::Handle lock(win::OpenSetupLock(root / "data/setup.lock"));
    CHECK(lock && !PrepareWindowsUpdate(root, archive, asset, ui, &error));
    CHECK(error.find(L"Close the game") != error.npos);
  }
  ui.cancelled = true;
  CHECK(!PrepareWindowsUpdate(root, archive, asset, ui, &error) && error == L"Cancelled.");
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
  CHECK(!PrepareWindowsUpdate(root, kept, asset, ui, &error));
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
