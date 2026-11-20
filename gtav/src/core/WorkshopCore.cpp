#include "WorkshopCore.h"

#include <array>
#include <utility>

namespace gtav::workshop {
namespace {
constexpr Destination kDestinations[] = {
  {"LSIA terminal", {-1034.f, -2733.f, 20.f}}, {"Legion Square", {195.f, -934.f, 32.f}},
  {"Del Perro pier", {-1610.f, -1018.f, 13.f}}, {"Vinewood sign", {711.f, 1199.f, 349.f}},
  {"Sandy Shores airfield", {1740.f, 3283.f, 42.f}}, {"Mount Chiliad lookout", {493.f, 5588.f, 797.f}},
  {"Paleto Bay", {-152.f, 6329.f, 33.f}}, {"Observatory", {-421.f, 1160.f, 326.f}},
};
constexpr const char* kWeather[] = {"Sunny", "Clear", "Clouds", "Rain", "Thunder", "Snow", "Halloween"};
constexpr const char* kMap = "Workshop map";
constexpr bool IsBuild(Op op) { return op >= Op::Place && op <= Op::DeletePreset; }
constexpr bool CashAmount(uint32_t value) { return value == 1000 || value == 50000 || value == 1000000; }
BuildResult Done(bool applied) { return applied ? BuildResult::Okay : BuildResult::Failed; }
}
std::span<const Destination> Destinations() { return kDestinations; }
std::span<const char* const> WeatherNames() { return kWeather; }
WorkshopCore::WorkshopCore(Config config, bool practice, std::function<void(std::string_view)> audit,
                           std::function<void(PlayerId, std::string_view)> disconnect)
    : permissions_(std::move(config.permissions), std::move(config.bans)), mode_(config.map),
      build_(std::move(config.favorites), std::move(config.presets)), audit_(std::move(audit)),
      disconnect_(std::move(disconnect)), practice_(practice) {}
PlayerId WorkshopCore::Admit(std::string_view identity, std::string_view name) {
  return authoritative_ ? players_.Admit(permissions_, Seat::Remote, identity, name) : 0;
}
void WorkshopCore::Disconnect(PlayerId id, WorkshopGame& game) {
  if (const auto* player = players_.Find(id); player && id != kLocalOwner) {
    if (player->seat == Seat::Simulated && player->entity) retired_.push_back(player->entity);
    else if (player->frozen && game.Exists(player->entity)) game.Anchor(player->entity, false);
    cheats_.Forget(game, id);
    mode_.Forget(id);
    build_.Forget(id);
    players_.Remove(id);
  }
}
Role WorkshopCore::RoleOf(PlayerId id) const {
  if (const auto* p = players_.Find(id); authoritative_ && p) return permissions_.Get(p->id, p->identity);
  return Role::Player;
}
bool WorkshopCore::CanAdmin(PlayerId actor) const { return PermissionManager::AdminOS(RoleOf(actor)); }
bool WorkshopCore::CanTarget(PlayerId actor, PlayerId target) const {
  return players_.Find(actor) && players_.Find(target) && PermissionManager::Target(RoleOf(actor), RoleOf(target), actor == target);
}
bool WorkshopCore::CanBuild(PlayerId actor) const {
  return authoritative_ && playable_ && players_.Find(actor) &&
    PermissionManager::Build(RoleOf(actor), mode_.Effective(actor), permissions_.Settings().players_build);
}
bool WorkshopCore::Ready(WorkshopGame& game, const Player& target) const {
  return playable_ && target.entity && game.Exists(target.entity);
}
void WorkshopCore::Audit(const Player& actor, std::string_view action, std::string_view target, std::string_view reason) {
  if (!audit_) return;
  std::string line = UtcNow();
  line += " [ADMIN AUDIT] "; line += actor.name; line += " -> "; line += action; line += " -> "; line += target;
  if (!reason.empty()) { line += " | "; line += reason; }
  audit_(line);
}

BuildResult WorkshopCore::SpawnPractice(PlayerId actor, WorkshopGame& game, uint64_t frame) {
  if (!practice_ || actor != kLocalOwner || !caps_.Has(Feature::Ped)) return BuildResult::Invalid;
  const auto* local = players_.Find(actor);
  const auto anchor = game.Locate(local->entity);
  if (!anchor) return BuildResult::Missing;
  if (retired_.size() >= 6 || spawns_.Busy()) return BuildResult::Busy;
  constexpr std::string_view names[] = {"Alex", "Noa", "Sam"};
  constexpr std::string_view identities[] = {"sim:alex", "sim:noa", "sim:sam"};
  constexpr std::string_view models[] = {"a_m_y_skater_01", "a_f_y_hiker_01", "s_m_y_cop_01"};
  unsigned pending = 0;
  for (unsigned i = 0; i < std::size(names); ++i) {
    if (permissions_.IsBanned(identities[i])) continue;
    const auto* existing = players_.FindIdentity(identities[i]);
    const auto id = existing ? existing->id : players_.Admit(permissions_, Seat::Simulated, identities[i], names[i]);
    if (!id || (existing && game.Exists(existing->entity))) continue;
    players_.Bind(id, {});
    if (spawns_.Push({ModelHash(models[i]), Kind::Ped, Compose(*anchor, {{(float(i)-1.f)*2.f, 5.f, .15f}, 180.f})}, frame,
        [this, id, actor] { return playable_ && caps_.Has(Feature::Ped) && CanAdmin(actor) && players_.Find(id); },
        [this, id](SpawnResult r, Entity ped) {
          if (r == SpawnResult::Created) {
            if (!players_.Bind(id, ped)) retired_.push_back(ped);
          } else result_ = r == SpawnResult::Timeout ? BuildResult::Timeout : BuildResult::Failed;
        })) ++pending;
  }
  return pending ? BuildResult::Queued : BuildResult::Missing;
}

BuildResult WorkshopCore::Build(PlayerId actor, const Command& c, WorkshopGame& game, uint64_t frame) {
  if (!CanBuild(actor)) return BuildResult::Denied;
  const auto* p = players_.Find(actor);
  const auto* piece = c.piece ? build_.Find(c.piece) : nullptr;
  if (c.piece && !piece) return BuildResult::Missing;
  if (piece && piece->owner != actor) return BuildResult::Denied;
  if (piece) {
    const auto from = game.Locate(p->entity), at = game.Locate(piece->entity);
    if (!from || !at || (at->at - from->at).Dot(at->at - from->at) > 65.f*65.f) return BuildResult::Missing;
  }
  const auto allowed = [this, actor, revision = build_revision_, ped = p->entity] {
    const auto* user = players_.Find(actor);
    return CanBuild(actor) && revision == build_revision_ && user && user->entity == ped && bool(ped);
  };
  const auto limit = permissions_.Settings().prop_limit;
  switch (c.op) {
    case Op::Undo: case Op::Redo: return build_.Undo(actor, c.op == Op::Redo, limit, game, spawns_, frame, allowed);
    case Op::Delete: return build_.Delete(actor, c.piece, game);
    case Op::Anchor:
      return caps_.Has(Feature::Anchor) ? build_.Lock(actor, c.piece, c.on, game) : BuildResult::Invalid;
    case Op::Cleanup: {
      const auto outcome = build_.Cleanup(actor, allowed);
      if (outcome == BuildResult::Queued) Audit(*p, "Cleanup", p->name);
      return outcome;
    }
    case Op::Favorite: case Op::SavePreset: case Op::DeletePreset:
      // These are local preferences/document edits, not shared map state.
      if (p->seat != Seat::Local) return BuildResult::Denied;
      if (c.op == Op::Favorite && build_.ToggleFavorite(c.value)) { Dirty(Document::Favorites); return BuildResult::Okay; }
      if ((c.op == Op::SavePreset && build_.SavePreset(std::string(Trim(c.text)), actor, c.piece, game)) ||
          (c.op == Op::DeletePreset && build_.DeletePreset(c.text))) { Dirty(Document::Presets); return BuildResult::Okay; }
      return BuildResult::Invalid;
    default: break;
  }
  if (!Ready(game, *p)) return BuildResult::Missing;
  std::optional<Pose> pose;
  if (p->seat == Seat::Local && c.placement) {
    if (const auto ray = game.Camera(c.placement->lens)) pose = Project(game, *ray, *c.placement);
  } else if (p->seat != Seat::Local && c.proposed && c.proposed->Valid()) pose = c.proposed;
  if (!pose) return BuildResult::Missing;
  std::vector<SpawnSpec> items;
  if (c.op == Op::LoadPreset) {
    if (p->seat != Seat::Local) return BuildResult::Denied;
    for (const auto& preset : build_.Presets()) if (preset.name == c.text) {
      for (const auto& saved : preset.pieces) {
        const auto* model = FindModel(saved.model);
        if (!model) return BuildResult::Invalid;
        items.push_back({saved.model, model->kind, Compose(*pose, saved.relative), saved.locked});
      }
      break;
    }
  } else {
    const auto* model = FindModel(c.op == Op::Duplicate && piece ? piece->spec.model : c.value);
    if (!model || (c.op != Op::Place && (c.op != Op::Duplicate || !piece))) return BuildResult::Invalid;
    items.push_back({model->hash, model->kind, *pose, c.placement ? c.placement->lock : c.on});
  }
  const auto at = game.Locate(p->entity);
  for (const auto& item : items) {
    if (!at || !item.pose.Valid() || !caps_.Has(ForKind(item.kind)) || (item.locked && !caps_.Has(Feature::Anchor)) ||
        (item.pose.at - at->at).Dot(item.pose.at - at->at) > 90.f*90.f) return BuildResult::Invalid;
  }
  return build_.Place(actor, std::move(items), limit, spawns_, frame, allowed);
}

BuildResult WorkshopCore::TravelTo(PlayerId actor, PlayerId mover, PlayerId reference, Vec3 to,
                                   WorkshopGame& game, uint64_t frame) {
  if (travel_) return BuildResult::Busy;
  const auto* m = players_.Find(mover);
  const auto* other = players_.Find(reference);
  if (!m || !Ready(game, *m) || !to.Valid() || (reference && (!other || !Ready(game, *other)))) return BuildResult::Missing;
  if (!caps_.Has(Feature::Teleport)) return BuildResult::Invalid;
  const auto entity = game.TravelEntity(m->entity, m->seat == Seat::Local);
  if (!entity || !game.Exists(*entity)) return BuildResult::Missing;
  if (!caps_.Has(Feature::Ground)) {
    // The solo practice roster supports short on-foot moves near the local player without the
    // optional streaming ABI. Never use this fallback for arbitrary coordinates or vehicles.
    const auto from = game.Locate(m->entity);
    const auto dest = other ? game.Locate(other->entity) : std::nullopt;
    if (!other || (m->seat != Seat::Simulated && other->seat != Seat::Simulated) || !from || !dest ||
        *entity != m->entity || std::abs(dest->at.z - from->at.z) > 6.f ||
        (dest->at - from->at).Dot(dest->at - from->at) > 35.f*35.f) return BuildResult::Invalid;
    const auto near = dest->at + Vec3{1.f, 1.f, .45f};
    return Done(near.Valid() && game.Relocate(*entity, near));
  }
  travel_ = Travel{actor, mover, reference, m->entity, *entity, other ? other->entity : Entity{}, to, 0, frame};
  return BuildResult::Queued;
}

BuildResult WorkshopCore::Execute(PlayerId actor, const Command& c, WorkshopGame& game, uint64_t frame) {
  const auto run = [&]() -> BuildResult {
    if (!authoritative_ || !players_.Find(actor)) return BuildResult::Denied;
    if (c.text.size() > 160) return BuildResult::Invalid;
    if (IsBuild(c.op)) return Build(actor, c, game, frame);
    if (!CanAdmin(actor)) return BuildResult::Denied;
    const auto role = RoleOf(actor);
    auto* origin = players_.Find(actor);
    const PlayerId id = c.target ? c.target : actor;
    auto* target = players_.Find(id);
    const auto target_role = RoleOf(id);
    const auto reason = Trim(c.text);
    switch (c.op) {
      case Op::MapMode:
        if (c.value > unsigned(Mode::Survival) || !mode_.SetDefault(role, Mode(c.value))) return BuildResult::Invalid;
        Dirty(Document::Permissions); Audit(*origin, "Map mode", kMap, Label(Mode(c.value))); return BuildResult::Okay;
      case Op::MapRule:
        if (c.value >= unsigned(Rule::Count) || !caps_.Has(ForRule(Rule(c.value))) ||
            !mode_.SetRule(role, Rule(c.value), c.on)) return BuildResult::Invalid;
        Dirty(Document::Permissions); return BuildResult::Okay;
      case Op::BuildRule:
        if (!permissions_.SetBuildRules(role, c.on, c.value)) return BuildResult::Invalid;
        ++build_revision_; Dirty(Document::Permissions); return BuildResult::Okay;
      case Op::Unban:
        if (!permissions_.Unban(role, reason)) return BuildResult::Denied;
        Dirty(Document::Bans); Audit(*origin, "Unban", reason); return BuildResult::Okay;
      case Op::Practice: return playable_ ? SpawnPractice(actor, game, frame) : BuildResult::Missing;
      default: break;
    }
    if (!target || !CanTarget(actor, id)) return BuildResult::Denied;
    switch (c.op) {
      case Op::Promote:
        if (role != Role::Owner || id == kLocalOwner || !permissions_.SetAdmin(role, target->identity, c.on)) return BuildResult::Denied;
        Dirty(Document::Permissions); Audit(*origin, c.on ? "Promote" : "Demote", target->name); return BuildResult::Okay;
      case Op::PlayerMode: {
        if (c.value > 2 || !mode_.SetOverride(role, target_role, id, actor == id,
              c.value == 2 ? std::nullopt : std::optional(Mode(c.value)))) return BuildResult::Invalid;
        Audit(*origin, "Player mode", target->name, c.value == 2 ? "Map default" : Label(Mode(c.value)));
        return BuildResult::Okay;
      }
      case Op::Kick: case Op::Ban: {
        if (!PermissionManager::Moderate(role, target_role, actor == id)) return BuildResult::Denied;
        if (!ValidText(reason, 160)) return BuildResult::Invalid;
        if (c.op == Op::Ban) {
          if (!permissions_.AddBan(role, target_role, {target->identity, target->name, std::string(reason), UtcNow()}))
            return BuildResult::Invalid;
          Dirty(Document::Bans);
        }
        Audit(*origin, c.op == Op::Ban ? "Ban" : "Kick", target->name, reason);
        if (target->seat == Seat::Remote && disconnect_) disconnect_(id, reason);
        Disconnect(id, game);
        return BuildResult::Okay;
      }
      case Op::Cash:
        if (!CashAmount(c.value)) return BuildResult::Invalid;
        target->wallet += std::min(c.value, 100'000'000u - target->wallet);
        return BuildResult::Okay;
      default: break;
    }
    if (!Ready(game, *target)) return BuildResult::Missing;
    const Entity ped = target->entity;
    const auto use = [&](Tool tool, uint32_t value = 0u) {
      return caps_.Has(tool) ? Done(game.Perform(tool, ped, value)) : BuildResult::Invalid;
    };
    switch (c.op) {
      case Op::Freeze:
        if (!PermissionManager::Moderate(role, target_role, actor == id)) return BuildResult::Denied;
        if (!caps_.Has(Feature::Anchor) || !game.Anchor(ped, c.on)) return BuildResult::Failed;
        target->frozen = c.on; return BuildResult::Okay;
      case Op::Effect:
        if (actor != id || c.value >= unsigned(Effect::Count) || !caps_.Has(Effect(c.value)) ||
            (c.on && mode_.Effective(actor) != Mode::Creative)) return BuildResult::Denied;
        return Done(cheats_.Toggle(actor, Effect(c.value), c.on));
      case Op::Speed:
        return actor == id && mode_.Effective(actor) == Mode::Creative ? Done(cheats_.Speed(actor, c.value)) : BuildResult::Denied;
      case Op::Heal: return use(Tool::Heal);
      case Op::Revive: return use(Tool::Revive);
      case Op::Damage: return use(Tool::Damage);
      case Op::Weapon: return KnownWeapon(c.value) ? use(Tool::Weapon, c.value) : BuildResult::Invalid;
      case Op::Armory:
        if (!caps_.Has(Tool::Weapon)) return BuildResult::Invalid;
        if (loadout_) return BuildResult::Busy;
        loadout_ = Loadout{actor, id, ped};
        Audit(*origin, "Mass give queued", target->name);
        return BuildResult::Queued;
      case Op::Kit: {
        if (!caps_.Has(Tool::Heal) || !caps_.Has(Tool::Weapon)) return BuildResult::Invalid;
        const bool healed = game.Perform(Tool::Heal, ped);
        const bool given = game.Exists(ped) && game.Perform(Tool::Weapon, ped, ModelHash("weapon_pistol"));
        if (healed && given) Audit(*origin, "Give survival kit", target->name);
        return Done(healed && given);
      }
      case Op::QuickVehicle: {
        const auto* car = FindModel(c.value);
        const auto from = game.Locate(ped);
        if (actor != id || target->seat != Seat::Local || mode_.Effective(actor) != Mode::Creative ||
            !caps_.Has(Feature::Vehicle) || (c.on && !caps_.Has(Tool::Warp)) || !car || car->kind != Kind::Vehicle || !from)
          return BuildResult::Invalid;
        const auto result = build_.Place(actor, {{car->hash, car->kind, Compose(*from, {{0, 6, .5f}, 0})}},
            permissions_.Settings().prop_limit, spawns_, frame, [this, actor, ped, rev = build_revision_] {
              const auto* p = players_.Find(actor);
              return CanBuild(actor) && p && p->entity == ped && rev == build_revision_;
            });
        if (result == BuildResult::Queued && c.on) warp_ped_ = ped;
        return result;
      }
      case Op::Repair: case Op::Clean: {
        const Tool tool = c.op == Op::Repair ? Tool::Repair : Tool::Clean;
        const auto car = game.TravelEntity(ped, target->seat == Seat::Local);
        return car && car->kind == Kind::Vehicle && game.Exists(*car) && caps_.Has(tool)
                   ? Done(game.Perform(tool, *car)) : BuildResult::Missing;
      }
      case Op::GoTo: case Op::Bring: {
        const auto* other = c.op == Op::GoTo ? target : origin;
        const auto at = game.Locate(other->entity);
        return actor != id && at ? TravelTo(actor, c.op == Op::GoTo ? actor : id, other->id,
                      Compose(*at, {{2, 1, .4f}, 0}).at, game, frame) : BuildResult::Missing;
      }
      case Op::Waypoint:
        if (actor == id && origin->seat == Seat::Local && caps_.Has(Feature::Waypoint)) {
          if (auto waypoint = game.Waypoint()) return TravelTo(actor, id, 0, *waypoint, game, frame);
          return BuildResult::Missing;
        }
        return BuildResult::Invalid;
      case Op::Landmark:
        return actor == id && c.value < Destinations().size() ? TravelTo(actor, id, 0, Destinations()[c.value].at, game, frame)
                                                            : BuildResult::Invalid;
      case Op::Time: return c.value < 24 ? use(Tool::Clock, c.value) : BuildResult::Invalid;
      case Op::PauseClock: {
        const auto r = use(Tool::PauseClock, c.on);
        if (r == BuildResult::Okay) clock_frozen_ = c.on;
        return r;
      }
      case Op::Weather: {
        const auto r = c.value < WeatherNames().size() ? use(Tool::Weather, c.value) : BuildResult::Invalid;
        if (r == BuildResult::Okay) weather_override_ = true;
        return r;
      }
      case Op::Gravity: {
        const auto r = use(Tool::Gravity, c.on);
        if (r == BuildResult::Okay) low_gravity_ = c.on;
        return r;
      }
      default: return BuildResult::Invalid;
    }
  };
  result_ = run();
  for (const auto& p : players_.All())
    if (!CanAdmin(p.id) || mode_.Effective(p.id) == Mode::Survival) cheats_.Tick(game, p.id, p.entity, false, {});
  if (preview_ && !CanBuild(kLocalOwner)) StopGhost(game);
  return result_;
}

void WorkshopCore::StopGhost(WorkshopGame& game) {
  ++ghost_version_;
  preview_.reset(); projection_.reset(); aimed_ = 0;
  if (ghost_ && game.Destroy(ghost_)) ghost_ = {};
}
void WorkshopCore::PreviewTick(WorkshopGame& game, uint64_t frame, const std::optional<Preview>& input) {
  if (!input || !CanBuild(kLocalOwner) || !input->opts.Valid()) {
    if (preview_ || ghost_ || ghost_pending_) StopGhost(game);
    return;
  }
  if (!preview_ || preview_->model != input->model || preview_->epoch != input->epoch) {
    StopGhost(game);
    ghost_retry_ = 0;
  }
  preview_ = input;
  const auto ray = game.Camera(input->opts.lens);
  aimed_ = ray ? build_.Aimed(game, *ray, 60.f) : 0;
  projection_ = ray ? Project(game, *ray, input->opts) : std::nullopt;
  const auto* model = FindModel(input->model);
  if (!projection_ || !model || !caps_.Has(Feature::Ghost) || !caps_.Has(ForKind(model->kind))) {
    if (ghost_ && game.Destroy(ghost_)) ghost_ = {};
    return;
  }
  if (ghost_) {
    if (ghost_.kind != model->kind || !game.Exists(ghost_) || !game.MoveGhost(ghost_, *projection_)) {
      if (game.Destroy(ghost_)) ghost_ = {};
    }
    return;
  }
  if (ghost_pending_ || spawns_.Busy() || frame < ghost_retry_) return;
  const auto version = ghost_version_;
  ghost_pending_ = spawns_.Push({model->hash, model->kind, *projection_, true, true}, frame,
      [this, version] { return CanBuild(kLocalOwner) && version == ghost_version_ && projection_.has_value(); },
      [this, frame](SpawnResult r, Entity e) {
        ghost_pending_ = false;
        if (r == SpawnResult::Created) ghost_ = e;
        else ghost_retry_ = frame + 330;
      });
}
void WorkshopCore::StreamTravel(WorkshopGame& game, uint64_t frame) {
  if (!travel_) return;
  auto& job = *travel_;
  const auto* p = players_.Find(job.mover);
  const auto* ref = players_.Find(job.reference);
  if (!playable_ || !CanTarget(job.actor, job.mover) || (job.reference && (!CanTarget(job.actor, job.reference) ||
      !ref || ref->entity != job.other || !Ready(game, *ref))) || !p || p->entity != job.ped || !Ready(game, *p) ||
      game.TravelEntity(p->entity, p->seat == Seat::Local) != job.entity) {
    travel_.reset(); result_ = BuildResult::Cancelled; return;
  }
  if (frame < job.start || frame - job.start >= EntitySpawnQueue::kTimeoutFrames) {
    travel_.reset(); result_ = BuildResult::Timeout; return;
  }
  // No moving the player up/down to force streaming. Wait at the original location until a
  // finite ground result is available, or leave the actor untouched on timeout.
  if (auto coords = game.StreamGround(job.to, job.probe++); coords && coords->Valid()) {
    result_ = Done(game.Exists(job.entity) && game.Relocate(job.entity, *coords));
    travel_.reset();
  }
}
void WorkshopCore::GiveNext(WorkshopGame& game) {
  if (!loadout_) return;
  auto& job = *loadout_;
  const auto* p = players_.Find(job.target);
  if (!p || p->entity != job.ped || !CanTarget(job.actor, job.target) || !Ready(game, *p) || !caps_.Has(Tool::Weapon)) {
    loadout_.reset(); result_ = BuildResult::Cancelled; return;
  }
  if (game.Perform(Tool::Weapon, job.ped, Weapons()[job.at++].hash)) ++job.given;
  if (job.at == Weapons().size()) {
    Audit(*players_.Find(job.actor), "Mass give completed", p->name, std::to_string(job.given) + " valid weapons");
    result_ = Done(job.given != 0);
    loadout_.reset();
  }
}
void WorkshopCore::Reconcile(WorkshopGame& game, bool playable, Motion motion) {
  for (const auto& p : players_.All())
    cheats_.Tick(game, p.id, p.entity, playable && CanAdmin(p.id) && mode_.Effective(p.id) == Mode::Creative,
                 p.seat == Seat::Local ? motion : Motion{});
  cheats_.CleanupRetired(game);
  if (!playable) {
    if (clock_frozen_ && game.Perform(Tool::PauseClock, {}, 0)) clock_frozen_ = false;
    if (low_gravity_ && game.Perform(Tool::Gravity, {}, 0)) low_gravity_ = false;
    if (weather_override_ && game.Perform(Tool::Weather, {}, UINT32_MAX)) weather_override_ = false;
  }
  for (const auto r : {Rule::Traffic, Rule::Peds, Rule::Police, Rule::PvP, Rule::FallDamage}) {
    const bool enabled = !playable || mode_.Rules().Get(r);
    if (r == Rule::Police) {
      if (caps_.Has(Feature::Police) && police_disabled_ == enabled) { game.WorldRule(r, enabled); police_disabled_ = !enabled; }
    } else if (caps_.Has(ForRule(r))) {
      if (r == Rule::Traffic || r == Rule::Peds) {
        if (!enabled) game.WorldRule(r, false);  // frame-scoped density natives
      } else if (playable) game.WorldRule(r, enabled);  // a future damage-authoritative backend
    }
  }
}
void WorkshopCore::BeginFrame(WorkshopGame& game, bool authority, bool playable) {
  authoritative_ = authority;
  playable_ = authority && playable;
  caps_ = game.Features();
}
void WorkshopCore::Tick(WorkshopGame& game, uint64_t frame, bool authority, bool playable, bool can_stream,
                        bool ui_capture, const std::optional<Preview>& preview, Motion input) {
  BeginFrame(game, authority, playable);
  PreviewTick(game, frame, preview);
  spawns_.Tick(game, frame, playable_, can_stream);
  if (auto r = build_.TakeResult()) {
    result_ = r->result;
    if (auto ped = std::exchange(warp_ped_, {}); ped && r->last_created && CanBuild(kLocalOwner) &&
        players_.Find(kLocalOwner)->entity == *ped && game.Exists(*ped) && game.Exists(r->last_created)) {
      if (!game.Perform(Tool::Warp, r->last_created, ped->handle)) result_ = BuildResult::Failed;
    }
  }
  if (last_frame_ && *last_frame_ == frame && playable_) return;
  last_frame_ = frame;
  Reconcile(game, playable_, ui_capture ? Motion{} : input);
  if (ui_capture && playable_) game.SuppressFire(players_.Find(kLocalOwner)->entity);
  StreamTravel(game, frame);
  GiveNext(game);
  build_.Tick(game, playable_);
  if (!retired_.empty() && game.Destroy(retired_.back())) retired_.pop_back();
  if (playable && frame % 15 == 0 && caps_.Has(Feature::ClockRead)) hour_ = game.Hour();
}
CoreView WorkshopCore::View(PlayerId actor) const {
  CoreView v;
  v.authority = authoritative_; v.playable = playable_; v.build = CanBuild(actor); v.admin = CanAdmin(actor);
  v.practice = practice_ && actor == kLocalOwner; v.caps = caps_; v.map = mode_.Rules();
  v.players_build = permissions_.Settings().players_build; v.limit = permissions_.Settings().prop_limit;
  v.count = unsigned(build_.Count(actor)); v.total = unsigned(build_.Total()); v.busy = build_.Busy();
  v.undo = build_.CanUndo(actor); v.redo = build_.CanUndo(actor, true);
  v.cheats = cheats_.State(actor);
  if (actor == kLocalOwner) { v.favorites = build_.Favorites(); v.presets = build_.Presets(); }
  const auto recent = build_.Recent(actor);
  v.recent.assign(recent.begin(), recent.end());
  if (v.admin) v.bans = permissions_.Bans();
  for (const auto& p : players_.All())
    v.players.push_back({p.id, p.name, p.seat, RoleOf(p.id), mode_.Effective(p.id), mode_.Override(p.id), bool(p.entity), p.frozen, p.wallet});
  if (actor == kLocalOwner && v.build) {
    if (const auto* p = build_.Find(aimed_); p && p->owner == actor) {
      v.aimed = p->id; v.aimed_model = p->spec.model; v.aimed_locked = p->spec.locked;
    }
    v.projection = projection_; v.ghost = bool(ghost_);
  }
  v.hour = hour_; v.result = result_; v.clock_frozen = clock_frozen_; v.low_gravity = low_gravity_;
  return v;
}
Config WorkshopCore::Export() const {
  return {permissions_.Settings(), mode_.Rules(), permissions_.Bans(), build_.Favorites(), build_.Presets()};
}
uint8_t WorkshopCore::TakeDirty() { return std::exchange(dirty_, 0); }
bool WorkshopCore::WorkPending() const {
  return spawns_.Busy() || cheats_.NeedsTick() || build_.Busy() || travel_ || loadout_ || !retired_.empty() || ghost_ ||
      clock_frozen_ || low_gravity_ || weather_override_ || police_disabled_ || !mode_.Rules().Get(Rule::Traffic) || !mode_.Rules().Get(Rule::Peds) ||
      !mode_.Rules().Get(Rule::Police);
}
}  // namespace gtav::workshop
