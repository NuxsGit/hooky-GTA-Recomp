#include "menu.h"

#include "../debug_tools.h"
#include "../sandbox/desktop.h"

#ifdef GTAV_DESKTOP_WORKSHOP
#include "../phone/AdminCheatApp.h"
#include "../sandbox/BuildMenuUI.h"
#include "../sandbox/CreativeRuntime.h"
#include <SDL3/SDL.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include "input.h"
#include "touch_controls.h"

#include "graphics_menu_ui.h"
#include "settings_runtime.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gtav::native::menu {

namespace {

struct Input {
  enum Type { kMove, kButton, kWheel, kKey, kText, kText16 } type;
  float x = 0, y = 0;
  int code = 0;
  bool down = false;
};

std::mutex g_input_mutex;
std::vector<Input> g_input;
using debug::Panel;
std::atomic<Panel> g_panel{Panel::Closed};
bool g_clear_imgui_input = false;
#ifdef GTAV_DESKTOP_WORKSHOP
#endif

ImGuiContext* g_context = nullptr;
bool g_font_uploaded = false;
float g_scale = 1.0f;

// Coalesce synchronous preference writes during slider drags and rapid edits.
// Live cvar edits are applied immediately; disk writes wait for idle or close.
std::mutex g_save_mutex;
std::chrono::steady_clock::time_point g_last_edit{};
bool g_pending_save = false;

void NotePendingSave() {
  std::lock_guard lock(g_save_mutex);
  g_last_edit = std::chrono::steady_clock::now();
  g_pending_save = true;
}

void FlushPendingSaveImpl(bool force) {
  std::lock_guard lock(g_save_mutex);
  if (!g_pending_save) return;
  auto now = std::chrono::steady_clock::now();
  if (!force && now - g_last_edit < std::chrono::milliseconds(350)) return;
  // Settings runtime is owned by the render thread after BeginRun, but Commit
  // is also safe to call from the window thread for force flushes: the file
  // is the same and edits are already in the shared Preferences object.
  settings::Commit();
  g_pending_save = false;
}

void EnsureContext(uint32_t h) {
  if (g_context) return;
  IMGUI_CHECKVERSION();
  ImGuiContext* previous = ImGui::GetCurrentContext();
  g_context = ImGui::CreateContext();
  ImGui::SetCurrentContext(g_context);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifdef GTAV_DESKTOP_WORKSHOP
  auto& platform = ImGui::GetPlatformIO();
#ifndef _WIN32
  platform.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
    static std::string clipboard;
    clipboard.clear();
    SDL_RunOnMainThread([](void* opaque) {
      char* text = SDL_GetClipboardText();
      if (!text) return;
      auto& output = *static_cast<std::string*>(opaque);
      output.assign(text, std::min(std::char_traits<char>::length(text), size_t{256}));
      SDL_free(text);
    }, &clipboard, true);
    return clipboard.c_str();
  };
#endif
  platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
    char* copy = SDL_strdup(text);
    if (copy && !SDL_RunOnMainThread([](void* opaque) {
          SDL_SetClipboardText(static_cast<const char*>(opaque));
          SDL_free(opaque);
        }, copy, false)) SDL_free(copy);
  };
#endif
  g_scale = std::max(1.0f, float(h) / 720.0f);
#if defined(__APPLE__) && TARGET_OS_IPHONE
  g_scale *= 1.6f;  // a 5.4-inch screen driven by fingers: larger text and controls
#endif
  ImFontConfig font;
  font.SizePixels = 15.0f * g_scale;
  io.Fonts->AddFontDefault(&font);
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 10.0f;
  style.FrameRounding = 6.0f;
  style.GrabRounding = 6.0f;
  style.TabRounding = 8.f;
  style.ScaleAllSizes(g_scale);
  // Global GTA tones — dark window, green selection
  ImVec4* c = style.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.07f,0.07f,0.08f,0.97f);
  c[ImGuiCol_Header] = ImVec4(0.18f,0.85f,0.45f,0.18f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.18f,0.85f,0.45f,0.30f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.18f,0.85f,0.45f,0.45f);
  c[ImGuiCol_Button] = ImVec4(0.13f,0.13f,0.14f,1.f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.18f,0.85f,0.45f,0.28f);
  c[ImGuiCol_CheckMark] = ImVec4(0.18f,0.85f,0.45f,1.f);
  c[ImGuiCol_SliderGrab] = ImVec4(0.18f,0.85f,0.45f,1.f);
  c[ImGuiCol_Tab] = ImVec4(0.11f,0.11f,0.12f,1.f);
  c[ImGuiCol_TabHovered] = ImVec4(0.18f,0.85f,0.45f,0.28f);
  c[ImGuiCol_TabActive] = ImVec4(0.18f,0.85f,0.45f,0.42f);
  if (previous) ImGui::SetCurrentContext(previous);
}

