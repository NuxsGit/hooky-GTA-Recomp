#pragma once

#include "WorkshopTypes.h"

#include <deque>
#include <functional>
#include <optional>

namespace gtav::workshop {
struct SpawnSpec {
  uint32_t model{};
  Kind kind = Kind::Prop;
  Pose pose;
  bool locked = false;
  bool ghost = false;
  bool operator==(const SpawnSpec&) const = default;
};
enum class Load { Waiting, Ready, Rejected };
enum class SpawnResult { Created, Rejected, Timeout, Cancelled };
class SpawnBackend {
 public:
  virtual ~SpawnBackend() = default;
  virtual bool RequestModel(const SpawnSpec&) = 0;
  virtual Load PollModel(uint32_t model) = 0;
  virtual Entity Create(const SpawnSpec&) = 0;
  virtual bool Exists(Entity) = 0;
  virtual bool Destroy(Entity) = 0;
  virtual void ReleaseModel(uint32_t model) = 0;
};
class EntitySpawnQueue {
 public:
  static constexpr uint64_t kTimeoutFrames = 300;
  static constexpr size_t kCapacity = 48;
  struct Job {
    SpawnSpec spec;
    // Both authorization and the command's lifetime are checked again after streaming.
    std::function<bool()> allowed;
    std::function<void(SpawnResult, Entity)> done;
    uint64_t queued_at;
  };
  bool Push(SpawnSpec spec, uint64_t frame, std::function<bool()> allowed,
            std::function<void(SpawnResult, Entity)> done);
  void Tick(SpawnBackend&, uint64_t frame, bool available, bool can_stream = true);
  void Cancel(SpawnBackend&);
  bool Busy() const { return !jobs_.empty() || current_.has_value(); }
  bool HoldingModel() const { return current_.has_value(); }
 private:
  void Complete(SpawnBackend&, SpawnResult, Entity = {});
  std::deque<Job> jobs_;
  std::optional<Job> current_;
  std::optional<uint64_t> last_tick_;
};
}  // namespace gtav::workshop
