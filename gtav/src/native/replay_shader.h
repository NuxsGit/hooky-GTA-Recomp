#pragma once

#include <cstdint>

namespace gtav::native {

// Null in guest state is a real depth/stencil-only draw. Null from the host shader cache
// for a nonzero hash must not affect attachments or make it into the pipeline cache.
constexpr bool SkipMissingPixelShader(uint64_t guest_hash, bool native_shader_ready) {
  return guest_hash != 0 && !native_shader_ready;
}

}  // namespace gtav::native
