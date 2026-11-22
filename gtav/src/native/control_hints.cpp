#include "control_hints.h"

#include <imgui.h>

#include <atomic>
#include <cstdio>

namespace gtav::control_hints {

namespace {
std::atomic<Device> g_device{Device::Keyboard};

// Map SDL_GamepadType (int) to our Device. SDL headers may not be available on Win32 raw-input path,
// so we take int and compare to known values (SDL_GAMEPAD_TYPE_*).
Device FromSDLType(int type) {
  // SDL_GamepadType values (SDL 3.2): 0 UNKNOWN, 1 STANDARD, 2 XBOX360, 3 XBOXONE, 4 PS3, 5 PS4, 6 PS5, 7 SWITCH_PRO, 8 SWITCH_JOYCON_LEFT etc.
  // We check via runtime values to avoid hard include dependency on Win32.
  // PS5 = 6, PS4 = 5, XBOX360=2, XBOXONE=3, SWITCH_PRO=7
  switch (type) {
    case 5: return Device::PS4;
    case 6: return Device::PS5;
    case 2:
    case 3: return Device::Xbox;
    case 7:
    case 8:
    case 9: return Device::Switch;
    default:
      if (type == 0) return Device::GenericPad;
      // Heuristic: type 1 = standard -> treat as Xbox layout
      return Device::GenericPad;
  }
}
}  // namespace

Device CurrentDevice() { return g_device.load(std::memory_order_relaxed); }
void NotifyKeyboard() { g_device.store(Device::Keyboard, std::memory_order_relaxed); }
void NotifyGamepad(int sdl_gamepad_type) {
  g_device.store(FromSDLType(sdl_gamepad_type), std::memory_order_relaxed);
}

const char* DeviceLabel(Device d) {
  switch (d) {
    case Device::Keyboard: return "Clavier";
    case Device::PS4: return "Manette PS4";
    case Device::PS5: return "Manette PS5";
    case Device::Xbox: return "Manette Xbox";
    case Device::Switch: return "Manette Switch";
    case Device::GenericPad: return "Manette";
  }
  return "Clavier";
}

const char* GlyphFor(Action a, Device d) {
  const bool is_keyboard = d == Device::Keyboard;
  const bool is_ps = d == Device::PS4 || d == Device::PS5;
  const bool is_xbox = d == Device::Xbox;
  const bool is_switch = d == Device::Switch;

  switch (a) {
    case Action::Cheats: // F5
      if (is_keyboard) return "F5";
      if (is_ps) return "Touchpad"; // PS4/PS5 touchpad
      if (is_xbox) return "View"; // Xbox View (Back)
      if (is_switch) return "-"; // Switch minus
      return "Touchpad/View";
    case Action::Workshop: // F6
      if (is_keyboard) return "F6";
      if (is_ps) return "Options";
      if (is_xbox) return "Menu";
      if (is_switch) return "+";
      return "Options/Menu";
    case Action::Creator: // F7
      if (is_keyboard) return "F7";
      if (is_ps) return "Create"; // PS5 Create / PS4 Share
      if (is_xbox) return "View+Menu";
      if (is_switch) return "Capture";
      return "Create/View";
    case Action::Build: // F8
      if (is_keyboard) return "F8";
      if (is_ps) return "Triangle"; // △
      if (is_xbox) return "Y";
      if (is_switch) return "X";
      return "Triangle/Y";
    case Action::Settings: // F1
      if (is_keyboard) return "F1";
      if (is_ps) return "Options";
      if (is_xbox) return "Menu";
      if (is_switch) return "+";
      return "F1/Options";
    case Action::Debug: // F2
      if (is_keyboard) return "F2";
      if (is_ps) return "Share+Options";
      if (is_xbox) return "View+Menu";
      if (is_switch) return "- + +";
      return "F2";
    case Action::Phone: // P / Up
      if (is_keyboard) return "P";
      if (is_ps) return "Haut"; // D-pad Up
      if (is_xbox) return "Haut";
      if (is_switch) return "Haut";
      return "P / Haut";
    case Action::Close: // Esc / O
      if (is_keyboard) return "Esc";
      if (is_ps) return "Rond"; // ○
      if (is_xbox) return "B";
      if (is_switch) return "A";
      return "Esc / B";
    case Action::NavigateUp:
      if (is_keyboard) return "Haut";
      if (is_ps) return "Haut / Stick";
      if (is_xbox) return "Haut / Stick";
      return "Haut";
    case Action::NavigateDown:
      if (is_keyboard) return "Bas";
      return "Bas / Stick";
    case Action::NavigateLeft:
      if (is_keyboard) return "Gauche";
      return "Gauche / Stick";
    case Action::NavigateRight:
      if (is_keyboard) return "Droite";
      return "Droite / Stick";
    case Action::Confirm:
      if (is_keyboard) return "Entree";
      if (is_ps) return "Croix"; // ×
      if (is_xbox) return "A";
      if (is_switch) return "B";
      return "Entree / A";
    case Action::Cancel:
      if (is_keyboard) return "Retour";
      if (is_ps) return "Rond";
      if (is_xbox) return "B";
      return "Retour / B";
  }
  return "?";
}

bool DrawHint(const char* label, Action action, bool* clicked) {
  const Device dev = CurrentDevice();
  const char* glyph = GlyphFor(action, dev);
  // Build button text: "Label [Glyph]" — glyph in brackets with color
  char button_text[128];
  std::snprintf(button_text, sizeof button_text, "%s  [%s]", label, glyph);

  // Color by device for visual feedback
  ImVec4 bg = ImVec4(0.13f, 0.13f, 0.14f, 1.f);
  ImVec4 hover = ImVec4(0.18f, 0.85f, 0.45f, 0.28f);
  if (dev == Device::PS4 || dev == Device::PS5) {
    bg = ImVec4(0.12f, 0.18f, 0.28f, 1.f); // PS blue
    hover = ImVec4(0.20f, 0.40f, 0.80f, 0.45f);
  } else if (dev == Device::Xbox) {
    bg = ImVec4(0.14f, 0.22f, 0.13f, 1.f); // Xbox green
    hover = ImVec4(0.20f, 0.60f, 0.20f, 0.45f);
  } else if (dev == Device::Switch) {
    bg = ImVec4(0.22f, 0.13f, 0.13f, 1.f); // Switch red
    hover = ImVec4(0.80f, 0.20f, 0.20f, 0.45f);
  }

  ImGui::PushStyleColor(ImGuiCol_Button, bg);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
  bool pressed = ImGui::Button(button_text);
  ImGui::PopStyleColor(2);
  if (pressed && clicked) *clicked = true;

  // Tooltip shows alternative device glyph
  if (ImGui::IsItemHovered()) {
    const char* kb = GlyphFor(action, Device::Keyboard);
    const bool is_ps_dev = (dev == Device::PS4 || dev == Device::PS5);
    const char* pad = is_ps_dev ? GlyphFor(action, Device::PS5) : GlyphFor(action, Device::Xbox);
    ImGui::SetTooltip("%s: %s\nClavier: %s\nManette: %s\n(%s actif)", label, glyph, kb, pad, DeviceLabel(dev));
  }
  return pressed;
}

}  // namespace gtav::control_hints
