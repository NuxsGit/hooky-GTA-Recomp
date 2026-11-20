#include "settings_runtime.h"
#include "paths.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstdlib>
#include <optional>

REXCVAR_DEFINE_STRING(gtav_settings_path, "", "GTAV",
                      "Path to the graphics preferences file (launcher supplies its data/config/graphics.ini)");

namespace gtav::native::settings {
namespace {
std::optional<Preferences> player;

std::string PathText(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}
}  // namespace

void Initialize() {
  if (player) return;
  auto get = [](Key key) { return rex::cvar::GetFlagByName(Spec(key).name); };
  auto set = [](Key key, std::string_view value) {
    const bool ok = rex::cvar::SetFlagByName(Spec(key).name, value);
    if (!ok) REXLOG_WARN("[graphics] rejected {}={}", Spec(key).name, value);
    return ok;
  };
  Locks forced{};
  using rex::cvar::Source;
#ifdef GTAV_DESKTOP_WORKSHOP
  for (size_t i = 0; i < kSettings.size(); ++i) {
    const auto source = rex::cvar::GetFlagSource(kSettings[i].name);
    forced[i] = source == Source::kCommandLine || source == Source::kEnvironment;
  }
#endif  // Mobile args.txt is generated with boot defaults; preserve the existing F1 precedence.
  const auto& configured = REXCVAR_GET(gtav_settings_path);
  const auto legacy = ExeDir() / "gtav_settings.ini";
  const auto path = configured.empty() ? legacy : std::filesystem::u8path(configured);
  if (!configured.empty()) {
    // Launcher defaults are real defaults, not CLI overrides. Keep the old managed launch
    // behavior when there is no player preference or SDK TOML/ENV/CLI value for these flags.
    if (rex::cvar::GetFlagSource(Spec(Key::Vsync).name) == Source::kDefault) set(Key::Vsync, "false");
    if (rex::cvar::GetFlagSource(Spec(Key::GuestGpuLag).name) == Source::kDefault) set(Key::GuestGpuLag, "4");
  }
#ifdef _WIN32
  constexpr bool windows = true;
#else
  constexpr bool windows = false;
#endif
  player.emplace(path, legacy, std::getenv("GTAV_IGNORE_SETTINGS") != nullptr, windows, forced, get, set);
  player->Load();
  REXLOG_INFO("[graphics] preferences: {}{}", PathText(path), player->Ignored() ? " (disabled for this run)" : "");
  if (player->ReadFailed()) REXLOG_WARN("[graphics] could not read preferences; won't overwrite them");
  if (!player->LoadedPath().empty())
    REXLOG_INFO("[graphics] loaded {} ({} invalid/rejected entries)",
                PathText(player->LoadedPath()), player->InvalidEntries());
  for (size_t i = 0; i < kSettings.size(); ++i) {
    const auto key = Key(i);
    if (player->Available(key))
      REXLOG_INFO("[graphics] startup {}={}{}", Spec(key).name, get(key), forced[i] ? " (launch override)" : "");
  }
}

Preferences& Player() { return *player; }

SaveState Commit() {
  const auto state = Player().Save();
  if (state == SaveState::Failed) REXLOG_WARN("[graphics] could not save {}", PathText(Player().Path()));
  return state;
}

void TestSet(std::string_view name, std::string_view value) {
  const auto key = FindKey(name);
  auto& prefs = Player();
  // CLI/ENV-locked values and the benchmark mode may still be A/B tested live. Never
  // write benchmark/forced values to disk or alter an already sampled startup setting.
  bool ok;
  if (!key || ((prefs.Ignored() || prefs.Forced(*key)) && !Spec(*key).restart)) {
    ok = rex::cvar::SetFlagByName(name, value);
  } else {
    ok = prefs.Edit(*key, value);
    if (ok) Commit();
  }
  if (ok) REXLOG_INFO("[native] menu test: {} = {}{}", name, value, key && prefs.Pending(*key) ? " (next launch)" : "");
  else REXLOG_WARN("[native] menu test rejected: {} = {}", name, value);
}

}  // namespace gtav::native::settings
