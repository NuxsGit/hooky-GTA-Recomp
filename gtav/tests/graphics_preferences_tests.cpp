#include "graphics_fixture.h"
#include "native/graphics_choices.h"

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
  CHECK(p.Edit(Key::Output, "1080") && p.Edit(Key::Fsr1, "true") && p.Edit(Key::FsrSharpen, "false"));
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
  CHECK(fresh[Key::Fsr1] == "true" && fresh[Key::FsrSharpen] == "false" && fresh[Key::Vsync] == "false");
  CHECK(fresh[Key::ShortQueue] == "true" && fresh[Key::AmdAntiLag] == "true");
  // Benchmark mode permits live edits only; neither reading nor writing this file is allowed.
  Flags bench;
  const auto original = Read(primary);
  auto ignored = bench.Open(primary, legacy, {}, true);
  ignored.BeginRun();
  CHECK(bench[Key::Output] == "-1" && !ignored.Edit(Key::Output, "1440"));
  CHECK(ignored.Edit(Key::Vsync, "true") && bench[Key::Vsync] == "true");
  CHECK(ignored.Save() == SaveState::Ignored && Read(primary) == original);
  // A more recent path is authoritative: fallback only on missing, never on failed read.
  const auto unreadable = dir.root / "too-long.ini";
  const std::string long_file(17 * 1024, 'x');
  Write(unreadable, long_file);
  Flags error_flags;
  auto broken = error_flags.Open(unreadable, legacy);
  CHECK(broken.ReadFailed() && broken.LoadedPath().empty() && error_flags[Key::Output] == "-1");
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
  auto other = linux_system.Open(primary, legacy, {}, false, gfx::Platform::Desktop);
  CHECK(!other.Available(Key::Fsr1) && !other.Edit(Key::AmdAntiLag, "false") && linux_system[Key::Output] == "-1");
  CHECK(other.Edit(Key::Vsync, "true") && other.Save() == SaveState::Saved);
  CHECK(Read(primary).find("gtav_native_amd_anti_lag=true\n") != std::string::npos);
  const auto presets_file = dir.root / "presets.ini";
  constexpr auto& standard = gfx::kPresets[0];
  constexpr auto& ultra = gfx::kPresets[1];
  constexpr auto& potato = gfx::kPresets[2];
  constexpr auto& extra_potato = gfx::kPresets[3];
  Flags preset_flags;
  auto presets = preset_flags.Open(presets_file);
  CHECK(gfx::PresetIndex(presets) == 0);
  CHECK(gfx::CanonicalValue(Key::SceneScale, "1.500000") == "1.5");
  CHECK(gfx::CanonicalValue(Key::SceneScale, "1.501").empty());
  CHECK(gfx::CanonicalValue(Key::Output, "540") == "540");
  CHECK(gfx::ApplyPreset(presets, ultra).edited && presets.Save() == SaveState::Saved);
  CHECK(gfx::PresetIndex(presets) == 1 && preset_flags[Key::SceneScale] == "1.5");
  CHECK(preset_flags[Key::MaterialFiltering] == "true" && !presets.PendingRestart());
  CHECK(presets.Edit(Key::Output, "1440") && presets.Edit(Key::FrameLimit, "60"));
  presets.BeginRun();
  CHECK(gfx::ApplyPreset(presets, potato).edited && presets.Save() == SaveState::Saved);
  CHECK(gfx::PresetIndex(presets) == 2 && presets.PendingRestart());
  CHECK(preset_flags[Key::LowMemory] == "false" && preset_flags[Key::MaterialFiltering] == "true");
  CHECK(preset_flags[Key::SceneScale] == "1.5" && presets.Value(Key::SceneScale) == "0.5");
  CHECK(presets.Value(Key::FrameLimit) == "60" && presets.Value(Key::Output) == "1440");
  Flags potato_boot;
  auto potato_reloaded = potato_boot.Open(presets_file);
  potato_reloaded.BeginRun();
  CHECK(potato_boot[Key::LowMemory] == "true" && potato_boot[Key::MaterialFiltering] == "false");
  CHECK(gfx::ApplyPreset(potato_reloaded, standard).edited && potato_reloaded.Save() == SaveState::Saved);
  // Extra Potato: ultra-lean, GTA IV faces, runs on 3x rien; check pending restart and persisted flags.
  {
    Flags extra_flags;
    auto extra_prefs = extra_flags.Open(presets_file);
    extra_prefs.BeginRun();
    CHECK(gfx::ApplyPreset(extra_prefs, extra_potato).edited && extra_prefs.Save() == SaveState::Saved);
    CHECK(gfx::PresetIndex(extra_prefs) == 3 && extra_prefs.PendingRestart());
    CHECK(extra_flags[Key::LowMemory] == "false" && extra_prefs.Value(Key::LowMemory) == "true");
    CHECK(extra_prefs.Value(Key::SceneScale) == "0.333333" && extra_prefs.Value(Key::ExtraPotato) == "true");
    Flags extra_boot;
    auto extra_reloaded = extra_boot.Open(presets_file);
    extra_reloaded.BeginRun();
    CHECK(extra_boot[Key::LowMemory] == "true" && extra_boot[Key::ExtraPotato] == "true");
    CHECK(extra_boot[Key::SceneScale] == "0.333333");
    CHECK(gfx::ApplyPreset(extra_reloaded, standard).edited && extra_reloaded.Save() == SaveState::Saved);
  }
  Flags standard_boot;
  CHECK(gfx::PresetIndex(standard_boot.Open(presets_file)) == 0);
  CHECK(standard_boot[Key::LowMemory] == "false" && standard_boot[Key::MaterialFiltering] == "false");
  CHECK(standard_boot[Key::ExtraPotato] == "false");
  // A force-locked scene value doesn't get overwritten on disk or mislabelled as Ultra.
  gfx::Locks scale_lock{};
  scale_lock[Index(Key::SceneScale)] = true;
  Flags cli;
  cli[Key::SceneScale] = "0.75";
  auto locked = cli.Open(presets_file, {}, scale_lock);
  CHECK(gfx::ApplyPreset(locked, ultra).edited && locked.Save() == SaveState::Saved);
  CHECK(cli[Key::SceneScale] == "0.75" && gfx::PresetIndex(locked) == std::size(gfx::kPresets));
  CHECK(Read(presets_file).find("gtav_native_render_scale=1\n") != std::string::npos);
  const auto saved_presets = Read(presets_file);
  Flags mobile, benchmark, linux_flags;
  auto phone = mobile.Open(presets_file, {}, {}, false, gfx::Platform::Mobile);
  CHECK(!phone.Available(Key::SceneScale) && !phone.Available(Key::LowMemory));
  CHECK(!gfx::ApplyPreset(phone, potato).edited && phone.Save() == SaveState::Unchanged);
  auto headless = benchmark.Open(presets_file, {}, {}, true);
  CHECK(gfx::ApplyPreset(headless, potato).edited && headless.Save() == SaveState::Ignored);
  CHECK(benchmark[Key::LowMemory] == "true" && Read(presets_file) == saved_presets);
  headless.BeginRun();
  CHECK(!gfx::ApplyPreset(headless, ultra).edited && benchmark[Key::LowMemory] == "true");
  auto desktop = linux_flags.Open(presets_file, {}, {}, false, gfx::Platform::Desktop);
  CHECK(gfx::ApplyPreset(desktop, potato).edited && gfx::PresetIndex(desktop) == 2);
  CHECK(!desktop.Available(Key::Output) && linux_flags[Key::LowMemory] == "true" && linux_flags[Key::SceneScale] == "0.5");
  CHECK(Read(presets_file) == saved_presets);

  const auto realism_file = dir.root / "realism.ini";
  Flags realism_flags;
  auto realism = realism_flags.Open(realism_file);
  realism.BeginRun();
  CHECK(realism.Value(Key::ContactRays) == "0" && realism.Value(Key::MaterialAnisotropy) == "0");
  CHECK(!realism.Edit(Key::ContactRays, "4") && !realism.Edit(Key::ContactRays, "low"));
  CHECK(!realism.Edit(Key::ContactStrength, "101") && !realism.Edit(Key::ContactStrength, "50%"));
  CHECK(!realism.Edit(Key::MaterialAnisotropy, "3") && !realism.Edit(Key::MaterialAnisotropy, "-1"));
  CHECK(realism.Edit(Key::ContactRays, "2") && realism.Edit(Key::ContactStrength, "75"));
  CHECK(realism_flags[Key::ContactRays] == "2" && realism_flags[Key::ContactStrength] == "75" && !realism.PendingRestart());
  CHECK(realism.Edit(Key::MaterialAnisotropy, "16") && realism.Pending(Key::MaterialAnisotropy));
  CHECK(realism_flags[Key::MaterialAnisotropy] == "0" && realism.Save() == SaveState::Saved);
  Flags realism_boot;
  auto graphics = realism_boot.Open(realism_file, {}, {}, false, gfx::Platform::Desktop);
  CHECK(realism_boot[Key::MaterialAnisotropy] == "16" && realism_boot[Key::ContactRays] == "2");
  for (const auto& preset : gfx::kPresets) {
    // N100 Ultra is Cyberpunk-low: it intentionally forces 1x aniso, no contact, fog stripped
    if (preset.ultra_potato) {
      Flags ultra_flags;
      ultra_flags[Key::MaterialAnisotropy] = "16";
      ultra_flags[Key::ContactRays] = "2";
      ultra_flags[Key::ContactStrength] = "75";
      ultra_flags[Key::ReduceFog] = "false";
      auto ultra_prefs = ultra_flags.Open(dir.root / "realism_ultra.ini", {}, {}, false, gfx::Platform::Desktop);
      // Test without BeginRun so staged restart values are visible via flags as well as Value
      CHECK(gfx::ApplyPreset(ultra_prefs, preset).edited);
      CHECK(ultra_flags[Key::MaterialAnisotropy] == "1" && ultra_flags[Key::ContactRays] == "0" && ultra_flags[Key::ReduceFog] == "true" && ultra_flags[Key::ContactStrength] == "75");
      continue;
    }
    CHECK(gfx::ApplyPreset(graphics, preset).edited);
    CHECK(graphics.Value(Key::MaterialAnisotropy) == "16" && graphics.Value(Key::ContactRays) == "2" &&
          graphics.Value(Key::ContactStrength) == "75");
  }
  gfx::Locks contact_lock{};
  contact_lock[Index(Key::ContactRays)] = true;
  Flags off;
  auto cli_off = off.Open(realism_file, {}, contact_lock);
  CHECK(cli_off.Value(Key::ContactRays) == "0" && !cli_off.Edit(Key::ContactRays, "3"));
  CHECK(cli_off.Edit(Key::ContactStrength, "0") && cli_off.Save() == SaveState::Saved);
  CHECK(Read(realism_file).find("gtav_native_contact_rays=2\n") != std::string::npos);
  auto no_mobile = mobile.Open(realism_file, {}, {}, false, gfx::Platform::Mobile);
  CHECK(!no_mobile.Available(Key::ContactRays) && !no_mobile.Available(Key::MaterialAnisotropy));
  CHECK(mobile[Key::ContactRays] == "0" && mobile[Key::MaterialAnisotropy] == "0");
  std::puts("Graphics preferences: legacy, live vs restart, realism overrides, Unicode, ignore and atomic retry passed");
}
