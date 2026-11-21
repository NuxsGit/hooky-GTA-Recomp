// safe_read.h: copying bytes that may not be mapped (process_vm_readv on Linux/Android,
// mach vm_read_overwrite on macOS). Reading our own, already-mapped memory must always
// succeed and copy the bytes; an address that is certainly unmapped must fail, not crash.
#include "safe_read.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

int main() {
  std::vector<uint8_t> src(1024);
  for (uint32_t i = 0; i < src.size(); i++) src[i] = uint8_t(i ^ (i >> 3));
  std::vector<uint8_t> dst(1024, 0);

  if (!gtav::native::SafeReadMemory(dst.data(), src.data(), src.size())) {
    std::cerr << "safe read of own memory failed\n";
    return 1;
  }
  if (dst != src) {
    std::cerr << "safe read copied wrong bytes\n";
    return 1;
  }

  // Empty copy: nothing to read, nothing to fail.
  if (!gtav::native::SafeReadMemory(dst.data(), src.data(), 0)) {
    std::cerr << "empty safe read failed\n";
    return 1;
  }

  // Address far above any sane mapping: must report failure, never crash or garbage.
  auto bad = gtav::native::SafeReadMemory(dst.data(), reinterpret_cast<const uint8_t*>(0x00007fff0000ull), 16);
  if (bad) {
    std::cerr << "unreadable address must fail\n";
    return 1;
  }
  return 0;
}
