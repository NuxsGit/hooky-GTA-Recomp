// Linear upload storage for one GPU frame slot. Storage owns the mapped buffer; moving a chunk
// must not move the underlying allocation. No chunk can be freed before the slot's fence.
#pragma once

#include "graphics_policy.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace gtav::native {

template <typename Storage>
class UploadArena {
 public:
  struct Slice {
    Storage& storage;
    uint64_t offset;
  };

  template <typename Create>
  Slice Allocate(uint64_t bytes, uint64_t alignment, uint64_t nominal, Create&& create) {
    offset_ = AlignUpload(offset_, alignment);
    while (current_ < chunks_.size() && !FitsUpload(chunks_[current_].capacity, offset_, bytes)) {
      ++current_;
      offset_ = 0;
    }
    if (current_ == chunks_.size()) {
      const uint64_t capacity = UploadCapacity(nominal, bytes);
      chunks_.push_back({std::forward<Create>(create)(capacity), capacity, epoch_});
    }
    auto& chunk = chunks_[current_];
    chunk.last_use = epoch_;
    const uint64_t offset = offset_;
    offset_ += bytes;
    return {chunk.storage, offset};
  }

  uint64_t ReservedBytes() const {
    uint64_t bytes = 0;
    for (const auto& chunk : chunks_) bytes += chunk.capacity;
    return bytes;
  }

  // Called once per completed submission, not per game tick or before BeginFrame. Retain the
  // ordinary working set across frames. Trim an unused chunk only after sustained inactivity,
  // to avoid reallocating it on every short streaming burst; one-off oversize chunks go at once.
  uint64_t AfterFence(uint64_t nominal, bool lean) {
    uint64_t reclaimed = 0;
    if (lean) {
      std::erase_if(chunks_, [&](const Chunk& chunk) {
        if (chunk.capacity <= nominal && epoch_ - chunk.last_use < kUploadIdleFrames) return false;
        reclaimed += chunk.capacity;
        return true;
      });
    }
    ++epoch_;
    current_ = 0;
    offset_ = 0;
    return reclaimed;
  }

 private:
  struct Chunk {
    Storage storage;
    uint64_t capacity, last_use;
  };
  std::vector<Chunk> chunks_;
  size_t current_ = 0;
  uint64_t offset_ = 0, epoch_ = 0;
};

}  // namespace gtav::native
