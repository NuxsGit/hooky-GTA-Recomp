#pragma once

#include "graphics_preferences.h"

#include <span>

namespace gtav::native::settings {
struct Text {
  const char* en;
  const char* fr;
  const char* In(bool french) const { return french ? fr : en; }
};
struct Choice { const char* value; Text title; };
struct Row {
  Key key;
  Text title, help;
  std::span<const Choice> choices;
};
inline constexpr Choice kOutputChoices[]{
    {"-1", {"Auto (preset)", "Auto (préréglage)"}}, {"540", {"540p", "540p"}},
    {"720", {"720p", "720p"}}, {"1080", {"1080p", "1080p"}}, {"1440", {"QHD / 1440p", "QHD / 1440p"}},
    {"2160", {"UHD / 4K", "UHD / 4K"}}, {"0", {"Borderless desktop", "Bureau sans bordure"}},
};
inline constexpr Choice kScaleChoices[]{
    {"1.5", {"150%", "150%"}}, {"1", {"100%", "100%"}}, {"0.875", {"87.5%", "87,5%"}}, {"0.75", {"75%", "75%"}},
    {"0.666667", {"66.7%", "66,7%"}}, {"0.5", {"50%", "50%"}},
    {"0.333333", {"33.3% (Extra Potato)", "33,3% (Extra Potato)"}},
    {"0.25", {"25% (N100 Ultra)", "25% (N100 Ultra)"}},
};
inline constexpr Choice kUpscalingChoices[]{
    {"false", {"Linear", "Linéaire"}}, {"true", {"AMD FSR 1", "AMD FSR 1"}},
};
inline constexpr Choice kPacingChoices[]{
    {"30", {"30 FPS (game limiter)", "30 FPS (limite du jeu)"}}, {"60", {"60 FPS", "60 FPS"}},
    {"0", {"Uncapped", "Sans limite"}},
};
inline constexpr Choice kSwitchChoices[]{
    {"false", {"Off", "Désactivé"}}, {"true", {"On", "Activé"}},
};
inline constexpr Choice kQueueChoices[]{
    {"false", {"Standard / 2", "Standard / 2"}}, {"true", {"Short / 1", "Courte / 1"}},
};
inline constexpr Row kOutputRow{Key::Output, {"Window output", "Sortie de la fenêtre"},
    {"Auto: 720p, or 540p with Potato. Output and scene scale are independent.",
     "Auto : 720p, ou 540p en Potato. N’affecte pas l’échelle de la scène."}, kOutputChoices};
inline constexpr Row kScaleRow{Key::SceneScale, {"Scene scale", "Échelle de la scène"},
    {"Scales selected 3D passes; 150% gives more detail even without a larger window.",
     "Modifie certaines passes 3D. À 150 %, plus de détails, même sans agrandir."}, kScaleChoices};
inline constexpr Row kUpscalingRow{Key::Fsr1, {"Upscaling", "Mise à l’échelle"},
    {"FSR 1 is spatial; it runs only when the game image is enlarged.",
     "FSR 1 est spatial et ne s’exécute que si l’image est agrandie."}, kUpscalingChoices};
inline constexpr Row kFrameLimitRow{Key::FrameLimit, {"Frame limit", "Limite d’images"},
    {"Caps the game's real frames. VSync can add a separate limit.",
     "Limite les images réelles du jeu. VSync peut aussi limiter le débit."}, kPacingChoices};
inline constexpr Row kVsyncRow{Key::Vsync, {"VSync", "VSync"},
    {"Off requests unsynchronized presentation, if offered by the display/driver.",
     "Sans VSync, la présentation immédiate reste soumise au pilote et à l’écran."}, kSwitchChoices};
inline constexpr Row kQueueRow{Key::ShortQueue, {"Guest frame queue", "File d’images invitées"},
    {"Short queues at most one frame before replay. No drops; throughput may be lower.",
     "Une image en attente avant le rendu, sans suppression. Débit parfois inférieur."}, kQueueChoices};
inline constexpr Row kAntiLagRow{Key::AmdAntiLag, {"AMD Anti-Lag", "AMD Anti-Lag"},
    {"Experimental Vulkan driver pacing. Needs VK_AMD_anti_lag on the selected GPU.",
     "Cadencement Vulkan expérimental. Nécessite VK_AMD_anti_lag sur le GPU choisi."}, kSwitchChoices};

inline constexpr Choice kAnisotropyChoices[]{
    {"0", {"Follow preset", "Suivre le préréglage"}}, {"1", {"Game filtering", "Filtrage du jeu"}},
    {"2", {"2x minimum", "2× minimum"}}, {"4", {"4x minimum", "4× minimum"}},
    {"8", {"8x minimum", "8× minimum"}}, {"16", {"16x minimum", "16× minimum"}},
};
inline constexpr Row kAnisotropyRow{Key::MaterialAnisotropy, {"Material filtering", "Filtrage des matériaux"},
    {"Anisotropic filtering for eligible surfaces. Keeps existing assets and respects renderer and GPU limits.",
     "Filtrage anisotrope des surfaces compatibles, sans remplacer les textures ni dépasser les limites du moteur et du GPU."}, kAnisotropyChoices};
inline constexpr Choice kContactRayChoices[]{
    {"0", {"Off", "Désactivé"}}, {"1", {"Low / 12 steps", "Faible / 12 pas"}},
    {"2", {"Medium / 20 steps", "Moyen / 20 pas"}}, {"3", {"High / 40 steps", "Élevé / 40 pas"}},
};
inline constexpr Row kContactRayRow{Key::ContactRays, {"Contact rays", "Rayons de contact"},
    {"Experimental screen-space occlusion before lighting. Independent of presets. Not hardware ray tracing.",
     "Occlusion expérimentale dans l’espace écran avant l’éclairage. Indépendante des préréglages. Pas de ray tracing matériel."}, kContactRayChoices};

inline constexpr Choice kFrameGenChoices[]{
    {"0", {"Off", "Désactivé"}},
    {"2", {"x2", "x2"}},
    {"3", {"x3", "x3"}},
    {"4", {"x4", "x4"}},
    {"8", {"x8", "x8"}},
};
inline constexpr Row kFrameGenRow{Key::FrameGeneration, {"Frame generation", "Génération d'images"},
    {"Experimental interpolation x2-x8. Requires validated motion vectors; no generated frames without them. Choose multiplier; actual generation depends on driver and frame pacing.",
     "Interpolation expérimental x2-x8. Nécessite vecteurs de mouvement validés ; pas d'images générées sans eux."}, kFrameGenChoices};

inline constexpr std::array kStartupRows{kOutputRow, kScaleRow, kUpscalingRow, kFrameLimitRow,
                                         kVsyncRow, kQueueRow, kAntiLagRow};

// A preset is a set of edits, not a second source of configuration. Output, FPS cap, VSync,
// driver/queue choices and explicit realism overrides are left alone. Matching the accessible
// preset values avoids displaying "Ultra" when, for example, CLI locked the scene to 75%.
// Extra Potato reduces triangle density to GTA IV levels (extra lean caches, lower LOD) for "3x rien" hardware; Vulkan still required.
struct Preset {
  Text title, help;
  const char* scale;
  bool filtering, lean, fsr1;
  bool extra_potato = false;
  bool ultra_potato = false;
};
inline constexpr Preset kPresets[]{
    {{"Standard", "Standard"}, {"Native scene detail and normal caches.", "Détail natif et caches normaux."},
      "1", false, false, false},
    {{"Ultra", "Ultra"}, {"150% scene sampling including water. Sharper distant materials.",
                           "Scène à 150 %, eau comprise. Textures distantes plus nettes."},
      "1.5", true, false, false},
    {{"Potato", "Potato"}, {"50% scene, lighter caches. Vulkan still required.",
                             "Scène à 50 %, caches réduits. Vulkan reste nécessaire."},
      "0.5", false, true, false},
    {{"Extra Potato", "Extra Potato"}, {"33% scene, ultra-lean caches, aggressive LOD (GTA IV faces). Runs on 3x rien; Vulkan still required.",
                             "Scène à 33 %, caches ultra-réduits, LOD agressif (visages GTA IV). Tourne sur 3x rien ; Vulkan requis."},
      "0.333333", false, true, false, true, false},
    // N100 Ultra - Cyberpunk-style ultra-low: 25% scene, ultra-lean + FSR1, 1x aniso, no contact, fog off - for Intel N100 8GB
    {{"N100 Ultra", "N100 Ultra"}, {"25% scene, 64 MiB/16 MiB, FSR1, 1x aniso, no contact rays. Cyberpunk-low for N100 8GB. Vulkan still required.",
                             "Scène à 25 %, 64/16 MiB, FSR1, 1× aniso, sans rayons. Ultra-low type Cyberpunk pour N100 8 Go. Vulkan requis."},
      "0.25", false, true, true, true, true},
    {{"Balanced", "Équilibré"}, {"87.5% scene, with FSR 1 on Windows.", "Scène à 87,5 %, avec FSR 1 sous Windows."},
      "0.875", false, false, true},
    {{"Performance", "Performance"}, {"75% scene, with FSR 1 on Windows.", "Scène à 75 %, avec FSR 1 sous Windows."},
      "0.75", false, false, true},
};

struct PresetEdit { Key key; const char* value; };
// Max edits per preset (ultra adds N100 Cyberpunk tweaks)
inline constexpr size_t kMaxPresetEdits = 9;
using PresetEditsArray = std::array<PresetEdit, kMaxPresetEdits>;
constexpr PresetEditsArray PresetEdits(const Preset& preset) {
  if (preset.ultra_potato) {
    // N100 Ultra Cyberpunk: 25% scene + ultra-lean + FSR1 + 1x aniso + no contact + fog stripped - full Cyberpunk low
    return PresetEditsArray{PresetEdit{Key::SceneScale, preset.scale},
        PresetEdit{Key::MaterialFiltering, preset.filtering ? "true" : "false"},
        PresetEdit{Key::LowMemory, preset.lean ? "true" : "false"},
        PresetEdit{Key::Fsr1, preset.fsr1 ? "true" : "false"},
        PresetEdit{Key::ExtraPotato, "true"},
        PresetEdit{Key::UltraPotato, "true"},
        PresetEdit{Key::MaterialAnisotropy, "1"},
        PresetEdit{Key::ContactRays, "0"},
        PresetEdit{Key::ReduceFog, "true"}};
  }
  if (preset.extra_potato) {
    return PresetEditsArray{PresetEdit{Key::SceneScale, preset.scale},
        PresetEdit{Key::MaterialFiltering, preset.filtering ? "true" : "false"},
        PresetEdit{Key::LowMemory, preset.lean ? "true" : "false"},
        PresetEdit{Key::Fsr1, preset.fsr1 ? "true" : "false"},
        PresetEdit{Key::ExtraPotato, "true"},
        PresetEdit{Key::UltraPotato, "false"},
        PresetEdit{Key::Count, ""},
        PresetEdit{Key::Count, ""},
        PresetEdit{Key::Count, ""}};
  }
  // Standard presets also clear Extra/Ultra Potato when switching away.
  return PresetEditsArray{PresetEdit{Key::SceneScale, preset.scale},
      PresetEdit{Key::MaterialFiltering, preset.filtering ? "true" : "false"},
      PresetEdit{Key::LowMemory, preset.lean ? "true" : "false"},
      PresetEdit{Key::Fsr1, preset.fsr1 ? "true" : "false"},
      PresetEdit{Key::ExtraPotato, "false"},
      PresetEdit{Key::UltraPotato, "false"},
      PresetEdit{Key::Count, ""},
      PresetEdit{Key::Count, ""},
      PresetEdit{Key::Count, ""}};
}

inline size_t PresetIndex(const Preferences& prefs) {
  size_t i = 0;
  for (const auto& preset : kPresets) {
    bool match = true;
    for (auto [key, value] : PresetEdits(preset)) {
      if (key == Key::Count) continue;
      if (prefs.Available(key) && CanonicalValue(key, prefs.Value(key)) != value) { match = false; break; }
    }
    if (match) return i;
    ++i;
  }
  return i;  // Custom
}

inline bool CanApplyPreset(const Preferences& prefs) {
  return prefs.CanEdit(Key::SceneScale) || prefs.CanEdit(Key::MaterialFiltering) || prefs.CanEdit(Key::LowMemory) ||
         prefs.CanEdit(Key::ExtraPotato) || prefs.CanEdit(Key::UltraPotato);
}

struct PresetResult { bool edited = false; const char* rejected = nullptr; };
inline PresetResult ApplyPreset(Preferences& prefs, const Preset& preset) {
  PresetResult result;
  if (!CanApplyPreset(prefs)) return result;
  for (auto [key, value] : PresetEdits(preset)) {
    if (key == Key::Count) continue;
    if (!prefs.CanEdit(key)) continue;
    if (prefs.Edit(key, value)) result.edited = true;
    else result.rejected = Spec(key).name.data();
  }
  return result;
}

inline size_t ChoiceIndex(const Row& row, std::string_view value) {
  const auto canonical = CanonicalValue(row.key, value);
  for (size_t i = 0; i < row.choices.size(); ++i) if (row.choices[i].value == canonical) return i;
  return row.choices.size();
}

}  // namespace gtav::native::settings
