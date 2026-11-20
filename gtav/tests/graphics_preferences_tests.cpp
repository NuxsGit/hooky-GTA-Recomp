#include "graphics_fixture.h"

using namespace graphics_fixture;
using gfx::SaveState;

int main() {
  Temp dir;
  auto primary = dir.root / "data/config/graphics.ini";
  auto legacy = dir.root / "gtav_settings.ini";
  const std::string old = "\xef\xbb\xbf# legacy\r\n gtav_native_output = 1440 \r\n"
      "gtav_native_render_scale=0.875000\r\ngtav_native_render_scale=nan\r\n"
      "gtav_fps_limit=60\r\ngtav_native_vsync=1\r\ngtav_native_exact_cache=true\r\n"
      "gtav_native_texture_recheck_frames=14zzz\r\nno-equals\r\ngtav_mode=multiplayer\r\n";
  Write(legacy, old);
  Flags flags;
  flags[Key::FrameLimit] = "30";  // explicit CLI
  flags.reject = Key::ExactCache;
  gfx::Locks forced{};
  forced[Index(Key::FrameLimit)] = true;
  auto p = flags.Open(primary, legacy, forced);
  CHECK(p.LoadedPath() == legacy && p.InvalidEntries() == 4 && !p.ReadFailed());
  CHECK(flags[Key::FrameLimit] == "30" && flags[Key::Vsync] == "true" && flags[Key::Output] == "1440");
  CHECK(flags[Key::SceneScale] == "0.875" && flags[Key::ExactCache] == "false");
  CHECK(!fs::exists(primary) && p.Save() == SaveState::Unchanged);
  CHECK(!p.Edit(Key::FrameLimit, "0") && !p.Edit(Key::Vsync, "junk") && !p.Edit(Key::ExactCache, "true"));
  CHECK(!p.Edit(Key::SceneScale, "1,0") && !p.Edit(Key::Output, "4320"));
  // The hub applies edits, including startup values, before the guest is launched.
  CHECK(p.Edit(Key::Output, "1080") && p.Edit(Key::Fsr1, "true"));
  CHECK(flags[Key::Output] == "1080" && flags[Key::Fsr1] == "true" && !p.PendingRestart());
  CHECK(p.Save() == SaveState::Saved && Read(legacy) == old);
  // F1 applies live edits and stages restart edits without changing the active cvar.
  p.BeginRun();
  CHECK(p.BootValue(Key::SceneScale) == "0.875");
  CHECK(p.Edit(Key::SceneScale, ".75") && flags[Key::SceneScale] == "0.875" && p.Value(Key::SceneScale) == "0.75");
  CHECK(p.PendingRestart() && p.Edit(Key::SceneScale, "0.875000") && !p.PendingRestart());
  CHECK(p.Edit(Key::SceneScale, ".666667") && p.Edit(Key::ShortQueue, "true") && p.Edit(Key::AmdAntiLag, "true"));
  CHECK(p.Edit(Key::Vsync, "0") && flags[Key::Vsync] == "false" && p.PendingRestart());
  CHECK(p.Save() == SaveState::Saved);
  CHECK(Read(primary).find("gtav_fps_limit=60\n") != std::string::npos);  // not poisoned by the CLI override
  CHECK(Read(primary).find("gtav_mode") == std::string::npos);
  Flags fresh;
  auto restarted = fresh.Open(primary, legacy);
  restarted.BeginRun();
  CHECK(restarted.LoadedPath() == primary && restarted.InvalidEntries() == 0 && !restarted.PendingRestart());
  CHECK(fresh[Key::FrameLimit] == "60" && fresh[Key::Output] == "1080" && fresh[Key::SceneScale] == "0.666667");
  CHECK(fresh[Key::Fsr1] == "true" && fresh[Key::Vsync] == "false" && fresh[Key::ShortQueue] == "true" && fresh[Key::AmdAntiLag] == "true");
  // Benchmark mode permits live edits only; neither reading nor writing this file is allowed.
  Flags bench;
  const auto original = Read(primary);
  auto ignored = bench.Open(primary, legacy, {}, true);
  ignored.BeginRun();
  CHECK(bench[Key::Output] == "720" && !ignored.Edit(Key::Output, "1440"));
  CHECK(ignored.Edit(Key::Vsync, "true") && bench[Key::Vsync] == "true");
  CHECK(ignored.Save() == SaveState::Ignored && Read(primary) == original);
  // A more recent path is authoritative: fallback only on missing, never on failed read.
  const auto unreadable = dir.root / "too-long.ini";
  const std::string long_file(17 * 1024, 'x');
  Write(unreadable, long_file);
  Flags error_flags;
  auto broken = error_flags.Open(unreadable, legacy);
  CHECK(broken.ReadFailed() && broken.LoadedPath().empty() && error_flags[Key::Output] == "720");
  CHECK(broken.Edit(Key::Vsync, "true") && broken.Save() == SaveState::Failed && Read(unreadable) == long_file);
  // Trigger a replacement failure even for users with administrator privileges.
  auto blocked = dir.root / "write-failure" / "graphics.ini";
  auto retry = error_flags.Open(blocked);
  Write(blocked / "keep", "old contents");
  CHECK(retry.Edit(Key::FrameLimit, "60") && retry.Save() == SaveState::Failed);
  CHECK(Read(blocked / "keep") == "old contents" && std::distance(fs::directory_iterator(blocked.parent_path()), fs::directory_iterator{}) == 1);
  fs::remove_all(blocked);
  CHECK(retry.Save() == SaveState::Saved);
  Flags after_failure;
  CHECK(after_failure.Open(blocked).Value(Key::FrameLimit) == "60");
  // Files carried across OSes retain valid Windows choices without trying to apply their cvars.
  Flags linux_system;
  auto other = linux_system.Open(primary, legacy, {}, false, false);
  CHECK(!other.Available(Key::Fsr1) && !other.Edit(Key::AmdAntiLag, "false") && linux_system[Key::Output] == "720");
  CHECK(other.Edit(Key::Vsync, "true") && other.Save() == SaveState::Saved);
  CHECK(Read(primary).find("gtav_native_amd_anti_lag=true\n") != std::string::npos);
  std::puts("Graphics preferences: legacy, live vs restart, overrides, Unicode, ignore and atomic retry passed");
}
