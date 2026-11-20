#include "EntitySpawnQueue.h"

#include <utility>

namespace gtav::workshop {
bool EntitySpawnQueue::Push(SpawnSpec spec, uint64_t frame, std::function<bool()> allowed,
                            std::function<void(SpawnResult, Entity)> done) {
  if (jobs_.size() + size_t(HoldingModel()) >= kCapacity || !allowed || !done || !spec.model || !spec.pose.Valid() ||
      (spec.kind != Kind::Prop && spec.kind != Kind::Vehicle && spec.kind != Kind::Ped)) return false;
  jobs_.push_back({spec, std::move(allowed), std::move(done), frame});
  return true;
}
void EntitySpawnQueue::Complete(SpawnBackend& game, SpawnResult result, Entity entity) {
  auto job = std::move(*current_);
  current_.reset();
  game.ReleaseModel(job.spec.model);
  job.done(result, entity);
}
void EntitySpawnQueue::Tick(SpawnBackend& game, uint64_t frame, bool available, bool can_stream) {
  const auto expired = [&](const Job& job) { return frame < job.queued_at || frame - job.queued_at >= kTimeoutFrames; };
  if (current_ && (!available || !current_->allowed())) Complete(game, SpawnResult::Cancelled);
  if (current_ && expired(*current_)) Complete(game, SpawnResult::Timeout);
  // Bound cancellation even when rendering is stopped. Callbacks may enqueue new work, so pop first.
  for (size_t n = jobs_.size(); n && !jobs_.empty(); --n) {
    auto& queued = jobs_.front();
    if (available && queued.allowed() && !expired(queued)) break;
    auto job = std::move(queued);
    jobs_.pop_front();
    job.done(!available || !job.allowed() ? SpawnResult::Cancelled : SpawnResult::Timeout, {});
  }
  if (!available || !can_stream || (last_tick_ && *last_tick_ == frame)) return;
  last_tick_ = frame;
  if (current_) {
    switch (game.PollModel(current_->spec.model)) {
      case Load::Waiting: return;
      case Load::Rejected: Complete(game, SpawnResult::Rejected); return;
      case Load::Ready: break;
    }
    if (!current_->allowed()) { Complete(game, SpawnResult::Cancelled); return; }
    const auto entity = game.Create(current_->spec);
    const bool okay = entity && entity.kind == current_->spec.kind && game.Exists(entity);
    Complete(game, okay ? SpawnResult::Created : SpawnResult::Rejected, okay ? entity : Entity{});
  } else if (!jobs_.empty()) {
    auto job = std::move(jobs_.front());
    jobs_.pop_front();
    if (game.RequestModel(job.spec)) current_ = std::move(job);
    else job.done(SpawnResult::Rejected, {});
  }
}
void EntitySpawnQueue::Cancel(SpawnBackend& game) {
  auto queued = std::exchange(jobs_, {});
  if (current_) Complete(game, SpawnResult::Cancelled);
  for (auto& job : queued) job.done(SpawnResult::Cancelled, {});
}
}  // namespace gtav::workshop
