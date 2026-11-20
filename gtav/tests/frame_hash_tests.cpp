// frame.h: the per-draw vertex/index hash published at capture and the frame byte arena.
// DrawDataHash is deterministic and pure: it never reads memory it does not own
// through the Translate functor, and an inline/empty draw must return the FNV offset basis.
#include "frame.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

constexpr uint64_t kFnvBasis = 1469598103934665603ull;  // FNV-1a 64 offset basis

// A flat guest-memory model: physical addresses are offsets into g_mem.
std::vector<uint8_t> g_mem(8192, 0x5A);

const uint8_t* Physical(uint32_t addr) {
  if (uint64_t(addr) + 16 > g_mem.size()) return nullptr;
  return g_mem.data() + addr;
}

void PutU32BE(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

}  // namespace

int main() {
  // Inline draw: nothing to read.
  gtav::native::DrawCommand inline_draw{};
  inline_draw.is_inline = true;
  if (gtav::native::DrawDataHash(inline_draw, Physical) != kFnvBasis) {
    std::cerr << "inline draw must return the FNV basis\n";
    return 1;
  }

  // Non-indexed draw with no streams: also FNV basis.
  gtav::native::DrawCommand empty{};
  if (gtav::native::DrawDataHash(empty, Physical) != kFnvBasis) {
    std::cerr << "stream-less draw must return the FNV basis\n";
    return 1;
  }

  // Non-indexed draw with one full stream: non-trivial hash, stable across calls.
  // (Stream address must be non-zero: 0 means "no stream" and is skipped.)
  gtav::native::DrawCommand d{};
  d.start = 0; d.count = 4;
  d.streams[0].address = 256; d.streams[0].size = 4096; d.streams[0].offset = 0; d.streams[0].stride = 4;
  gtav::native::StreamBinding zero_addr{};
  d.streams[1] = zero_addr;
  const uint64_t h1 = gtav::native::DrawDataHash(d, Physical);
  const uint64_t h2 = gtav::native::DrawDataHash(d, Physical);
  if (h1 == kFnvBasis) { std::cerr << "stream hash must not be the FNV basis\n"; return 1; }
  if (h1 != h2) { std::cerr << "hash must be deterministic\n"; return 1; }

  // Same draw but the stream is pinned beyond the recorded size: the draw must
  // fall back to the basis (never read past the buffer).
  gtav::native::DrawCommand over{};
  over.start = 2; over.count = 4;  // hi = 8 * 4 = 32 > size 16
  over.streams[0].address = 256; over.streams[0].size = 16; over.streams[0].offset = 0; over.streams[0].stride = 4;
  if (gtav::native::DrawDataHash(over, Physical) != kFnvBasis) {
    std::cerr << "over-range draw must return the FNV basis\n";
    return 1;
  }

  // Indexed, 32-bit indices: covers vertices 1..7 -> one 7-vertex range.
  std::vector<uint8_t> idx(16);
  PutU32BE(&idx[0], 1); PutU32BE(&idx[4], 3); PutU32BE(&idx[8], 7); PutU32BE(&idx[12], 1);
  std::memcpy(&g_mem[0], idx.data(), 16);
  gtav::native::DrawCommand indexed{};
  indexed.indexed = true; indexed.index32 = true;
  indexed.start = 0; indexed.count = 4; indexed.index_address = 0; indexed.index_size = 16;
  indexed.base_vertex = 0;
  indexed.streams[0].address = 256; indexed.streams[0].size = 8192; indexed.streams[0].stride = 4; indexed.streams[0].offset = 0;
  const uint64_t h3 = gtav::native::DrawDataHash(indexed, Physical);
  if (h3 == kFnvBasis) { std::cerr << "indexed hash must not be the FNV basis\n"; return 1; }

  // An index slice past the recorded index buffer: basis again.
  gtav::native::DrawCommand bad{};
  bad.indexed = true; bad.index32 = true;
  bad.start = 0; bad.count = 8; bad.index_address = 0; bad.index_size = 16;
  if (gtav::native::DrawDataHash(bad, Physical) != kFnvBasis) {
    std::cerr << "over-range index slice must return the FNV basis\n";
    return 1;
  }

  // base_vertex must shift the reached vertex range. The shifted range reads bytes that
  // are distinctively written (the non-shifted draw only reads uniform 0x5A fill), so the
  // hash must change.
  gtav::native::DrawCommand base{};
  base.indexed = true; base.index32 = true;
  base.start = 0; base.count = 4; base.index_address = 0; base.index_size = 16;
  base.base_vertex = 1024;
  base.streams[0] = indexed.streams[0];
  std::vector<uint8_t> base_idx(16);
  PutU32BE(&base_idx[0], 2); PutU32BE(&base_idx[4], 9); PutU32BE(&base_idx[8], 10); PutU32BE(&base_idx[12], 5);
  std::memcpy(&g_mem[0], base_idx.data(), 16);
  for (uint32_t i = 0; i < 48; i++) g_mem[4356 + i] = uint8_t(0xA0 + i);
  const uint64_t h4 = gtav::native::DrawDataHash(base, Physical);
  if (h4 == kFnvBasis || h4 == h3) { std::cerr << "base_vertex must change the reached range\n"; return 1; }

  // 16-bit 0xFFFF (negative-index) values: the walk finds no valid vertex range. Note the
  // function still returns the FNV hash of the index slice — the index contribution is NOT
  // reset when the fallback triggers (observed behavior; do not rely on the basis here).
  // The important part is that NO stream bytes are mixed in (no valid range), so the hash
  // must be stable and must not depend on the stream contents at the would-be range.
  std::vector<uint8_t> idx16(4);
  idx16[0] = 0xFF; idx16[1] = 0xFF; idx16[2] = 0xFF; idx16[3] = 0xFF;
  std::memcpy(&g_mem[0], idx16.data(), 4);
  gtav::native::DrawCommand neg{};
  neg.indexed = true; neg.index32 = false;
  neg.start = 0; neg.count = 2; neg.index_address = 0; neg.index_size = 4;
  neg.streams[0] = indexed.streams[0];
  const uint64_t h_neg = gtav::native::DrawDataHash(neg, Physical);
  const uint64_t h_neg2 = gtav::native::DrawDataHash(neg, Physical);
  if (h_neg == kFnvBasis || h_neg != h_neg2) {
    std::cerr << "all-negative indices: hash must be the (non-trivial, stable) index-slice hash\n";
    return 1;
  }
  // Zero stream (address 0): no stream is mixed, the same index-slice hash must come back.
  neg.streams[0] = gtav::native::StreamBinding{};
  if (gtav::native::DrawDataHash(neg, Physical) != h_neg) {
    std::cerr << "fallback hash must not depend on stream contents\n";
    return 1;
  }
  // ByteArena: growth, clear, shrink; Push/At on a packet.
  gtav::native::ByteArena arena;
  uint8_t* arena_p = arena.Grow(64);
  for (uint32_t i = 0; i < 64; i++) arena_p[i] = uint8_t(0x40 + (i & 0x0F));
  if (arena.size() != 64) { std::cerr << "arena size wrong\n"; return 1; }
  arena.Shrink(10);
  if (arena.size() != 54) { std::cerr << "arena shrink wrong\n"; return 1; }
  arena.clear();
  if (arena.size() != 0) { std::cerr << "arena clear wrong\n"; return 1; }

  gtav::native::FramePacket packet;
  uint8_t payload[24];
  for (uint32_t i = 0; i < 24; i++) payload[i] = uint8_t(i);
  const uint32_t off = packet.Push(payload, 24);
  if (off != 0) { std::cerr << "first push must return offset 0 (got " << off << ")\n"; return 1; }
  if (std::memcmp(packet.At(off), payload, 24) != 0) { std::cerr << "At must read back the pushed bytes\n"; return 1; }
  // A 0x80000000-tagged offset addresses the fence arena, not the packet arena.
  packet.fence_arena.Grow(8);
  uint8_t fence_payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  std::memcpy(packet.fence_arena.data(), fence_payload, 8);
  if (std::memcmp(packet.At(gtav::native::FramePacket::kFenceBit), fence_payload, 8) != 0) {
    std::cerr << "fence bit must address the fence arena\n";
    return 1;
  }
  packet.Clear();
  if (packet.arena.size() != 0 || packet.index != 0) { std::cerr << "packet clear failed\n"; return 1; }
  return 0;
}
