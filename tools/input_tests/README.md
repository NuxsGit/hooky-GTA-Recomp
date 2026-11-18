# Input smoke tests

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
