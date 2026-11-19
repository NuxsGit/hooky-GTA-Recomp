#pragma once

#include "CheatController.h"
#include "PlayerRegistry.h"
#include "../sandbox/BuildManager.h"

namespace gtav::workshop {

enum class Document : uint8_t { Permissions = 1, Bans = 2, Favorites = 4, Presets = 8 };
struct Config {
  Permissions permissions;
  MapRules map;
  std::vector<Ban> bans;
  std::vector<uint32_t> favorites;
  std::vector<Preset> presets;
};
enum class Op : uint8_t {
  MapMode, PlayerMode, MapRule, BuildRule, Promote, Kick, Ban, Unban, Practice, Freeze,
  Effect, Speed, Heal, Revive, Damage, Weapon, Armory, Cash, Kit, QuickVehicle, Repair, Clean,
  GoTo, Bring, Waypoint, Landmark, Time, PauseClock, Weather, Gravity,
  Place, Duplicate, Delete, Anchor, Undo, Redo, Cleanup, Favorite, SavePreset, LoadPreset, DeletePreset, Count
};
struct Command {
  Op op = Op::Count;
  PlayerId target{};
  uint32_t value{};
  PieceId piece{};
  bool on{};
  std::string text{};
  std::optional<Placement> placement{};
  // A verified host transport may propose a placement in world coordinates. It is constrained
  // to the authenticated actor's observed reach. The desktop UI uses a live camera instead.
  std::optional<Pose> proposed{};
};
struct Destination { const char* name; Vec3 at; };
std::span<const Destination> Destinations();
std::span<const char* const> WeatherNames();

struct Preview {
  uint32_t model{};
  Placement opts;
  uint64_t epoch{};
};
struct PlayerView {
  PlayerId id{};
  std::string name;
  Seat seat{};
  Role role{};
  Mode mode{};
  std::optional<Mode> override_mode;
  bool ready{}, frozen{};
  uint32_t wallet{};
};
struct CoreView {
  bool authority{}, playable{}, build{}, admin{}, practice{}, busy{}, undo{}, redo{};
  unsigned limit{}, count{}, total{};
  Capabilities caps;
  MapRules map;
  bool players_build{};
  CheatState cheats;
  std::vector<PlayerView> players;
  std::vector<Ban> bans;
  std::vector<uint32_t> favorites, recent;
  std::vector<Preset> presets;
  PieceId aimed{};
  uint32_t aimed_model{};
  bool aimed_locked{};
  std::optional<Pose> projection;
  bool ghost{}, clock_frozen{}, low_gravity{};
  std::optional<unsigned> hour;
  BuildResult result = BuildResult::Okay;
};

// Single-writer authoritative core. A local mailbox binds kLocalOwner; a host transport can
// bind its own authenticated connection to a PlayerId via Admit. No UI types, sockets, PPC
// contexts, or trust in self-declared roles. Clients receive a view, not a mutable core.
class WorkshopCore {
 public:
  explicit WorkshopCore(Config config = {}, bool practice = false,
                        std::function<void(std::string_view)> audit = {},
                        std::function<void(PlayerId, std::string_view)> disconnect = {});
  // Queued actions retain this authority's address; the host owns it for the session.
  WorkshopCore(const WorkshopCore&) = delete;
  WorkshopCore& operator=(const WorkshopCore&) = delete;
  PlayerId Admit(std::string_view verified_identity, std::string_view name);
  bool Bind(PlayerId actor, Entity entity) { return players_.Bind(actor, entity); }
  void Disconnect(PlayerId, WorkshopGame&);
  BuildResult Execute(PlayerId bound_actor, const Command&, WorkshopGame&, uint64_t frame);
  void BeginFrame(WorkshopGame&, bool authoritative, bool playable);
  void Tick(WorkshopGame&, uint64_t frame, bool authoritative, bool playable, bool can_stream,
            bool ui_capture, const std::optional<Preview>&, Motion);
  CoreView View(PlayerId actor) const;
  Config Export() const;
  uint8_t TakeDirty();
  bool WorkPending() const;
  bool HoldingModel() const { return spawns_.HoldingModel(); }
  bool CanBuild(PlayerId actor) const;
  Role RoleOf(PlayerId actor) const;
 private:
  struct Travel { PlayerId actor, mover, reference; Entity ped, entity, other; Vec3 to; unsigned probe{}; uint64_t start; };
  struct Loadout { PlayerId actor, target; Entity ped; size_t at{}, given{}; };
  bool Ready(WorkshopGame&, const Player&) const;
  bool CanAdmin(PlayerId actor) const;
  bool CanTarget(PlayerId actor, PlayerId target) const;
  void Audit(const Player& actor, std::string_view action, std::string_view target, std::string_view reason = {});
  void Dirty(Document d) { dirty_ |= uint8_t(d); }
  void PreviewTick(WorkshopGame&, uint64_t frame, const std::optional<Preview>&);
  void StopGhost(WorkshopGame&);
  void StreamTravel(WorkshopGame&, uint64_t frame);
  void GiveNext(WorkshopGame&);
  BuildResult TravelTo(PlayerId actor, PlayerId mover, PlayerId reference, Vec3 to, WorkshopGame&, uint64_t frame);
  BuildResult Build(PlayerId actor, const Command&, WorkshopGame&, uint64_t frame);
  BuildResult SpawnPractice(PlayerId actor, WorkshopGame&, uint64_t frame);
  void Reconcile(WorkshopGame&, bool playable, Motion motion);
  PermissionManager permissions_;
  GamemodeManager mode_;
  PlayerRegistry players_;
  CheatController cheats_;
  BuildManager build_;
  EntitySpawnQueue spawns_;
  std::function<void(std::string_view)> audit_;
  std::function<void(PlayerId, std::string_view)> disconnect_;
  std::vector<Entity> retired_;
  std::optional<Loadout> loadout_;
  std::optional<Entity> warp_ped_;
  std::optional<Travel> travel_;
  std::optional<Preview> preview_;
  std::optional<Pose> projection_;
  Entity ghost_;
  bool ghost_pending_ = false;
  uint64_t ghost_version_ = 0, ghost_retry_ = 0;
  PieceId aimed_ = 0;
  uint64_t build_revision_ = 0;
  Capabilities caps_;
  std::optional<unsigned> hour_;
  std::optional<uint64_t> last_frame_;
  uint8_t dirty_ = 0;
  bool authoritative_ = false, playable_ = false, practice_ = false;
  bool clock_frozen_ = false, low_gravity_ = false, weather_override_ = false, police_disabled_ = false;
  BuildResult result_ = BuildResult::Okay;
};
}  // namespace gtav::workshop
