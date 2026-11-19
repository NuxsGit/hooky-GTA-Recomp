// F9 text-only capture: a command timeline, not a framebuffer dump. It reads only the FramePacket
// and recording outcomes; it neither changes the replay path nor touches GPU/guest pixel data.
#pragma once

#include "frame.h"
#include "paths.h"
#include "textures.h"
#include "xenos_regs.h"

#include <fmt/format.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace gtav::native {

class FrameTrace {
 public:
  FrameTrace(const FramePacket& frame, float scale, bool resolve_trial) : f_(frame) {
    Log("trace=1 guest_frame={} swap={:08X} scene_scale={} resolve_trial={}\n"
        "# In replay order. recorded means CPU command recording, not GPU verification.\n"
        "# bound2d lists guest fetch slots, not proof the shader reads them. fmt is the guest format.\n",
        f_.index, f_.front_buffer, scale, resolve_trial);
  }

  void Draw(const DrawCommand& d, bool recorded) {
    const uint8_t* r = f_.At(d.regs);
    const auto surface = xenos_regs::Surface(r);
    Log("{} draw recorded={} vs={:016X} ps={:016X} ucode_ps={:016X} prim={} count={} pitch={} msaa={} "
        "depth={:08X}/{:08X} dc={:08X} sref={:08X} mask={:04X} mode={} vp=({},{},{},{}) rt=",
        sequence_++, recorded, d.vs_hash, d.ps_hash, d.ps_ucode_hash, d.primitive, d.count, surface.pitch, surface.msaa,
        d.depth_stencil, xenos_regs::Read(r, xenos_regs::RB_DEPTH_INFO), xenos_regs::Read(r, xenos_regs::RB_DEPTHCONTROL),
        xenos_regs::Read(r, xenos_regs::RB_STENCILREFMASK), xenos_regs::Read(r, xenos_regs::RB_COLOR_MASK),
        xenos_regs::Read(r, xenos_regs::RB_MODECONTROL) & 7, xenos_regs::ReadFloat(r, xenos_regs::PA_CL_VPORT_XOFFSET),
        xenos_regs::ReadFloat(r, xenos_regs::PA_CL_VPORT_YOFFSET), xenos_regs::ReadFloat(r, xenos_regs::PA_CL_VPORT_XSCALE),
        xenos_regs::ReadFloat(r, xenos_regs::PA_CL_VPORT_YSCALE));
    for (int slot = 0; slot < 4; ++slot) {
      const auto t = xenos_regs::Color(r, slot);
      Log("{}{:08X}/{:03X}/{}", slot ? "," : "", d.render_targets[slot], t.base, uint32_t(t.format));
    }
    Log(" bound2d=");
    const uint8_t* fetch = f_.At(d.fetch);
    for (uint32_t slot = 0; slot < kFetchSize / 24; ++slot, fetch += 24) {
      if ((Word(fetch) & 3) != 2 || ((Word(fetch + 20) >> 9) & 3) != 1) continue;
      const uint32_t w1 = Word(fetch + 4), w2 = Word(fetch + 8), address = w1 & 0x1FFFF000;
      if (address)
        Log(" {}:{:08X}/f{}/{}x{}", slot, address, w1 & 0x3F, (w2 & 0x1FFF) + 1, ((w2 >> 13) & 0x1FFF) + 1);
    }
    Log("\n");
  }

  void Resolve(const ResolveCommand& c, ResolveReplay replay) {
    const uint32_t* w = c.dest_words + 7;
    const uint8_t* r = f_.At(c.regs);
    const auto ops = DecodeResolve(c.flags);
    const auto t = xenos_regs::Color(r, ops.copy_depth ? 0 : int(ops.source));
    const auto& p = c.probe.packets;
    Log("{} resolve recorded={} cleared_depth={} flags={:08X} source={} src_base={:03X} dst={:08X} fmt={} "
        "size={}x{} mip={} face={} swap_texture={} rect={}:{},{}-{},{} point={}:{},{} z_arg={} "
        "applied_z={} applied_stencil={} pm4={} control={} depth_clear={} pm4_surface={} pm4_depth={} surface={:08X}->{:08X} "
        "depth={:08X}->{:08X} same_depth_surface={}\n",
        sequence_++, replay.copied, replay.cleared_depth, c.flags, ops.source,
        ops.copy_depth ? xenos_regs::Depth(r).base : t.base, FetchBaseAddress(w) & 0x1FFFFFFF, w[1] & 0x3F,
        (w[2] & 0x1FFF) + 1, ((w[2] >> 13) & 0x1FFF) + 1, c.dest_level, c.dest_face,
        c.dest_texture != 0 && c.dest_texture == f_.front_buffer, c.has_rect, c.source_rect[0], c.source_rect[1],
        c.source_rect[2], c.source_rect[3], c.has_point, c.dest_point[0], c.dest_point[1], c.clear_z,
        replay.applied_z, replay.applied_stencil, ResolveScanName(p.scan),
        Hex(p.scan == ResolveScan::kDirectCopy ? std::optional{p.control} : std::nullopt),
        Hex(p.depth_clear), Hex(p.surface), Hex(p.depth), xenos_regs::Read(r, xenos_regs::RB_SURFACE_INFO),
        c.probe.surface_after, xenos_regs::Read(r, xenos_regs::RB_DEPTH_INFO), c.probe.depth_after,
        c.probe.same_depth_surface);
  }

  void Clear(const ClearCommand& c) {
    const uint8_t* r = f_.At(c.regs);
    Log("{} clear flags={:08X} z={} stencil={} surface={:08X} depth={:08X} rt0={:03X} rects={}", sequence_++, c.flags,
        c.z, c.stencil, xenos_regs::Read(r, xenos_regs::RB_SURFACE_INFO), xenos_regs::Read(r, xenos_regs::RB_DEPTH_INFO),
        xenos_regs::Color(r, 0).base, c.rect_count);
    for (uint32_t i = 0; i < c.rect_count; ++i)
      Log(" {},{}-{},{}", c.rects[i][0], c.rects[i][1], c.rects[i][2], c.rects[i][3]);
    Log("\n");
  }

  void Query(const QueryCommand& q) {
    Log("{} query {} {:08X} serial={}\n", sequence_++, q.begin ? "begin" : "end", q.query, q.serial);
  }

  bool Write() const {
    const auto dir = ExeDir() / "logs" / "native_trace";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    std::ofstream file(dir / fmt::format("frame_{}.txt", f_.index), std::ios::binary);
    file.write(text_.data(), std::streamsize(text_.size()));
    file.close();
    return bool(file);
  }

 private:
  static uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  }
  static std::string Hex(std::optional<uint32_t> word) {
    return word ? fmt::format("{:08X}", *word) : "-";
  }
  template <typename... Args>
  void Log(fmt::format_string<Args...> format, Args&&... args) {
    fmt::format_to(std::back_inserter(text_), format, std::forward<Args>(args)...);
  }

  const FramePacket& f_;
  uint32_t sequence_ = 0;
  std::string text_;
};

}  // namespace gtav::native
