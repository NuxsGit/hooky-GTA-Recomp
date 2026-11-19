#include "input.h"

#include "../debug_tools.h"

#include "menu.h"
#include "paths.h"

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/sdl_virtual_key.h>

#include <atomic>
#include <bitset>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

REXCVAR_DECLARE(std::string, gtav_native);
REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, mnk_mouse);

namespace gtav::native::input {
namespace {

using rex::input::mnk::ExternalInput;
using rex::ui::VirtualKey;

std::mutex state_mutex;
ExternalInput state;
bool focused = false;
std::atomic<bool> running{false};

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
constexpr bool kDesktop = false;
#else
constexpr bool kDesktop = true;
#endif

#ifdef _WIN32
std::atomic<HWND> native_window{nullptr};
#else
std::atomic<SDL_WindowID> window_id{0};
std::atomic<bool> sdl_focused{false};
std::atomic<bool> capture_requested{false};
std::atomic<bool> capture_queued{false};
#endif

bool HasMenuFocus() {
#ifdef _WIN32
  const auto window = native_window.load();
  return window && GetForegroundWindow() == window;
#else
  return sdl_focused;
#endif
}

bool Read(ExternalInput& out) {
  std::lock_guard lock(state_mutex);
  if (!focused) return false;  // the SDK's own window can still be used in shadow mode
  out = state;
  state.dx = state.dy = 0;
  state.reset_motion = false;
  return true;
}

void ClearMotion() {
  std::lock_guard lock(state_mutex);
  state.dx = state.dy = 0;
  state.reset_motion = true;
}

void Focus(bool value) {
  {
    std::lock_guard lock(state_mutex);
    focused = value;
    state = {};
    state.reset_motion = true;
  }
  if (!value && debug::PanelControls().Open()) debug::PanelControls().Reset();
}

void DebugKey(VirtualKey key, bool down) {
  if (!debug::PanelControls().Open()) return;
  using K = debug::Key;
  using V = VirtualKey;
  switch (key) {
    case V::kW: debug::PanelControls().SetKey(K::Forward, down); break;
    case V::kA: debug::PanelControls().SetKey(K::Left, down); break;
    case V::kS: debug::PanelControls().SetKey(K::Back, down); break;
    case V::kD: debug::PanelControls().SetKey(K::Right, down); break;
    case V::kQ: debug::PanelControls().SetKey(K::Down, down); break;
    case V::kE: debug::PanelControls().SetKey(K::Up, down); break;
    case V::kI: debug::PanelControls().SetKey(K::LookUp, down); break;
    case V::kJ: debug::PanelControls().SetKey(K::LookLeft, down); break;
    case V::kK: debug::PanelControls().SetKey(K::LookDown, down); break;
    case V::kL: debug::PanelControls().SetKey(K::LookRight, down); break;
    default: break;
  }
}

void Key(VirtualKey key, bool down) {
  const auto code = static_cast<uint32_t>(key);
  if (!code || code >= std::size(state.keys)) return;
  std::lock_guard lock(state_mutex);
  if (focused && !menu::IsOpen() && (!down || REXCVAR_GET(mnk_mode))) state.keys[code] = down;
}

void Motion(float dx, float dy) {
  std::lock_guard lock(state_mutex);
  if (focused && !menu::IsOpen()) {
    state.dx += dx;
    state.dy += dy;
  }
}

bool WantsMouse() {
  return running && REXCVAR_GET(mnk_mode) && REXCVAR_GET(mnk_mouse) &&
         rex::input::mnk::IsMouseLookActive() && !menu::IsOpen();
}

void Default(std::string_view name, std::string_view value) {
  // In particular, don't override an explicit --mnk_mode=false (the SDK's factory default).
  if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault)
    rex::cvar::SetFlagByName(name, value);
}

std::optional<menu::Key> MenuKey(SDL_GamepadButton button) {
  switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return menu::Key::kUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return menu::Key::kDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return menu::Key::kLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return menu::Key::kRight;
    case SDL_GAMEPAD_BUTTON_SOUTH: return menu::Key::kSpace;
    default: return std::nullopt;
  }
}

