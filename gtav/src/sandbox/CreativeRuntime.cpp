#include "CreativeRuntime.h"

#include "desktop.h"
#include "../utils/ConfigStorage.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace gtav::workshop {
namespace {
struct Queued { Command cmd; int64_t at; uint64_t capture_epoch, world_epoch; };
class StorageWriter {
 public:
  StorageWriter(ConfigStorage storage, std::function<void(uint8_t)> failed)
      : storage_(std::move(storage)), error_(storage_.Unreadable()), failed_(std::move(failed)),
        thread_([this] { Run(); }) {}
  ~StorageWriter() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_one();
    thread_.join();  // any queued write finishes before shutdown returns
  }
  void Post(Config config, uint8_t dirty) {
    if (!dirty) return;
    {
      std::lock_guard lock(mutex_);
      pending_ = std::move(config);
      documents_ |= dirty;
    }
    cv_.notify_one();
  }
  uint8_t Error() const { return error_; }
 private:
  void Run() {
    for (;;) {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
      if (!pending_) return;
      const auto flags = std::exchange(documents_, 0);
      auto cfg = std::exchange(pending_, {});
      lock.unlock();
      const auto failed = storage_.Save(*cfg, flags);
      error_ = (error_.load() & ~flags) | failed;
      if (failed && failed_) failed_(failed);
    }
  }
  ConfigStorage storage_;
  std::atomic<uint8_t> error_;
  std::function<void(uint8_t)> failed_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<Config> pending_;
  uint8_t documents_{};
  bool stopping_ = false;
  std::thread thread_;
};

struct Runtime {
  std::mutex lifecycle;  // Stop never destroys a core while GuestFrame is using it.
  std::mutex input;
  std::mutex publication;
  std::unique_ptr<WorkshopCore> core;
  std::unique_ptr<StorageWriter> writer;
  std::deque<Queued> inbox;
  std::optional<Preview> preview;
  std::shared_ptr<const DesktopView> view = std::make_shared<const DesktopView>();
  void Publish(std::shared_ptr<const DesktopView> next) {
    { std::lock_guard lock(publication); view.swap(next); }  // release old snapshots outside the lock
  }
  std::atomic<unsigned> keys{0}, capture{0};  // 1 = Build, 2 = Phone, 4 = another overlay
  std::atomic<bool> pending{false}, ready{false}, work{false};
  uint64_t epoch{}, world_epoch{};  // protected by input mutex
  bool last_authority{};
  Entity last_local;
  int64_t next_view{};
};
Runtime& State() { static Runtime state; return state; }
}  // namespace

