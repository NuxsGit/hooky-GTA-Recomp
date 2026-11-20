// gtav - native renderer backend: replays a recorded guest frame through Plume.
//
// Render-thread only. Owns the bindless descriptor heaps, the pipeline layout the XenosRecomp
// shaders expect (texture heaps in sets 0-2, samplers in set 3, three buffer addresses as push
// constants), and the caches keyed by guest state: shaders, pipelines, render targets (by EDRAM
// base/format/pitch), resolve targets (by guest address), textures, samplers and vertex/index
// buffers.

#pragma once

#include "presentation.h"
#include "graphics_policy.h"

#include <plume_render_interface.h>

#include <cstdint>
#include <vector>
#include <memory>
#include <string>

struct ImDrawData;

namespace gtav::native {

struct FramePacket;

struct FrontBuffer {
  uint32_t descriptor = 0;
  Extent size;
  explicit operator bool() const { return !size.Empty(); }
};

// Tree-orb diagnostic (backend.cpp DiagFor): F11 cycles the mode, logs/diag_mode sets it.
void CycleDiagMode();
void SetDiagMode(uint32_t mode);

class Backend {
 public:
  Backend();
  ~Backend();
  bool Init(plume::RenderDevice* device, const GraphicsMemoryPolicy& memory);
  float RenderScale() const;
  // <name> (e.g. "ps_<HASH>.spv") was in the SPIR-V folder read at start-up (no disk access).
  static bool SpirvListed(const std::string& name);

  // Records the frame into `list`. Returns the shader-readable front buffer's descriptor and
  // physical extent, or an empty FrontBuffer when the frame has none yet.
  FrontBuffer Execute(const FramePacket& frame, plume::RenderCommandList* list);

  // Frames in flight: up to two slots; lean mode always uses slot 0. Everything a frame
  // leaves for after its fence (uploads, deferred frees, resolve writebacks) is kept per
  // slot: BeginFrame(slot) before Execute, EndFrame(slot) once the command list is recorded, and
  // FrameComplete(slot) after that slot's fence passed (and before its next BeginFrame).
  static constexpr uint32_t kFramesInFlight = kMaxFrameSlots;
  void BeginFrame(uint32_t slot);
  void EndFrame(uint32_t slot);
  void FrameComplete(uint32_t slot);

  // A frame dump is being recorded (the renderer waits for that frame synchronously).
  bool Dumping() const;
  // Dump the next executed frame (F9 in the native window), like --gtav_native_dump_frame.
  void RequestDump();

  // Counters of the last executed frame, for the hitch log.
  struct FrameStats {
    uint32_t draws, skipped, pipelines_created, textures;
    uint32_t rewritten;  // draws whose guest data changed since the D3D call, drawn without a copy
    double shader_ms, pipeline_ms, texture_ms, resolve_ms;  // time in cache misses / resolves
    double pipeline_wait_ms;
  };
  FrameStats LastStats() const;
  PipelineInfo PipelineStatus() const;
  // Tracked cache and upload sizes across the recording state and parked GPU slots.
  GraphicsMemoryStats MemoryStats() const;
  RealismInfo Realism() const;

  // One line per draw of the last executed frame, for the glitch inspector (--gtav_native_flicker_catch).
  struct DrawNote {
    uint64_t vs, ps;
    uint32_t rt0, depth, vb, vb_size, stride, ib, start, count, prim;
    int32_t base_vertex;
    uint32_t tex[4];  // base addresses of the first four bound textures
    uint8_t copied, rewritten, skipped, uploads;  // data from a copy / changed since the D3D call / texture uploads
    float vsc[48];  // vertex shader constants c0-c11 (only those the shader reads are captured)
  };
  const std::vector<DrawNote>& LastNotes() const;

  // Frame dump (--gtav_native_dump_frame=N): every resolve of guest frame N is captured right
  // after it executes; WriteDump saves them as PNGs once the frame's fence passed.
  void WriteDump();

  // Draws `descriptor` (a 2D texture in the heap) over the bound framebuffer.
  void Blit(plume::RenderCommandList* list, uint32_t descriptor, plume::RenderFormat target_format);

#ifdef _WIN32
  // Presents the game with aspect correction and optional FSR. Leaves the output framebuffer
  // bound with a full-size viewport/scissor so that the host overlays stay unscaled.
  PresentationInfo PresentGame(plume::RenderCommandList* list, FrontBuffer front,
                               const plume::RenderFramebuffer* output, Extent extent,
                               plume::RenderFormat format, bool fsr1, bool sharpen = true);
#endif

  // Draws `value` as an on-screen counter with its top-left corner at (x, y).
  void DrawCounter(plume::RenderCommandList* list, uint32_t value, int32_t x, int32_t y, uint32_t scale,
                   plume::RenderFormat target_format);

  // The settings menu's ImGui draw lists over the bound framebuffer (menu.cpp).
  void DrawMenu(plume::RenderCommandList* list, const ImDrawData* data, plume::RenderFormat target_format);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace gtav::native
