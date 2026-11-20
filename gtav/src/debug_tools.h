#pragma once

#include "debug_control.h"

namespace gtav::debug {

Controls& PanelControls();
void InstallLogCapture();
void DrawPanel(unsigned fps, double frame_ms, float scale);

}  // namespace gtav::debug
