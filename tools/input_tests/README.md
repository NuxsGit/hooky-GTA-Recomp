# Controller verification

## Windows: USB and Bluetooth before release

The console probe needs no game or graphics driver. Build it against **the same SDK install**
that will be used for the package (Windows x64, RelWithDebInfo, shared SDL3):

```bat
cmake -S tools/input_tests/sdl_probe -B build/controller-check -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_PREFIX_PATH=%CD%/rexglue-sdk/out/install/win-amd64"
cmake --build build/controller-check
ctest --test-dir build/controller-check --output-on-failure
build\controller-check\gtav_controller_check.exe
```

The probe prints SDL's mapped pad name, vendor/product, reported connection type, button and
axis events for one minute. `--self-test` connects a virtual joystick and checks its events; it
does **not** exercise USB or Bluetooth. Run the normal probe twice with a real DualSense:

1. **USB:** close Steam Input, DS4Windows or similar wrappers, turn off the pad's Bluetooth
   connection and use a USB **data** cable. The pad should appear as a mapped PlayStation
   controller (`type=ps5` for a DualSense), not only as a virtual Xbox pad. Press Cross/Circle,
   use the D-pad, move both sticks and pull both triggers. Check that the events appear.
2. **Bluetooth:** unplug the cable; pair the controller in Windows (hold **Create + PS** until
   the lights flash). Run the same check. Disconnect and reconnect it while the probe is open
   to check hot-plug. Some drivers report an unknown transport; verify the actual connection.
3. **In the unpacked Windows release candidate:** use `input_backend = "sdl"` in `gtav.toml`
   if an older install set `"xinput"`. Remove scripted pad overrides during the test. Confirm
   that Cross selects (A), Circle cancels (B), the sticks move the player and camera, and both
   triggers respond **in the game** over each connection. Record the tested commit, controller
   model and Windows version. A passing probe alone is not a release sign-off.

For a virtual-pad-only run on Linux/macOS, configure the same `sdl_probe` project with
`-DCMAKE_PREFIX_PATH` pointing at an installed SDL or the patched SDK. These platforms also
have the game input bridge tests below.

## Linux/macOS: game input bridge smoke tests

These Linux/macOS tests exercise the actual game input bridge and the patched SDK input drivers
with a headless SDL window, synthetic keyboard/mouse and touch events, and virtual joysticks. No
GTA V files, generated game code, GPU renderer, physical controller or display are needed.

Build and install ReXGlue SDK v0.10.0 with `rexglue-sdk-gtav-linux.patch` applied and then run:

```sh
cmake -S tools/input_tests -B build/input-tests -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/installed/rexglue-sdk
cmake --build build/input-tests
ctest --test-dir build/input-tests --output-on-failure
```

The tests cover explicit keyboard opt-out, physical movement bindings, simultaneous actions,
relative mouse sampling, release on focus loss, merged guest packets, controller mappings and
hot-plug, controller overlay isolation, touch, and scripted input precedence. They don't replace
in-game checks of mouse feel, platform capture, rumble, or physical USB and Bluetooth devices.