void Push(const Input& in) {
  if (!IsOpen()) return;
  std::lock_guard lock(g_input_mutex);
  if (IsOpen() && g_input.size() < 1024) g_input.push_back(in);
}

ImGuiKey ToImGui(Key key) {
  switch (key) {
    case Key::kUp: return ImGuiKey_UpArrow;
    case Key::kDown: return ImGuiKey_DownArrow;
    case Key::kLeft: return ImGuiKey_LeftArrow;
    case Key::kRight: return ImGuiKey_RightArrow;
    case Key::kEnter: return ImGuiKey_Enter;
    case Key::kSpace: return ImGuiKey_Space;
    case Key::kTab: return ImGuiKey_Tab;
    case Key::kBackspace: return ImGuiKey_Backspace;
    case Key::kDelete: return ImGuiKey_Delete;
    case Key::kHome: return ImGuiKey_Home;
    case Key::kEnd: return ImGuiKey_End;
    case Key::kCtrl: return ImGuiMod_Ctrl;
    case Key::kShift: return ImGuiMod_Shift;
    case Key::kSuper: return ImGuiMod_Super;
    case Key::kA: return ImGuiKey_A;
    case Key::kC: return ImGuiKey_C;
    case Key::kV: return ImGuiKey_V;
    case Key::kX: return ImGuiKey_X;
    case Key::kY: return ImGuiKey_Y;
    case Key::kZ: return ImGuiKey_Z;
  }
  return ImGuiKey_None;
}

void SwitchPanel(Panel wanted, std::optional<Panel> only_if);
void BuildSettings(uint32_t fps, double frame_ms, const PresentationInfo& info) {
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  const ImVec2 available(std::max(1.0f, display.x - 24.0f), std::max(1.0f, display.y - 24.0f));
  ImGui::SetNextWindowSize(ImVec2(std::min(680.0f * g_scale, available.x),
                                 std::min(575.0f * g_scale, available.y)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), available);
  bool open = true;
  if (ImGui::Begin("Graphics settings (F1)", &open, ImGuiWindowFlags_NoCollapse)) {
    auto& prefs = settings::Player();
    if (settings::DrawGraphicsMenu(prefs, {uint32_t(display.x), uint32_t(display.y)}, fps, frame_ms, info, g_scale))
      NotePendingSave();
    // Flush once the user idles or stops dragging. While a slider is active we
    // keep the disk quiet and only update the live cvar.
    const bool dragging = ImGui::IsAnyItemActive();
    if (!dragging) FlushPendingSaveImpl(false);
  }
  ImGui::End();
  if (!open && g_panel == Panel::Settings) {
    FlushPendingSaveImpl(true);
    Close();
  } else if (open) {
    // Also flush on panel switch without closing the window: the user moved
    // to Debug/Workshop; the F1 render thread may return before next BuildSettings.
    FlushPendingSaveImpl(false);
  }
}

