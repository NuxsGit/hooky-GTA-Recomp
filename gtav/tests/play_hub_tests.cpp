// Optional asset-free test of the real SDL mode/graphics hub (no game runtime or GPU).
#include "graphics_fixture.h"
#include "../src/frontend/play_hub.cpp"

using namespace gtav::frontend;
using namespace graphics_fixture;

struct Keys {
  std::vector<SDL_Keycode> values;
  bool sent = false;
  static bool Send(void* ctx, SDL_Event* event) {
    auto& self = *static_cast<Keys*>(ctx);
    if (event->type == SDL_EVENT_WINDOW_SHOWN && !self.sent) {
      auto* window = SDL_GetWindowFromID(event->window.windowID);
      if (!window || std::string_view(SDL_GetWindowTitle(window)) != kTitle) return true;
      self.sent = true;
      for (auto key : self.values) {
        SDL_Event input{};
        input.type = SDL_EVENT_KEY_DOWN;
        input.key.windowID = event->window.windowID;
        input.key.key = key;
        CHECK(SDL_PushEvent(&input));
      }
      SDL_Event quit{};
      quit.type = SDL_EVENT_QUIT;  // don't hang if routing regresses
      CHECK(SDL_PushEvent(&quit));
    }
    return true;
  }
};

int main(int argc, char** argv) {
  // Release SDKs intentionally omit the dummy driver. In that case use their regular
  // window driver; queued SDL events below still need neither a focused OS window nor a GPU.
  for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) {
    if (std::string_view(SDL_GetVideoDriver(i)) == "dummy") {
      SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
      break;
    }
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL initialization error: %s\n", SDL_GetError());
    return 1;
  }
  Temp dir;
  Flags flags;
  const auto file = dir.root / "data/config/graphics.ini";
  const auto legacy = dir.root / "gtav_settings.ini";
  const std::string old = "gtav_native_output=720\ngtav_fps_limit=60\n";
  Write(legacy, old);
  auto prefs = flags.Open(file, legacy);
  CHECK(prefs.LoadedPath() == legacy && !fs::exists(file));
  auto launch = [&](bool enabled, std::vector<SDL_Keycode> values, std::optional<Mode> mode) {
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    Keys keys{std::move(values)};
    CHECK(SDL_AddEventWatch(Keys::Send, &keys));
    CHECK(ChooseMode(enabled, prefs) == mode);
    SDL_RemoveEventWatch(Keys::Send, &keys);
    CHECK(keys.sent);
  };
  launch(true, {SDLK_ESCAPE}, {});
  launch(true, {SDLK_RETURN}, Mode::Solo);
  launch(true, {SDLK_DOWN, SDLK_RETURN}, Mode::Multiplayer);
  launch(true, {SDLK_LEFT, SDLK_DOWN, SDLK_RETURN}, Mode::CampaignCoop);
  launch(true, {SDLK_LEFT, SDLK_RETURN}, Mode::Campaign);
  launch(false, {SDLK_RETURN}, Mode::Campaign);
  launch(false, {SDLK_LEFT, SDLK_DOWN, SDLK_RETURN}, {}); // co-op requires native
  // F1 opens the top preset row. Right applies Ultra, a second right applies Potato.
  launch(true, {SDLK_F1, SDLK_RIGHT, SDLK_ESCAPE, SDLK_RETURN}, Mode::Solo);
  CHECK(flags[Key::SceneScale] == "1.5" && flags[Key::MaterialFiltering] == "true");
  launch(true, {SDLK_F1, SDLK_RIGHT, SDLK_ESCAPE, SDLK_RETURN}, Mode::Solo);
  CHECK(flags[Key::SceneScale] == "0.5" && flags[Key::LowMemory] == "true");
  Flags restarted;
  {
    auto reloaded = restarted.Open(file, legacy);
    CHECK(reloaded.LoadedPath() == file && reloaded.Value(Key::Output) == "720");
    CHECK(reloaded.Value(Key::SceneScale) == "0.5" && reloaded.Value(Key::FrameLimit) == "60");
    CHECK(reloaded.Value(Key::LowMemory) == "true" && Read(legacy) == old);
  }
  {
    gfx::Locks forced{};
    forced[Index(Key::SceneScale)] = true;
    Flags overridden;
    overridden[Key::SceneScale] = "0.75";
    auto locked = overridden.Open(file, legacy, forced);
    Hub hub(true, false, locked);
    hub.SelectTab(2);
    hub.Horizontal(1);  // Custom -> Standard; locked scale must never be persisted.
    hub.Horizontal(1);  // Standard -> Ultra even though the resulting settings are still custom.
    CHECK(locked.LastSave() == gfx::SaveState::Saved && locked.Value(Key::SceneScale) == "0.75");
    CHECK(locked.Value(Key::MaterialFiltering) == "true");
    CHECK(Read(file).find("gtav_native_render_scale=0.5\n") != std::string::npos);
  }
  {
    Hub french(true, true, prefs), english(true, false, prefs);
    CHECK(french.Okay() && english.Okay());
    CHECK(!french.Click({1200, 181}, 1) && french.GraphicsOpen());
    CHECK(!french.Click({297, 306}, 1));  // Standard button, clears Potato (6 presets: Extra Potato shifts right-aligned row)
    CHECK(flags[Key::SceneScale] == "1" && flags[Key::LowMemory] == "false");
    CHECK(!french.Click({413, 306}, 1));  // Ultra button
    CHECK(flags[Key::SceneScale] == "1.5" && flags[Key::MaterialFiltering] == "true");
    CHECK(!french.Click({913, 468}, 1));  // right chevron on the spatial upscaler row
    CHECK(flags[Key::Fsr1] == "true" && prefs.LastSave() == gfx::SaveState::Saved);
    CHECK(!french.Click({529, 306}, 1));  // Potato resets FSR and visual extras
    CHECK(flags[Key::LowMemory] == "true" && flags[Key::MaterialFiltering] == "false" && flags[Key::Fsr1] == "false");
    if (argc >= 2) {
      const std::string prefix = argv[1];
      CHECK(SDL_SavePNG(french.Draw(), (prefix + "-graphics-fr.png").c_str()));
      CHECK(!french.Back());
      CHECK(SDL_SavePNG(french.Draw(), (prefix + "-play-fr.png").c_str()));
      english.SelectTab(2);
      CHECK(SDL_SavePNG(english.Draw(), (prefix + "-graphics-en.png").c_str()));
    }
  }
  SDL_Quit();
  std::puts("SDL: rendered both hub pages, keyboard/mouse navigation, three routes, locked co-op and graphics restart passed");
}
