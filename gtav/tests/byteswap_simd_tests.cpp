// byteswap_simd.h: big-endian guest data -> host order, 16 bytes at a time (SSSE3 / NEON / portable).
// unit = 4 swaps 32-bit words, unit = 2 swaps 16-bit halves. The output must be byte-exact
// regardless of platform — the same function is shared by the vertex upload path and tests.
#include "byteswap_simd.h"

#include <cstdint>
#include <cstring>
#include <iostream>

static void Expect(const uint8_t* out, const uint8_t* in, uint32_t unit) {
  uint8_t expected[16];
  for (uint32_t i = 0; i < 16; i += unit) {
    for (uint32_t b = 0; b < unit; b++) expected[i + b] = in[i + unit - 1 - b];
  }
  if (std::memcmp(out, expected, 16) != 0) {
    std::cerr << "unit " << unit << ": wrong byte order\n";
    std::exit(1);
  }
}

int main() {
  uint8_t in[16] = {};
  for (uint32_t i = 0; i < 16; i++) in[i] = uint8_t(0x10 + i);
  uint8_t out[16] = {};
  gtav::native::Swap16Bytes(out, in, 4);
  Expect(out, in, 4);
  gtav::native::Swap16Bytes(out, in, 2);
  Expect(out, in, 2);

  // All-zero and all-ones: must be identical to the input for either unit.
  std::memset(out, 0, 16);
  std::memset(in, 0, 16);
  gtav::native::Swap16Bytes(out, in, 4);
  std::memset(out, 0xFF, 16);
  std::memset(in, 0xFF, 16);
  gtav::native::Swap16Bytes(out, in, 2);

  // Cross-check against the portable fallback for a pattern where 16-bit and 32-bit
  // swaps disagree.
  uint8_t in2[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                     0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
  uint8_t out4[16] = {}, out2[16] = {};
  gtav::native::Swap16Bytes(out4, in2, 4);
  gtav::native::Swap16Bytes(out2, in2, 2);
  // 4-unit: swap the two 32-bit words' bytes within each word (01 02 03 04 -> 04 03 02 01).
  if (out4[0] != 0x04 || out4[1] != 0x03 || out4[2] != 0x02 || out4[3] != 0x01) {
    std::cerr << "unit 4: wrong byte order\n";
    return 1;
  }
  if (out4[8] != 0x0C || out4[9] != 0x0B || out4[10] != 0x0A || out4[11] != 0x09) {
    std::cerr << "unit 4: wrong byte order (second word)\n";
    return 1;
  }
  // 2-unit: swap each 2-byte half.
  if (out2[0] != 0x02 || out2[1] != 0x01 || out2[2] != 0x04 || out2[3] != 0x03) {
    std::cerr << "unit 2: wrong byte order\n";
    return 1;
  }
  return 0;
}
