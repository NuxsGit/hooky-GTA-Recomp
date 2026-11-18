#include "input.h"
#include "menu.h"
#include "touch_controls.h"

#include <SDL3/SDL.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>

// This flag is normally provided by renderer.cpp; the test needs no renderer.
REXCVAR_DEFINE_STRING(gtav_native, "on", "GTAV", "Use the separate native window");

namespace {
using namespace rex::input;
using rex::X_RESULT;
namespace input = gtav::native::input;
namespace menu = gtav::native::menu;

void Require(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "%s (SDL: %s)\n", what, SDL_GetError());
  std::exit(1);
}

// The SDK's pad driver initializes on attachment to a UI window. Keep that
// lifecycle but don't require a GPU presenter or a registered SDK SDL window.
class TestContext : public rex::ui::WindowedAppContext {
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};

class TestWindow : public rex::ui::Window {
 public:
  explicit TestWindow(TestContext& context) : Window(context, "SDK input host", 640, 480) {}
  ~TestWindow() override { EnterDestructor(); }

 private:
  bool OpenImpl() override { return true; }
  void RequestCloseImpl() override {
    WindowDestructionReceiver receiver(this);
    OnBeforeClose(receiver);
    OnAfterClose();
  }
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override { return {}; }
  void RequestPaintImpl() override {}
};

X_INPUT_STATE ReadState(InputSystem& input_system) {
  X_INPUT_STATE state{};
  Require(input_system.GetState(0, &state) == X_ERROR_SUCCESS, "guest has no user 0 controller");
  return state;
}

X_INPUT_GAMEPAD Read(InputSystem& input_system) { return ReadState(input_system).gamepad; }

void Focus(SDL_WindowID id, bool gained) {
  SDL_Event event{};
  event.type = gained ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
  event.window.windowID = id;
  Require(SDL_PushEvent(&event), "could not push focus event");
}

void Key(SDL_WindowID id, SDL_Scancode scan, bool down = true) {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.windowID = id;
  event.key.scancode = scan;
  event.key.down = down;
  Require(SDL_PushEvent(&event), "could not push key event");
}

void Mouse(SDL_WindowID id, Uint8 button, bool down = true, SDL_MouseID which = 1) {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  event.button.windowID = id;
  event.button.which = which;
  event.button.button = button;
  event.button.down = down;
  Require(SDL_PushEvent(&event), "could not push mouse button event");
}

void Touch(SDL_WindowID id, Uint32 type, SDL_TouchID touch_id, float x, float y) {
  SDL_Event event{};
  event.type = type;
  event.tfinger.windowID = id;
  event.tfinger.touchID = touch_id;
  event.tfinger.fingerID = 1;
  event.tfinger.x = x;
  event.tfinger.y = y;
  Require(SDL_PushEvent(&event), "could not push touch event");
}

SDL_JoystickID VirtualPad(bool mapped) {
  SDL_VirtualJoystickDesc desc{};
  SDL_INIT_INTERFACE(&desc);
  desc.type = mapped ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
  // The virtual driver maps the first nbuttons in SDL's normal button order.
  desc.nbuttons = mapped ? SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1 : 3;
  desc.naxes = mapped ? SDL_GAMEPAD_AXIS_COUNT : 2;
  desc.vendor_id = 0xf123;
  desc.product_id = mapped ? 2 : 1;
  desc.name = mapped ? "Smoke test SDL gamepad" : "Smoke test generic joystick";
  const auto id = SDL_AttachVirtualJoystick(&desc);
  Require(id != 0, "could not attach virtual joystick");
  return id;
}

void PadButton(SDL_Joystick* pad, int button, bool down) {
  Require(SDL_SetJoystickVirtualButton(pad, button, down), "could not set virtual pad button");
  SDL_UpdateJoysticks();
}
}  // namespace

