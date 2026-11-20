# Desktop native renderer: graphics presets, Windows output and FSR 1

Open **Graphismes** in the desktop mode selector (click the tab, press **F1**, or press **Y** on
an Xbox pad). Values set here are used when you press **Jouer**. In the running game press **F1**:
**Display** shows actual output, game buffer, upscaling and VSync; **Performance** has presets and
CPU/GPU trade-offs; **Memory** shows the renderer's cache and upload usage; **Latency & support**
shows the selected Vulkan device's available features. **Realism** has the optional material
filtering override and experimental contact rays. See [realism options](GRAPHICS_REALISM.md).

| Applies live in F1 | Applies next time the game starts |
| --- | --- |
| FSR 1 and RCAS, VSync (subject to the swap chain), frame limit, FPS counter, texture checks, per-pixel sun shadows, background pipeline compilation, guest GPU delay, contact-ray quality and strength, distant-fog reduction (TU27 strip only) | Output mode, scene scale, material filtering, reduced memory usage, guest frame queue depth, request for Vulkan AMD Anti-Lag |

F1 coalesces disk writes while dragging (e.g. **Contact strength**): the cvar updates live,
the file `data/config/graphics.ini` is written after ~350 ms idle, on tab switch or on close
(F1/Esc/B, panel switch, window close). **Performance** shows the bounded background-pipeline
wait (`--gtav_native_pipeline_wait_ms`, 40 ms/frame) and `pending`/`warmed` pipelines; **Realism**
shows `fog suppressed/protected` for the fog strip.

Restart-required edits are staged without changing the active renderer. F1 reports both the
requested and active output/scale and marks changes waiting for restart. Quit the game, not just
F1, and choose Play again. A VSync request can be refused by the display/driver: the swap-chain
readout still shows the actual mode.

## Choosing a graphics preset

**Graphismes → Préréglage graphique** applies the settings before **Jouer**. **F1 → Performance**
has the same choices in the running native renderer; restart the game for scene, filtering and
memory changes. **Standard** restores the native resolution, normal caches and guest filtering
when **Material filtering** is on **Follow preset**. Presets keep your explicit realism overrides,
window size, FPS limit, shadow/compile options, VSync and driver settings.
A launch override wins; a profile with mismatched settings is shown as custom. Presets work on
Windows, Linux and macOS desktop. Windows-only controls (FSR 1, fixed output modes and Anti-Lag)
stay Windows-only; mobile is unchanged.

| Preset | Selected full-width scene passes | Other changes |
| --- | --- | --- |
| Standard | 100% (1280 × 720 at a 720p guest viewport) | Guest material filtering and normal memory policy; linear presentation |
| Ultra | 150% (1920 × 1080 at the same guest viewport) | Enhanced anisotropy on eligible materials; linear presentation |
| Potato | 50% (640 × 360 at the same guest viewport) | Reduced caches, one GPU slot, 540p with Auto output; linear presentation |
| Balanced | 87.5% | Spatial FSR 1 on Windows when enlarging |
| Performance | 75% | Spatial FSR 1 on Windows when enlarging |

### Ultra and water

Ultra actually rasterizes selected single-sample, 1280-wide passes and keeps their larger native
resolves. This includes the full-size water color/forward pass, so surface shading and edges get
more samples (2.25× the pixels in those passes), not just a larger window or a screen tint. For
eligible repeating, trilinear, block-compressed materials with uploaded mip levels, anisotropy is
raised to 8× where the GPU and the user's cap allow it; higher guest requests are retained. The
option does not change texture assets, the 128×128 wave state, the 320×180/640×360 water pre-pass
aliases, the guest MSAA sample counts, or the depth-bias rules. It is not a new reflection model or
a fix for ocean height drift. The renderer logs it if the adapter's image size limit forces native
scene scale instead. The extra rendering is more expensive than Standard.

### Potato on an integrated GPU

For the lowest load, choose **Potato**, leave **Sortie de la fenêtre / Window output** on **Auto**
(or choose **540p** on Windows), and keep the **30 FPS** game limiter. It uses one GPU slot instead
of two (the guest frame queue is a separate control); upload buffers grow in 16 MiB chunks rather
than 64 MiB. One-off oversized chunks are released after their GPU fence. Ordinary chunks unused
for **90 completed frames** (about three seconds at 30 FPS) are also released after their fence,
so a busy loading screen need not set the upload footprint for the rest of the session. Chunks in
regular use are reused. The default soft cache targets are **192 MiB** of decoded textures and
**64 MiB** of converted vertex/index buffers. Idle assets are eligible for earlier eviction; active
assets, guest allocations, driver memory, render targets and shader caches are not capped by
these targets. It skips shader-module preloading and pipeline warm-up,
uses fewer compile workers, and reuses on-disk shaders and the Vulkan driver cache. New locations
can stutter more. Lower material filtering and mip sampling reduce bandwidth, not the number of
mips stored in a texture. Expert CVar overrides of the separate budgets and sampler caps still win.

**4 GB of RAM with roughly 1 GB shared graphics memory is an experimental play target, not a
verified minimum.** The installer/recompiler is a separate workload; 8 GB or more is still
recommended. An integrated GPU is fine if its driver supports the native Vulkan renderer (Vulkan
1.2 loader/driver, descriptor indexing with sampled-image update-after-bind and buffer device
addresses). There is no software renderer in this preset. The selected adapter and initialization
failures are written to `data/logs/gtav.log` on a managed install. On Windows the runtime still uses
its existing guest backend; Potato does not change platform requirements. Closing other programs
is especially important on shared memory: the same physical RAM backs graphics and the rest of the
system.

