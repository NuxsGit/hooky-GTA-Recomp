#include "menu.h"

#include "../debug_tools.h"
#include "../sandbox/desktop.h"

#ifdef GTAV_DESKTOP_WORKSHOP
#include <SDL3/SDL.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include "input.h"
#include "touch_controls.h"

#include "paths.h"

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace gtav::native::menu {

namespace {

struct Input {
  enum Type { kMove, kButton, kWheel, kKey, kText } type;
  float x = 0, y = 0;
  int code = 0;
  bool down = false;
};

std::mutex g_input_mutex;
std::vector<Input> g_input;
using debug::Panel;
std::atomic<Panel> g_panel{Panel::Closed};
bool g_clear_imgui_input = false;

ImGuiContext* g_context = nullptr;
bool g_font_uploaded = false;
float g_scale = 1.0f;

// Every setting the menu shows, by cvar name. Saved to gtav_settings.ini as name=value.
constexpr const char* kSettings[] = {
    "gtav_fps_limit",
    "gtav_native_vsync",
    "gtav_native_fps",
#ifdef _WIN32
    "gtav_native_output",
    "gtav_native_fsr1",
    "gtav_native_render_scale",
#endif
    "gtav_native_tile_penumbra_all",
    "gtav_native_async_pipelines",
    "gpu_skip_rendering_lag_ms",
    "gtav_native_exact_cache",
    "gtav_native_texture_recheck_frames",
    "gtav_touch_controls",
};

std::filesystem::path SettingsPath() { return ExeDir() / "gtav_settings.ini"; }

void SaveSettings() {
  std::ofstream out(SettingsPath());
  out << "# GTA V native renderer settings (F1 menu). name=value, applied over the command line.\n";
  for (const char* name : kSettings) out << name << '=' << rex::cvar::GetFlagByName(name) << '\n';
}

bool GetBool(const char* name) { return rex::cvar::GetFlagByName(name) == "true"; }
void SetBool(const char* name, bool value) { rex::cvar::SetFlagByName(name, value ? "true" : "false"); }

// Presets: the two ends of the trade-off. "Best image" draws everything exactly as soon as it
// appears (new pipelines are built on the spot, a short hitch the first time an effect shows up)
// with the per-pixel sun shadows; "Max performance" keeps the frame moving (new effects are
// skipped for a frame or two while they compile), lets the game skip shadow work per tile and
// lets the emulated GPU trail the game less.
// How far the emulated GPU trails the game's kicks (ms): 6 is the tested value (foliage stopped
// flickering while driving); 2 flickered. 4 is ~10% faster on Windows (88 -> 98 fps running
// around, 10-29) and no worse than 6 in a headless flicker-catch A/B; not yet checked driving.
constexpr const char* kSafeGpuLag = "6";
constexpr const char* kFastGpuLag = "4";

void ApplyPreset(bool best) {
#ifdef __ANDROID__
  // Phones: the per-pixel sun tiles cost no measurable fps (same-run A/B on an Adreno 730), and
  // without them the game's per-tile shortcut shows gray 16x16 blocks at object edges.
  SetBool("gtav_native_tile_penumbra_all", true);
#else
  SetBool("gtav_native_tile_penumbra_all", best);
#endif
  SetBool("gtav_native_async_pipelines", !best);
  rex::cvar::SetFlagByName("gpu_skip_rendering_lag_ms", best ? kSafeGpuLag : kFastGpuLag);
#ifdef __ANDROID__
  // Phones: "Best image" is also the steady one, locked at 30 fps with vsync (sustained play heats
  // the phone less and throttles later); "Max performance" runs uncapped.
  rex::cvar::SetFlagByName("gtav_fps_limit", best ? "30" : "0");
  SetBool("gtav_native_vsync", best);
#else
  if (!best) {
    rex::cvar::SetFlagByName("gtav_fps_limit", "0");
    SetBool("gtav_native_vsync", false);
  }
#endif
  SaveSettings();
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
  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.ScaleAllSizes(g_scale);
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
  }
  return ImGuiKey_None;
}

#ifdef _WIN32
bool BuildDisplaySettings(Extent output, const PresentationInfo& info) {
  ImGui::TextUnformatted("Display");
  if (info.input.Empty()) ImGui::Text("Output: %u x %u", output.width, output.height);
  else ImGui::Text("Output: %u x %u  |  Game buffer: %u x %u", output.width, output.height,
                   info.input.width, info.input.height);

  constexpr OutputMode modes[]{OutputMode::k720p, OutputMode::k1080p, OutputMode::k1440p,
                               OutputMode::k2160p, OutputMode::kDesktop};
  const char* outputs[]{"1280 x 720", "1920 x 1080 (1080p)", "2560 x 1440 (QHD)",
                        "3840 x 2160 (4K)", "Borderless desktop"};
  const auto mode = ParseOutputMode(std::atoi(rex::cvar::GetFlagByName("gtav_native_output").c_str()));
  int selected = int(std::find(std::begin(modes), std::end(modes), mode) - std::begin(modes));
  ImGui::SetNextItemWidth(240.0f * g_scale);
  bool changed = ImGui::Combo("Output (restart)", &selected, outputs, IM_ARRAYSIZE(outputs));
  if (changed) rex::cvar::SetFlagByName("gtav_native_output", std::to_string(int32_t(modes[selected])));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("16:9 output, without stretching. Uses borderless on a matching monitor,\n"
                      "or fits a window within the monitor's work area. Does not render the guest at 4K.");

  // This is the renderer's existing startup-only scale, not a change to the Xbox display mode.
  constexpr const char* values[]{"1", "0.875", "0.75", "0.666667", "0.5"};
  const char* scales[]{"100% (original)", "87.5%", "75%", "66.7%", "50%", "Custom"};
  const double scale = std::atof(rex::cvar::GetFlagByName("gtav_native_render_scale").c_str());
  selected = IM_ARRAYSIZE(values);
  for (int i = 0; i < IM_ARRAYSIZE(values); ++i)
    if (std::abs(scale - std::atof(values[i])) < 0.0001) selected = i;
  ImGui::SetNextItemWidth(240.0f * g_scale);
  if (ImGui::Combo("Scene scale (restart)", &selected, scales,
                   selected == IM_ARRAYSIZE(values) ? IM_ARRAYSIZE(scales) : IM_ARRAYSIZE(values)) &&
      selected < IM_ARRAYSIZE(values)) {
    rex::cvar::SetFlagByName("gtav_native_render_scale", values[selected]);
    changed = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Lowering the size of full-width scene targets can reduce GPU load;\n"
                      "it doesn't speed up CPU-bound scenes. Keep 100%% for the sharpest input.");

  const char* filters[]{"Linear", "AMD FSR 1 (spatial)"};
  selected = GetBool("gtav_native_fsr1") ? 1 : 0;
  ImGui::SetNextItemWidth(240.0f * g_scale);
  if (ImGui::Combo("Upscaling", &selected, filters, IM_ARRAYSIZE(filters))) {
    SetBool("gtav_native_fsr1", selected == 1);
    changed = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("FSR 1 sharpens the scaled game image (including its HUD). Applies live.\n"
                      "It does not generate frames or change lighting or depth.");
  switch (info.status) {
    case UpscaleStatus::kFsr1:
      ImGui::TextDisabled("FSR 1 active: %u x %u (up to 2x per axis)", info.fsr_output.width, info.fsr_output.height);
      break;
    case UpscaleStatus::kNoUpscale: ImGui::TextDisabled("FSR 1 bypassed: no upscale needed."); break;
    case UpscaleStatus::kUnavailable: ImGui::TextDisabled("FSR 1 unavailable: using linear."); break;
    case UpscaleStatus::kLinear: break;
  }
  ImGui::Separator();
  return changed;
}
#endif

void BuildSettings(uint32_t fps, double frame_ms, [[maybe_unused]] const PresentationInfo& info) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  // Centred: the screen's sides are under the rounded corners and the notch.
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
#else
  ImGui::SetNextWindowPos(ImVec2(40.0f * g_scale, 40.0f * g_scale), ImGuiCond_FirstUseEver);
#endif
  ImGui::SetNextWindowSize(ImVec2(520.0f * g_scale, 0.0f), ImGuiCond_FirstUseEver);
#ifdef _WIN32
  const auto output = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::max(1.0f, output.x - 80.0f * g_scale),
                                                           std::max(1.0f, output.y - 80.0f * g_scale)));
