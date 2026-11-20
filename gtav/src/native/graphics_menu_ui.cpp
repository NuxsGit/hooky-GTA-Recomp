#include "graphics_menu_ui.h"
#include "graphics_choices.h"

#include <imgui.h>

#include <algorithm>
#include <charconv>

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

  void Percent(Key key, const char* label, int fallback, bool enabled) {
    const auto value = CanonicalValue(key, prefs.Value(key));
    int number = fallback;
    if (!value.empty()) std::from_chars(value.data(), value.data() + value.size(), number);
    ImGui::SetNextItemWidth(std::min(244.0f * scale, ImGui::GetContentRegionAvail().x * 0.56f));
    ImGui::BeginDisabled(!enabled || !prefs.CanEdit(key));
    if (ImGui::SliderInt(label, &number, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
      rejected = nullptr;
      Edit(key, std::to_string(number).c_str());
    }
    ImGui::EndDisabled();
    StateLabel(key);
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
  ImGui::TextUnformatted("Window & scaling");
  ImGui::Text("Output  %u x %u", output.width, output.height);
  if (!info.input.Empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("|  Game buffer  %u x %u", info.input.width, info.input.height);
  }
  if (ui.prefs.Available(Key::Output)) {
    const auto index = ChoiceIndex(kOutputRow, std::to_string(int32_t(info.boot_output)));
    const char* output_name = index < kOutputRow.choices.size() ? kOutputChoices[index].title.en : "Custom";
    ImGui::TextDisabled("This run: %s", output_name);
  }
  if (ui.prefs.Available(Key::SceneScale)) ImGui::TextDisabled("Scene scale this run: %.1f%%", info.scene_scale * 100);
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
  ImGui::Separator();
  ImGui::TextUnformatted("VSync & counters");
  ui.Combo(kVsyncRow);
  ImGui::TextDisabled("Swap-chain VSync: %s", info.vsync ? "On" : "Off");
  if (info.vsync && ui.prefs.Value(Key::Vsync) == "false") {
    ImGui::TextWrapped("If this persists, the display/driver did not offer unsynchronized presentation.");
  }
  ui.Check(Key::Counter, "FPS counter", "Counts rendered frames, not simulated or generated frames.");
}

void Performance(Controls& ui, const PresentationInfo& info) {
  if (ui.prefs.Available(Key::LowMemory)) {
    ImGui::TextUnformatted("Presets");
    const size_t selected = PresetIndex(ui.prefs);
    ImGui::Text("Requested preset: %s", selected < std::size(kPresets) ? kPresets[selected].title.en : "Custom");
    ImGui::BeginDisabled(!CanApplyPreset(ui.prefs));
    for (size_t i = 0; i < std::size(kPresets); i++) {
      if (i % 3 != 0) ImGui::SameLine();
      const auto& preset = kPresets[i];
      if (ImGui::Button(preset.title.en)) {
        const auto result = ApplyPreset(ui.prefs, preset);
        ui.changed |= result.edited;
        rejected = result.rejected;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", preset.help.en);
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Preset changes keep the FPS limit, display overrides and locked options. Potato targets low-memory PCs; Extra Potato (33%% scene, GTA IV faces) runs on 3x rien ultra-low hardware. Compatible Vulkan graphics are still required.");
    ImGui::TextDisabled("This run: %.0f%% scene  /  %u GPU slot%s  /  %s caches%s", info.scene_scale * 100,
                        info.gpu_slots, info.gpu_slots == 1 ? "" : "s", info.lean_memory ? "reduced" : "normal",
                        info.scene_scale < 0.4f ? " (Extra Potato)" : "");
    ImGui::Separator();
    ImGui::TextUnformatted("Quality tweaks");
    ui.Check(Key::MaterialFiltering, "Enhanced material filtering (restart)",
             "Raises anisotropy to 8x on eligible repeating, mipmapped materials when supported. Wave state and depth are unchanged.");
    ui.Check(Key::LowMemory, "Reduced memory usage (restart)",
             "Reduces cache targets, uploads and GPU frames in flight. Shaders load as needed. Not a RAM limit.");
    ui.Check(Key::ExtraPotato, "Extra Potato (restart)",
             "33% scene, ultra-lean caches, aggressive LOD: GTA IV-level faces for 3x rien ultra-low hardware. Vulkan still required.");
    if (ui.prefs.Value(Key::ExtraPotato) == "true")
      ImGui::TextColored(kAmber, "Extra Potato active: GTA IV-style geometry on minimal hardware.");
  }
#ifdef __ANDROID__
  // Keep the existing mobile presets: these change pacing as well as compile policy.
  if (ImGui::Button("Best image")) {
    ui.Edit(Key::SunShadows, "true");
    ui.Edit(Key::AsyncPipelines, "false");
    ui.Edit(Key::FrameLimit, "30");
    ui.Edit(Key::Vsync, "true");
    ui.Edit(Key::GuestGpuLag, "6");
  }
  ImGui::SameLine();
  if (ImGui::Button("Max performance")) {
    ui.Edit(Key::SunShadows, "true");
    ui.Edit(Key::AsyncPipelines, "true");
    ui.Edit(Key::FrameLimit, "0");
    ui.Edit(Key::Vsync, "false");
    ui.Edit(Key::GuestGpuLag, "4");
  }
  ImGui::Separator();
#endif
  ImGui::Separator();
  ImGui::TextUnformatted("Frame pacing & compilation");
  ui.Combo(kFrameLimitRow);
  ui.Check(Key::SunShadows, "Per-pixel sun shadows", "Disabling can expose blocky shadows/reflections.");
  ui.Check(Key::AsyncPipelines, "Compile new effects in the background", "Reduces first-use hitches. An effect can be missing until it compiles.");
  if (info.pipelines) {
    const auto& p = *info.pipelines;
    if (p.wait_ms > 0.5) ImGui::TextColored(kAmber, "Background pipeline wait: %.1f ms this frame (%u pending)", p.wait_ms, p.pending);
    else ImGui::TextDisabled("Background pipeline wait: %.1f ms  (%u pending, %u warmed)", p.wait_ms, p.pending, p.warmed);
    if (info.lean_memory) ImGui::TextDisabled("Pipeline warmup skipped in reduced-memory mode.");
    else if (p.warmed) ImGui::TextDisabled("%u cached pipelines reused since launch.", p.warmed);
  } else {
    ImGui::TextDisabled("Background pipeline wait: waiting for first frame.");
  }
  ImGui::Spacing();
  ImGui::TextUnformatted("Texture validation");
  ui.Check(Key::ExactCache, "Exact texture and buffer checks", "Full checks reduce stale textures at a CPU cost.");
  if (ui.prefs.Value(Key::ExactCache) != "true") ui.Combo(kTextureRow);
  ui.Check(Key::Touch, "Touch controls", "On-screen gamepad; hidden when a controller is connected.");
}

void Realism(Controls& ui, const RealismInfo& info) {
  ImGui::TextUnformatted("Materials");
  ui.Combo(kAnisotropyRow);
  if (info.anisotropy_cap <= 1) ImGui::TextDisabled("Anisotropic sampling unavailable or limited to 1x.");
  else if (info.material_floor > 1)
    ImGui::TextDisabled("Eligible surfaces: %ux minimum, %ux renderer cap.", info.material_floor, info.anisotropy_cap);
  else ImGui::TextDisabled("No extra material filtering (renderer cap %ux).", info.anisotropy_cap);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("Screen-space contact occlusion");
  ui.Combo(kContactRayRow);
  const auto quality = ChoiceIndex(kContactRayRow, ui.prefs.Value(Key::ContactRays));
  ui.Percent(Key::ContactStrength, "Contact strength", 50, quality > 0 && quality < kContactRayRow.choices.size());
  switch (info.contact_rays) {
    case ContactRayStatus::Off: ImGui::TextDisabled("Contact rays: off, no extra pass."); break;
    case ContactRayStatus::NoCompatiblePass: ImGui::TextColored(kAmber, "Contact rays: no compatible shadow pass in this frame."); break;
    case ContactRayStatus::NoAlignedDepth: ImGui::TextColored(kAmber, "Contact rays: no aligned scene depth; original shadows."); break;
    case ContactRayStatus::Recorded: ImGui::TextColored(kMint, "Contact rays: recorded in the scene shadow mask."); break;
    case ContactRayStatus::Unavailable: ImGui::TextColored(kAmber, "Contact rays: unavailable in this build or on this driver."); break;
  }
  ImGui::TextWrapped("Experimental, screen-space only. Uses visible scene depth; no new textures, meshes or off-screen reflections.");
  ImGui::TextDisabled("Hardware ray tracing is not integrated.");
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("Distant fog");
  ui.Check(Key::ReduceFog, "Reduce distant fog (experimental)",
           "Live toggle. Omits one TU27 fog strip (PS 9DFE7F549A326A57) over distant hills. Other weather, sky bands and hills remain. Exact match except for that draw.");
  if (ui.prefs.Available(Key::ReduceFog)) {
    const auto& fog = info.fog;
    if (fog.reduced) {
      if (fog.suppressed) ImGui::TextColored(kMint, "Fog: %u draw%s omitted this frame.", fog.suppressed, fog.suppressed == 1 ? "" : "s");
      else if (fog.protected_draws) ImGui::TextColored(kAmber, "Fog: %u matching draw%s protected (depth/stencil/query).", fog.protected_draws, fog.protected_draws == 1 ? "" : "s");
      else ImGui::TextDisabled("Fog: reduced — no matching draw this frame.");
    } else {
      ImGui::TextDisabled("Fog: original — all distant fog drawn.");
    }
    ImGui::TextWrapped("Only that strip is skipped, with depth/stencil/MRT/query checks. If haze persists, other passes or weather may contribute — verify with F9 traces.");
  }
}

void Memory(const PresentationInfo& info) {
  if (!info.memory) {
    ImGui::TextDisabled("Waiting for renderer counters.");
    return;
  }
  const auto& stats = *info.memory;
  const auto cache = [](const char* name, const CacheUsage& usage) {
    if (usage.OverTarget())
      ImGui::TextColored(kAmber, "%s: %.1f / %.0f MiB (over soft target)", name,
                         usage.bytes / double(kMiB), usage.target / double(kMiB));
    else if (usage.target)
      ImGui::Text("%s: %.1f / %.0f MiB", name, usage.bytes / double(kMiB), usage.target / double(kMiB));
    else
      ImGui::Text("%s: %.1f MiB (no target)", name, usage.bytes / double(kMiB));
  };
  ImGui::TextUnformatted("Renderer caches and uploads");
  cache("Decoded textures", stats.decoded_textures);
  cache("Converted vertex/index buffers", stats.converted_buffers);
  ImGui::Text("Upload buffers reserved: %.1f MiB", stats.upload_reserved / double(kMiB));
  if (info.lean_memory)
    ImGui::TextDisabled("Upload buffers released since launch: %.1f MiB", stats.upload_reclaimed / double(kMiB));
  if (info.pipelines) {
    ImGui::Spacing();
    ImGui::TextDisabled("Pipelines warmed: %u reused, %u pending this frame.", info.pipelines->warmed, info.pipelines->pending);
  }
  ImGui::Spacing();
  ImGui::TextWrapped("Cache targets are not caps. Resources that are in use or waiting for the GPU may still occupy memory.");
  ImGui::TextWrapped("These counters are not total RAM or VRAM. They exclude game memory, render targets, shaders and driver overhead.");
}

void Latency(Controls& ui, const PresentationInfo& info) {
  if (ui.prefs.Available(Key::ShortQueue)) {
    ImGui::TextUnformatted("Queue & driver pacing");
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
    if (ImGui::BeginTabItem("Performance")) { Performance(ui, info); ImGui::EndTabItem(); }
    if (prefs.Available(Key::ContactRays) && ImGui::BeginTabItem("Realism")) { Realism(ui, info.realism); ImGui::EndTabItem(); }
    if (prefs.Available(Key::LowMemory) && ImGui::BeginTabItem("Memory")) { Memory(info); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Latency & support")) { Latency(ui, info); ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }
  ImGui::Spacing();
  ImGui::TextDisabled("RB + Back: settings    D-pad + A: select    B / Esc: close");
  return ui.changed;
}

}  // namespace gtav::native::settings
