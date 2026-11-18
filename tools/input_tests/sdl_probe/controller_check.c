// Observe SDL's mapped gamepad events without initializing a renderer or reading game files.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <string.h>

static const char *named(const char *name) { return name ? name : "unknown"; }

static const char *transport(SDL_JoystickConnectionState state) {
    switch (state) {
    case SDL_JOYSTICK_CONNECTION_WIRED: return "wired";
    case SDL_JOYSTICK_CONNECTION_WIRELESS: return "wireless";
    default: return "unknown transport";
    }
}

static void report(const SDL_Event *event) {
    switch (event->type) {
    case SDL_EVENT_JOYSTICK_ADDED:
        if (!SDL_IsGamepad(event->jdevice.which)) {
            printf("Joystick #%u: %s (no SDL gamepad mapping)\n", (unsigned)event->jdevice.which,
                   named(SDL_GetJoystickNameForID(event->jdevice.which)));
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED: {
        const SDL_JoystickID id = event->gdevice.which;
        if (SDL_GetGamepadFromID(id)) break;
        SDL_Gamepad *pad = SDL_OpenGamepad(id);
        if (!pad) {
            printf("Cannot open gamepad #%u: %s\n", (unsigned)id, SDL_GetError());
            break;
        }
        printf("Gamepad #%u: %s (%04x:%04x), type=%s, %s\n", (unsigned)id,
               named(SDL_GetGamepadName(pad)), (unsigned)SDL_GetGamepadVendor(pad),
               (unsigned)SDL_GetGamepadProduct(pad),
               named(SDL_GetGamepadStringForType(SDL_GetRealGamepadType(pad))),
               transport(SDL_GetGamepadConnectionState(pad)));
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(event->gdevice.which);
        if (pad) SDL_CloseGamepad(pad);
        printf("Gamepad #%u disconnected\n", (unsigned)event->gdevice.which);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        printf("#%u %s %s\n", (unsigned)event->gbutton.which,
               named(SDL_GetGamepadStringForButton(event->gbutton.button)),
               event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? "pressed" : "released");
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        // Suppress tiny stick drift and throttle analog events for the terminal.
        static Uint64 last_print[SDL_GAMEPAD_AXIS_COUNT];
        const unsigned axis = event->gaxis.axis;
        const int value = event->gaxis.value;
        const Uint64 now = SDL_GetTicks();
        if (axis < SDL_GAMEPAD_AXIS_COUNT && (value > 8192 || value < -8192) &&
            (!last_print[axis] || now - last_print[axis] >= 120)) {
            last_print[axis] = now;
            printf("#%u %s %d\n", (unsigned)event->gaxis.which,
                   named(SDL_GetGamepadStringForAxis(event->gaxis.axis)), value);
        }
        break;
    }
    default: break;
    }
    fflush(stdout);
}

static bool wait_for(SDL_JoystickID id, Uint32 type) {
    const Uint64 end = SDL_GetTicks() + 2000;
    while (SDL_GetTicks() < end) {
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, 10)) continue;
        report(&event);
        if (event.type != type) continue;
        if (type == SDL_EVENT_GAMEPAD_ADDED || type == SDL_EVENT_GAMEPAD_REMOVED) {
            if (event.gdevice.which == id) return true;
        } else if (type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            if (event.gaxis.which == id && event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX &&
                event.gaxis.value == 24576) return true;
        } else if (event.gbutton.which == id && event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
            return true;
        }
    }
    fprintf(stderr, "Missing virtual gamepad event 0x%x\n", (unsigned)type);
    return false;
}

static bool self_test(void) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.name = "Virtual test gamepad";
    desc.nbuttons = desc.naxes = 1;
    desc.button_mask = 1u << SDL_GAMEPAD_BUTTON_SOUTH;
    desc.axis_mask = 1u << SDL_GAMEPAD_AXIS_LEFTX;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (!id || !wait_for(id, SDL_EVENT_GAMEPAD_ADDED)) return false;
    SDL_Gamepad *pad = SDL_GetGamepadFromID(id);
    if (!pad) return false;
    SDL_Joystick *joy = SDL_GetGamepadJoystick(pad);
    return joy && SDL_SetJoystickVirtualButton(joy, 0, true) && wait_for(id, SDL_EVENT_GAMEPAD_BUTTON_DOWN) &&
           SDL_SetJoystickVirtualButton(joy, 0, false) && wait_for(id, SDL_EVENT_GAMEPAD_BUTTON_UP) &&
           SDL_SetJoystickVirtualAxis(joy, 0, 24576) && wait_for(id, SDL_EVENT_GAMEPAD_AXIS_MOTION) &&
           SDL_DetachVirtualJoystick(id) && wait_for(id, SDL_EVENT_GAMEPAD_REMOVED);
}

int main(int argc, char **argv) {
    const bool virtual_only = argc == 2 && strcmp(argv[1], "--self-test") == 0;
    if (argc != 1 && !virtual_only) {
        fprintf(stderr, "Usage: gtav_controller_check [--self-test]\n");
        return 2;
    }
    SDL_SetMainReady();  // a console executable, no SDL_main wrapper
    // The Windows game has a native Win32 window, not a focused SDL window.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL gamepad initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    printf("SDL %d on %s\n", SDL_GetVersion(), SDL_GetPlatform());
    int result = 0;
    if (virtual_only) {
        if (self_test()) {
            puts("Virtual SDL gamepad OK (not a physical controller test).");
        } else {
            fprintf(stderr, "Virtual SDL gamepad test failed: %s\n", SDL_GetError());
            result = 1;
        }
    } else {
        puts("Looking for gamepads for 60 seconds. Press buttons, move the sticks and pull the triggers.\n"
             "Ctrl+C stops the test. The game itself is not running.");
        fflush(stdout);
        const Uint64 end = SDL_GetTicks() + 60000;
        while (SDL_GetTicks() < end) {
            SDL_Event event;
            if (SDL_WaitEventTimeout(&event, 50)) report(&event);
        }
        puts("End of SDL observation (not an in-game test).");
    }
    SDL_Quit();
    return result;
}