void BuildDebug(uint32_t fps, double frame_ms) {
  const auto display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowSize(ImVec2(std::min(820.f * g_scale, display.x - 20),
                                std::min(540.f * g_scale, display.y - 20)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(display.x * .5f, display.y * .5f), ImGuiCond_Appearing, ImVec2(.5f, .5f));
  bool open = true;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f * g_scale);
  if (ImGui::Begin("HOOKY  //  DEBUG  [F2]", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::TextColored(ImVec4(0.18f,0.85f,0.45f,1.f), "LOS SANTOS  //  DEBUG & WORKSHOP");
    ImGui::TextDisabled("F2 Debug | F6 Workshop | F5 Cheats | F8 Build | Esc fermer");
#ifdef GTAV_DESKTOP_WORKSHOP
    // Quick Workshop shortcuts inside F2 — the Cheats panel (AdminOS) lives on F5
    if (sandbox::Active()) {
      ImGui::Separator();
      if (ImGui::Button("Cheats (F5)")) { SwitchPanel(Panel::Cheats, Panel::Debug); }
      ImGui::SameLine();
      if (ImGui::Button("Workshop (F6)")) { SwitchPanel(Panel::Workshop, Panel::Debug); }
      ImGui::SameLine();
      if (workshop::BuildAllowed() && ImGui::Button("Build (F8)")) { SwitchPanel(Panel::Build, Panel::Debug); }
      ImGui::Separator();
    }
#endif
    debug::DrawPanel(fps, frame_ms, g_scale);
  }
  ImGui::End();
  ImGui::PopStyleVar();
  if (!open && g_panel == Panel::Debug) Close();
}

#ifdef GTAV_DESKTOP_WORKSHOP
void BuildCheats() {
  const auto screen = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(screen.x * .5f, screen.y * .5f), ImGuiCond_Appearing, ImVec2(.5f, .5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(900.f * g_scale, screen.x - 16), std::min(660.f * g_scale, screen.y - 16)),
                           ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(screen.x - 12, screen.y - 12));
  bool open = true;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f * g_scale);
  if (ImGui::Begin("CHEATS  //  ADMIN  [F5]", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::TextColored(ImVec4(0.18f,0.85f,0.45f,1.f), "CHEATS  //  ADMIN OS");
    ImGui::TextDisabled("F5 pour fermer  |  Esc pour fermer  |  admin OS = owner / admin, session authoritatif");
    ImGui::Separator();
    const auto view = workshop::Snapshot();
    if (!view) {
      ImGui::TextDisabled("Quick workshop bridge unavailable.");
    } else {
      auto self = std::find_if(view->players.begin(), view->players.end(), [](const workshop::PlayerView& p) { return p.id == workshop::kLocalOwner; });
      const auto& local = self != view->players.end() ? *self : workshop::PlayerView{};
      workshop::DrawAdminCheatApp(*view, local, g_scale);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
  if (!open && g_panel == Panel::Cheats) Close();
}

void BuildWorkshop() {
  const auto screen = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(screen.x * .5f, screen.y * .5f), ImGuiCond_Appearing, ImVec2(.5f, .5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(760.f * g_scale, screen.x - 16), std::min(565.f * g_scale, screen.y - 16)),
                           ImGuiCond_FirstUseEver);
  bool open = true;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f * g_scale);
  if (ImGui::Begin("WORKSHOP  //  LOS SANTOS  [F6]", &open, ImGuiWindowFlags_NoCollapse)) {
    sandbox::DrawWorkshop();
  }
  ImGui::End();
  ImGui::PopStyleVar();
  if (!open && g_panel == Panel::Workshop) Close();
}
#endif

void SwitchPanel(Panel wanted, std::optional<Panel> only_if = {}) {
  // Flush any pending F1 save before the panel changes or closes: the
  // F1 render thread may return before building widgets (early-out).
  if (wanted == Panel::Closed || g_panel.load() == Panel::Settings) FlushPendingSaveImpl(true);
  std::lock_guard lock(g_input_mutex);
  if (only_if && g_panel.load() != *only_if) return;
  const auto next = debug::TogglePanel(g_panel.load(), wanted);
  g_panel = next;
  g_input.clear();
  g_clear_imgui_input = true;
  debug::PanelControls().SetOpen(next == Panel::Debug);
  input::Reset();
#ifdef GTAV_DESKTOP_WORKSHOP
  if (sandbox::Active()) {
    workshop::SetCapture(next == Panel::Build, next == Panel::Workshop || next == Panel::Cheats,
                         next != Panel::Closed && next != Panel::Build && next != Panel::Workshop && next != Panel::Cheats);
  }
#endif
}

}  // namespace

void Toggle() { SwitchPanel(Panel::Settings); }
void ToggleDebug() { SwitchPanel(Panel::Debug); }
#ifdef GTAV_DESKTOP_WORKSHOP
void OpenWorkshop() { if (!WorkshopOpen() && sandbox::Active()) SwitchPanel(Panel::Workshop); }
bool ToggleWorkshop() {
  if (!sandbox::Active()) return false;
  SwitchPanel(Panel::Workshop);
  return true;
}
bool WorkshopOpen() { return g_panel.load() == Panel::Workshop; }
bool ToggleBuild() {
  if (!sandbox::Active()) return false;
  if (g_panel.load() == Panel::Build) SwitchPanel(Panel::Closed, Panel::Build);
  else if (workshop::BuildAllowed()) SwitchPanel(Panel::Build);
  return true;
}
bool ToggleCheats() {
  if (!sandbox::Active()) return false;
  if (g_panel.load() == Panel::Cheats) { SwitchPanel(Panel::Closed, Panel::Cheats); return true; }
  if (IsOpen()) return false;
  SwitchPanel(Panel::Cheats, Panel::Closed);
  return true;
}
bool CheatsOpen() { return g_panel.load() == Panel::Cheats; }
void CloseCreative() {
  const auto panel = g_panel.load();
  if (panel == Panel::Build || panel == Panel::Cheats) SwitchPanel(Panel::Closed, panel);
}
bool TextPanel() {
  const auto panel = g_panel.load();
  return panel == Panel::Workshop || panel == Panel::Build || panel == Panel::Cheats;
}
void OnText(uint32_t ch) {
  if (ch >= 32 && ch != 127 && ch <= 0x10ffff && TextPanel()) Push({Input::kText, 0, 0, int(ch)});
}
void OnText16(uint32_t ch) {
  if (ch >= 32 && ch != 127 && ch <= 0xffff && TextPanel()) Push({Input::kText16, 0, 0, int(ch)});
}
#endif
void Close() { SwitchPanel(Panel::Closed); }
bool IsOpen() { return g_panel.load() != Panel::Closed; }
void OnMouseMove(float x, float y) { Push({Input::kMove, x, y}); }
void OnMouseButton(int button, bool down) { Push({Input::kButton, 0, 0, button, down}); }
void OnMouseWheel(float dy) { Push({Input::kWheel, 0, dy}); }
void OnKey(Key key, bool down) { Push({Input::kKey, 0, 0, int(key), down}); }

