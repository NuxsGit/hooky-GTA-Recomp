// Draw the production shaders, pipeline and blend over synthetic depths. No window or game data.
#include "graphics_fixture.h"
#include "native/contact_rays_pass.h"

#include "native_blit_vs.h"
#include "native_contact_rays_ps.h"
#include <SDL3/SDL.h>
#include <plume_render_interface_builders.h>
#include <plume_vulkan.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace gtav::native;
using namespace plume;

int main() {
  if (!SDL_getenv("SDL_VIDEODRIVER")) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
  if (!SDL_Init(SDL_INIT_VIDEO) || !SDL_Vulkan_LoadLibrary(nullptr)) {
    std::fprintf(stderr, "SDL/Vulkan initialization: %s\n", SDL_GetError());
    return 1;
  }
  struct QuitSdl { ~QuitSdl() { SDL_Vulkan_UnloadLibrary(); SDL_Quit(); } } quit_sdl;
  VulkanInterface api;
  CHECK(api.isValid());
  auto gpu = api.createDevice("");
  CHECK(gpu);
  auto queue = gpu->createCommandQueue(RenderCommandListType::DIRECT);
  CHECK(queue);
  auto list = queue->createCommandList();
  auto fence = gpu->createCommandFence();
  CHECK(list && fence);

  constexpr Extent size{128, 72};
  constexpr auto mask_format = RenderFormat::R8G8B8A8_UNORM, depth_format = RenderFormat::R32_FLOAT;
  constexpr auto bytes = size.width * size.height * 4;
  auto depth = gpu->createTexture(RenderTextureDesc::Texture2D(size.width, size.height, 1, depth_format));
  auto mask = gpu->createTexture(RenderTextureDesc::ColorTarget(size.width, size.height, mask_format));
  auto upload = gpu->createBuffer(RenderBufferDesc::UploadBuffer(bytes));
  auto readback = gpu->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
  CHECK(depth && mask && upload && readback);
  const RenderTexture* attachment = mask.get();
  auto framebuffer = gpu->createFramebuffer(RenderFramebufferDesc(&attachment, 1));
  CHECK(framebuffer);

  // Use descriptor 1 to exercise push-constant selection, with the same layout and stages
  // as the game. The fixture needs neither a 32k-entry heap nor device addresses.
  RenderDescriptorSetBuilder builder;
  builder.begin();
  builder.addTexture(0, 2);
  builder.end(true, 2);
  auto textures = builder.create(gpu.get());
  CHECK(textures);
  textures->setTexture(1, depth.get(), RenderTextureLayout::SHADER_READ);
  RenderPipelineLayoutBuilder lb;
  lb.begin(false, true);
  lb.addDescriptorSet(builder);
  lb.addPushConstant(0, 4, sizeof(ContactRayPush), RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
  lb.end();
  auto layout = lb.create(gpu.get());
  auto vs = gpu->createShader(g_native_blit_vs, sizeof(g_native_blit_vs), "VSMain", RenderShaderFormat::SPIRV);
  auto ps = gpu->createShader(g_native_contact_rays_ps, sizeof(g_native_contact_rays_ps), "PSContactRays", RenderShaderFormat::SPIRV);
  CHECK(layout && vs && ps);
  auto pipeline = gpu->createGraphicsPipeline(ContactRayPipelineDesc(layout.get(), vs.get(), ps.get()));
  CHECK(pipeline);

  std::vector<float> proximity(size.width * size.height, 0.4f);
  auto at = [&](uint32_t x, uint32_t y) -> float& { return proximity[y * size.width + x]; };
  constexpr uint8_t initial[]{37, 91, 173, 211};
  const RenderColor clear(initial[0] / 255.f, initial[1] / 255.f, initial[2] / 255.f, initial[3] / 255.f);
  auto run = [&](int quality, int strength, bool reversed, Extent viewport = {}) {
    if (viewport.Empty()) viewport = size;
    auto* mapped = static_cast<float*>(upload->map());
    CHECK(mapped);
    for (size_t i = 0; i < proximity.size(); ++i) mapped[i] = reversed ? proximity[i] : 1.0f - proximity[i];
    upload->unmap();
    list->begin();
    list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(depth.get(), RenderTextureLayout::COPY_DEST));
    list->copyTextureRegion(RenderTextureCopyLocation::Subresource(depth.get()),
                            RenderTextureCopyLocation::PlacedFootprint(upload.get(), depth_format, size.width,
                                                                        size.height, 1, size.width));
    list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(depth.get(), RenderTextureLayout::SHADER_READ));
    list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(mask.get(), RenderTextureLayout::COLOR_WRITE));
    list->setFramebuffer(framebuffer.get());
    list->clearColor(0, clear);
    const auto options = ContactOptions(quality, strength);
    if (options.Enabled()) {
      list->setGraphicsPipelineLayout(layout.get());
      list->setGraphicsDescriptorSet(textures.get(), 0);
      RecordContactRayMask(list.get(), pipeline.get(), viewport,
                           {1, size.width, size.height, options.quality, options.strength, uint32_t(reversed)});
    }
    list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(mask.get(), RenderTextureLayout::COPY_SOURCE));
    list->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(), mask_format, size.width,
                                                                        size.height, 1, size.width),
                            RenderTextureCopyLocation::Subresource(mask.get()));
    list->end();
    queue->executeCommandLists(list.get(), fence.get());
    queue->waitForCommandFence(fence.get());
    const auto* pixels = static_cast<const uint8_t*>(readback->map());
    CHECK(pixels);
    std::vector<uint8_t> alpha(proximity.size());
    for (size_t i = 0; i < alpha.size(); ++i) {
      CHECK(std::memcmp(pixels + 4 * i, initial, 3) == 0); // never modify RGB, even next to an occluder
      alpha[i] = pixels[4 * i + 3];
      CHECK(alpha[i] <= initial[3]);
    }
    readback->unmap();
    return alpha;
  };
  auto neutral = [&](const auto& result) {
    CHECK(std::all_of(result.begin(), result.end(), [&](uint8_t alpha) { return alpha == initial[3]; }));
  };
  auto alpha = [&](const auto& result, uint32_t x, uint32_t y) { return result[y * size.width + x]; };

  neutral(run(3, 100, false));  // flat ground
  for (uint32_t y = 0; y < size.height; ++y)
    for (uint32_t x = 0; x < size.width; ++x) at(x, y) = 0.25f + 0.001f * x + 0.0015f * y;
  neutral(run(3, 100, false));  // planar slope: no self-occlusion
  std::fill(proximity.begin(), proximity.end(), 0.4f);
  for (uint32_t y = 30; y < 42; ++y) {
    for (uint32_t x = 64; x < 74; ++x) at(x, y) = 0.45f;
    at(0, y) = 0.0f;  // far plane at a screen edge
    at(20, y) = std::numeric_limits<float>::quiet_NaN();
    at(22, y) = 0.88f;  // large depth jump should not create a long halo
  }
  neutral(run(0, 100, false));
  neutral(run(3, 0, false));
  const auto weak = run(2, 25, false), strong = run(2, 100, false), reverse = run(2, 100, true);
  const uint8_t contact = alpha(strong, 63, 36);
  CHECK(contact < alpha(weak, 63, 36) && alpha(weak, 63, 36) < initial[3]);
  CHECK(alpha(strong, 0, 36) == initial[3] && alpha(strong, 20, 36) == initial[3]);
  CHECK(alpha(strong, 21, 36) == initial[3] && alpha(strong, 40, 36) == initial[3]);
  for (size_t i = 0; i < reverse.size(); ++i) CHECK(std::abs(int(reverse[i]) - int(strong[i])) <= 1);
  const auto half = run(2, 100, true, {64, 36});
  CHECK(alpha(half, 31, 18) < initial[3] && alpha(half, 63, 36) == initial[3]);

  std::fill(proximity.begin(), proximity.end(), 0.4f);
  at(52, 34) = 0.45f;  // low samples 1, 3, 5 pixels; medium samples the intervening texel
  CHECK(alpha(run(1, 100, true), 50, 34) == initial[3]);
  CHECK(alpha(run(2, 100, true), 50, 34) < initial[3]);
  at(52, 34) = 0.4f;
  at(51, 35) = 0.45f;  // only high traces diagonal rays
  CHECK(alpha(run(2, 100, true), 50, 34) == initial[3]);
  CHECK(alpha(run(3, 100, true), 50, 34) < initial[3]);
  neutral(run(0, 100, true));  // returning to Off must not accumulate visibility
  std::printf("%s: Vulkan contact mask, alpha %u -> %u; off, slopes, sky, depth polarity, half-size and quality passed\n",
              gpu->getDescription().name.c_str(), unsigned(initial[3]), unsigned(contact));
}
