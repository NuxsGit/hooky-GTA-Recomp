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
    {"720", {"720p", "720p"}}, {"1080", {"1080p", "1080p"}}, {"1440", {"QHD / 1440p", "QHD / 1440p"}},
    {"2160", {"UHD / 4K", "UHD / 4K"}}, {"0", {"Borderless desktop", "Bureau sans bordure"}},
};
inline constexpr Choice kScaleChoices[]{
    {"1", {"100%", "100%"}}, {"0.875", {"87.5%", "87,5%"}}, {"0.75", {"75%", "75%"}},
    {"0.666667", {"66.7%", "66,7%"}}, {"0.5", {"50%", "50%"}},
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
    {"Output fits the display. This does not change the scene resolution.",
     "La sortie s’adapte à l’écran. Ce n’est pas la résolution de la scène."}, kOutputChoices};
inline constexpr Row kScaleRow{Key::SceneScale, {"Scene scale", "Échelle de la scène"},
    {"Lower values reduce GPU work on full-width scene targets, not CPU work.",
     "Réduis le coût GPU de certaines cibles de scène, pas le travail CPU."}, kScaleChoices};
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

inline constexpr std::array kStartupRows{kOutputRow, kScaleRow, kUpscalingRow, kFrameLimitRow,
                                         kVsyncRow, kQueueRow, kAntiLagRow};

inline size_t ChoiceIndex(const Row& row, std::string_view value) {
  const auto canonical = CanonicalValue(row.key, value);
  for (size_t i = 0; i < row.choices.size(); ++i) if (row.choices[i].value == canonical) return i;
  return row.choices.size();
}

}  // namespace gtav::native::settings