ImDrawData* Frame(uint32_t w, uint32_t h, uint32_t fps, double frame_ms, const PresentationInfo& info) {
  const bool touch_overlay = touch::Visible();
#ifdef GTAV_DESKTOP_WORKSHOP
  const bool workshop_hud = sandbox::Active();
#else
  const bool workshop_hud = false;
#endif
  if (!IsOpen() && !touch_overlay && !workshop_hud) {
    FlushPendingSaveImpl(false);
    return nullptr;
  }
  EnsureContext(h);
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(g_context);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(w), float(h));
  io.DeltaTime = 1.0f / 60.0f;
  {
    std::lock_guard lock(g_input_mutex);
    if (g_clear_imgui_input) {
      io.ClearEventsQueue();
      io.ClearInputKeys();
      io.ClearInputMouse();
      g_clear_imgui_input = false;
    }
    for (const auto& in : g_input) {
      switch (in.type) {
        case Input::kMove: io.AddMousePosEvent(in.x, in.y); break;
        case Input::kButton: io.AddMouseButtonEvent(in.code, in.down); break;
        case Input::kWheel: io.AddMouseWheelEvent(0.0f, in.y); break;
        case Input::kKey: io.AddKeyEvent(ToImGui(Key(in.code)), in.down); break;
        case Input::kText: io.AddInputCharacter(unsigned(in.code)); break;
        case Input::kText16: io.AddInputCharacterUTF16(ImWchar16(in.code)); break;
      }
    }
    g_input.clear();
  }
  if (!g_font_uploaded) {  // legacy atlas: the backend uploads it before the first draw
    unsigned char* pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
  }
  ImGui::NewFrame();
  switch (g_panel.load()) {
    case Panel::Settings: BuildSettings(fps, frame_ms, info); break;
    case Panel::Debug: BuildDebug(fps, frame_ms); break;
    case Panel::Workshop:
#ifdef GTAV_DESKTOP_WORKSHOP
      BuildWorkshop();
#endif
      break;
    case Panel::Build:
#ifdef GTAV_DESKTOP_WORKSHOP
      if (!workshop::BuildAllowed()) SwitchPanel(Panel::Closed, Panel::Build);
      else {
        const auto view = workshop::Snapshot();
        // Desktop presents across the viewport; only the mobile renderer letterboxes.
        if (!workshop::DrawBuildMenu(*view, g_scale, {0, 0, {w, h}})) SwitchPanel(Panel::Closed, Panel::Build);
      }
#endif
      break;
    case Panel::Cheats:
#ifdef GTAV_DESKTOP_WORKSHOP
      BuildCheats();
#endif
      break;
    case Panel::Closed:
#ifdef GTAV_DESKTOP_WORKSHOP
      if (workshop_hud) sandbox::DrawHud();
#endif
      break;
  }
  if (touch_overlay) touch::Draw(ImGui::GetForegroundDrawList(), float(w), float(h));
  ImGui::Render();
  ImDrawData* data = ImGui::GetDrawData();
  if (previous && previous != g_context) ImGui::SetCurrentContext(previous);
  return data;
}

bool TakeFontAtlas(const uint8_t** pixels, int* width, int* height) {
  if (g_font_uploaded || !g_context) return false;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(g_context);
  unsigned char* p;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&p, width, height);
  *pixels = p;
  if (previous && previous != g_context) ImGui::SetCurrentContext(previous);
  return true;
}

void SetFontTexture(uint32_t descriptor) {
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(g_context);
  ImGui::GetIO().Fonts->SetTexID(ImTextureID(descriptor));
  g_font_uploaded = true;
  if (previous && previous != g_context) ImGui::SetCurrentContext(previous);
}

void FlushPendingSave(bool force) { FlushPendingSaveImpl(force); }

}  // namespace gtav::native::menu
