#pragma once

#include "modes.h"

namespace gtav::native::settings { class Preferences; }

namespace gtav::frontend {

// Main UI thread, before the retail runtime mounts any profile or starts the title.
// Closing the hub (without pressing Play) returns no mode.
std::optional<Mode> ChooseMode(bool workshop_enabled, native::settings::Preferences& prefs);

}  // namespace gtav::frontend
