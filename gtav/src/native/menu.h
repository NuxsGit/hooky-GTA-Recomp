// Settings menu of the native renderer: F1 opens it over the game (Dear ImGui, drawn by the
// backend into the swap chain). Input arrives from the window thread (SDL event watch / WndProc)
// and is queued; the render thread builds the UI once per presented frame.
#pragma once

#include "presentation.h"

#include <cstdint>
#include <string>

struct ImDrawData;

namespace gtav::native::menu {

// Window thread. Coordinates are window pixels; buttons 0 left, 1 right, 2 middle.
void Toggle();  // Settings (F1 / controller)
void ToggleDebug();  // F2 (Fn + F2 on media-key keyboards)
#ifdef GTAV_DESKTOP_WORKSHOP
void OpenWorkshop();  // startup, before the native window appears
bool ToggleWorkshop();  // F6, sandbox modes only
bool WorkshopOpen();
void OnText(uint32_t ascii);
#endif
void Close();
bool IsOpen();
void OnMouseMove(float x, float y);
void OnMouseButton(int button, bool down);
void OnMouseWheel(float dy);
enum class Key {
  kUp, kDown, kLeft, kRight, kEnter, kSpace, kTab,
  kBackspace, kDelete, kHome, kEnd, kCtrl, kShift, kSuper, kA, kC, kV, kX
};
void OnKey(Key key, bool down);

// Applies gtav_settings.ini (next to the exe) over the command line. Once, before the first frame.
void LoadSettings();

// Render thread: sets one of the menu's settings as its controls do (logs/menu_set test hook).
void TestSet(const std::string& name, const std::string& value);

// Render thread. Builds the menu for a display of w x h pixels when it is open; null when closed.
// `fps` is the presented rate over the last second, `frame_ms` the average time per frame.
ImDrawData* Frame(uint32_t w, uint32_t h, uint32_t fps, double frame_ms, const PresentationInfo& info);

// The font atlas (RGBA8) for the backend's upload; true once, the first time the menu opens.
bool TakeFontAtlas(const uint8_t** pixels, int* width, int* height);
void SetFontTexture(uint32_t descriptor);

}  // namespace gtav::native::menu
