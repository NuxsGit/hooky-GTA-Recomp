// Input for the native renderer's window. ReXGlue still owns the guest pad and its bindings.
#pragma once

#ifdef _WIN32
#include <windows.h>
#else
struct SDL_Window;
#endif

namespace gtav::native::input {

void Configure();  // After the SDK has loaded config and command-line flags.
void Shutdown();
bool GameplayActive();  // Native overlay and foreground gate for guest input.
void Reset();  // Forget held keys and pending motion when the settings menu changes focus.

#ifdef _WIN32
// Called on the Win32 window thread. True only when the message was consumed.
bool HandleMessage(HWND window, UINT message, WPARAM wp, LPARAM lp);
void UpdateMouseCapture(HWND window);
#else
// Phones also register the overlay window here, but leave keyboard/mouse to the SDK.
void AttachWindow(SDL_Window* window);
void UpdateMouseCapture();
#endif

}  // namespace gtav::native::input
