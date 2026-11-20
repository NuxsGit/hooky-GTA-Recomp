#pragma once

#include "graphics_preferences.h"

namespace gtav::native::settings {

// Called after SDK CLI/environment/TOML resolution, before any presentation or guest thread.
void Initialize();
Preferences& Player();
SaveState Commit();

// Existing logs/menu_set diagnostic hook. Unknown-to-F1 cvars may be set for this run only.
void TestSet(std::string_view name, std::string_view value);

}  // namespace gtav::native::settings
