// F9 text-only capture: a command timeline, not a framebuffer dump. It reads only the FramePacket
// and recording outcomes; it neither changes the replay path nor touches GPU/guest pixel data.
#pragma once

#include "frame.h"
#include "paths.h"
#include "presentation.h"
#include "resolve_policy.h"
#include "xenos_regs.h"

#include <fmt/format.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace gtav::native {

// Host metadata from the actual resolve path, not inferred from the guest texture header.
struct ResolveTraceHost {
  ResolveRegion copy;
  Extent extent;  // physical size of the selected subresource, not just the part written
  uint32_t src_format = 0, dst_format = 0, src_msaa = 0;
  uint32_t descriptor = 0;  // direct 2D binding only; zero when not attributable
  std::string_view route;
};

class FrameTrace {
 public:
  FrameTrace(const FramePacket& frame, float scale, bool resolve_trial) : f_(frame) {
    Log("trace=2 guest_frame={} swap={:08X} scene_scale={} resolve_trial={}\n"
        "# In replay order. recorded means CPU command recording, not GPU verification.\n"
        "# bound2d is guest state. resolved2d links direct host bindings to resolve command numbers in this frame.\n"
        "# Neither proves shader sampling. fmt is the guest format; host_fmt and host_src_fmt are Plume formats.\n",
        f_.index, f_.front_buffer, scale, resolve_trial);
  }

  // The first 32 shared constants are the direct 2D descriptors actually supplied to the draw.
  // A skipped draw has no host bindings, even when the guest's fetch constants are set.
  void Draw(const DrawCommand& d, const uint32_t* descriptors2d = nullptr) {
    const bool recorded = descriptors2d != nullptr;
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
      if (((Word(fetch + 20) >> 9) & 3) != 1) continue;
      const uint32_t w1 = Word(fetch + 4), w2 = Word(fetch + 8), address = TextureAddress(Word(fetch), w1);
      if (address)
        Log(" {}:{:08X}/f{}/{}x{}", slot, address, w1 & 0x3F, (w2 & 0x1FFF) + 1, ((w2 >> 13) & 0x1FFF) + 1);
    }
    Log(" resolved2d=");
    if (descriptors2d) {
      for (uint32_t slot = 0; slot < 32; ++slot)
        if (auto writer = LastWriter(descriptors2d[slot])) Log(" {}:{}", slot, *writer);
    }
    Log("\n");
  }

  void Resolve(const ResolveCommand& c, ResolveReplay replay, const ResolveTraceHost& host) {
    const uint32_t* w = c.dest_words + 7;
    const uint8_t* r = f_.At(c.regs);
    const auto ops = DecodeResolve(c.flags);
    const auto t = xenos_regs::Color(r, ops.copy_depth ? 0 : int(ops.source));
    const auto& p = c.probe.packets;
    const uint32_t sequence = sequence_++;
    if (replay.copied && host.descriptor) last_writer_[host.descriptor] = sequence;
    Log("{} resolve recorded={} cleared_depth={} flags={:08X} source={} src_base={:03X} dst={:08X} fmt={} "
        "size={}x{} mip={} face={} swap_texture={} rect={}:{},{}-{},{} point={}:{},{} z_arg={} "
        "applied_z={} applied_stencil={} pm4={} control={} depth_clear={} pm4_surface={} pm4_depth={} surface={:08X}->{:08X} "
        "depth={:08X}->{:08X} same_depth_surface={} kind={} route={}",
        sequence, replay.copied, replay.cleared_depth, c.flags, ops.source,
        ops.copy_depth ? xenos_regs::Depth(r).base : t.base, TextureAddress(w[0], w[1]), w[1] & 0x3F,
        (w[2] & 0x1FFF) + 1, ((w[2] >> 13) & 0x1FFF) + 1, c.dest_level, c.dest_face,
        c.dest_texture != 0 && c.dest_texture == f_.front_buffer, c.has_rect, c.source_rect[0], c.source_rect[1],
        c.source_rect[2], c.source_rect[3], c.has_point, c.dest_point[0], c.dest_point[1], c.clear_z,
        replay.applied_z, replay.applied_stencil, ResolveScanName(p.scan),
        Hex(p.scan == ResolveScan::kDirectCopy ? std::optional{p.control} : std::nullopt),
        Hex(p.depth_clear), Hex(p.surface), Hex(p.depth), xenos_regs::Read(r, xenos_regs::RB_SURFACE_INFO),
        c.probe.surface_after, xenos_regs::Read(r, xenos_regs::RB_DEPTH_INFO), c.probe.depth_after,
        c.probe.same_depth_surface, ops.copy_depth ? "depth" : "color", replay.copied ? host.route : "skipped");
    if (!host.extent.Empty()) {
      const auto& b = host.copy;
      Log(" host_fmt={} host_src_fmt={} host_src_msaa={} host_size={}x{} host_rect={},{}-{},{} host_point={},{}",
          host.dst_format, host.src_format, host.src_msaa, host.extent.width, host.extent.height, b.left, b.top,
          b.right, b.bottom, b.dest_x, b.dest_y);
    }
    Log("\n");
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

  void ContactRays(std::string_view status) { Log("# contact_rays={} (recording only)\n", status); }
  void Fog(FogInfo info) {
    Log("# fog={} suppressed={} protected={} (recording only)\n", info.reduced ? "reduced" : "original",
        info.suppressed, info.protected_draws);
  }

  // This identifies the result of Execute, not a successful swap-chain present. When a previous
  // frame is held, the writer below still describes the *current* swap texture, not the held image.
  void Front(bool pipeline_draw_loss, bool held, uint32_t address = 0, uint32_t descriptor = 0,
             uint32_t format = 0, Extent size = {}) {
    const auto writer = LastWriter(descriptor);
    Log("front status={} pipeline_draw_loss={} dst={:08X} host_fmt={} host_size={}x{} current_writer={}\n",
        size.Empty() ? "missing" : held ? "held_previous" : "current", pipeline_draw_loss, address, format,
        size.width, size.height, writer ? std::to_string(*writer) : "-");
  }

  std::string_view Text() const { return text_; }

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
  std::optional<uint32_t> LastWriter(uint32_t descriptor) const {
    const auto it = last_writer_.find(descriptor);
    return it == last_writer_.end() ? std::nullopt : std::optional{it->second};
  }

  // Same packed Xenos fetch address as the replay (ignore the upper physical alias bits).
  static constexpr uint32_t TextureAddress(uint32_t w0, uint32_t w1) {
    return (w0 & 3) == 2 ? w1 & 0x1FFFF000 : 0;
  }

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
  // Indices are not recycled until a fence is retired, outside this frame's recording.
  std::unordered_map<uint32_t, uint32_t> last_writer_;
  std::string text_;
};

}  // namespace gtav::native
