// The same allocation and retirement code as the native renderer, with mock mapped storage.
#include "graphics_fixture.h"
#include "native/upload_arena.h"

#include <memory>
#include <set>
#include <vector>

using namespace gtav::native;

struct AllocationLog {
  uint32_t created = 0;
  std::set<uint32_t> mapped;
  struct Buffer {
    AllocationLog& log;
    uint32_t id;
    uint64_t size;
    Buffer(AllocationLog& log, uint64_t size) : log(log), id(++log.created), size(size) {
      log.mapped.insert(id);
    }
    ~Buffer() { CHECK(log.mapped.erase(id) == 1); }
  };
  auto Create(uint64_t size) { return std::make_unique<Buffer>(*this, size); }
};

int main() {
  constexpr uint64_t nominal = MakeGraphicsMemoryPolicy(true, 64 * kMiB).upload_chunk;
  constexpr uint64_t oversize = 37 * kMiB;
  AllocationLog log;
  const auto create = [&](uint64_t capacity) { return log.Create(capacity); };
  UploadArena<std::unique_ptr<AllocationLog::Buffer>> arena;

  // A burst touches four nominal chunks and one oversize. Nothing can be released until the
  // submission completes. Allocation results keep referring to the same underlying buffers.
  std::vector<uint32_t> burst;
  uint32_t big_id = 0;
  const AllocationLog::Buffer* first_buffer = nullptr;
  for (const uint64_t request : {nominal, nominal, oversize, nominal, nominal}) {
    auto slice = arena.Allocate(request, 512, nominal, create);
    CHECK(slice.offset == 0 && slice.storage->size == request);
    if (!first_buffer) first_buffer = slice.storage.get();
    if (request == oversize) big_id = slice.storage->id;
    else burst.push_back(slice.storage->id);
  }
  CHECK(first_buffer->id == burst[0]);
  CHECK(log.mapped.size() == 5 && arena.ReservedBytes() == 4 * nominal + oversize);
  CHECK(arena.AfterFence(nominal, true) == oversize);
  CHECK(!log.mapped.contains(big_id) && arena.ReservedBytes() == 4 * nominal);

  // The same two aligned writes fit the first chunk in subsequent frames. Keep extra chunks
  // warm through short bursts; reclaim them once, after 90 completed idle frames.
  for (uint32_t frame = 1; frame <= kUploadIdleFrames; ++frame) {
    auto first = arena.Allocate(3, 256, nominal, create);
    auto second = arena.Allocate(17, 512, nominal, create);
    CHECK(first.storage->id == burst[0] && second.storage->id == burst[0]);
    CHECK(first.offset == 0 && second.offset == 512);
    CHECK(arena.ReservedBytes() == 4 * nominal);
    CHECK(arena.AfterFence(nominal, true) == (frame == kUploadIdleFrames ? 3 * nominal : 0));
  }
  CHECK(log.mapped == std::set<uint32_t>{burst[0]} && arena.ReservedBytes() == nominal);
  CHECK(log.created == 5);  // reused; no alloc/free loop during normal rendering

  // Another spike fits normally. When a formerly idle chunk is used, its own age restarts.
  CHECK(arena.Allocate(nominal, 512, nominal, create).storage->id == burst[0]);
  const uint32_t extra = arena.Allocate(nominal, 512, nominal, create).storage->id;
  CHECK(extra != burst[0]);
  CHECK(arena.AfterFence(nominal, true) == 0);
  for (uint32_t i = 0; i < kUploadIdleFrames - 1; ++i) {
    arena.Allocate(1, 512, nominal, create);
    CHECK(arena.AfterFence(nominal, true) == 0);
  }
  arena.Allocate(nominal, 512, nominal, create);
  CHECK(arena.Allocate(1, 512, nominal, create).storage->id == extra);
  CHECK(arena.AfterFence(nominal, true) == 0 && log.mapped.contains(extra));

  // Standard slots never trim on reset. Retiring one slot must not reclaim storage in another.
  decltype(arena) standard;
  const uint32_t standard_id = standard.Allocate(oversize, 512, nominal, create).storage->id;
  CHECK(standard.AfterFence(nominal, false) == 0 && standard.ReservedBytes() == oversize);
  for (uint32_t i = 0; i < kUploadIdleFrames; ++i) {
    CHECK(standard.AfterFence(nominal, false) == 0);
  }
  CHECK(arena.ReservedBytes() == 2 * nominal);
  for (uint32_t i = 1; i <= kUploadIdleFrames; ++i) {
    CHECK(arena.AfterFence(nominal, true) == (i == kUploadIdleFrames ? 2 * nominal : 0));
  }
  CHECK(log.mapped == std::set<uint32_t>{standard_id} && arena.ReservedBytes() == 0);
  CHECK(standard.Allocate(1, 512, nominal, create).storage->id == standard_id);
  const auto restarted = arena.Allocate(8, 512, nominal, create);
  CHECK(restarted.offset == 0 && restarted.storage->id != extra && arena.ReservedBytes() == nominal);
  std::puts("Upload arena: aligned allocation, fence-only oversize/idle reclamation and slot isolation passed");
}
