# Temporal rendering on Windows/Vulkan: TU27 integration prerequisites

**Not enabled:** FSR 4, XeSS 2 and DLSS 5. The Windows build's FSR 1 is a spatial filter on the
finished Xbox front buffer (including GTA's HUD). Loading a DLL, or reporting a feature enum,
would not make this buffer a valid temporal input.

## SDKs checked

| Requested feature | Verified API/SDK evidence | Before enabling in GTA |
| --- | --- | --- |
| **AMD FSR 4** (upscaling) | [AMD FSR SDK 2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.4.0) contains **FSR Upscaling 4.1.2** for Vulkan. [Input and jitter requirements](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/docs/techniques/super-resolution-ml.md). | Use the signed `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll`. Query `ffxQueryDescGetVersions` for the actual Vulkan device; the loader can also provide FSR 3.x, which is not FSR 4. Check the shipping and driver-provided implementations and their device requirements before reporting an active version. |
| **Intel XeSS 2** (Super Resolution) | [XeSS SDK 2.1.1](https://github.com/intel/xess/releases/tag/v2.1.1) has [public Vulkan SR API and device setup](https://github.com/intel/xess/blob/v2.1.1/doc/xess_sr_developer_guide_english.md). | Query the SDK for required **instance** extensions before `vkCreateInstance` and required **device** extensions/features for the selected physical device before `vkCreateDevice`. The 2.1.1 guide includes `shaderStorageImageWriteWithoutFormat` and device-dependent feature chains. A vendor name alone is not a capability check. |
| **NVIDIA DLSS 5** (3D-guided neural rendering) | [NVIDIA describes an independent lighting/material pass](https://nvidianews.nvidia.com/news/nvidia-dlss-5-delivers-ai-powered-breakthrough-in-visual-fidelity-for-games). [Streamline 2.14.16](https://github.com/NVIDIA-RTX/Streamline/tree/v2.14.16) declares `kFeatureDLSS_NR` and uplift buffers; its changelog mentions `sl.dlss_nr`. The public feature guides checked do not document a standalone Vulkan NR integration. | Require the official NR plugin/API contract and confirmed Vulkan support for this host. Neither `kFeatureDLSS`, the public NGX Super Sampling APIs, nor Plume's optional DLSS code is DLSS 5. Do not use DLLs copied from another game or rename DLSS Super Resolution. |

Use official SDK packages only. AMD and Intel runtime redistribution has its own conditions and
notices ([AMD](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/docs/license.md),
[Intel](https://github.com/intel/xess/blob/v2.1.1/LICENSE.txt)). No vendor runtime or headers have
been added for these providers. XeSS-FG/XeLL, AMD frame generation/Anti-Lag and NVIDIA frame
generation/Reflex are separate features; none follows from a Super Resolution API being present.

## Capture from a retail installation

In PowerShell, from the extracted Windows package, launch Campaign at 100% scene scale and with
no spatial filter. This command also works on packages with the original text-only F9 trace:

```powershell
.\gtav_launcher.exe --play -- --gtav_mode=campaign --gtav_native_trace_only=true --gtav_native_dump_frames=3 --gtav_native_render_scale=1 --gtav_native_fsr1=false
```

At a repeatable outdoor spot, wait for the scene to load. Press **F9 once per case**:

1. Character and camera stationary, HUD and radar visible.
2. Same shot with HUD and radar turned off in GTA's settings (close the menu before capturing).
3. With HUD still off, pan the camera while the character remains still.
4. With the camera still, let a vehicle or other actor cross the view.

Send only the corresponding `logs/native_trace/frame_*.txt` files (relative to the **game
executable**) with the case labels, game location and GPU/driver. Three consecutive guest
frames are requested per keypress; check the frame numbers. Archive those files before another
launch can overwrite matching filenames. No saves, game files, screenshots, or `native_dump`
folder are needed for this first step.

### What the trace proves (and what it doesn't)

- `recorded=true` means a command was recorded, not that its GPU work completed or an image was
  displayed. Skip samples with pipeline draw loss or a held/missing front for motion validation.
- Trace v2 adds `kind`, `route`, and `host_*` for recorded resolves. `host_fmt` is the Plume
  destination format, not Xbox `fmt`; `host_rect` and `host_point` give the actual clipped/scaled
  copy in physical texels. `host_size` is the size of the chosen destination subresource, not
  necessarily the size of the copy. Adjacent tile resolves may build just one image.
- On a recorded draw, `resolved2d= 2:154` means slot 2 was bound to a direct 2D descriptor last
  written by command 154 in **this** frame. It is not a shader-use analysis. Bindings from
  previous frames, mip chains/cube faces and derived views aren't attributed to a writer. The
  older v1 traces have only `bound2d`: treat matching guest addresses as leads, not proof, as
  memory aliases and unused bindings are common.
- The v2 `front` line reports the render result returned by `Backend::Execute`, not a successful
  swap-chain present. `current_writer` is the last attributed resolve into the current swap
  texture even if `status=held_previous`. No GPU readback or new waits are injected by the trace.

## Gate for real dispatch

Start with the last verified scene writer before GTA's HUD, which can be a draw into the same
render target, not just a resolve. Compare both HUD captures across shader IDs and destinations;
follow the candidate depth and motion writers/bindings in the moving cases. Use shader
instructions and **local** raw texels to verify the meaning of candidates before selecting them;
a two-channel texture could just as easily be shadows or water simulation. The full F9 diagnostic
without `gtav_native_trace_only` can provide pixels at 100% scene scale (~350 MB per frame); it
has different readback overhead and is not an FPS test.

- Prove scene color/depth alignment, depth range/convention and exposure/color space. Identify
  the position relative to postprocessing; keep the guest and host HUD out of history.
- Prove screen-space **camera and object** vectors, sign/units, handling of disocclusion and
  jitter. Find where subpixel projection jitter can actually be applied before replay/resolve;
  setting a jitter parameter alone does not jitter a rendered image.
- Identify frame timing, resets (camera cuts, missing/held frames, resize, pause/resume) and the
  output-size HUD path. Do not feed duplicate presentations or incomplete frames as new scene
  history. Validate on real hardware under motion before offering the feature in the menu.
- Before initializing any Vulkan provider, merge only its verified requirements with Plume's
  existing instance/device feature paths (including optional Anti-Lag). Keep a working Linear
  or FSR 1 path when support, resources or dispatch fail. Compare **rendered** frame times and
  image quality in the same scene; neural rendering and frame generation need separate tests.

## Frame generation cadence prototype

`gtav/src/native/frame_generation_schedule.h` plans 1, 3, 5 or 7 intermediate
presentation slots for x2, x4, x6 or x8. It rejects discontinuous or ineligible
source pairs and intervals that cannot sustain the requested presentation rate.
This is a tested CPU scheduling primitive only. No interpolation provider,
validated TU27 motion inputs, or swap-chain integration is enabled by it. The
launcher must continue to report frame generation as unavailable until those
pieces can generate and present real intermediate images on supported hardware.

### Offline x2 experiment

For a local constant-rate gameplay recording, run:

```powershell
python tools/native/framegen_x2.py capture.mp4 capture-x2.mp4 --fps 30
```

Requires FFmpeg with `minterpolate` and `libx264`. The experiment estimates motion
between successive frames and synthesizes a midpoint, with FFmpeg's scene-change
fallback. It produces a silent comparison clip, not extra real-time Vulkan
presentations. Because its input is the finished recording, the HUD is part of
interpolation and can distort; use a HUD-free capture for motion evaluation.
