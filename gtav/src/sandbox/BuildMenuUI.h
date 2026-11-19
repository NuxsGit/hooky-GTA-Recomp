#pragma once

#include "../native/presentation.h"

namespace gtav::workshop {
struct DesktopView;
// Called inside the existing render-thread ImGui frame; false asks the overlay to close.
bool DrawBuildMenu(const DesktopView&, float scale, native::OutputRect viewport);
}  // namespace gtav::workshop