void Start(const std::filesystem::path& profile, bool practice, RuntimeLog log) {
  auto& s = State();
  std::lock_guard lock(s.lifecycle);
  if (s.core) return;
  ConfigStorage storage(profile);
  auto cfg = storage.Load();
  auto defaults = storage.NeedsDefaults();
  s.core = std::make_unique<WorkshopCore>(cfg, practice, std::move(log.audit));
  s.writer = std::make_unique<StorageWriter>(std::move(storage), std::move(log.storage_failed));
  s.writer->Post(std::move(cfg), defaults);
  auto initial = std::make_shared<DesktopView>();
  initial->storage_error = s.writer->Error();
  s.Publish(std::move(initial));
  s.ready = true;
}
void Stop() {
  auto& s = State();
  std::lock_guard lock(s.lifecycle);
  s.ready = false;
  SetCapture(false, false);
  s.core.reset();
  s.writer.reset();  // flush before the game profile lock is released
  s.Publish(std::make_shared<const DesktopView>());
  s.work = false;
  s.next_view = 0;
}
std::shared_ptr<const DesktopView> Snapshot() {
  auto& s = State();
  std::lock_guard lock(s.publication);
  return s.view;
}
bool Fresh(const DesktopView& v) { return v.sampled_at && sandbox::NowMs() - v.sampled_at <= 900; }
bool LocalAuthority() {
  if (sandbox::CurrentMode() == sandbox::Mode::Solo) return true;
  if (sandbox::CurrentMode() != sandbox::Mode::Multiplayer) return false;
  const auto link = sandbox::Party().LinkState();
  return link == sandbox::Link::Idle || link == sandbox::Link::Hosting || link == sandbox::Link::Failed;
}
bool BuildAllowed() {
  const auto v = Snapshot();
  return v->build && Fresh(*v) && LocalAuthority();
}
bool Submit(Command c, bool build_panel) {
  auto& s = State();
  if (!s.ready || !LocalAuthority()) return false;
  std::lock_guard lock(s.input);
  if (!s.ready || s.inbox.size() >= 32 || !(s.capture & (build_panel ? 1u : 2u))) return false;
  s.inbox.push_back({std::move(c), sandbox::NowMs(), s.epoch, s.world_epoch});
  s.pending = true;
  return true;
}
void SetCapture(bool build, bool phone, bool other) {
  auto& s = State();
  std::lock_guard lock(s.input);
  s.capture = build ? 1u : phone ? 2u : other ? 4u : 0u;
  ++s.epoch;
  s.preview.reset();
  s.keys = 0;
  // Overlay actions from the previous panel must not follow the next mouse/key focus.
  s.inbox.clear();
  s.pending = false;
}
bool ActiveCapture() { return (State().capture.load() & 3) != 0; }
void SetPreview(uint32_t model, Placement placement) {
  auto& s = State();
  std::lock_guard lock(s.input);
  if (s.capture & 1) s.preview = Preview{model, placement, s.epoch};
}
bool FlightKey(unsigned bit, bool down) {
  auto& s = State();
  if (!s.ready || bit >= 6) return false;
  if (!down) s.keys.fetch_and(~(1u << bit), std::memory_order_relaxed);
  if (s.capture) return false;
  const auto v = Snapshot();
  if ((!v->cheats.Has(Effect::Fly) && !v->cheats.Has(Effect::Noclip)) || !Fresh(*v) || !LocalAuthority()) return false;
  if (down) {
    std::lock_guard lock(s.input);
    if (s.capture || !s.ready) return false;
    s.keys.fetch_or(1u << bit, std::memory_order_relaxed);
  }
  return true;
}
void ResetInput() { State().keys = 0; }
bool WantsFrame() {
  const auto& s = State();
  return s.ready && (ActiveCapture() || s.pending || s.work);
}
bool GuestFrame(WorkshopGame& game, Entity local, uint64_t frame, bool can_stream) {
  auto& s = State();
  if (!s.ready) return false;
  std::lock_guard lock(s.lifecycle);
  if (!s.core) return false;
  const bool authority = LocalAuthority();
  auto& core = *s.core;
  core.Bind(kLocalOwner, authority ? local : Entity{});
  uint64_t epoch, world_epoch;
  std::optional<Preview> preview;
  std::deque<Queued> commands;
  {
    std::lock_guard input(s.input);
    epoch = s.epoch;
    if (s.last_local != local || s.last_authority != authority) {
      ++s.world_epoch;
      s.last_local = local;
      s.last_authority = authority;
      s.keys = 0;
    }
    world_epoch = s.world_epoch;
    preview = s.preview;
    for (unsigned i = 0; i < 4 && !s.inbox.empty(); ++i) {
      commands.push_back(std::move(s.inbox.front()));
      s.inbox.pop_front();
    }
    s.pending = !s.inbox.empty();
  }
  core.BeginFrame(game, authority, bool(local));
  const auto now = sandbox::NowMs();
  if (authority) for (const auto& q : commands) {
    if (now - q.at > 2000 || q.capture_epoch != epoch || q.world_epoch != world_epoch || !LocalAuthority()) continue;
    { std::lock_guard input(s.input); if (q.capture_epoch != s.epoch) break; }
    core.Execute(kLocalOwner, q.cmd, game, frame);
  }
  // A mode/revocation command enqueued in time for this frame wins over a loaded model.
  const bool still_authority = authority && LocalAuthority();
  unsigned capture, keys;
  {
    std::lock_guard input(s.input);
    if (epoch != s.epoch) preview.reset();
    capture = s.capture;
    // Input may have been cleared by focus loss while the guest prepared this frame.
    keys = capture || !still_authority || !local ? 0 : s.keys.load();
  }
  const auto axis = [=](unsigned plus, unsigned minus) { return float(int((keys >> plus) & 1) - int((keys >> minus) & 1)); };
  core.Tick(game, frame, still_authority, bool(local), can_stream, (capture & 3) != 0, preview,
            {axis(0, 1), axis(3, 2), axis(4, 5)});
  if (const auto dirty = core.TakeDirty()) s.writer->Post(core.Export(), dirty);
  if (now >= s.next_view || !commands.empty()) {
    auto v = std::make_shared<DesktopView>();
    static_cast<CoreView&>(*v) = core.View(kLocalOwner);
    v->sampled_at = now;
    v->storage_error = s.writer->Error();
    if (!v->cheats.Has(Effect::Fly) && !v->cheats.Has(Effect::Noclip)) s.keys = 0;
    s.Publish(std::move(v));
    s.next_view = now + 32;
  }
  s.work = core.WorkPending();
  return core.HoldingModel();
}
}  // namespace gtav::workshop