#endif
  bool open = true;
  if (!ImGui::Begin("Settings (F1 / Esc / B to close)", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open && g_panel == Panel::Settings) Close();
    return;
  }
  ImGui::Text("%u fps  (%.1f ms per frame)", fps, frame_ms);
  ImGui::Separator();

  ImGui::TextUnformatted("Preset");
  if (ImGui::Button("Best image")) ApplyPreset(true);
  ImGui::SameLine();
  if (ImGui::Button("Max performance")) ApplyPreset(false);
  ImGui::TextDisabled("Compare the fps above after switching.");
  ImGui::Separator();

  bool changed = false;
#ifdef _WIN32
  changed = BuildDisplaySettings({uint32_t(output.x), uint32_t(output.y)}, info);
#endif
  int limit = std::atoi(rex::cvar::GetFlagByName("gtav_fps_limit").c_str());
  int choice = limit == 30 ? 0 : limit == 60 ? 1 : limit <= 0 ? 2 : 3;
  const char* limits[] = {"30 (the game's own limiter)", "60", "Uncapped", "Custom (command line)"};
  ImGui::SetNextItemWidth(260.0f * g_scale);
  if (ImGui::Combo("Frame rate limit", &choice, limits, choice == 3 ? 4 : 3) && choice < 3) {
    rex::cvar::SetFlagByName("gtav_fps_limit", choice == 0 ? "30" : choice == 1 ? "60" : "0");
    changed = true;
  }

  auto check = [&](const char* label, const char* name, const char* help) {
    bool v = GetBool(name);
    if (ImGui::Checkbox(label, &v)) {
      SetBool(name, v);
      changed = true;
    }
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
  };
  check("VSync", "gtav_native_vsync", "Wait for the display refresh: no tearing, fps capped at the refresh rate.");
  check("FPS counter", "gtav_native_fps", "Frames per second in the top-left corner.");
  ImGui::Separator();
  ImGui::TextUnformatted("Accuracy");
  check("Per-pixel sun shadows", "gtav_native_tile_penumbra_all",
        "On: every screen tile computes sun shadows per pixel (no blocky shadows or car reflections).\n"
        "Off: the game's own per-tile shortcut, which goes blocky on this renderer (gray 16x16 blocks).");
  check("Compile new effects in the background", "gtav_native_async_pipelines",
        "On: no hitch when a new effect first appears; it is skipped for a frame or two instead.\n"
        "Off: it is drawn from its first frame, with a short hitch while it is built.");
  {
    bool fast = std::atoi(rex::cvar::GetFlagByName("gpu_skip_rendering_lag_ms").c_str()) < std::atoi(kSafeGpuLag);
    if (ImGui::Checkbox("Low GPU latency", &fast)) {
      rex::cvar::SetFlagByName("gpu_skip_rendering_lag_ms", fast ? kFastGpuLag : kSafeGpuLag);
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("On: the emulated GPU trails the game by 4 ms instead of 6.\n"
                        "Off: the more conservative setting when checking streaming artifacts.");
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Textures");
  check("Exact texture checks", "gtav_native_exact_cache",
        "On: every texture is fully re-checked each frame: no stale gray blocks, but more CPU time.\n"
        "Off: large textures are spot-checked between full checks (faster).");
  {
    int frames = std::atoi(rex::cvar::GetFlagByName("gtav_native_texture_recheck_frames").c_str());
    ImGui::SetNextItemWidth(200.0f * g_scale);
    if (ImGui::SliderInt("Full re-check every N frames", &frames, 1, 30)) {
      rex::cvar::SetFlagByName("gtav_native_texture_recheck_frames", std::to_string(frames));
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Lower: textures the game streams in show up sooner (fewer gray blocks), more CPU.");
  }
  ImGui::Separator();
  check("Touch controls", "gtav_touch_controls", "The on-screen gamepad (hidden while a controller is connected).");
  ImGui::TextDisabled("Controller: RB + Back opens/closes this menu; D-pad + A to change, B to close.");
  if (changed) SaveSettings();
  ImGui::End();
  if (!open && g_panel == Panel::Settings) Close();
}

void BuildDebug(uint32_t fps, double frame_ms) {
  const auto display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowSize(ImVec2(std::min(820.f * g_scale, display.x - 20),
                                std::min(520.f * g_scale, display.y - 20)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(display.x * .5f, display.y * .5f), ImGuiCond_Appearing, ImVec2(.5f, .5f));
  bool open = true;
  if (ImGui::Begin("GTA V Recomp | Debug (F2 / Esc)", &open, ImGuiWindowFlags_NoCollapse))
    debug::DrawPanel(fps, frame_ms, g_scale);
  ImGui::End();
  if (!open && g_panel == Panel::Debug) Close();
}

#ifdef GTAV_DESKTOP_WORKSHOP
void BuildWorkshop() {
  const auto screen = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(screen.x * .5f, screen.y * .5f), ImGuiCond_Appearing, ImVec2(.5f, .5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(760.f * g_scale, screen.x - 16), std::min(545.f * g_scale, screen.y - 16)),
                           ImGuiCond_FirstUseEver);
  bool open = true;
  if (ImGui::Begin("GTA V Recomp | Workshop (F6 / Esc)", &open, ImGuiWindowFlags_NoCollapse)) sandbox::DrawWorkshop();
  ImGui::End();
  if (!open && g_panel == Panel::Workshop) Close();
}
#endif

void SwitchPanel(Panel wanted) {
  std::lock_guard lock(g_input_mutex);
  const auto next = debug::TogglePanel(g_panel.load(), wanted);
  g_panel = next;
  g_input.clear();
  g_clear_imgui_input = true;
  debug::PanelControls().SetOpen(next == Panel::Debug);
  input::Reset();
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
void OnText(uint32_t ascii) {
  if (ascii >= 32 && ascii < 127 && WorkshopOpen()) Push({Input::kText, 0, 0, int(ascii)});
}
#endif
void Close() { SwitchPanel(Panel::Closed); }
bool IsOpen() { return g_panel.load() != Panel::Closed; }
void OnMouseMove(float x, float y) { Push({Input::kMove, x, y}); }
void OnMouseButton(int button, bool down) { Push({Input::kButton, 0, 0, button, down}); }
void OnMouseWheel(float dy) { Push({Input::kWheel, 0, dy}); }
void OnKey(Key key, bool down) { Push({Input::kKey, 0, 0, int(key), down}); }

void TestSet(const std::string& name, const std::string& value) {
  REXLOG_INFO("[native] menu test: {} = {}", name, value);
  rex::cvar::SetFlagByName(name, value);
  if (std::getenv("GTAV_IGNORE_SETTINGS")) return;  // benchmark runs leave gtav_settings.ini alone
  SaveSettings();
  REXLOG_INFO("[native] menu test: saved");
}

void LoadSettings() {
  // GTAV_IGNORE_SETTINGS=1: command-line settings only (benchmark runs).
  if (std::getenv("GTAV_IGNORE_SETTINGS")) return;
  std::ifstream in(SettingsPath());
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string name = line.substr(0, eq), value = line.substr(eq + 1);
    if (std::find_if(std::begin(kSettings), std::end(kSettings), [&](const char* s) { return name == s; }) ==
        std::end(kSettings))
      continue;
    if (rex::cvar::SetFlagByName(name, value)) REXLOG_INFO("[native] setting {} = {} (gtav_settings.ini)", name, value);
  }
}

ImDrawData* Frame(uint32_t w, uint32_t h, uint32_t fps, double frame_ms, const PresentationInfo& info) {
  const bool touch_overlay = touch::Visible();
#ifdef GTAV_DESKTOP_WORKSHOP
  const bool workshop_hud = sandbox::Active();
#else
  const bool workshop_hud = false;
#endif
  if (!IsOpen() && !touch_overlay && !workshop_hud) return nullptr;
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

}  // namespace gtav::native::menu