bool GamepadEvent(const SDL_Event& event) {
  // The shortcut must come from one controller, and unplugging it must release its buttons.
  static std::mutex mutex;
  static std::unordered_map<SDL_JoystickID, std::bitset<SDL_GAMEPAD_BUTTON_COUNT>> pads;
  std::lock_guard lock(mutex);
  if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
    if (auto it = pads.find(event.gdevice.which); it != pads.end()) {
      for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        if (!it->second[b]) continue;
        if (auto key = MenuKey(SDL_GamepadButton(b))) menu::OnKey(*key, false);
      }
      pads.erase(it);
    }
    return false;
  }
  const auto button = SDL_GamepadButton(event.gbutton.button);
  if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) return false;
  const auto key = MenuKey(button);
  const bool focused_menu = running && HasMenuFocus();
  const bool combo = button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER || button == SDL_GAMEPAD_BUTTON_BACK;
  const bool was_open = menu::IsOpen();
  const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  if (!key && !combo && button != SDL_GAMEPAD_BUTTON_EAST) return was_open && down;
  auto it = pads.find(event.gbutton.which);
  if (it == pads.end()) {
    if (!down) return false;
    it = pads.try_emplace(event.gbutton.which).first;
  }
  auto& held = it->second;
  if (held[button] == down) return was_open && down;
  held[button] = down;
  const bool toggle = focused_menu && combo && down && held[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] &&
                      held[SDL_GAMEPAD_BUTTON_BACK];
  if (toggle) {
    menu::Toggle();
  } else if (focused_menu && button == SDL_GAMEPAD_BUTTON_EAST && was_open && down) {
    menu::Close();
  } else if (key && (focused_menu || !down)) {
    menu::OnKey(*key, down);
  }
  if (held.none()) pads.erase(it);
  return down && (was_open || toggle);
}

#ifdef _WIN32
// SDL's own relative-mouse path services the separate SDK window. The native renderer keeps its
// Win32 message thread; register the mouse here only while this window has gameplay focus.
bool mouse_captured = false;
bool capture_failed = false;
bool sizing = false;

VirtualKey PhysicalKey(WPARAM vk, LPARAM lp) {
  const unsigned scan = (lp >> 16) & 0x1ff;
  // Set-1 physical positions, in the same US-based names as the SDK's SDL scancode translator.
  if (scan >= 0x02 && scan <= 0x0B) return VirtualKey("1234567890"[scan - 0x02]);
  if (scan >= 0x10 && scan <= 0x19) return VirtualKey("QWERTYUIOP"[scan - 0x10]);
  if (scan >= 0x1E && scan <= 0x26) return VirtualKey("ASDFGHJKL"[scan - 0x1E]);
  if (scan >= 0x2C && scan <= 0x32) return VirtualKey("ZXCVBNM"[scan - 0x2C]);
  switch (scan) {
    case 0x0C: return VirtualKey::kOemMinus;
    case 0x0D: return VirtualKey::kOemPlus;
    case 0x1A: return VirtualKey::kOem4;
    case 0x1B: return VirtualKey::kOem6;
    case 0x27: return VirtualKey::kOem1;
    case 0x28: return VirtualKey::kOem7;
    case 0x29: return VirtualKey::kOem3;
    case 0x2B: return VirtualKey::kOem5;
    case 0x33: return VirtualKey::kOemComma;
    case 0x34: return VirtualKey::kOemPeriod;
    case 0x35: return VirtualKey::kOem2;
    default: return VirtualKey(vk);
  }
}

bool OwnsRawMouse(HWND window) {
  UINT count = 0;
  if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == UINT(-1) || !count)
    return false;
  std::vector<RAWINPUTDEVICE> devices(count);
  const UINT filled = GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE));
  if (filled == UINT(-1)) return false;
  for (UINT i = 0; i < filled; ++i) {
    const auto& device = devices[i];
    if (device.usUsagePage == 0x01 && device.usUsage == 0x02) return device.hwndTarget == window;
  }
  return false;
}

void ReleaseMouse(HWND window) {
  if (!mouse_captured) return;
  mouse_captured = false;
  // Registration is process-wide. The SDK window may already have taken it over.
  if (OwnsRawMouse(window)) {
    RAWINPUTDEVICE mouse{0x01, 0x02, RIDEV_REMOVE, nullptr};
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
  }
  ClipCursor(nullptr);
  SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
}

