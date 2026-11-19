# Windows native renderer: output, FSR 1 and occlusion

Press **F1** in the game. Under **Display**, select **Output (restart)**:

| Preset | Output pixels |
| --- | --- |
| 720p (default window) | 1280 × 720 |
| 1080p | 1920 × 1080 |
| QHD / 1440p | 2560 × 1440 |
| UHD / 4K | 3840 × 2160 |
| Borderless desktop | Current monitor resolution |

A matching monitor uses borderless for 1080p, QHD and 4K; otherwise the window fits inside the
monitor's work area. This does not change Windows's display mode. The game stays 16:9; a wider
or taller window has black bars. F1 shows the actual output and the size of the game buffer.

**Scene scale (restart)** controls the existing internal size of full-width render targets, not the
monitor. Start at **100%** for the best source detail. Try **87.5%** or **75%** if GPU-bound, keeping
output and FPS limit constant when comparing. Changing this scale doesn't speed up the CPU part of
replay or streaming. 4K output is **not** native 4K rendering or a high-resolution texture pack.

**Upscaling** switches between Linear (default) and **AMD FSR 1** immediately. FSR 1 runs AMD's
EASU and RCAS passes on the SDR front buffer, including GTA's HUD. The host overlays remain at
output resolution. It bypasses the extra passes at 1:1 or when downscaling. The intermediate is
capped at 2× per axis; for a 720p source on a 4K panel, FSR processes 1440p and the last blit scales
that to 4K. If the driver refuses the optional pipelines or scratch resources, it falls back to
Linear. FSR 1 itself costs GPU time; any FPS gain comes from reducing internal rendering work.

F1 saves the settings to `gtav_settings.ini` next to the game executable. For a one-off test in
PowerShell (saved F1 values ordinarily take precedence):

```powershell
$env:GTAV_IGNORE_SETTINGS='1'
.\gtav_launcher.exe --play -- --gtav_native_output=1440 --gtav_native_fsr1=true --gtav_native_render_scale=0.75
Remove-Item Env:GTAV_IGNORE_SETTINGS
```

## When a prop appears in front of the player

Upscaling never affects depth or draw order. The replay now skips a draw that requests a pixel
shader the host doesn't have yet, without writing depth/stencil or caching a depth-only pipeline
for it. Draws that actually request **no** pixel shader still run as before. This fixes a specific
failure mode (especially with a cold shader cache); it is not a confirmed reproduction of all
occlusion artifacts. No global change to depth comparisons or draw order was made.

### Compare the same scene with a text-only F9 capture

Start with **Linear**, **100%** scene scale and **Best image**. Note the mission/location, game
mode (Campaign or Workshop), GPU and driver, and whether the overlap persists when standing still.
From the extracted Windows package directory:

```powershell
.\gtav_launcher.exe --play -- --gtav_native_trace_only=true
```

Press **F9** with the artifact on screen. Share `logs/native_trace/frame_*.txt` (relative to the
**game executable**) and the repro steps. With this flag, F9 does not export pixels, geometry,
shaders or saves. The trace includes command order, render targets, depth/stencil state, shader
IDs, bound 2D fetch slots, and directly identifiable GPU copy registers. `recorded=true` means the
host recorded a command, not that a GPU result was verified. Bound slots alone don't tell us what
a shader reads. The launcher's main log is `data/logs/gtav.log`.

For an A/B at exactly the same angle, quit, relaunch with both flags, and capture again:

```powershell
.\gtav_launcher.exe --play -- --gtav_native_trace_only=true --gtav_native_resolve_depth_clear=true
```

This **opt-in experiment** can apply the depth/stencil clear after a color copy, or preserve the
requested stencil value after a depth copy. It requires a supported, single-sampled copy with
matching `RB_COPY_CONTROL`/`RB_DEPTH_CLEAR` evidence and stable depth attachment state. If the call
emits no directly recognizable copy (for example indirect or predicated work), the experiment
leaves it alone. `cleared_depth` in the trace shows what the native renderer actually recorded.
This is not a confirmed fix for any particular prop. Omit the experiment flag to restore the old
clear policy. If the issue occurs only while streaming, try a separate A/B with
`--gtav_native_fence_wait=true`; that changes waiting behavior and can reduce throughput.

Without `--gtav_native_trace_only=true`, F9 still creates the full GPU dump in
`logs/native_dump/frame_*` (about 350 MB per frame). F10 captures the unfiltered front buffer.
Use text-only capture for sharing by default. Diagnostics are not a performance benchmark.

## Scope and validation

FSR 1 is spatial. DLSS and frame generation are not integrated. Extra presentations of the same
image are not frame generation. The front buffer already contains GTA's HUD; use the text-only
F9 trace to look for separate inputs rather than feeding the final image to a temporal SDK.

For input discovery, use `--gtav_native_trace_only=true --gtav_native_dump_frames=3` (one F9
captures three consecutive guest frames). Capture the **same** scene stationary, with camera-only
motion, and with a moving actor and a stationary camera. Repeat with GTA's HUD off. Compare the
last writers and first consumers by shader ID, RT/texture identity, size and format. A matching
address or a two-channel texture is only a lead: Xbox memory aliases and unused bindings are
common. A local full F9 dump at 100% scene scale and shader disassembly can verify the candidate
texels and shader reads/writes. If the image is modified in place, locate the last scene draw
before the first HUD draw, not merely the last resolve.

Before enabling a provider: verify color/depth alignment and depth convention, that vectors
encode both camera and object motion (direction, units, jitter and coverage), and where to obtain
unjittered camera transforms, reset flags and frame timing. For frame generation, identify the
postprocessed scene before HUD and keep HUD composition separate. Check supported features and
multipliers on the actual adapter/driver and validate pacing, input latency, and resets in-game.
No DLSS/FG multiplier is claimed by the current build.

The optional `--gtav_native_replay=true` buffer still dumps with **F8** in Campaign. In
Workshop use **Shift+F8**; F8 on its own is reserved for Build.

Run the asset-free presentation and resolve replay tests with:

```sh
cmake -S gtav/tests -B build/graphics-tests
cmake --build build/graphics-tests --config Release
ctest --test-dir build/graphics-tests -C Release --output-on-failure
```

On a Windows Vulkan machine with the installed game, check each output preset at matching desktop
resolutions, an ultrawide or resizable window, and at non-100% Windows DPI. Toggle FSR while
playing, resize, minimize/restore, and compare 720p/1080p/QHD/4K with the F1 output readout. Verify
that settings persist and that F1 and the FPS overlay are sharp. Compare FPS/frame times in the
same scene without changing vsync or frame limit; recheck the reported prop with both a warm and a
cold shader cache. Asset-free shader and build checks do not replace this test.
