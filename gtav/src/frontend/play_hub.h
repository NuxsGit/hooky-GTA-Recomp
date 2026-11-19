#pragma once

#include "modes.h"

namespace gtav::frontend {

// Main UI thread, before the retail runtime mounts any profile or starts the title.
// Closing the hub (without pressing Play) returns no mode.
std::optional<Mode> ChooseMode(bool workshop_enabled);

}  // namespace gtav::frontend