**F1 → Memory** shows the live decoded texture and converted buffer sizes against their effective
soft targets, plus reserved upload buffers across the GPU slots. Potato also shows the cumulative
upload space released after fences; this is not extra free system RAM. These are renderer counts,
not measured process RAM or GPU memory. They exclude game data, render targets, shaders, driver
overhead and cache entries already retired but still in flight. A cache can temporarily exceed its
soft target when little is idle enough to evict. The periodic `caches:` line in `data/logs/gtav.log`
records the same counters without opening F1.

To check locally, compare **Standard** and **Ultra** from the same save and camera at the shore at
the same output size, with no frame dump running. F10 captures the front buffer through a 720p
capture target; a text-only F9 can show the actual native resolve sizes. For Potato, try a clean boot,
the prologue and a drive through a busy area with a populated shader cache. Check F1 after several
seconds of steady play following the drive. Also compare process memory, GPU shared/dedicated usage
and frame time with the OS tools, and report the adapter/driver. No retail water A/B or physical
4 GB test is implied by the asset-free fixtures.

### Windows output and upscaling

| Preset | Output pixels |
| --- | --- |
| Auto (default) | 1280 × 720, or 960 × 540 with reduced memory usage |
| 540p | 960 × 540 |
| 720p | 1280 × 720 |
| 1080p | 1920 × 1080 |
| QHD / 1440p | 2560 × 1440 |
| UHD / 4K | 3840 × 2160 |
| Borderless desktop | Current monitor resolution |

A matching monitor uses borderless for 1080p, QHD and 4K; otherwise the window fits inside the
monitor's work area. This does not change Windows's display mode. The game stays 16:9; a wider
or taller window has black bars. F1 shows the actual output and the size of the game buffer.

**Scene scale (restart)** controls the existing internal size of full-width render targets, not the
monitor. Start at **100%** for native detail. Try **87.5%** or **75%** if GPU-bound, keeping
output and FPS limit constant when comparing. Changing this scale doesn't speed up the CPU part of
replay or streaming. 4K output is **not** native 4K rendering or a high-resolution texture pack.

**Upscaling** switches between Linear (default) and **AMD FSR 1** immediately. FSR runs EASU
spatial upscaling on the SDR game buffer including its HUD; the host overlays are drawn separately
at output resolution. Optional **FSR 1 sharpening (RCAS)** can be disabled live. EASU alone can
write directly to a matching 16:9 output without an intermediate pass. Both modes bypass FSR at
1:1 or when downscaling. The intermediate is capped at 2× per axis; with a 720p source on a 4K panel,
FSR processes 1440p and the last blit scales it to 4K. If the optional pipelines/scratch allocations
fail, the output falls back to Linear. FSR 1 itself costs GPU time; not every scene will get faster.

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

Start with **Standard** (Linear, 100% scene scale). Note the mission/location, game
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

## If a character portrait shows corrupt pixels in GTA's pause menu

The native guest-memory uploader had different rules for reading and caching 2D textures.
Updates in the last tile row or packed mip tail could be missed, and some packed images were
read from the wrong offsets. The loader now uses the SDK's 2D subresource layout and covers the
same bytes for decoding, hashing and write watches. This applies to guest-memory texture uploads,
not to textures sampled directly from native render targets. The regression tests reproduce
both corrupt texels and missed updates with synthetic images; the retail pause screen still
needs a visual check.

In Campaign, wait until the world and shaders have loaded, open the GTA pause screen where the
portrait appears, and verify that it shows the current character. Close and reopen the menu, then
check again after switching characters if available. If corruption remains, use this one-off
launch and press **F9** while the bad portrait is visible:

```powershell
.\gtav_launcher.exe --play -- --gtav_mode=campaign --gtav_native_render_scale=1 --gtav_native_fsr1=false --gtav_native_trace_only=true --gtav_native_dump_frames=3
```

Share the portrait's tab/character, GPU/driver, and only the matching `logs/native_trace/frame_*.txt`
files (see the text-only capture above). These traces can link a texture binding to a native resolve
in the same frame; they do not contain or verify the actual pixels. Don't send game data or saves.

## Scope and validation

The front buffer already contains GTA's HUD. Use the text-only F9 trace to look for separate
inputs rather than feeding the final image to a temporal SDK. See
[Temporal rendering: FSR 4, XeSS 2 and DLSS 5](TEMPORAL_UPSCALING.md) for the retail capture
procedure, trace fields, SDK status and requirements before enabling any provider.

The optional `--gtav_native_replay=true` buffer still dumps with **F8** in Campaign. In
Workshop use **Shift+F8**; F8 on its own is reserved for Build.

Run the asset-free presentation, resolve replay and graphics preference tests with:

```sh
cmake -S gtav/tests -B build/graphics-tests
cmake --build build/graphics-tests --config Release
ctest --test-dir build/graphics-tests -C Release --output-on-failure
```

Adding `-DGTAV_FMT_SOURCE=<patched SDK>/thirdparty/fmt` also tests the F9 trace output.
Adding `-DGTAV_TEXTURE_SDK_SOURCE=<patched SDK>` tests texture uploads and change detection with
synthetic guest memory (use `-DGTAV_TEXTURE_PLUME_SOURCE=<patched Plume>` if Plume isn't under
`thirdparty/plume`). Adding `-DGTAV_IMGUI_SOURCE=<patched SDK>/thirdparty/imgui` tests the real F1 widgets. With
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
