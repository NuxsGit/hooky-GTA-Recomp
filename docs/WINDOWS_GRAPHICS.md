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

To reproduce, try the same camera angle with **Linear**, **100%** scene scale and **Best image**.
Note the mission/location, game mode (Campaign or Workshop) and whether the overlap persists when
standing still. **F9** dumps the next frame; `logs/native_dump/frame_*/commands.txt` has attachment,
depth and shader state. **F10** captures the unfiltered front buffer. The renderer diagnostics are
relative to the game executable; the Windows launcher's main log is `data/logs/gtav.log`.
If the overlap is intermittent while streaming, compare with `--gtav_native_fence_wait=true` on a
fresh launch; the more conservative fence policy can also reduce throughput. Share the log and
repro steps, not game images, discs or saves.

## Scope and validation

FSR 1 is spatial. DLSS and frame generation are not exposed by these settings. Before integrating
a temporal provider, the renderer needs validated pre-HUD color, depth, motion vectors, camera
jitter and reset/timing information from the game, plus a supported SDK/device. Extra presentations
of the same image are not frame generation.

Run the asset-free output tests with:

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