void ConstrainMouse(HWND window) {
  RECT client{};
  if (!GetClientRect(window, &client) || IsRectEmpty(&client)) return;
  POINT top_left{client.left, client.top}, bottom_right{client.right, client.bottom};
  if (!ClientToScreen(window, &top_left) || !ClientToScreen(window, &bottom_right)) return;
  RECT screen{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
  RECT current{};
  if (!GetClipCursor(&current) || !EqualRect(&current, &screen)) ClipCursor(&screen);
}
#else
VirtualKey MouseButton(uint8_t button) {
  switch (button) {
    case SDL_BUTTON_LEFT: return VirtualKey::kLButton;
    case SDL_BUTTON_RIGHT: return VirtualKey::kRButton;
    case SDL_BUTTON_MIDDLE: return VirtualKey::kMButton;
    case SDL_BUTTON_X1: return VirtualKey::kXButton1;
    case SDL_BUTTON_X2: return VirtualKey::kXButton2;
    default: return VirtualKey::kNone;
  }
}

float MenuPixelDensity(SDL_WindowID id) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  // SDL reports window points; the iPhone overlay is drawn in swap-chain pixels.
  if (auto* window = SDL_GetWindowFromID(id)) return SDL_GetWindowPixelDensity(window);
#else
  (void)id;
#endif
  return 1.0f;
}

void Keyboard(SDL_Scancode scan, bool down, bool repeat) {
  if (scan == SDL_SCANCODE_F1 || scan == SDL_SCANCODE_AC_BACK || (kDesktop && scan == SDL_SCANCODE_F2)) {
    if (down && !repeat) {
      if (scan == SDL_SCANCODE_F2) menu::ToggleDebug();
      else menu::Toggle();
    }
    return;
  }
  if (menu::IsOpen()) {
    if (kDesktop && !repeat) DebugKey(rex::ui::TranslateSDLScancode(scan), down);
    switch (scan) {
      case SDL_SCANCODE_ESCAPE: if (down && !repeat) menu::Close(); break;
      case SDL_SCANCODE_UP: menu::OnKey(menu::Key::kUp, down); break;
      case SDL_SCANCODE_DOWN: menu::OnKey(menu::Key::kDown, down); break;
      case SDL_SCANCODE_LEFT: menu::OnKey(menu::Key::kLeft, down); break;
      case SDL_SCANCODE_RIGHT: menu::OnKey(menu::Key::kRight, down); break;
      case SDL_SCANCODE_RETURN: menu::OnKey(menu::Key::kEnter, down); break;
      case SDL_SCANCODE_SPACE: menu::OnKey(menu::Key::kSpace, down); break;
      case SDL_SCANCODE_TAB: menu::OnKey(menu::Key::kTab, down); break;
      default: break;
    }
    return;
  }
  if (kDesktop && !repeat) Key(rex::ui::TranslateSDLScancode(scan), down);
}

bool SDLCALL WindowEvent(void*, SDL_Event* event) {
  const auto id = window_id.load();
  if (!running || !id) return true;
  switch (event->type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_DESTROYED:
      if (event->window.windowID == id) {
        const bool gained = event->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        sdl_focused = gained;
        if (kDesktop) Focus(gained);
      }
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      if (event->key.windowID == id ||
          (!kDesktop && event->key.windowID == 0 && event->key.scancode == SDL_SCANCODE_AC_BACK))
        Keyboard(event->key.scancode, event->key.down, event->key.repeat);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      if (event->button.windowID != id) break;
      const auto button = MouseButton(event->button.button);
      if (menu::IsOpen()) {
        // A tap can arrive without preceding motion.
        const float density = MenuPixelDensity(id);
        menu::OnMouseMove(event->button.x * density, event->button.y * density);
        const int index = button == VirtualKey::kLButton ? 0 :
                          button == VirtualKey::kRButton ? 1 :
                          button == VirtualKey::kMButton ? 2 : -1;
        if (index >= 0) menu::OnMouseButton(index, event->button.down);
      } else if (kDesktop && event->button.which != SDL_TOUCH_MOUSEID) {
        Key(button, event->button.down);
      }
      break;
    }
    case SDL_EVENT_MOUSE_MOTION:
      if (event->motion.windowID != id) break;
      if (menu::IsOpen()) {
        const float density = MenuPixelDensity(id);
        menu::OnMouseMove(event->motion.x * density, event->motion.y * density);
      } else if (kDesktop && event->motion.which != SDL_TOUCH_MOUSEID && WantsMouse()) {
        Motion(event->motion.xrel, event->motion.yrel);
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      if (event->wheel.windowID == id && menu::IsOpen()) {
        menu::OnMouseWheel(event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y);
      }
      break;
    default: break;
  }
  return true;
}
#endif

}  // namespace

