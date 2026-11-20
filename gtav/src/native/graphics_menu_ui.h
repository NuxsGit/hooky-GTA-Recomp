#pragma once

#include "graphics_preferences.h"
#include "presentation.h"

namespace gtav::native::settings {

// Draws inside the F1 window. Returns true when the caller should save (once per UI frame).
bool DrawGraphicsMenu(Preferences& prefs, Extent output, uint32_t fps, double frame_ms,
                      const PresentationInfo& info, float scale);

}  // namespace gtav::native::settings
