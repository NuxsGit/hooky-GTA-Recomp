#include "native/pipeline_cache_io.h"
#include "native/pipeline_completion.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace gtav::native;

static void Check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

int main() {
  // Visit: last record wins, partial tail removed.
  {
    std::vector<uint8_t> data;
    auto append = [&](uint64_t key, uint64_t vs, uint64_t ps, std::string_view bytes) {
      uint32_t head[2] = {kPipelineRecordMagic, uint32_t(24 + bytes.size())};
      uint64_t ids[3] = {key, vs, ps};
      size_t at = data.size();
      data.resize(at + 32 + bytes.size());
      std::memcpy(data.data() + at, head, sizeof(head));
      std::memcpy(data.data() + at + 8, ids, sizeof(ids));
      if (!bytes.empty()) std::memcpy(data.data() + at + 32, bytes.data(), bytes.size());
    };
    append(0x1, 0xAAA, 0xBBB, "first");
    append(0x1, 0xAAA, 0xBBB, "second");
    append(0x2, 0x111, 0x222, "ok");
    // partial tail: header says 24 but no payload/padding
    uint32_t tail[2] = {kPipelineRecordMagic, 24};
    data.insert(data.end(), reinterpret_cast<uint8_t*>(tail), reinterpret_cast<uint8_t*>(tail) + 8);
    data.push_back(0xFF); // incomplete
    int count = 0;
    std::string second;
    size_t valid = VisitPipelineRecords(data, [&](uint64_t key, uint64_t vs, uint64_t ps, std::span<const uint8_t> bytes) {
      ++count;
      if (key == 0x1 && second.empty()) second = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      // caller deduplicates: last wins, so we expect two visits for key 0x1
    });
    Check(count == 3, "visit count");
    Check(second == "first", "first visit of duplicate");
    Check(valid == data.size() - 9, "valid stops before partial tail");
    // oversize header should stop
    std::vector<uint8_t> bad;
    uint32_t h[2] = {kPipelineRecordMagic, 100000};
    bad.resize(32);
    std::memcpy(bad.data(), h, sizeof(h));
    valid = VisitPipelineRecords(bad, [&](auto, auto, auto, auto){ Check(false, "bad should not visit"); });
    Check(valid == 0, "oversize header stops");
  }
  // ReadCacheBytes limits
  {
    auto tmp = std::filesystem::temp_directory_path() / "gtav_pipeline_limit.bin";
    {
      std::vector<uint8_t> big(kDriverCacheLimit + 1, 0xAB);
      std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
      std::fwrite(big.data(), 1, big.size(), f);
      std::fclose(f);
    }
    auto r = ReadCacheBytes(tmp, kDriverCacheLimit);
    Check(!r.has_value(), "oversized returns nullopt");
    std::filesystem::remove(tmp);
    auto missing = ReadCacheBytes(tmp, kDriverCacheLimit);
    Check(missing && missing->empty(), "missing returns empty");
  }
  // PipelineCompletion budget sharing
  {
    PipelineCompletion c;
    double waited = 0;
    Check(c.Wait(0, waited) == false && waited == 0, "zero budget does not wait");
    std::thread t([&]{ std::this_thread::sleep_for(std::chrono::milliseconds(20)); c.Signal(); });
    double w2 = 0;
    // Budget must leave generous headroom for slow/loaded runners (e.g. CI macOS runners),
    // otherwise a 20 ms signaler that has simply been scheduled late can time out and flake.
    bool ok = c.Wait(500, w2);
    Check(ok && w2 >= 10 && w2 <= 450, "wait measured");
    double w3 = 0;
    Check(c.Wait(10, w3) && w3 == 0, "ready is lock-free");
    t.join();
  }
  // Writer coalesces and respects limits
  {
    auto dir = std::filesystem::temp_directory_path() / "gtav_pipeline_writer";
    auto file = dir / "pipelines.bin";
    std::filesystem::remove_all(dir);
    PipelineRecordWriter w;
    std::vector<uint8_t> seed(100, 0x11);
    uint32_t head[2] = {kPipelineRecordMagic, uint32_t(24 + seed.size())};
    uint64_t ids[3] = {0xDEAD, 0xAAA, 0xBBB};
    std::vector<uint8_t> rec(32 + seed.size());
    std::memcpy(rec.data(), head, sizeof(head));
    std::memcpy(rec.data() + 8, ids, sizeof(ids));
    std::memcpy(rec.data() + 32, seed.data(), seed.size());
    std::filesystem::create_directories(dir);
    {
      std::FILE* f = std::fopen(file.string().c_str(), "wb");
      std::fwrite(rec.data(), 1, rec.size(), f);
      std::fwrite(rec.data(), 1, 5, f); // partial tail
      std::fclose(f);
    }
    size_t valid = VisitPipelineRecords(*ReadCacheBytes(file, kPipelineJournalLimit), [&](auto,auto,auto,auto){});
    Check(valid == rec.size(), "valid size");
    w.Start(file, valid, true);
    std::vector<uint8_t> desc(200, 0x22);
    Check(w.TryAppend(0xBEEF, 0x1, 0x2, desc), "append ok");
    // oversize desc should be rejected
    std::vector<uint8_t> huge(2 * 1024 * 1024, 0);
    Check(!w.TryAppend(0xCAFE, 0x1, 0x2, huge), "huge rejected");
    // let writer flush
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    // destructor joins
  }
  std::puts("pipeline cache and completion: visit, limits, wait and writer checks passed");
  return 0;
}
