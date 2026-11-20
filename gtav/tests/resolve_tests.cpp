#include "resolve_pm4.h"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <vector>

using namespace gtav::native;

#define CHECK(...)                                                                     \
  do {                                                                                 \
    if (!(__VA_ARGS__)) {                                                              \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n';           \
      std::exit(1);                                                                    \
    }                                                                                 \
  } while (false)

namespace {

std::vector<uint8_t> BigEndian(std::initializer_list<uint32_t> words) {
  std::vector<uint8_t> bytes;
  for (const uint32_t w : words)
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(uint8_t(w >> shift));
  return bytes;
}

constexpr uint32_t Cmd(uint32_t op, uint32_t count, bool predicated = false) {
  return 0xC0000000 | ((count - 1) << 16) | (op << 8) | uint32_t(predicated);
}

constexpr uint32_t kSurface = 1280, kDepth = 0x42, kColorDepthClear = 0x301, kClearWord = 0x800000A7;
constexpr uint32_t kInlineCopy = Cmd(0x36, 1), kDrawRect = 0x00030088;

void Packets() {
  const auto bytes = BigEndian({0, 0x80000000, 0x40000100, 0, 0,       // padding + type 1
                                0x00022000, kSurface, 0x11, kDepth,   // surface/color/depth, type 0
                                0x00002208, 6,                      // enter copy mode
                                0x00052318, kColorDepthClear, 0, 0, 0, 0, kClearWord,
                                kInlineCopy, kDrawRect,
                                0x00002318, 4, 0x0000231D, 0, 0x00002208, 4});  // restore after copy
  auto p = ScanResolvePackets(bytes);
  CHECK(p.scan == ResolveScan::kDirectCopy);
  CHECK(p.control == kColorDepthClear && p.depth_clear == kClearWord);
  CHECK(p.surface == kSurface && p.depth == kDepth);
  CHECK(ScanResolvePackets(std::span(bytes).first(bytes.size() - 1)).scan == ResolveScan::kUnverified);
  CHECK(ScanResolvePackets(BigEndian({0x00022318, 0})).scan == ResolveScan::kUnverified);
  CHECK(ScanResolvePackets(BigEndian({})).scan == ResolveScan::kNoCopy);
  CHECK(ScanResolvePackets(BigEndian({0x2318, kColorDepthClear})).scan == ResolveScan::kNoCopy);

  for (const uint32_t op : {0x2Du, 0x55u, 0x56u}) {
    const uint32_t base = op == 0x2D ? 0x40318 : 0x2318;
    p = ScanResolvePackets(BigEndian({Cmd(op, 2), base, kColorDepthClear,
                                      0x0001A31D, 0xFFFFFFFF, kClearWord,  // repeated register write
                                      0x2208, 6, Cmd(0x22, 2), 0, kDrawRect}));
    CHECK(p.scan == ResolveScan::kDirectCopy);
    CHECK(p.control == kColorDepthClear && p.depth_clear == kClearWord);
  }
  CHECK(ScanResolvePackets(BigEndian({0x2208, 6, 0x2318, 0, kInlineCopy, kDrawRect})).scan == ResolveScan::kDirectCopy);
  CHECK(ScanResolvePackets(BigEndian({kInlineCopy, kDrawRect})).scan == ResolveScan::kUnverified);
  CHECK(ScanResolvePackets(BigEndian({0x2208, 4, 0x2318, 0, kInlineCopy, kDrawRect})).scan == ResolveScan::kUnverified);
  CHECK(ScanResolvePackets(BigEndian({0x2208, 6, 0x2318, 0, Cmd(0x36, 1, true), kDrawRect})).scan == ResolveScan::kUnverified);
  CHECK(ScanResolvePackets(BigEndian({0x2208, 6, 0x2318, 0, Cmd(0x22, 2), 1, kDrawRect})).scan == ResolveScan::kUnverified);
  CHECK(ScanResolvePackets(BigEndian({0x2208, 6, 0x2318, 0, kInlineCopy, kDrawRect, kInlineCopy, kDrawRect})).scan ==
        ResolveScan::kUnverified);
  CHECK(ScanResolvePackets(BigEndian({Cmd(0x3F, 2), 0, 4})).scan == ResolveScan::kUnverified);  // indirect, don't follow
}

void DepthClear() {
  const auto ops = DecodeResolve(kColorDepthClear);
  CHECK(ops.source == 1 && !ops.copy_depth && ops.clear_depth && ops.clear_color);
  CHECK(DecodeResolve(0x304).copy_depth && !DecodeResolve(0x304).clear_color);
  CHECK(!DecodeResolve(1).clear_depth && !DecodeResolve(1).clear_color);
  ResolveProbe probe;
  probe.surface_after = kSurface;
  probe.depth_after = kDepth;
  probe.same_depth_surface = true;
  probe.packets = ScanResolvePackets(BigEndian({0x2208, 6, 0x2318, kColorDepthClear, 0x231D, kClearWord,
                                               kInlineCopy, kDrawRect}));
  auto test = [&](bool enabled, uint32_t flags = kColorDepthClear) {
    return ReplayResolveDepthClear(flags, 0.25f, kSurface, kDepth, probe, enabled);
  };
  CHECK(!test(false));
  CHECK(!test(true, 1) && !test(true, 0x101));  // no depth clear requested, regardless of captured value
  const auto clear = test(true);
  CHECK(clear && clear->stencil == 0xA7 && clear->z == 8388608.0f / 16777215.0f);
  CHECK(!test(true, 0x202));  // source mismatch
  CHECK(!VerifiedResolveClear(kColorDepthClear, kSurface | 0x10000, kDepth, probe));  // MSAA
  probe.packets.control = 0x101;
  CHECK(!test(true));  // caller flag without the GPU bit
  probe.packets.control = kColorDepthClear | (2 << 20);
  CHECK(!test(true));  // unsupported copy command
  probe.packets.control = kColorDepthClear;
  probe.depth_after++;
  CHECK(!test(true));  // shadow changed during the call
  probe.depth_after = kDepth;
  probe.packets.depth = kDepth + 1;
  CHECK(!test(true));  // different attachment written into PM4
  probe.packets.depth.reset();
  probe.same_depth_surface = false;
  CHECK(!test(true));  // no bound (or switched) depth object
  probe.same_depth_surface = true;
  probe.packets.depth_clear.reset();
  CHECK(!test(true));  // don't substitute the legacy clear_z/stencil for a color copy
  probe.packets.control = 0x204;
  probe.packets.depth_clear = kClearWord;
  const auto depth_clear = test(true, 0x204);
  CHECK(depth_clear && depth_clear->z == clear->z && depth_clear->stencil == 0xA7);
  const auto legacy = test(false, 0x204);
  CHECK(legacy && legacy->z == 0.25f && !legacy->stencil);
  probe.packets.scan = ResolveScan::kUnverified;
  const auto fallback = test(true, 0x204);
  CHECK(fallback && fallback->z == 0.25f && !fallback->stencil);
  CHECK(!ReplayResolveDepthClear(0x204, std::numeric_limits<float>::quiet_NaN(), kSurface, kDepth, probe, false));

  CHECK(ResolveDepthValue(0, true) == 0.0f);
  CHECK(ResolveDepthValue(0x000001A7, true) == std::ldexp(1.0f, -34));
  CHECK(ResolveDepthValue(0xE00000A7, true) == 0.5f);
  CHECK(ResolveDepthValue(0xF00000A7, true) == 1.0f);
  CHECK(ResolveDepthValue(0xFFFFFFA7, false) == 1.0f);
  probe.packets.scan = ResolveScan::kDirectCopy;
  probe.packets.depth_clear = 0xFFFFFFA7;
  CHECK(!VerifiedResolveClear(0x204, kSurface, probe.depth_after = kDepth | 0x10000, probe));  // outside host range
}

void Regions() {
  CHECK(ResolveCopyRegion(1280, 720, 1280, 720, nullptr, nullptr) == ResolveRegion{0, 0, 1280, 720, 0, 0});
  CHECK(ResolveCopyRegion(8, 8, 32, 32, nullptr, nullptr) == ResolveRegion{0, 0, 8, 8, 0, 0});
  const uint32_t rect[]{3, 4, 13, 18}, point[]{6, 7};
  CHECK(ResolveCopyRegion(12, 20, 10, 12, rect, point) == ResolveRegion{3, 4, 7, 9, 6, 7});
  CHECK(ResolveCopyRegion(12, 20, 10, 12, rect, nullptr) == ResolveRegion{3, 4, 10, 12, 3, 4});
  CHECK(ResolveCopyRegion(3, 20, 10, 12, rect, point).Empty());  // off source
  CHECK(ResolveCopyRegion(12, 20, 6, 7, rect, point).Empty());  // off destination
  const uint32_t inverted[]{3, 4, 2, 18}, wrap[]{0xFFFFFFFFu, 0, 5, 5};
  CHECK(ResolveCopyRegion(16, 16, 10, 10, inverted, nullptr).Empty());
  CHECK(ResolveCopyRegion(16, 16, 10, 10, wrap, nullptr).Empty());
  // Coordinates remain safe even if a guest supplies a very large output or rectangle.
  const uint32_t large[]{9, 7, UINT32_MAX, UINT32_MAX};
  CHECK(ResolveCopyRegion(12, 20, UINT32_MAX, UINT32_MAX, large, point) == ResolveRegion{9, 7, 12, 20, 6, 7});
  // Fractional scale can round a valid guest copy beyond the end of the actual host mip.
  CHECK(ScaleResolveRegion({0, 0, 3, 3, 0, 0}, 0.5f, 1, 1) == ResolveRegion{0, 0, 1, 1, 0, 0});
  CHECK(ScaleResolveRegion({0, 0, 3, 3, 1, 1}, 0.5f, 3, 3) == ResolveRegion{0, 0, 2, 2, 1, 1});
  CHECK(ScaleResolveRegion({1, 1, 2, 2, 0, 0}, 0.5f, 4, 4).Empty());
  CHECK(ScaleResolveRegion({2, 3, 7, 7, 6, 5}, 1.0f, 10, 9) == ResolveRegion{2, 3, 6, 7, 6, 5});
}

}  // namespace

int main() {
  Packets();
  DepthClear();
  Regions();
}
