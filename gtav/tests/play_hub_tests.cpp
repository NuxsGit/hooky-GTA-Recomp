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
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  CHECK(SDL_Init(SDL_INIT_VIDEO));
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
  launch(true, {SDLK_LEFT, SDLK_DOWN, SDLK_RETURN, SDLK_UP, SDLK_RETURN}, Mode::Campaign);
  launch(false, {SDLK_RETURN}, Mode::Campaign);
  launch(true, {SDLK_F1, SDLK_RIGHT, SDLK_DOWN, SDLK_RIGHT, SDLK_ESCAPE, SDLK_RETURN}, Mode::Solo);
  CHECK(flags[Key::Output] == "1080" && flags[Key::SceneScale] == "0.875");
  Flags restarted;
  {
    auto reloaded = restarted.Open(file, legacy);
    CHECK(reloaded.LoadedPath() == file && reloaded.Value(Key::Output) == "1080");
    CHECK(reloaded.Value(Key::SceneScale) == "0.875" && reloaded.Value(Key::FrameLimit) == "60");
    CHECK(Read(legacy) == old);
  }
  {
    gfx::Locks forced{};
    forced[Index(Key::Output)] = true;
    Flags overridden;
    overridden[Key::Output] = "1440";
    auto locked = overridden.Open(file, legacy, forced);
    Hub hub(true, false, locked);
    hub.SelectTab(2);
    hub.Horizontal(1);  // the locked output row does not edit or poison the stored value
    hub.Vertical(1);
    hub.Horizontal(-1);
    CHECK(locked.LastSave() == gfx::SaveState::Saved && locked.Value(Key::Output) == "1440");
    CHECK(Read(file).find("gtav_native_output=1080\n") != std::string::npos);
    CHECK(Read(file).find("gtav_native_render_scale=1\n") != std::string::npos);
    CHECK(prefs.Edit(Key::SceneScale, "0.875") && prefs.Save() == gfx::SaveState::Saved);
  }
  {
    Hub french(true, true, prefs), english(true, false, prefs);
    CHECK(french.Okay() && english.Okay());
    CHECK(!french.Click({1200, 181}, 1) && french.GraphicsOpen());
    CHECK(!french.Click({913, 420}, 1));  // right chevron on the spatial upscaler row
    CHECK(flags[Key::Fsr1] == "true" && prefs.LastSave() == gfx::SaveState::Saved);
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
