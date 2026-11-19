#include "windows_update.h"
#include "win_process.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace loader;

// No online dependency: mismatch/cancellation are exercised with a deterministic digest.
namespace loader {
bool HashUpdateArchive(const fs::path& path, std::string* digest, uint64_t* size, InstallUi& ui) {
  *size = fs::file_size(path);
  *digest = std::string(64, 'a');
  return !ui.Cancelled();
}
}  // namespace loader

#define CHECK(x) do { if (!(x)) { std::wcerr << L"Failed at line " << __LINE__ << L": " << error << '\n'; std::exit(1); } } while (false)

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
}  // namespace

int main() {
  std::wstring error;
  // Non-ASCII filename and paths with spaces. All fixtures are tiny, invented files.
  const auto root = fs::temp_directory_path() / (L"GTAV update \u00e9 fixture " + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(root, ec);
  const auto work = root / ".gtav-update";
  const auto stage = work / "stage/gtav-recomp-windows-x64";
  const std::array protected_paths = {"data/user/save", "data/game/original", "data/build/obj/cache",
      "gtav_game.dll", "gtav.toml", "gtav_settings.ini", "args.txt", "gamecontrollerdb.txt", "shader_cache/kept", "android/export"};
  for (const auto* path : protected_paths) Put(root / path, "local content");
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
  for (const auto* path : protected_paths) CHECK(Text(root / path) == "local content");

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
  for (const auto* path : protected_paths) CHECK(Text(root / path) == "local content");
  fs::remove_all(root, ec);
  CHECK(!ec);
  return 0;
}
