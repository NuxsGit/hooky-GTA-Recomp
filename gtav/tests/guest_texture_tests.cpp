#include "textures.h"

#include <rex/graphics/pipeline/texture/util.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace gtav::native;
namespace xenos = rex::graphics::xenos;
namespace tex = rex::graphics::texture_util;

#define CHECK(...)                                                                    \
  do {                                                                                \
    if (!(__VA_ARGS__)) {                                                             \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n';          \
      std::exit(1);                                                                   \
    }                                                                                 \
  } while (false)

namespace {
constexpr uint32_t kBase = 0x1000, kMips = 0x8000;
using Words = std::array<uint32_t, 6>;
using Rgba = std::array<uint8_t, 4>;

Words FetchRGBA(uint32_t w, uint32_t h, uint32_t pitch, bool tiled, bool packed,
                uint32_t max_mip = 0, uint32_t base = kBase) {
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.tiled = tiled;
  fetch.packed_mips = packed;
  fetch.size_2d.width = w - 1;
  fetch.size_2d.height = h - 1;
  fetch.pitch = pitch / 32;
  fetch.format = xenos::TextureFormat::k_8_8_8_8;
  fetch.endianness = xenos::Endian::k8in32;
  fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  fetch.base_address = base >> 12;
  fetch.mip_max_level = max_mip;
  fetch.mip_address = max_mip ? kMips >> 12 : 0;
  Words words;
  std::memcpy(words.data(), &fetch, sizeof(fetch));
  return words;
}

void Store(std::vector<uint8_t>& ram, size_t addr, Rgba pixel) {
  CHECK(addr + pixel.size() <= ram.size());
  std::reverse_copy(pixel.begin(), pixel.end(), ram.data() + addr);  // Xbox k8in32
}

Rgba Pixel(const DecodedTexture& image, uint32_t level, uint32_t x, uint32_t y) {
  CHECK(level < image.mip_count && image.format == plume::RenderFormat::R8G8B8A8_UNORM);
  const auto& mip = image.mips[level];
  CHECK(x < mip.width && y < mip.height);
  const size_t offset = mip.offset + 4 * (size_t(y) * mip.row_texels + x);
  Rgba pixel;
  CHECK(offset + pixel.size() <= image.data.size());
  std::memcpy(pixel.data(), image.data.data() + offset, pixel.size());
  return pixel;
}

Rgba Pattern(uint32_t x, uint32_t y) { return {uint8_t(x), uint8_t(y), uint8_t(x ^ y), 255}; }

void TiledLastRow() {
  constexpr uint32_t w = 80, h = 36, pitch = 96;
  const auto words = FetchRGBA(w, h, pitch, true, false);
  const auto address = [](uint32_t x, uint32_t y) { return kBase + tex::GetTiledOffset2D(x, y, pitch, 2); };
  std::vector<uint8_t> ram(kBase + pitch * 64 * 4, 0xB7);  // two rows of macrotiles
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) Store(ram, address(x, y), Pattern(x, y));
  const auto last = address(w - 1, h - 1);
  CHECK(last >= kBase + pitch * h * 4);  // the old cache hash stopped before this tile
  DecodedTexture image;
  CHECK(DecodeGuestTexture(words.data(), ram, &image));
  CHECK(image.width == w && image.height == h && image.mip_count == 1);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) CHECK(Pixel(image, 0, x, y) == Pattern(x, y));
  const auto before = HashGuestTexture(words.data(), ram);
  const Rgba replacement{32, 239, 16, 255};
  Store(ram, last, replacement);
  CHECK(before && HashGuestTexture(words.data(), ram) != before);
  uint32_t ranges[4];
  CHECK(GuestTextureRanges(words.data(), ranges));
  CHECK(ranges[0] == kBase && last + 4 <= kBase + ranges[1] && ranges[3] == 0);
  CHECK(DecodeGuestTexture(words.data(), ram, &image) && Pixel(image, 0, w - 1, h - 1) == replacement);
}

void PackedRectangularMips() {
  // A 128x32 image packs levels 1..4 into the mip allocation. Its fourth level (8x2)
  // starts at x=32 in the 64-block row, not x=8 as the legacy per-level calculation assumed.
  const auto words = FetchRGBA(128, 32, 128, true, true, 4);
  std::vector<uint8_t> ram(kMips + 8192, 0xB7);
  const auto address = [](uint32_t x, uint32_t y) { return kMips + tex::GetTiledOffset2D(x, y, 64, 2); };
  constexpr Rgba yellow{237, 220, 0, 255};
  Store(ram, address(63, 31), yellow);  // level 1 at (0, 16)
  Store(ram, address(39, 1), yellow);   // level 4 at (32, 0)
  DecodedTexture image;
  CHECK(DecodeGuestTexture(words.data(), ram, &image));
  CHECK(image.mip_count == 5 && Pixel(image, 1, 63, 15) == yellow && Pixel(image, 4, 7, 1) == yellow);
  uint32_t ranges[4];
  CHECK(GuestTextureRanges(words.data(), ranges));
  CHECK(ranges[2] == kMips && ranges[2] + ranges[3] >= address(39, 1) + 4);
  const auto before = HashGuestTexture(words.data(), ram);
  Store(ram, address(39, 1), {0, 128, 234, 255});
  CHECK(before && HashGuestTexture(words.data(), ram) != before);
}

void AlignedMipStorage() {
  auto words = FetchRGBA(64, 64, 64, true, false, 2);
  words[1] = (words[1] & ~0xFFu) | uint32_t(xenos::TextureFormat::k_8);  // R8, no endian swap
  // Unpacked levels each start on a 4 KiB page, even if a tile uses only 1024 bytes.
  const size_t last = kMips + 4096 + tex::GetTiledOffset2D(15, 15, 32, 0);
  std::vector<uint8_t> ram(kMips + 8192, 0);
  ram[last] = 242;
  DecodedTexture image;
  CHECK(DecodeGuestTexture(words.data(), ram, &image));
  CHECK(image.format == plume::RenderFormat::R8_UNORM && image.mip_count == 3);
  const auto& mip = image.mips[2];
  CHECK(mip.width == 16 && mip.height == 16 && image.data[mip.offset + 255] == 242);
  const auto before = HashGuestTexture(words.data(), ram);
  ram[last] = 3;
  CHECK(before && HashGuestTexture(words.data(), ram) != before);
}

void LinearPackedBase() {
  constexpr uint32_t w = 15, h = 6, pitch = 64, y_start = 16;
  constexpr uint32_t used = ((y_start + h - 1) * pitch + w) * 4;
  const auto words = FetchRGBA(w, h, pitch, false, true, 0, kBase | 0xA0000000u);
  std::vector<uint8_t> ram(kBase + used, 0xB7);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) Store(ram, kBase + ((y_start + y) * pitch + x) * 4, Pattern(x, y));
  DecodedTexture image;
  CHECK(DecodeGuestTexture(words.data(), ram, &image));
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) CHECK(Pixel(image, 0, x, y) == Pattern(x, y));
  uint32_t ranges[4];
  CHECK(GuestTextureRanges(words.data(), ranges));
  CHECK(ranges[0] == kBase && ranges[1] == used && ranges[3] == 0);
  const auto before = HashGuestTexture(words.data(), ram);
  Store(ram, uint32_t(ram.size()) - 4, {5, 8, 18, 255});
  CHECK(before && HashGuestTexture(words.data(), ram) != before);
  ram.pop_back();
  CHECK(!HashGuestTexture(words.data(), ram) && !DecodeGuestTexture(words.data(), ram, &image));
  const auto past_physical = FetchRGBA(w, h, pitch, false, true, 0, kGuestTextureAddressSpace - 4096);
  CHECK(!GuestTextureRanges(past_physical.data(), ranges));
}
}  // namespace

int main() {
  TiledLastRow();
  PackedRectangularMips();
  AlignedMipStorage();
  LinearPackedBase();
  std::cout << "Guest 2D texture layout, pixels and change detection passed\n";
}