int main(int argc, char** argv) {
  using namespace std::chrono_literals;
  if (argc > 1 && std::string_view(argv[1]) == "--check-opt-out") {
    Require(rex::cvar::SetFlagFromCommandLine("mnk_mode", "false"), "could not override mnk_mode");
    Require(rex::cvar::SetFlagFromCommandLine("keybind_y", "Return"), "could not set custom keybind");
    input::Configure();
    Require(!rex::cvar::Query<bool>("mnk_mode"), "keyboard default overrode explicit opt-out");
    Require(rex::cvar::GetFlagByName("keybind_y") == "Return", "bind default overrode user's key");
    input::Shutdown();
    return 0;
  }

  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen,dummy");
  Require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK), "could not initialize SDL in dummy mode");
  const auto custom_id = VirtualPad(false);
  char guid[33]{};
  SDL_GUIDToString(SDL_GetJoystickGUIDForID(custom_id), guid, sizeof(guid));
  const auto mapping = std::filesystem::current_path() / "input-test-mappings.txt";
  {
    std::ofstream out(mapping);
    out << guid << ",Custom test pad,a:b0,back:b1,rightshoulder:b2,leftx:a0,platform:"
        << SDL_GetPlatform() << ",\n";
    Require(bool(out), "could not write virtual pad mapping");
  }
  Require(rex::cvar::SetFlagByName("hid_mappings_file", mapping.string()), "could not set mapping path");
  Require(rex::cvar::SetFlagByName("hid_background_input", "true"), "could not enable pad events without SDL focus");
  input::Configure();
  Require(rex::cvar::Query<bool>("mnk_mode") && rex::cvar::Query<bool>("mnk_mouse"), "desktop input is not enabled");

  TestContext context;
  TestWindow sdk_window(context);
  Require(sdk_window.Open(), "could not open the SDK input host");
  auto input_system = CreateDefaultInputSystem(false);
  input_system->SetDeviceAssignment(std::make_unique<SharedAssignment>());
  input_system->AddDriver(gtav::native::touch::CreateDriver());
  input_system->AttachWindow(&sdk_window);
  input_system->AddActiveCallback(input::GameplayActive);
  std::filesystem::remove(mapping);
  Require(SDL_IsGamepad(custom_id), "generic joystick was not recognized with its mapping file");
  auto* custom = SDL_OpenJoystick(custom_id);
  Require(custom != nullptr, "could not open the custom virtual joystick");

  auto* window = SDL_CreateWindow("Native game window", 640, 480, 0);
  Require(window != nullptr, "could not open the native input window");
  auto* other = SDL_CreateWindow("Other window", 640, 480, SDL_WINDOW_HIDDEN);
  Require(other != nullptr, "could not open a second SDL window");
  const auto id = SDL_GetWindowID(window);
  input::AttachWindow(window);
  SDL_PumpEvents();
  Focus(id, true);
  Key(SDL_GetWindowID(other), SDL_SCANCODE_W);
  Require(Read(*input_system).thumb_ly == 0, "another SDL window moved the player");
  const uint32_t neutral_packet = ReadState(*input_system).packet_number;
  Key(id, SDL_SCANCODE_ESCAPE);
  Key(id, SDL_SCANCODE_SPACE);
  auto polled = ReadState(*input_system);
  Require((polled.gamepad.buttons & (X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_X)) ==
              (X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_X) && polled.packet_number != neutral_packet,
          "Escape or Space binding did not update the guest pad");
  Require(ReadState(*input_system).packet_number == polled.packet_number,
          "a stationary pad changed the merged packet number");
  Key(id, SDL_SCANCODE_ESCAPE, false);
  Key(id, SDL_SCANCODE_SPACE, false);
  Key(SDL_GetWindowID(other), SDL_SCANCODE_F1);
  Require(!menu::IsOpen(), "another SDL window opened native settings");
  Key(id, SDL_SCANCODE_F1);
  Require(menu::IsOpen(), "F1 did not open native settings");
  Key(id, SDL_SCANCODE_ESCAPE);
  Require(!menu::IsOpen() && !(Read(*input_system).buttons & X_INPUT_GAMEPAD_START),
          "Escape did not close settings cleanly");
  Key(id, SDL_SCANCODE_ESCAPE, false);
  Key(id, SDL_SCANCODE_W);
  Key(id, SDL_SCANCODE_LSHIFT);
  Mouse(id, SDL_BUTTON_LEFT, true, SDL_TOUCH_MOUSEID);
  auto state = Read(*input_system);
  Require(state.thumb_ly == 32767 && (state.buttons & X_INPUT_GAMEPAD_A) && state.right_trigger == 0,
          "sprint interrupted movement or touch triggered a mouse action");
  Mouse(id, SDL_BUTTON_LEFT);
  Mouse(id, SDL_BUTTON_RIGHT);
  Mouse(id, SDL_BUTTON_X1);
  Mouse(id, SDL_BUTTON_X2);
  Key(id, SDL_SCANCODE_LEFT);
  state = Read(*input_system);
  const uint16_t mouse_shoulder_and_dpad = X_INPUT_GAMEPAD_LEFT_SHOULDER |
                                           X_INPUT_GAMEPAD_RIGHT_SHOULDER | X_INPUT_GAMEPAD_DPAD_LEFT;
  Require(state.left_trigger == 255 && state.right_trigger == 255 &&
              (state.buttons & mouse_shoulder_and_dpad) == mouse_shoulder_and_dpad,
          "mouse triggers, side buttons or D-pad failed with Shift held");
  Key(id, SDL_SCANCODE_I);
  Require(Read(*input_system).thumb_ry == 32767, "keyboard-only camera fallback failed");
  Key(id, SDL_SCANCODE_I, false);

  SDL_Event motion{};
  motion.type = SDL_EVENT_MOUSE_MOTION;
  motion.motion.windowID = id;
  motion.motion.which = 1;
  motion.motion.xrel = 7;
  motion.motion.yrel = -5;
  Require(SDL_PushEvent(&motion), "could not push relative mouse motion");
  std::this_thread::sleep_for(25ms);
  state = Read(*input_system);
  Require(state.thumb_rx > 0 && state.thumb_ry > 0, "relative mouse did not move the right stick");
  state = Read(*input_system);
  Require(state.thumb_rx > 0 && state.thumb_ry > 0, "right-stick sample was lost on a repeated guest poll");
  Focus(id, false);
  state = Read(*input_system);
  Require(!state.buttons && !state.right_trigger && !state.thumb_ly && !state.thumb_rx,
          "focus loss left a held button, movement or camera motion");
  Focus(id, true);

  // The overlay shortcut follows the visible game window, not an unrelated window.
  Focus(id, false);
  PadButton(custom, 1, true);
  PadButton(custom, 2, true);
  Require(!menu::IsOpen(), "background pad opened the overlay");
  PadButton(custom, 1, false);
  PadButton(custom, 2, false);
  Focus(id, true);

  const auto standard_id = VirtualPad(true);  // hot-plug after startup while another pad is connected
  auto* standard = SDL_OpenJoystick(standard_id);
  Require(standard != nullptr, "could not open hot-plugged gamepad");
  (void)Read(*input_system);  // let the driver open the controllers before sending their first inputs
  PadButton(custom, 0, true);
  PadButton(standard, SDL_GAMEPAD_BUTTON_EAST, true);
  Key(id, SDL_SCANCODE_D);
  state = Read(*input_system);
  Require((state.buttons & (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B)) == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B) &&
              state.thumb_lx == 32767, "both gamepads and keyboard did not merge into user 0");
  PadButton(standard, SDL_GAMEPAD_BUTTON_EAST, false);
  PadButton(custom, 1, true);  // Back, with RB on a different device
  PadButton(standard, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
  Require(!menu::IsOpen(), "two different devices triggered the menu chord");
  PadButton(standard, SDL_GAMEPAD_BUTTON_BACK, true);
  Require(menu::IsOpen(), "gamepad did not open native settings");
  state = Read(*input_system);
  Require(!state.buttons && !state.thumb_lx, "gameplay got input while native settings were open");
  PadButton(standard, SDL_GAMEPAD_BUTTON_EAST, true);
  state = Read(*input_system);
  Require(!menu::IsOpen() && !state.thumb_lx && !(state.buttons & X_INPUT_GAMEPAD_B),
          "closing settings leaked the cancel button or held keyboard input");

  Require(SDL_DetachVirtualJoystick(standard_id), "could not unplug hot-plugged controller");
  SDL_CloseJoystick(standard);
  state = Read(*input_system);
  Require((state.buttons & X_INPUT_GAMEPAD_A) && !(state.buttons & X_INPUT_GAMEPAD_B),
          "unplugging a gamepad kept stale buttons or broke the other gamepad");
  Require(SDL_DetachVirtualJoystick(custom_id), "could not unplug mapped joystick");
  SDL_CloseJoystick(custom);
  Require(!Read(*input_system).buttons, "unplugging the last gamepad left stale buttons");

  // Touch is still a separate pad; a mouse-generated finger cannot move its stick.
  Require(rex::cvar::SetFlagByName("gtav_touch_controls", "true"), "could not enable touch controls");
  Require(menu::Frame(640, 480, 60, 16.7) != nullptr, "touch overlay did not produce a frame");
  Touch(id, SDL_EVENT_FINGER_DOWN, SDL_MOUSE_TOUCHID, 0.25f, 0.6f);
  Touch(id, SDL_EVENT_FINGER_MOTION, SDL_MOUSE_TOUCHID, 0.25f, 0.5f);
  Require(!Read(*input_system).thumb_ly, "mouse events became a second touch input");
  Touch(id, SDL_EVENT_FINGER_DOWN, 1, 0.25f, 0.6f);
  Touch(id, SDL_EVENT_FINGER_MOTION, 1, 0.25f, 0.5f);
  Require(Read(*input_system).thumb_ly > 0, "real touch stopped moving the player");
  Touch(id, SDL_EVENT_FINGER_CANCELED, 1, 0.25f, 0.5f);
  Require(!Read(*input_system).thumb_ly, "canceled touch left a held stick");

  const auto script = std::filesystem::current_path() / "input-test-script.txt";
  {
    std::ofstream out(script);
    out << "0 60 1000 0 255 0 16384 0 0\n";
    Require(bool(out), "could not write the test input script");
  }
  Require(rex::cvar::SetFlagByName("input_script", script.string()), "could not set input_script");
  Key(id, SDL_SCANCODE_A);  // The script should replace, not merge with, live input.
  state = Read(*input_system);
  std::filesystem::remove(script);
  Require(state.buttons == X_INPUT_GAMEPAD_A && state.right_trigger == 255 &&
              state.thumb_ly == 16384 && !state.thumb_lx, "scripted pad did not override live input");

  input::Shutdown();
  SDL_DestroyWindow(other);
  SDL_DestroyWindow(window);
  input_system->AttachWindow(nullptr);
  sdk_window.RequestClose();
  input_system->Shutdown();
  SDL_Quit();
  return 0;
}
