# Windows native renderer: output, FSR 1 and occlusion

Open **Graphismes** in the desktop mode selector (click the tab, press **F1**, or press **Y** on
an Xbox pad). Values set here are used when you press **Jouer**. In the running game press **F1**:
**Display** shows actual output, game buffer, upscaling and VSync; **Performance** has presets and
CPU/GPU trade-offs; **Latency & support** shows the selected Vulkan device's available features.

| Applies live in F1 | Applies next time the game starts |
| --- | --- |
| FSR 1 and RCAS, VSync (subject to the swap chain), frame limit, FPS counter, texture checks, per-pixel sun shadows, background pipeline compilation, guest GPU delay | Output mode, scene scale, guest frame queue depth, request for Vulkan AMD Anti-Lag |

Restart-required edits are staged without changing the active renderer. F1 reports both the
requested and active output/scale and marks changes waiting for restart. Quit the game, not just
F1, and choose Play again. A VSync request can be refused by the display/driver: the swap-chain
readout still shows the actual mode.

### Output and upscaling

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

**Upscaling** switches between Linear (default) and **AMD FSR 1** immediately. FSR runs EASU
spatial upscaling on the SDR game buffer including its HUD; the host overlays are drawn separately
at output resolution. Optional **FSR 1 sharpening (RCAS)** can be disabled live. EASU alone can
write directly to a matching 16:9 output without an intermediate pass. Both modes bypass FSR at
1:1 or when downscaling. The intermediate is capped at 2× per axis; with a 720p source on a 4K panel,
FSR processes 1440p and the last blit scales it to 4K. If the optional pipelines/scratch allocations
fail, the output falls back to Linear. FSR 1 itself costs GPU time; not every scene will get faster.

On **Performance**, Best image selects 100% and Linear. Balanced selects 87.5% and FSR 1;
Performance selects 75% and FSR 1. The last two compile new pipelines in the background (reduced
first-use hitches, but some effects can be missing briefly). All keep accurate sun shadows, and
none changes your FPS cap, queue, VSync or driver pacing. Compare the same scene at the same cap
and output resolution after restarting. In CPU-bound scenes, lowering scene scale often helps less
than changing texture checks or reusing the pipeline cache.

### Latency and compatibility

- **Guest frame queue / Short:** at most one captured guest frame waits to be replayed instead of
  two. No snapshots are discarded; this can reduce buffering but may lower throughput. It does
  not change swap-chain image count or interpolate frames.
- **AMD Anti-Lag (experimental, off by default):** the game checks its *selected* Vulkan physical
  device for `VK_AMD_anti_lag` and the `antiLag` feature, enables them only on request, and retries
  normal device creation if the optional request fails. It calls `vkAntiLagUpdateAMD` before the
  first gameplay state poll per guest frame, not in the hub, an open host overlay, shadow mode or
  a hidden process. This is standard driver input pacing (no paired INPUT/PRESENT markers and no
  additional driver FPS limiter). F1 shows unsupported, disabled, fallback, waiting for input or
  pacing. The status is not an input-to-display latency measurement. If several GPUs are present,
  verify the adapter name in F1; `--gtav_native_device=AMD` can prefer an AMD-named adapter if listed.
- **Guest GPU delay** is a separate emulation compatibility setting, not Anti-Lag. Use the
  conservative 6 ms if speed-focused 4 ms reveals streaming artefacts.
- **Temporal FSR, DLSS, XeSS, and frame generation ×2–×8:** not integrated. No such feature can
  be enabled by selecting FSR 1 or by duplicating presentations.

### Where the settings live

For a launcher installation the hub and F1 both write `<game home>/data/config/graphics.ini`,
shared by Campaign and Workshop without touching either profile. When that file is absent, the
legacy `gtav_settings.ini` next to the host is read; the first successful edit writes the new file
and leaves the legacy one alone. Developers launching the host directly without
`--gtav_settings_path` still use the legacy location. Save uses a checked atomic replacement; a
write failure is reported and can be retried. An unreadable file is not silently overwritten.

On desktop the order is **explicit command line → `REX_*` environment → saved graphics settings
→ `gtav.toml` → defaults**. The launcher no longer passes its FPS, native VSync or guest-lag defaults
as explicit overrides. Forced fields are marked and an unrelated edit will not save those forced
values. Developer runs via `play.sh` or `play.ps1` still pass their explicit FPS limit and, in
native-on mode, VSync and guest GPU delay. Use the installer launch to test persisted values.
For one-off A/B, use `GTAV_IGNORE_SETTINGS=1` to bypass both reading and writing the file:

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

The front buffer already contains GTA's HUD. Use the text-only F9 trace to look for separate
inputs rather than feeding the final image to a temporal SDK.

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

Run the asset-free presentation, resolve replay and graphics preference tests with:

```sh
cmake -S gtav/tests -B build/graphics-tests
cmake --build build/graphics-tests --config Release
ctest --test-dir build/graphics-tests -C Release --output-on-failure
```

Adding `-DGTAV_IMGUI_SOURCE=<patched SDK>/thirdparty/imgui` tests the real F1 widgets. With
`-DGTAV_TEST_PLAY_HUB=ON -DSDL3_DIR=<SDL3 package dir>`, the real SDL hub is tested with injected
SDL events. This checks editing and reloading on Windows CI without physical input devices.
The extracted Windows ZIP smoke also reads legacy and current preferences in an accented install
path, checks a CLI override and keeps Campaign/Workshop paths separate. None of these checks
launches retail GTA.

On a Windows Vulkan machine with the installed game, check each output preset at matching desktop
resolutions, an ultrawide or resizable window, and at non-100% Windows DPI. Toggle FSR while
playing, resize, minimize/restore, and compare 720p/1080p/QHD/4K with the F1 output readout. Verify
that settings persist and that F1 and the FPS overlay are sharp. Compare FPS/frame times in the
same scene without changing vsync or frame limit; recheck the reported prop with both a warm and a
cold shader cache. Asset-free shader and build checks do not replace this test.