void Configure() {
  running = true;
  const bool native = REXCVAR_GET(gtav_native) != "off";
  if (kDesktop) {
    Default("mnk_mode", "true");
    Default("mnk_mouse", "true");
    if (native) rex::input::mnk::SetExternalInputReader(Read);
  }
  if (native) rex::input::sdl::SetGamepadEventFilter(GamepadEvent);
#ifdef _WIN32
  // The visible game window is Win32, not SDL; otherwise SDL drops pad events even when it's focused.
  if (native) Default("hid_background_input", "true");
#endif

  Default("keybind_a", "Shift,Return");
  Default("keybind_b", "R,Backspace");
  Default("keybind_x", "Space");
  Default("keybind_y", "F");
  Default("keybind_left_trigger", "RMB,Q");
  Default("keybind_right_trigger", "LMB,E");
  Default("keybind_left_shoulder", "1,Mouse4");
  Default("keybind_right_shoulder", "3,Mouse5");
  Default("keybind_lstick_press", "Control");
  Default("keybind_rstick_press", "MMB,C");
  Default("keybind_rstick_up", "I");
  Default("keybind_rstick_down", "K");
  Default("keybind_rstick_left", "J");
  Default("keybind_rstick_right", "L");
  Default("keybind_dpad_up", "Up");
  Default("keybind_dpad_down", "Down");
  Default("keybind_dpad_left", "Left");
  Default("keybind_dpad_right", "Right");
  Default("keybind_back", "Tab");
  Default("keybind_start", "Escape");

  // Custom SDL mappings are optional. On mobile ExeDir is the user-accessible files directory.
  if (rex::cvar::GetFlagSource("hid_mappings_file") == rex::cvar::Source::kDefault) {
    const auto path = ExeDir() / "gamecontrollerdb.txt";
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
      const auto utf8 = path.u8string();
      Default("hid_mappings_file", {reinterpret_cast<const char*>(utf8.data()), utf8.size()});
    } else if (!std::filesystem::is_regular_file("gamecontrollerdb.txt", ec)) {
      Default("hid_mappings_file", "");  // SDL's builtin mappings still apply
    }
  }
}

bool GameplayActive() {
  if (!running || menu::IsOpen()) return false;
#ifdef _WIN32
  // SDL must receive controller events for a non-SDL HWND. Do not let those
  // events (or polled XInput) drive the game while another app is foreground.
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
#else
  return true;
#endif
}

void Reset() {
  std::lock_guard lock(state_mutex);
  state = {};
  state.reset_motion = true;
}

void Shutdown() {
  running = false;
  rex::input::mnk::SetExternalInputReader(nullptr);
  rex::input::sdl::SetGamepadEventFilter(nullptr);
  Focus(false);
#ifndef _WIN32
  if (const auto id = window_id.exchange(0)) {
    SDL_RemoveEventWatch(WindowEvent, nullptr);
    if (kDesktop) {
      if (auto* window = SDL_GetWindowFromID(id)) SDL_SetWindowRelativeMouseMode(window, false);
    }
    sdl_focused = false;
  }
#endif
}

