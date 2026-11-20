#include "graphics_menu_ui.h"
#include "graphics_choices.h"

#include <imgui.h>

#include <algorithm>

namespace gtav::native::settings {
namespace {
constexpr ImVec4 kMint{0.60f, 0.92f, 0.83f, 1};
constexpr ImVec4 kAmber{1, 0.74f, 0.50f, 1};
const char* rejected = nullptr;  // owned by the F1 render thread

class Controls {
 public:
  Controls(Preferences& prefs, float scale) : prefs(prefs), scale(scale) {}

  void Edit(Key key, const char* value) {
    // Presets skip deliberate launch overrides and restart settings in benchmark mode.
    if (!prefs.CanEdit(key)) return;
    if (prefs.Edit(key, value)) changed = true;
    else rejected = Spec(key).name.data();
  }

  void Combo(const Row& row) {
    if (!prefs.Available(row.key)) return;
    const auto value = prefs.Value(row.key);
    const size_t index = ChoiceIndex(row, value);
    auto label = std::string(row.title.en);
    if (Spec(row.key).restart) label += " (restart)";
    const bool editable = prefs.CanEdit(row.key);
    ImGui::PushID(int(row.key));
    ImGui::SetNextItemWidth(std::min(244.0f * scale, ImGui::GetContentRegionAvail().x * 0.56f));
    ImGui::BeginDisabled(!editable);
    if (ImGui::BeginCombo(label.c_str(), index < row.choices.size() ? row.choices[index].title.en : value.c_str())) {
      for (size_t i = 0; i < row.choices.size(); ++i) {
        const auto& choice = row.choices[i];
        if (ImGui::Selectable(choice.title.en, i == index)) {
          rejected = nullptr;
          Edit(row.key, choice.value);
        }
        if (i == index) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", row.help.en);
    StateLabel(row.key);
    ImGui::PopID();
  }

  void Check(Key key, const char* label, const char* help) {
    bool v = prefs.Value(key) == "true";
    ImGui::BeginDisabled(!prefs.CanEdit(key));
    if (ImGui::Checkbox(label, &v)) {
      rejected = nullptr;
      Edit(key, v ? "true" : "false");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", help);
    StateLabel(key);
  }

  void Preset(int selection) {
    rejected = nullptr;
    // Do not trade correct sun shadows for speed automatically. These presets leave the
    // display refresh, frame cap, queue depth and driver feature choices alone.
    Edit(Key::SunShadows, "true");
    Edit(Key::AsyncPipelines, selection == 0 ? "false" : "true");
    if (prefs.Available(Key::SceneScale)) Edit(Key::SceneScale, selection == 0 ? "1" : selection == 1 ? "0.875" : "0.75");
    if (prefs.Available(Key::Fsr1)) Edit(Key::Fsr1, selection == 0 ? "false" : "true");
  }

  Preferences& prefs;
  float scale;
  bool changed = false;

 private:
  void StateLabel(Key key) {
    if (prefs.Forced(key)) ImGui::TextDisabled("   Locked by launch arguments or environment");
    else if (prefs.Pending(key)) ImGui::TextColored(kAmber, "   Pending restart");
  }
};

constexpr Choice kLagChoices[]{
    {"6", {"Conservative / 6 ms", ""}}, {"4", {"Faster / 4 ms", ""}},
};
constexpr Row kGuestGpuLagRow{Key::GuestGpuLag, {"Guest GPU delay", ""},
    {"Not AMD Anti-Lag. A shorter emulated GPU delay may reveal streaming artefacts.", ""}, kLagChoices};
constexpr Choice kTextureChoices[]{
    {"1", {"Every frame", ""}}, {"4", {"Every 4 frames", ""}}, {"8", {"Every 8 frames", ""}},
    {"16", {"Every 16 frames", ""}}, {"30", {"Every 30 frames", ""}},
};
constexpr Row kTextureRow{Key::TextureRecheck, {"Full texture recheck", ""},
    {"Lower: sooner texture refreshes, higher CPU cost. Exact checks override this interval.", ""}, kTextureChoices};

void Display(Controls& ui, Extent output, const PresentationInfo& info) {
  ImGui::Text("Output  %u x %u", output.width, output.height);
  if (!info.input.Empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("|  Game buffer  %u x %u", info.input.width, info.input.height);
  }
  if (ui.prefs.Available(Key::Output)) {
    const auto index = ChoiceIndex(kOutputRow, std::to_string(int32_t(info.boot_output)));
    const char* output_name = index < kOutputRow.choices.size() ? kOutputChoices[index].title.en : "Custom";
    ImGui::TextDisabled("This run: %s  /  scene scale %.1f%%", output_name, info.scene_scale * 100);
  }
  ImGui::Spacing();
  ui.Combo(kOutputRow);
  ui.Combo(kScaleRow);
  ui.Combo(kUpscalingRow);
  if (ui.prefs.Available(Key::FsrSharpen) && ui.prefs.Value(Key::Fsr1) == "true")
    ui.Check(Key::FsrSharpen, "FSR 1 sharpening (RCAS)", "Turn off to use the spatial upscaler alone and save a pass on 16:9 screens.");
  switch (info.status) {
    case UpscaleStatus::kFsr1:
      ImGui::TextColored(kMint, "FSR 1 %s: %u x %u", info.fsr_sharpened ? "EASU + RCAS" : "EASU only",
                         info.fsr_output.width, info.fsr_output.height);
      break;
    case UpscaleStatus::kNoUpscale: ImGui::TextDisabled("FSR 1 bypassed: no upscaling needed."); break;
    case UpscaleStatus::kUnavailable: ImGui::TextColored(kAmber, "FSR 1 unavailable: using linear."); break;
    case UpscaleStatus::kLinear:
      ImGui::TextDisabled("%s", info.input.Empty() ? "Waiting for the game image" : "Linear presentation");
      break;
  }
  ImGui::Spacing();
  ui.Combo(kVsyncRow);
  ImGui::TextDisabled("Swap-chain VSync: %s", info.vsync ? "On" : "Off");
  if (info.vsync && ui.prefs.Value(Key::Vsync) == "false") {
    ImGui::TextWrapped("If this persists, the display/driver did not offer unsynchronized presentation.");
  }
  ui.Check(Key::Counter, "FPS counter", "Counts rendered frames, not simulated or generated frames.");
}

void Performance(Controls& ui) {
  if (ui.prefs.Available(Key::Output)) {
    ImGui::TextUnformatted("Rendering preset");
    if (ImGui::Button("Best image")) ui.Preset(0);
    ImGui::SameLine();
    if (ImGui::Button("Balanced")) ui.Preset(1);
    ImGui::SameLine();
    if (ImGui::Button("Performance")) ui.Preset(2);
    ImGui::TextWrapped("100%% / 87.5%% / 75%% scene scale on next launch. Balanced and Performance request FSR 1; the frame limit is independent.");
    ImGui::Separator();
  }
#ifdef __ANDROID__
  // Keep the existing mobile presets: these change pacing as well as compile policy.
  if (ImGui::Button("Best image")) {
    ui.Preset(0);
    ui.Edit(Key::FrameLimit, "30");
    ui.Edit(Key::Vsync, "true");
    ui.Edit(Key::GuestGpuLag, "6");
  }
  ImGui::SameLine();
  if (ImGui::Button("Max performance")) {
    ui.Preset(1);
    ui.Edit(Key::FrameLimit, "0");
    ui.Edit(Key::Vsync, "false");
    ui.Edit(Key::GuestGpuLag, "4");
  }
  ImGui::Separator();
#endif
  ui.Combo(kFrameLimitRow);
  ui.Check(Key::SunShadows, "Per-pixel sun shadows", "Disabling can expose blocky shadows/reflections.");
  ui.Check(Key::AsyncPipelines, "Compile new effects in the background", "Reduces first-use hitches. An effect can be missing until it compiles.");
  ui.Check(Key::ExactCache, "Exact texture and buffer checks", "Full checks reduce stale textures at a CPU cost.");
  if (ui.prefs.Value(Key::ExactCache) != "true") ui.Combo(kTextureRow);
  ui.Check(Key::Touch, "Touch controls", "On-screen gamepad; hidden when a controller is connected.");
}

void Latency(Controls& ui, const PresentationInfo& info) {
  if (ui.prefs.Available(Key::ShortQueue)) {
    ui.Combo(kQueueRow);
    ImGui::TextDisabled("This run: up to %u guest frames queued before replay", info.queue_limit);
    ui.Combo(kAntiLagRow);
    switch (info.anti_lag) {
      case AntiLagStatus::Unsupported: ImGui::TextDisabled("AMD Anti-Lag: not supported by this GPU/driver."); break;
      case AntiLagStatus::Off: ImGui::TextDisabled("AMD Anti-Lag: supported, disabled for this run."); break;
      case AntiLagStatus::Fallback: ImGui::TextColored(kAmber, "AMD Anti-Lag: unavailable this run; normal pacing."); break;
      case AntiLagStatus::Ready: ImGui::TextDisabled("AMD Anti-Lag: enabled; waiting for gameplay input."); break;
      case AntiLagStatus::Pacing: ImGui::TextColored(kMint, "AMD Anti-Lag: driver pacing enabled in gameplay."); break;
    }
  }
  ui.Combo(kGuestGpuLagRow);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("Not integrated in this renderer");
  ImGui::TextDisabled("DLSS  /  XeSS  /  temporal FSR  /  Frame generation x2-x8");
  ImGui::TextWrapped("Temporal features require validated motion, depth, and scene colour before the game's HUD. No generated frames are being presented.");
}
}  // namespace

bool DrawGraphicsMenu(Preferences& prefs, Extent output, uint32_t fps, double frame_ms,
                      const PresentationInfo& info, float scale) {
  Controls ui(prefs, scale);
  ImGui::TextColored(kMint, "%u FPS   /   %.1f ms", fps, frame_ms);
  if (!info.adapter.empty()) ImGui::TextWrapped("Vulkan: %.*s", int(info.adapter.size()), info.adapter.data());
  const auto path = prefs.Path().u8string();
  if (prefs.Ignored()) ImGui::TextColored(kAmber, "Benchmark mode: preferences are not read or saved.");
  else if (prefs.ReadFailed()) ImGui::TextColored(kAmber, "Cannot read preferences. Check the file, then relaunch.");
  else if (prefs.LastSave() == SaveState::Failed) {
    ImGui::TextColored(kAmber, "Not saved. Check write permissions or disk space.");
    if (ImGui::Button("Retry save")) ui.changed = true;
  } else if (prefs.LastSave() == SaveState::Saved) ImGui::TextColored(kMint, "Preferences saved");
  else ImGui::TextDisabled("Changes save automatically");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", reinterpret_cast<const char*>(path.c_str()));
  if (prefs.InvalidEntries() && prefs.LastSave() != SaveState::Saved)
    ImGui::TextColored(kAmber, "Ignored %u invalid or rejected preference entries.", prefs.InvalidEntries());
  if (prefs.PendingRestart()) {
    ImGui::TextColored(kAmber, "%s", prefs.LastSave() == SaveState::Saved ?
        "Saved changes take effect after restarting the game." : "Startup changes are pending; save and restart the game.");
  }
  if (rejected) ImGui::TextColored(kAmber, "Setting refused: %s", rejected);
  ImGui::Separator();
  if (ImGui::BeginTabBar("Graphics tabs")) {
    if (ImGui::BeginTabItem("Display")) { Display(ui, output, info); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Performance")) { Performance(ui); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Latency & support")) { Latency(ui, info); ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }
  ImGui::Spacing();
  ImGui::TextDisabled("RB + Back: settings    D-pad + A: select    B / Esc: close");
  return ui.changed;
}

}  // namespace gtav::native::settings
