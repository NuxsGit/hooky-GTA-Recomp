#include "play_hub.h"

#include <SDL3/SDL.h>

// Reuse the rasterizer already shipped with the SDK's Dear ImGui. The hub needs no GPU device,
// new SDL build options, game textures, or platform font lookup.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace gtav::frontend {
namespace {

#include "play_hub_assets.inc"

constexpr int kWidth = 1440, kHeight = 900;
constexpr char kTitle[] = "GTA V Recomp | Play";
constexpr SDL_Rect kAction{1115, 755, 265, 64}, kClose{1327, 40, 53, 42};
constexpr SDL_Rect kTabs[] = {{978, 183, 186, 49}, {1174, 183, 206, 49}};
using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
using Window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;

struct RGB { Uint8 r, g, b; };
constexpr RGB kWhite{250, 243, 232}, kMuted{180, 178, 191}, kSoft{130, 135, 152},
              kOrange{255, 187, 127}, kMint{153, 234, 212}, kLavender{224, 171, 244},
              kInk{21, 20, 27}, kPanel{26, 27, 39}, kLine{57, 55, 72}, kBlack{13, 14, 22};
constexpr RGB kAccents[] = {kOrange, kMint, kSoft, kLavender};

struct Text {
  const char* en;
  const char* fr;
  const char* In(bool french) const { return french ? fr : en; }
};
struct Tile {
  SDL_Rect rect;
  Text title, label, state, description;
};
constexpr std::array kTiles{
    Tile{{60, 284, 650, 251}, {"CAMPAIGN", "CAMPAGNE"}, {"01   /   STORY MODE", "01   /   MODE HISTOIRE"},
         {"SOLO", "SOLO"}, {"Continue your story with your local campaign saves.", "Reprends ton histoire avec tes sauvegardes de campagne."}},
    Tile{{730, 284, 650, 251}, {"SANDBOX", "BAC À SABLE"}, {"02   /   WORKSHOP", "02   /   ATELIER"},
         {"SOLO", "SOLO"}, {"Free roam, place props and build courses. Uses a separate profile.", "Explore, place des objets et crée des parcours. Profil séparé."}},
    Tile{{60, 555, 650, 163}, {"CAMPAIGN CO-OP", "CAMPAGNE COOP"}, {"03   /   STORY MODE", "03   /   MODE HISTOIRE"},
         {"UNAVAILABLE", "INDISPONIBLE"}, {"Story missions and progress cannot be synchronized between players.",
                                         "Les missions et la progression ne sont pas synchronisées entre joueurs."}},
    Tile{{730, 555, 650, 163}, {"PRIVATE SESSION", "SESSION PRIVÉE"}, {"04   /   SANDBOX MULTIPLAYER", "04   /   BAC À SABLE MULTI"},
         {"MULTIPLAYER", "MULTIJOUEUR"}, {"Open Workshop (F6) to host or join a friend. No public matchmaking.",
                                         "Ouvre l’Atelier (F6) pour héberger ou rejoindre un ami. Pas de matchmaking."}},
};

Surface MakeSurface(int width, int height) {
  Surface result(SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
  if (result) SDL_SetSurfaceBlendMode(result.get(), SDL_BLENDMODE_NONE);
  return result;
}

void Fill(SDL_Surface* surface, SDL_Rect rect, RGB rgb) {
  SDL_FillSurfaceRect(surface, &rect, SDL_MapSurfaceRGB(surface, rgb.r, rgb.g, rgb.b));
}

void Border(SDL_Surface* surface, SDL_Rect r, RGB color, int w) {
  Fill(surface, {r.x, r.y, r.w, w}, color);
  Fill(surface, {r.x, r.y + r.h - w, r.w, w}, color);
  Fill(surface, {r.x, r.y + w, w, r.h - 2 * w}, color);
  Fill(surface, {r.x + r.w - w, r.y + w, w, r.h - 2 * w}, color);
}

Uint8 Mix(Uint8 from, Uint8 to, unsigned alpha) {
  return Uint8((from * (255 - alpha) + to * alpha + 127) / 255);
}

// The only UTF-8 rendered here comes from the static copy above. Glyphs use a software RGBA
// surface, in the same drawing coordinates as the controls; the window may have any pixel DPI.
unsigned NextGlyph(std::string_view& text) {
  unsigned c = static_cast<unsigned char>(text.front());
  text.remove_prefix(1);
  if (c < 128) return c;
  const size_t tails = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
  c &= 0x7fu >> (tails + 1);
  for (size_t i = 0; i < tails && !text.empty(); ++i) {
    c = (c << 6) | (static_cast<unsigned char>(text.front()) & 63);
    text.remove_prefix(1);
  }
  return c;
}

class Font {
 public:
  explicit Font(const unsigned char* data) : okay_(stbtt_InitFont(&font_, data, stbtt_GetFontOffsetForIndex(data, 0))) {}
  bool Okay() const { return okay_; }

  int Label(SDL_Surface* out, std::string_view text, int x, int y, int size, RGB ink) const {
    const float scale = stbtt_ScaleForPixelHeight(&font_, float(size));
    int ascent = 0;
    stbtt_GetFontVMetrics(&font_, &ascent, nullptr, nullptr);
    const int baseline = y + int(std::round(ascent * scale));
    float cursor = float(x);
    unsigned last = 0;
    while (!text.empty()) {
      const unsigned code = NextGlyph(text);
      if (last) cursor += scale * stbtt_GetCodepointKernAdvance(&font_, last, int(code));
      if (out && code != ' ') {
        int w, h, left, top;
        auto bitmap = stbtt_GetCodepointBitmap(&font_, scale, scale, int(code), &w, &h, &left, &top);
        if (bitmap) {
          const int gx = int(std::round(cursor)) + left, gy = baseline + top;
          // A glyph (including an overhang) can touch the edge of the canvas.
          for (int row = std::max(0, -gy); row < std::min(h, out->h - gy); ++row) {
            auto* pixels = static_cast<Uint8*>(out->pixels) + (gy + row) * out->pitch;
            for (int col = std::max(0, -gx); col < std::min(w, out->w - gx); ++col) {
              auto* pixel = pixels + (gx + col) * 4;
              unsigned a = bitmap[row * w + col];
              if (a) {
                pixel[0] = Mix(pixel[0], ink.r, a);
                pixel[1] = Mix(pixel[1], ink.g, a);
                pixel[2] = Mix(pixel[2], ink.b, a);
              }
            }
          }
          stbtt_FreeBitmap(bitmap, font_.userdata);
        }
      }
      int advance = 0;
      stbtt_GetCodepointHMetrics(&font_, int(code), &advance, nullptr);
      cursor += advance * scale;
      last = code;
    }
    return int(std::round(cursor)) - x;
  }

  void Center(SDL_Surface* out, std::string_view text, SDL_Rect r, int size, RGB ink) const {
    Label(out, text, r.x + (r.w - Label(nullptr, text, 0, 0, size, ink)) / 2,
          r.y + (r.h - size) / 2, size, ink);
  }

 private:
  stbtt_fontinfo font_{};
  bool okay_;
};

template <size_t N>
Surface Art(const unsigned char (&image)[N], const SDL_Rect r) {
  Surface png(SDL_LoadPNG_IO(SDL_IOFromConstMem(image, N), true), SDL_DestroySurface);
  auto surface = MakeSurface(r.w, r.h);
  if (!png || !surface) return Surface(nullptr, SDL_DestroySurface);
  SDL_SetSurfaceBlendMode(png.get(), SDL_BLENDMODE_NONE);
  if (!SDL_BlitSurfaceScaled(png.get(), nullptr, surface.get(), nullptr, SDL_SCALEMODE_LINEAR))
    return Surface(nullptr, SDL_DestroySurface);
  for (int y = 0; y < r.h; ++y) {
    auto* row = static_cast<Uint8*>(surface->pixels) + y * surface->pitch;
    const float v = float(y) / r.h;
    for (int x = 0; x < r.w; ++x) {
      const float u = float(x) / r.w;
      const auto shade = unsigned(std::clamp(215.f - 240.f * u + 120.f * v * v, 19.f, 238.f));
      row[x * 4] = Mix(row[x * 4], kBlack.r, shade);
      row[x * 4 + 1] = Mix(row[x * 4 + 1], kBlack.g, shade);
      row[x * 4 + 2] = Mix(row[x * 4 + 2], kBlack.b, shade);
    }
  }
  return surface;
}

// One fixed-coordinate design scaled into the drawable, independently of the system DPI.
SDL_Rect Viewport(int w, int h) {
  const float scale = std::min(float(w) / kWidth, float(h) / kHeight);
  const int cw = int(kWidth * scale), ch = int(kHeight * scale);
  return {(w - cw) / 2, (h - ch) / 2, cw, ch};
}

class Hub {
 public:
  explicit Hub(bool workshop, bool french) : workshop_(workshop), french_(french) {
    if (backdrop_) {
      for (int y = 0; y < kHeight; ++y) {
        auto* row = static_cast<Uint8*>(backdrop_->pixels) + y * backdrop_->pitch;
        for (int x = 0; x < kWidth; ++x) {
          const float u = float(x) / kWidth, v = float(y) / kHeight;
          const float warm = u * u * (1 - v) * (1 - v);
          row[x * 4] = Uint8(14 + warm * 33);
          row[x * 4 + 1] = Uint8(15 + warm * 5);
          row[x * 4 + 2] = Uint8(24 + warm * 13);
          row[x * 4 + 3] = 255;
        }
      }
    }
  }

  bool Okay() const { return backdrop_ && canvas_ && story_ && sandbox_ && party_ && body_.Okay() && display_.Okay(); }
  Card focus = kDefaultCard;

  SDL_Surface* Draw(bool close_hover = false, bool action_hover = false) {
    auto* out = canvas_.get();
    const auto i = size_t(focus);
    const auto accent = kAccents[i];
    const bool playable = PlayMode(focus, workshop_).has_value();
    SDL_BlitSurface(backdrop_.get(), nullptr, out, nullptr);
    Fill(out, {60, 47, 56, 37}, kOrange);
    display_.Center(out, "V", {60, 45, 56, 38}, 34, kInk);
    display_.Label(out, "GTA V   /   RECOMP", 131, 45, 35, kWhite);
    body_.Label(out, L({"DESKTOP EDITION", "ÉDITION PC"}), 335, 58, 18, kMuted);
    Tag(out, {1101, 40, 211, 42}, L({"PC   /   LOCAL PLAY", "PC   /   JEU LOCAL"}), false, kMint);
    Tag(out, kClose, "X", close_hover, kWhite);
    Fill(out, {60, 107, 1320, 1}, kLine);

    body_.Label(out, L({"LOS SANTOS   /   PLAY HUB", "LOS SANTOS   /   MENU DE JEU"}), 60, 137, 21, kOrange);
    display_.Label(out, L({"CHOOSE YOUR MODE", "CHOISIS TON MODE"}), 56, 160, 96, kWhite);
    body_.Label(out, L({"One city. Your story or your rules.", "Une ville. Ton histoire ou tes règles."}), 60, 247, 27, kMuted);
    Tag(out, kTabs[0], L({"01   Campaign", "01   Campagne"}), Column(focus) == 0, kOrange);
    Tag(out, kTabs[1], L({"02   Sandbox", "02   Bac à sable"}), Column(focus) == 1, kMint);

    SDL_Surface* art[] = {story_.get(), sandbox_.get(), nullptr, party_.get()};
    for (size_t n = 0; n < kTiles.size(); ++n) {
      const auto card = Card(n);
      const auto& tile = kTiles[n];
      auto rect = tile.rect;
      const bool ready = PlayMode(card, workshop_).has_value();
      if (art[n]) SDL_BlitSurface(art[n], nullptr, out, &rect);
      else {
        Fill(out, rect, kPanel);
        // A quiet placeholder for the locked card, not an illustration suggesting playable co-op.
        for (int j = 0; j < 8; ++j)
          Fill(out, {rect.x + rect.w - 237 + j * 29, rect.y + 1, 1, rect.h - 2}, kLine);
        display_.Label(out, "II", rect.x + rect.w - 160, rect.y + 30, 145, {40, 41, 57});
      }
      Border(out, rect, card == focus ? kAccents[n] : kLine, card == focus ? 3 : 1);
      const auto state = ready ? L(tile.state) : L({"UNAVAILABLE", "INDISPONIBLE"});
      const int w = body_.Label(nullptr, state, 0, 0, 20, kMuted) + 27;
      Tag(out, {rect.x + rect.w - w - 20, rect.y + 18, w, 34}, state, false, ready ? kWhite : kMuted);
      body_.Label(out, L(tile.label), rect.x + 25, rect.y + rect.h - 89, 22, ready ? kAccents[n] : kMuted);
      display_.Label(out, L(tile.title), rect.x + 24, rect.y + rect.h - 71, 60, ready ? kWhite : kMuted);
    }

    Fill(out, {60, 741, 1320, 1}, kLine);
    Fill(out, {60, 762, 4, 50}, accent);
    body_.Label(out, L(kTiles[i].label), 77, 755, 21, accent);
    const char* description = (playable || focus == Card::StoryCoop) ? L(kTiles[i].description) :
        L({"Sandbox requires the native renderer.", "Le bac à sable nécessite le moteur de rendu natif."});
    body_.Label(out, description, 76, 783, 27, kWhite);
    Tag(out, kAction, playable ? L({"PLAY    >", "JOUER    >"}) : L({"UNAVAILABLE", "INDISPONIBLE"}),
        playable, playable ? (action_hover ? kWhite : accent) : kMuted, 26);
    Fill(out, {60, 835, 1320, 1}, kLine);
    body_.Label(out, L({"ARROWS / D-PAD   Select      ENTER / A   Play      ESC / B   Back",
                       "FLÈCHES / CROIX   Choisir      ENTRÉE / A   Jouer      ÉCHAP / B   Retour"}),
                60, 852, 21, kMuted);
    const auto local = L({"TU27   /   LOCAL PROFILES", "TU27   /   PROFILS LOCAUX"});
    body_.Label(out, local, 1380 - body_.Label(nullptr, local, 0, 0, 21, kSoft), 852, 21, kSoft);
    return out;
  }

 private:
  const char* L(Text text) const { return text.In(french_); }

  void Tag(SDL_Surface* out, SDL_Rect rect, const char* text, bool active, RGB color, int size = 20) {
    Fill(out, rect, active ? color : kBlack);
    if (!active) Border(out, rect, kLine, 1);
    body_.Center(out, text, rect, size, active ? kInk : color);
  }

  bool workshop_, french_;
  Font body_{kBodyTtf}, display_{kDisplayTtf};
  Surface backdrop_ = MakeSurface(kWidth, kHeight), canvas_ = MakeSurface(kWidth, kHeight);
  Surface story_ = Art(kStoryArt, kTiles[0].rect), sandbox_ = Art(kSandboxArt, kTiles[1].rect),
          party_ = Art(kPrivateArt, kTiles[3].rect);
};

SDL_Point Position(SDL_Window* window, float x, float y) {
  int w, h, pixels_w, pixels_h;
  if (!SDL_GetWindowSize(window, &w, &h) || !SDL_GetWindowSizeInPixels(window, &pixels_w, &pixels_h) || w <= 0 || h <= 0)
    return {-1, -1};
  auto v = Viewport(pixels_w, pixels_h);
  if (v.w <= 0 || v.h <= 0) return {-1, -1};
  return {int(std::floor((x * pixels_w / w - v.x) * kWidth / v.w)),
          int(std::floor((y * pixels_h / h - v.y) * kHeight / v.h))};
}

bool Show(SDL_Window* window, Hub& hub, SDL_Point pointer) {
  SDL_Surface* screen = SDL_GetWindowSurface(window);
  if (!screen) return false;
  auto viewport = Viewport(screen->w, screen->h);
  if (!viewport.w || !viewport.h) return true;
  SDL_Surface* view = hub.Draw(SDL_PointInRect(&pointer, &kClose), SDL_PointInRect(&pointer, &kAction));
  SDL_FillSurfaceRect(screen, nullptr, SDL_MapSurfaceRGB(screen, kBlack.r, kBlack.g, kBlack.b));
  return SDL_BlitSurfaceScaled(view, nullptr, screen, &viewport, SDL_SCALEMODE_LINEAR) && SDL_UpdateWindowSurface(window);
}

bool French() {
  int count = 0;
  SDL_Locale** locales = SDL_GetPreferredLocales(&count);
  const bool french = locales && count && locales[0]->language && std::string_view(locales[0]->language) == "fr";
  SDL_free(locales);
  return french;
}

class Controllers {
 public:
  Controllers() : initialized_(SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    if (!initialized_) return;
    int count = 0;
    auto ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) Add(ids[i]);
    SDL_free(ids);
  }
  ~Controllers() {
    for (const auto& [id, pad] : pads_) SDL_CloseGamepad(pad);
    if (initialized_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
  void Add(SDL_JoystickID id) {
    if (std::any_of(pads_.begin(), pads_.end(), [id](const auto& entry) { return entry.first == id; })) return;
    if (auto* pad = SDL_OpenGamepad(id)) pads_.emplace_back(id, pad);
  }
  void Remove(SDL_JoystickID id) {
    std::erase_if(pads_, [id](const auto& entry) {
      if (entry.first != id) return false;
      SDL_CloseGamepad(entry.second);
      return true;
    });
  }

 private:
  bool initialized_;
  std::vector<std::pair<SDL_JoystickID, SDL_Gamepad*>> pads_;
};

std::optional<Mode> Fallback(bool workshop, bool french) {
  const Text campaign{"Campaign solo", "Campagne solo"}, sandbox{"Sandbox solo", "Bac à sable solo"},
      multiplayer{"Sandbox multiplayer", "Bac à sable multi"}, close{"Back", "Retour"},
      help{"Choose a mode. Story co-op is not supported. Sandbox uses a separate profile.",
           "Choisis un mode. La campagne coop n’est pas disponible. Le bac à sable utilise un profil séparé."};
  std::vector<SDL_MessageBoxButtonData> buttons;
  if (workshop) {
    buttons.push_back({SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, sandbox.In(french)});
    buttons.push_back({0, 3, multiplayer.In(french)});
  }
  buttons.push_back({workshop ? 0u : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, campaign.In(french)});
  buttons.push_back({SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, -1, close.In(french)});
  const SDL_MessageBoxData dialog{SDL_MESSAGEBOX_INFORMATION, nullptr, kTitle, help.In(french),
                                 int(buttons.size()), buttons.data(), nullptr};
  int id = -1;
  if (SDL_ShowMessageBox(&dialog, &id) && id >= 0) return PlayMode(Card(id), workshop);
  return std::nullopt;
}

std::optional<Mode> RunHub(bool workshop_enabled) {
  const bool french = French();
  Hub hub(workshop_enabled, french);
  SDL_Rect bounds{};
  int width = 1152;
  if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &bounds))
    width = std::max(480, std::min({width, int(bounds.w * .9f), int(bounds.h * .85f * kWidth / kHeight)}));
  Window window(hub.Okay() ? SDL_CreateWindow(kTitle, width, width * kHeight / kWidth,
      SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN) : nullptr, SDL_DestroyWindow);
  if (!window) return Fallback(workshop_enabled, french);
  SDL_SetWindowMinimumSize(window.get(), 480, 300);
  SDL_SetWindowPosition(window.get(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  SDL_ShowWindow(window.get());
  SDL_RaiseWindow(window.get());
  SDL_Point pointer{-1, -1};
  if (!Show(window.get(), hub, pointer)) {
    window.reset();
    return Fallback(workshop_enabled, french);
  }
  const auto window_id = SDL_GetWindowID(window.get());
  Controllers controllers;
  for (;;) {
    SDL_Event e{};
    if (!SDL_WaitEvent(&e)) return std::nullopt;
    bool accept = false, redraw = false;
    auto& focus = hub.focus;
    switch (e.type) {
      case SDL_EVENT_QUIT: return std::nullopt;
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED: if (e.window.windowID == window_id) return std::nullopt; break;
      case SDL_EVENT_WINDOW_SHOWN:
      case SDL_EVENT_WINDOW_EXPOSED:
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      case SDL_EVENT_WINDOW_RESTORED:
        redraw = e.window.windowID == window_id;
        break;
      case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (e.window.windowID == window_id) { pointer = {-1, -1}; redraw = true; }
        break;
      case SDL_EVENT_KEY_DOWN:
        if (e.key.windowID != window_id) break;
        switch (e.key.key) {
          case SDLK_ESCAPE: return std::nullopt;
          case SDLK_TAB: focus = Cycle(focus, (e.key.mod & SDL_KMOD_SHIFT) != 0); break;
          case SDLK_LEFT: focus = SelectColumn(focus, 0); break;
          case SDLK_RIGHT: focus = SelectColumn(focus, 1); break;
          case SDLK_UP: focus = SelectRow(focus, 0); break;
          case SDLK_DOWN: focus = SelectRow(focus, 1); break;
          case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: accept = !e.key.repeat; break;
          default: break;
        }
        redraw = true;
        break;
      case SDL_EVENT_MOUSE_MOTION: {
        if (e.motion.windowID != window_id) break;
        auto next = Position(window.get(), e.motion.x, e.motion.y);
        redraw = SDL_PointInRect(&next, &kAction) != SDL_PointInRect(&pointer, &kAction) ||
                 SDL_PointInRect(&next, &kClose) != SDL_PointInRect(&pointer, &kClose);
        pointer = next;
        break;
      }
      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.windowID != window_id || e.button.button != SDL_BUTTON_LEFT) break;
        pointer = Position(window.get(), e.button.x, e.button.y);
        if (SDL_PointInRect(&pointer, &kClose)) return std::nullopt;
        if (SDL_PointInRect(&pointer, &kAction)) accept = true;
        for (int i = 0; i < 2; ++i) if (SDL_PointInRect(&pointer, &kTabs[i])) focus = SelectColumn(focus, i);
        for (size_t i = 0; i < kTiles.size(); ++i) {
          if (SDL_PointInRect(&pointer, &kTiles[i].rect)) {
            accept = focus == Card(i) && e.button.clicks >= 2;
            focus = Card(i);
          }
        }
        redraw = true;
        break;
      case SDL_EVENT_GAMEPAD_ADDED: controllers.Add(e.gdevice.which); break;
      case SDL_EVENT_GAMEPAD_REMOVED: controllers.Remove(e.gdevice.which); break;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (!(SDL_GetWindowFlags(window.get()) & SDL_WINDOW_INPUT_FOCUS)) break;
        switch (e.gbutton.button) {
          case SDL_GAMEPAD_BUTTON_SOUTH: accept = true; break;
          case SDL_GAMEPAD_BUTTON_EAST: return std::nullopt;
          case SDL_GAMEPAD_BUTTON_DPAD_LEFT: case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: focus = SelectColumn(focus, 0); break;
          case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: focus = SelectColumn(focus, 1); break;
          case SDL_GAMEPAD_BUTTON_DPAD_UP: focus = SelectRow(focus, 0); break;
          case SDL_GAMEPAD_BUTTON_DPAD_DOWN: focus = SelectRow(focus, 1); break;
          default: break;
        }
        redraw = true;
        break;
      default: break;
    }
    if (accept) {
      if (const auto choice = PlayMode(focus, workshop_enabled)) return choice;
    }
    if (redraw) (void)Show(window.get(), hub, pointer);
  }
}

}  // namespace

std::optional<Mode> ChooseMode(bool workshop_enabled) {
  // ReXApp has already opened its presentation window. Hide it while the mode hub owns
  // input, and restore it on a successful launch (also needed with the native renderer off).
  std::vector<SDL_WindowID> hidden;
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  for (int i = 0; windows && i < count; ++i) {
    if (!(SDL_GetWindowFlags(windows[i]) & SDL_WINDOW_HIDDEN) && SDL_HideWindow(windows[i]))
      hidden.push_back(SDL_GetWindowID(windows[i]));
  }
  SDL_free(windows);
  const auto chosen = RunHub(workshop_enabled);
  if (chosen) {
    for (auto id : hidden) {
      if (auto* window = SDL_GetWindowFromID(id)) SDL_ShowWindow(window);
    }
  }
  return chosen;
}

}  // namespace gtav::frontend