#ifdef _WIN32
void UpdateMouseCapture(HWND window) {
  if (!WantsMouse() || sizing || GetForegroundWindow() != window || GetFocus() != window ||
      IsIconic(window)) {
    ReleaseMouse(window);
    capture_failed = false;
    return;
  }
  if (capture_failed) return;
  if (mouse_captured && OwnsRawMouse(window)) {
    ConstrainMouse(window);
    return;
  }
  // An SDL shadow window can release its capture just after we regain focus.
  // The window thread's timer retries if that removed our raw registration.
  ReleaseMouse(window);
  RAWINPUTDEVICE mouse{0x01, 0x02, 0, window};  // keep legacy messages for mouse buttons/menu
  if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
    REXLOG_WARN("[input] Could not enable raw mouse motion: {}", GetLastError());
    capture_failed = true;
    return;
  }
  mouse_captured = true;
  ConstrainMouse(window);
  ClearMotion();
  SetCursor(nullptr);
}

bool HandleMessage(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  switch (message) {
    case WM_NCCREATE:
      native_window = window;
      break;
    case WM_SETFOCUS:
      Focus(true);
      UpdateMouseCapture(window);
      break;
    case WM_KILLFOCUS:
    case WM_DESTROY:
    case WM_CLOSE:
      if (message != WM_KILLFOCUS) native_window = nullptr;
      Focus(false);
      ReleaseMouse(window);
      capture_failed = false;
      break;
    case WM_ENTERSIZEMOVE:
      sizing = true;
      Reset();
      ReleaseMouse(window);
      break;
    case WM_EXITSIZEMOVE:
      sizing = false;
      UpdateMouseCapture(window);
      break;
    case WM_MOVE:
    case WM_SIZE:
      if (mouse_captured) ConstrainMouse(window);
      break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_KEYUP: case WM_SYSKEYUP:
      if (const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
          !down || !(lp & (1L << 30))) {
        if (menu::IsOpen()) DebugKey(PhysicalKey(wp, lp), down);
        else Key(PhysicalKey(wp, lp), down);
      }
      break;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
      Key(VirtualKey::kLButton, message == WM_LBUTTONDOWN);
      break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
      Key(VirtualKey::kRButton, message == WM_RBUTTONDOWN);
      break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
      Key(VirtualKey::kMButton, message == WM_MBUTTONDOWN);
      break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
      Key(HIWORD(wp) == XBUTTON1 ? VirtualKey::kXButton1 : VirtualKey::kXButton2, message == WM_XBUTTONDOWN);
      return true;
    case WM_SETCURSOR:
      if (mouse_captured && LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);
        return true;
      }
      break;
    case WM_INPUT:
      if (mouse_captured && WantsMouse() && GetForegroundWindow() == window) {
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size,
                            sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
          Motion(float(raw.data.mouse.lLastX), float(raw.data.mouse.lLastY));
        }
      }
      break;  // DefWindowProc must still clean up the WM_INPUT handle.
    default: break;
  }
  return false;
}
#else
void AttachWindow(SDL_Window* window) {
  if (!window) return;
  if (!SDL_AddEventWatch(WindowEvent, nullptr)) {
    REXLOG_ERROR("[input] Could not watch native window events: {}", SDL_GetError());
    return;
  }
  window_id = SDL_GetWindowID(window);
  const bool has_focus = SDL_GetKeyboardFocus() == window;
  sdl_focused = has_focus;
  if (kDesktop) Focus(has_focus);
}

void UpdateMouseCapture() {
  if (!kDesktop || !window_id) return;
  const bool wanted = sdl_focused && WantsMouse();
  if (capture_requested.exchange(wanted) == wanted || capture_queued.exchange(true)) return;
  // Outside the SDL event watch: changing relative mode can flush and produce events itself.
  if (!SDL_RunOnMainThread([](void*) {
        capture_queued = false;
        if (auto* window = SDL_GetWindowFromID(window_id.load())) {
          const bool capture = capture_requested && WantsMouse() && SDL_GetKeyboardFocus() == window;
          if (SDL_GetWindowRelativeMouseMode(window) != capture) {
            if (!SDL_SetWindowRelativeMouseMode(window, capture))
              REXLOG_WARN("[input] Could not change relative mouse mode: {}", SDL_GetError());
            ClearMotion();
          }
        }
      }, nullptr, false)) {
    capture_queued = false;
    capture_requested = !wanted;
  }
}
#endif

}  // namespace gtav::native::input
