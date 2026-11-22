#pragma once
// Visually functional control hints — adapts to keyboard vs PS4/PS5/Xbox/Switch.
// Shows the correct glyph for the current input device (last used).
#include <string>

namespace gtav::control_hints {

enum class Device {
  Keyboard,
  PS4,
  PS5,
  Xbox,
  Switch,
  GenericPad,
};

enum class Action {
  Cheats,      // F5
  Workshop,    // F6
  Creator,     // F7
  Build,       // F8
  Settings,    // F1
  Debug,       // F2
  Phone,       // P / Up
  Close,       // Esc / O
  NavigateUp,
  NavigateDown,
  NavigateLeft,
  NavigateRight,
  Confirm,
  Cancel,
};

// Current device, updated on every input event (keyboard vs gamepad).
Device CurrentDevice();

// Call from input handlers when a device is used.
void NotifyKeyboard();
void NotifyGamepad(int sdl_gamepad_type); // pass SDL_GamepadType int

// Glyph for an action on a given device. Never nullptr.
const char* GlyphFor(Action action, Device device);

// Short label for device, e.g. "Clavier", "Manette PS5".
const char* DeviceLabel(Device device);

// Draw a visually functional hint: button with glyph + label, returns true if clicked.
// Use in ImGui HUD / menus — the button actually triggers the action when pressed.
bool DrawHint(const char* label, Action action, bool* clicked = nullptr);

// Helper: returns glyph for CurrentDevice().
inline const char* CurrentGlyph(Action a) { return GlyphFor(a, CurrentDevice()); }

}  // namespace gtav::control_hints
