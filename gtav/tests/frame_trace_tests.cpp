#include "frame_trace.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace gtav::native;

static void Contains(std::string_view line, std::string_view part) {
  if (line.find(part) == line.npos) {
    std::cerr << "Missing '" << part << "' in '" << line << "'\n";
    std::exit(1);
  }
}

static std::string_view Line(std::string_view text, std::string_view prefix) {
  for (size_t start = 0; start < text.size();) {
    const auto end = text.find('\n', start);
    const auto line = text.substr(start, end == text.npos ? end : end - start);
    if (line.starts_with(prefix)) return line;
    if (end == text.npos) break;
    start = end + 1;
  }
  std::cerr << "Missing trace line with prefix '" << prefix << "'\n";
  std::exit(1);
}

int main() {
  FramePacket frame;
  frame.index = 321;
  frame.front_buffer = 0x1234;

  ResolveCommand resolve;
  resolve.dest_texture = frame.front_buffer;
  resolve.flags = 4;  // Depth resolve, including when its guest format aliases a color view.
  resolve.dest_words[7] = 2;
  resolve.dest_words[8] = 0xE0345016;
  resolve.dest_words[9] = 1279 | (719 << 13);
  std::array<uint8_t, kRegShadowSize> regs{};
  xenos_regs::Write(regs.data(), xenos_regs::RB_DEPTH_INFO, 0x19);
  resolve.regs = frame.Push(regs.data(), regs.size());

  DrawCommand draw;
  draw.regs = resolve.regs;
  draw.ps_hash = 0xABCD;
  std::array<uint8_t, kFetchSize> fetch{};
  auto store = [&](uint32_t slot, uint32_t word, uint32_t v) {
    uint8_t* b = fetch.data() + slot * 24 + word * 4;
    b[0] = uint8_t(v >> 24), b[1] = uint8_t(v >> 16), b[2] = uint8_t(v >> 8), b[3] = uint8_t(v);
  };
  // Two slots with the same guest address, only one actually binds descriptor 16. This can
  // happen when the backend chooses a different view or the guest overwrites a prior resolve.
  for (const auto slot : {1u, 2u}) {
    store(slot, 0, 2);
    store(slot, 1, resolve.dest_words[8]);
    store(slot, 2, resolve.dest_words[9]);
    store(slot, 5, 1 << 9);
  }
  draw.fetch = frame.Push(fetch.data(), fetch.size());
  std::array<uint32_t, 32> descriptors{};
  descriptors[2] = 16;
  descriptors[1] = 18;

  FrameTrace trace(frame, 0.75f, false);
  const ResolveTraceHost host{{16, 270, 976, 540, 0, 270}, {960, 540}, 40, 34, 0, 16, "shader"};
  trace.Resolve(resolve, {true}, host);  // Command 0 must not be confused with "unknown".
  trace.Draw(draw, descriptors.data());
  trace.Resolve(resolve, {}, host);      // Skipped: must not replace the old writer.
  trace.Draw(draw, descriptors.data());
  trace.Draw(draw);                      // Skipped draw: guest bindings are still visible.
  trace.Resolve(resolve, {}, {});        // Unsupported copy with no host geometry.
  trace.Resolve(resolve, {true}, host);  // Same descriptor, later partial write.
  trace.Draw(draw, descriptors.data());
  trace.ContactRays("no_aligned_depth");
  trace.Front(true, true, 0x00345000, 16, 34, {960, 540});

  auto text = trace.Text();
  Contains(text, "# contact_rays=no_aligned_depth (recording only)\n");
  Contains(text, "trace=2 guest_frame=321 swap=00001234 scene_scale=0.75 resolve_trial=false\n");
  const auto r0 = Line(text, "0 resolve ");
  Contains(r0, "recorded=true");
  Contains(r0, "src_base=019 dst=00345000 fmt=22 size=1280x720 mip=0 face=0 swap_texture=true");
  Contains(r0, "kind=depth route=shader host_fmt=34 host_src_fmt=40 host_src_msaa=0 host_size=960x540 "
               "host_rect=16,270-976,540 host_point=0,270");
  Contains(Line(text, "1 draw "), "bound2d= 1:00345000/f22/1280x720 2:00345000/f22/1280x720 resolved2d= 2:0");
  Contains(Line(text, "2 resolve "), "kind=depth route=skipped");
  Contains(Line(text, "3 draw "), "resolved2d= 2:0");
  Contains(Line(text, "4 draw "), "recorded=false");
  if (!Line(text, "4 draw ").ends_with(" resolved2d=") || Line(text, "5 resolve ").find("host_fmt=") != text.npos)
    return 1;
  Contains(Line(text, "7 draw "), "resolved2d= 2:6");
  Contains(Line(text, "front "), "status=held_previous pipeline_draw_loss=true dst=00345000 host_fmt=34 "
                                 "host_size=960x540 current_writer=6");

  FrameTrace next(frame, 1.0f, false);
  // Prior-frame descriptors and subresource writes without a direct 2D descriptor aren't linked.
  auto subresource = host;
  subresource.descriptor = 0;
  resolve.dest_level = 1;
  next.Resolve(resolve, {true}, subresource);
  next.Draw(draw, descriptors.data());
  next.Front(false, false, 0x00345000, 16, 34, {960, 540});
  if (!Line(next.Text(), "1 draw ").ends_with(" resolved2d=")) return 1;
  Contains(Line(next.Text(), "front "), "status=current pipeline_draw_loss=false");
  Contains(Line(next.Text(), "front "), "current_writer=-");

  FrameTrace missing(frame, 1.0f, false);
  missing.Front(false, false);
  Contains(Line(missing.Text(), "front "), "status=missing pipeline_draw_loss=false dst=00000000 "
                                          "host_fmt=0 host_size=0x0 current_writer=-");
}
