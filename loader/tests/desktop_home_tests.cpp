#include "desktop_home.h"
#include "setup_lock.h"
#include "sources.h"
#include "frontend/modes.h"
#ifdef _WIN32
#include "windows_package_layout.h"
#endif

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace loader;

#define CHECK(x) do { if (!(x)) { std::cerr << "Line " << __LINE__ << ": " << Narrow(error) << '\n'; throw std::runtime_error(#x); } } while (false)

namespace {
struct TempDir {
  fs::path path = fs::temp_directory_path() /
      PathFromUtf8("gtav-home-é-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { fs::create_directory(path); }
  ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

void Put(const fs::path& path, std::string_view content = "local only") {
  fs::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary);
  f << content;
  if (!f) throw std::runtime_error("fixture write failed");
}

std::string Read(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
}  // namespace

int main() try {
  std::wstring error;
  const auto default_home = DefaultGameHome();
  CHECK(default_home.is_absolute() && default_home.filename() == "GTAV Recomp" &&
        default_home.parent_path().filename() == "Games");
  TempDir tmp;
  const auto target = tmp.path / "Games/GTAV Recomp", package = tmp.path / "Unpacked package",
             old = tmp.path / "Old install", fresh = tmp.path / "Move to this folder";
  const auto story = fs::path("data/user/545408A7/save0"), workshop = fs::path("data/user/sandbox/profile/save0"),
             part = fs::path("data/user/0000000000000000/545408A7/00000002/part0.rpf");
  Put(old / story, "first story save");
  Put(old / workshop, "isolated workshop save");
  Put(old / part, "not a saved game");
  Put(old / "data/user/sandbox/courses/track.gtcourse", "sandbox course");
  Put(old / "data/build/obj/gtav_init.o", "local object");
  Put(old / "data/spirv/user.spv", "local shader");
  Put(old / "gtav_settings.ini", "local settings");
  Put(old / "logs/native_dump/big-dump", "do not import");

#ifdef __APPLE__
  const auto prepare = [&](const fs::path& dest, const fs::path& from, std::function<bool()> cancelled = {}) {
    return ImportMacHome(from, dest, &error, cancelled);
  };
#else
#ifdef _WIN32
  for (const auto* name : windows_package::kOwned) {
    if (windows_package::IsFolder(name)) Put(package / name / "fixture.txt");
    else Put(package / name, "shipped");
  }
  Put(package / "gtav_release.txt", "gtav-recomp release folder 99.0.0 (windows-x64)\n");
#else
  for (const auto* name : {"gtav_fast", "librexruntime.so", "librexgpu-xenos.so", "libTracyClient.so", "rexglue",
                           "README.txt", "gtav_launcher", "tools/fixture.txt", "LICENSES/fixture.txt"}) Put(package / name, "shipped");
  Put(package / "gtav_release.txt", "gtav-recomp release folder 99.0.0 (linux-x64)\n");
#endif
  Put(package / "unrelated.zip", "never copied");
  fs::path selected;
  const auto prepare = [&](const fs::path& dest, const fs::path& from, std::function<bool()> cancelled = {}) {
    return OpenDesktopHome(package, {dest, from}, &selected, &error, cancelled);
  };
  CHECK(OpenDesktopHome(old, {}, &selected, &error) && selected == old);  // keep a legacy root
  CHECK(OpenDesktopHome(package, {{}, {}, true}, &selected, &error) && selected == package);
  CHECK(!OpenDesktopHome(package, {target, {}, true}, &selected, &error));
  CHECK(prepare(target, {}) && selected == target);
  CHECK(Read(target / kDesktopLauncher) == "shipped");
  CHECK(!fs::exists(target / "unrelated.zip") && !fs::exists(target / "data"));
  Put(target / story, "keep me");
  Put(package / "README.txt", "the extracted download is not an installed update");
  CHECK(prepare(target, {}) && selected == target && Read(target / "README.txt") == "shipped");
  CHECK(!prepare(target, old) && Read(target / story) == "keep me");
#endif

  Put(tmp.path / "Conflicting saves" / story, "already played here");
  CHECK(!prepare(tmp.path / "Conflicting saves", old));
  CHECK(Read(tmp.path / "Conflicting saves" / story) == "already played here");
  CHECK(!prepare(old / "Subfolder", old));
  {
    SetupLock playing;
    CHECK(playing.Acquire(old / "data/setup.lock"));
    CHECK(!prepare(fresh, old) && !fs::exists(fresh));
  }
  Put(old / ".gtav-update/pending.txt");
  CHECK(!prepare(fresh, old) && !fs::exists(fresh));
  fs::remove_all(old / ".gtav-update");
  int copies = 0;
  CHECK(!prepare(fresh, old, [&] { return ++copies >= 4; }));
  CHECK(!fs::exists(fresh) && Read(old / story) == "first story save");
  for (auto& item : fs::directory_iterator(tmp.path))
    CHECK(!item.path().filename().string().starts_with(".gtav-incoming-"));

#ifndef _WIN32
  fs::create_symlink(old / story, old / "data/user/linked-save");
  CHECK(!prepare(fresh, old) && error.find(L"links") != error.npos && !fs::exists(fresh));
  fs::remove(old / "data/user/linked-save");
#endif
  fs::create_directory(fresh);  // an empty folder is OK
  CHECK(prepare(fresh, old));
  for (auto rel : {story, workshop, part, fs::path("data/user/sandbox/courses/track.gtcourse"),
                  fs::path("data/build/obj/gtav_init.o"), fs::path("data/spirv/user.spv"), fs::path("gtav_settings.ini")})
    CHECK(Read(fresh / rel) == Read(old / rel) && !Read(fresh / rel).empty());
  CHECK(!fs::exists(fresh / "logs") && !fs::exists(fresh / "data/setup.lock"));
  Put(fresh / story, "played after migration");
  CHECK(!prepare(fresh, old) && Read(old / story) == "first story save" && Read(fresh / story) == "played after migration");

  using namespace gtav::frontend;
  CHECK(PlayMode(kDefaultCard, true) == Mode::Solo && PlayMode(Card::Story, false) == Mode::Campaign);
  CHECK(PlayMode(SelectRow(Card::Sandbox, 1), true) == Mode::Multiplayer);
  CHECK(!PlayMode(Card::StoryCoop, true) && !PlayMode(Card::PrivateSession, false));
  CHECK(Cycle(Card::Story, true) == Card::PrivateSession && SelectColumn(Card::PrivateSession, 0) == Card::StoryCoop);
  return 0;
} catch (const std::exception& e) {
  std::cerr << "Desktop fixture: " << e.what() << '\n';
  return 1;
}
