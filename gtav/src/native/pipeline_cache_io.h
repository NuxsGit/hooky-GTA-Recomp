// Disk-side helpers for the native renderer's disposable pipeline caches.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace gtav::native {

inline constexpr size_t kDriverCacheLimit = 128 * 1024 * 1024;
inline constexpr size_t kPipelineJournalLimit = 32 * 1024 * 1024;
inline constexpr uint32_t kPipelineRecordMagic = 0x32504C47;  // GLP2, unchanged on disk

// Missing is a cold cache; a failed or oversized read is not safe to repair/append to.
inline std::optional<std::vector<uint8_t>> ReadCacheBytes(const std::filesystem::path& path, size_t limit) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec == std::errc::no_such_file_or_directory) return std::vector<uint8_t>{};
  if (ec || size > limit) return std::nullopt;
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (size && !file.read(reinterpret_cast<char*>(data.data()), std::streamsize(size))) return std::nullopt;
  return data;
}

// Visits complete records. A short/crashed tail can be removed without rewriting the prefix.
template <typename Visitor>
size_t VisitPipelineRecords(std::span<const uint8_t> data, Visitor visit) {
  size_t at = 0;
  while (data.size() - at >= 32) {
    uint32_t head[2];
    std::memcpy(head, data.data() + at, sizeof(head));
    if (head[0] != kPipelineRecordMagic || head[1] < 24 || head[1] > data.size() - at - sizeof(head)) break;
    uint64_t ids[3];
    std::memcpy(ids, data.data() + at + sizeof(head), sizeof(ids));
    visit(ids[0], ids[1], ids[2], data.subspan(at + 32, head[1] - 24));
    at += sizeof(head) + head[1];
  }
  return at;
}

class PipelineRecordWriter {
 public:
  ~PipelineRecordWriter() { Stop(); }

  // Single start, on the render thread. The worker owns all append/repair operations.
  void Start(std::filesystem::path path, size_t valid_bytes, bool repair_tail) {
    accepted_ = valid_bytes;
    worker_ = std::thread([this, path = std::move(path), valid_bytes, repair_tail] {
      Run(path, valid_bytes, repair_tail);
    });
  }

  bool TryAppend(uint64_t key, uint64_t vs, uint64_t ps, std::span<const uint8_t> desc) {
    constexpr size_t kHeaderSize = 32;
    if (desc.size() > kPendingLimit - kHeaderSize) return false;
    const size_t size = kHeaderSize + desc.size();
    {
      std::lock_guard lock(mutex_);
      if (!worker_.joinable() || stopping_ || failed_ || size > kPendingLimit - pending_.size() ||
          accepted_ + size > kPipelineJournalLimit) return false;
      const uint32_t head[] = {kPipelineRecordMagic, uint32_t(24 + desc.size())};
      const uint64_t ids[] = {key, vs, ps};
      pending_.resize(pending_.size() + size);
      uint8_t* out = pending_.data() + pending_.size() - size;
      std::memcpy(out, head, sizeof(head));
      std::memcpy(out + sizeof(head), ids, sizeof(ids));
      if (!desc.empty()) std::memcpy(out + kHeaderSize, desc.data(), desc.size());
      accepted_ += size;
    }
    cv_.notify_one();
    return true;
  }

 private:
  static constexpr size_t kPendingLimit = 1024 * 1024;
  static constexpr size_t kFlushBatch = 64 * 1024;

  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
  }

  void Fail() {
    std::lock_guard lock(mutex_);
    failed_ = true;
    pending_.clear();
  }

  void Run(const std::filesystem::path& path, size_t valid_bytes, bool repair_tail) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (!ec && repair_tail) std::filesystem::resize_file(path, valid_bytes, ec);
    if (ec) return Fail();
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return Fail();
    std::vector<uint8_t> batch;
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
        if (pending_.empty()) break;
        // Keep a short coalescing window even for sparse misses; sustained bursts flush early.
        cv_.wait_for(lock, std::chrono::milliseconds(25), [&] { return stopping_ || pending_.size() >= kFlushBatch; });
        batch.swap(pending_);
      }
      out.write(reinterpret_cast<const char*>(batch.data()), std::streamsize(batch.size()));
      out.flush();
      if (!out) return Fail();
      batch.clear();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<uint8_t> pending_;
  size_t accepted_ = 0;
  bool stopping_ = false, failed_ = false;
  std::thread worker_;
};

}  // namespace gtav::native
