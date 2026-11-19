#include "BuildManager.h"

#include <array>
#include <numbers>
#include <utility>

namespace gtav::workshop {
namespace {
constexpr Model M(std::string_view name, std::string_view model, Kind kind, std::string_view cat) {
  return {name, model, cat, kind, ModelHash(model)};
}
using K = Kind;
constexpr Model kModels[] = {
    M("Concrete wall", "prop_fncconstruc_01d", K::Prop, "Walls & floors"),
    M("Plywood panel", "prop_plywoodpile_01a", K::Prop, "Walls & floors"),
    M("Wood pallet", "prop_pallet_02a", K::Prop, "Walls & floors"),
    M("Stunt ramp", "prop_mp_ramp_01", K::Prop, "Ramps"),
    M("Wide ramp", "prop_mp_ramp_02", K::Prop, "Ramps"),
    M("Traffic cone", "prop_roadcone02a", K::Prop, "Barriers & roads"),
    M("Road barrier", "prop_barrier_work05", K::Prop, "Barriers & roads"),
    M("Crash barrier", "prop_barrier_work06a", K::Prop, "Barriers & roads"),
    M("Concrete block", "prop_barrier_work01a", K::Prop, "Barriers & roads"),
    M("Wood crate", "prop_box_wood02a_pu", K::Prop, "Industrial"),
    M("Shipping container", "prop_container_01a", K::Prop, "Industrial"),
    M("Oil drum", "prop_barrel_02a", K::Prop, "Industrial"),
    M("Bench", "prop_bench_01a", K::Prop, "Furniture"),
    M("Patio chair", "prop_chair_01a", K::Prop, "Furniture"),
    M("Picnic table", "prop_table_03", K::Prop, "Furniture"),
    M("Work light", "prop_worklight_03a", K::Prop, "Lights"),
    M("Streetlight", "prop_streetlight_01", K::Prop, "Lights"),
    M("Beach ball", "prop_beachball_02", K::Prop, "Fun"),
    M("Skate half-pipe", "prop_skate_halfpipe", K::Prop, "Fun"),
    M("Adder", "adder", K::Vehicle, "Super & sport"),
    M("Sultan", "sultan", K::Vehicle, "Super & sport"),
    M("Entity XF", "entityxf", K::Vehicle, "Super & sport"),
    M("Infernus", "infernus", K::Vehicle, "Super & sport"),
    M("Bifta", "bifta", K::Vehicle, "Off-road"),
    M("Sandking", "sandking", K::Vehicle, "Off-road"),
    M("Bodhi", "bodhi2", K::Vehicle, "Off-road"),
    M("Bati 801", "bati", K::Vehicle, "Motorcycles"),
    M("Sanchez", "sanchez", K::Vehicle, "Motorcycles"),
    M("Buzzard", "buzzard2", K::Vehicle, "Aircraft"),
    M("Duster", "duster", K::Vehicle, "Aircraft"),
    M("Mallard", "stunt", K::Vehicle, "Aircraft"),
    M("Dinghy", "dinghy", K::Vehicle, "Boats"),
    M("Jetmax", "jetmax", K::Vehicle, "Boats"),
    M("Seashark", "seashark", K::Vehicle, "Boats"),
    M("Blista", "blista", K::Vehicle, "Utility & emergency"),
    M("BMX", "bmx", K::Vehicle, "Utility & emergency"),
    M("Police cruiser", "police", K::Vehicle, "Utility & emergency"),
    M("Ambulance", "ambulance", K::Vehicle, "Utility & emergency"),
    M("Tow truck", "towtruck", K::Vehicle, "Utility & emergency"),
    M("Skater", "a_m_y_skater_01", K::Ped, "Civilians"),
    M("Hiker", "a_f_y_hiker_01", K::Ped, "Civilians"),
    M("Police officer", "s_m_y_cop_01", K::Ped, "Police"),
    M("Sheriff deputy", "s_m_y_sheriff_01", K::Ped, "Police"),
    M("Families member", "g_m_y_famca_01", K::Ped, "Gangs"),
    M("Ballas member", "g_m_y_ballaeast_01", K::Ped, "Gangs"),
    M("Rottweiler", "a_c_rottweiler", K::Ped, "Animals"),
    M("Deer", "a_c_deer", K::Ped, "Animals"),
    M("Vending machine", "prop_vend_soda_01", K::Prop, "Interactive models"),
    M("Payphone", "prop_phonebox_01a", K::Prop, "Interactive models"),
};
constexpr Weapon W(std::string_view name, std::string_view key) { return {name, key, ModelHash(key)}; }
// Last-gen base + update candidates, including separately mounted DLC. The native adapter tests
// IS_WEAPON_VALID; an absent DLC entry is not given. No enhanced/Online-only weapon data is loaded.
constexpr Weapon kWeapons[] = {
    W("Knife", "weapon_knife"), W("Nightstick", "weapon_nightstick"), W("Hammer", "weapon_hammer"),
    W("Bat", "weapon_bat"), W("Golf club", "weapon_golfclub"), W("Crowbar", "weapon_crowbar"),
    W("Broken bottle", "weapon_bottle"), W("Dagger", "weapon_dagger"), W("Hatchet", "weapon_hatchet"),
    W("Knuckle duster", "weapon_knuckle"), W("Machete", "weapon_machete"), W("Flashlight", "weapon_flashlight"),
    W("Pistol", "weapon_pistol"), W("Combat pistol", "weapon_combatpistol"), W("AP pistol", "weapon_appistol"),
    W("Pistol .50", "weapon_pistol50"), W("SNS pistol", "weapon_snspistol"), W("Heavy pistol", "weapon_heavypistol"),
    W("Vintage pistol", "weapon_vintagepistol"), W("Marksman pistol", "weapon_marksmanpistol"),
    W("Stun gun", "weapon_stungun"), W("Flare gun", "weapon_flaregun"),
    W("Micro SMG", "weapon_microsmg"), W("SMG", "weapon_smg"), W("Assault SMG", "weapon_assaultsmg"),
    W("Combat PDW", "weapon_combatpdw"), W("Machine pistol", "weapon_machinepistol"),
    W("Assault rifle", "weapon_assaultrifle"), W("Carbine rifle", "weapon_carbinerifle"),
    W("Advanced rifle", "weapon_advancedrifle"), W("Special carbine", "weapon_specialcarbine"),
    W("Bullpup rifle", "weapon_bullpuprifle"), W("MG", "weapon_mg"), W("Combat MG", "weapon_combatmg"),
    W("Gusenberg", "weapon_gusenberg"), W("Pump shotgun", "weapon_pumpshotgun"),
    W("Sawed-off", "weapon_sawnoffshotgun"), W("Assault shotgun", "weapon_assaultshotgun"),
    W("Bullpup shotgun", "weapon_bullpupshotgun"), W("Musket", "weapon_musket"),
    W("Heavy shotgun", "weapon_heavyshotgun"), W("Sniper rifle", "weapon_sniperrifle"),
    W("Heavy sniper", "weapon_heavysniper"), W("Marksman rifle", "weapon_marksmanrifle"),
    W("RPG", "weapon_rpg"), W("Grenade launcher", "weapon_grenadelauncher"),
    W("Minigun", "weapon_minigun"), W("Firework launcher", "weapon_firework"), W("Railgun", "weapon_railgun"),
    W("Homing launcher", "weapon_hominglauncher"), W("Grenade", "weapon_grenade"),
    W("Sticky bomb", "weapon_stickybomb"), W("Proximity mine", "weapon_proxmine"), W("Tear gas", "weapon_bzgas"),
    W("Smoke grenade", "weapon_smokegrenade"), W("Molotov", "weapon_molotov"), W("Snowball", "weapon_snowball"),
    W("Ball", "weapon_ball"), W("Flare", "weapon_flare"), W("Jerry can", "weapon_petrolcan"),
    W("Extinguisher", "weapon_fireextinguisher"), W("Parachute", "gadget_parachute"),
};
constexpr float kRadians = std::numbers::pi_v<float> / 180.f;
float Yaw(float angle) { return std::fmod(std::fmod(angle, 360.f) + 360.f, 360.f); }
Vec3 Rotate(Vec3 a, float yaw) {
  const float c = std::cos(yaw * kRadians), s = std::sin(yaw * kRadians);
  return {a.x*c - a.y*s, a.x*s + a.y*c, a.z};
}
char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }
bool Contains(std::string_view text, std::string_view query) {
  return query.empty() || std::search(text.begin(), text.end(), query.begin(), query.end(),
    [](char c, char q) { return Lower(c) == Lower(q); }) != text.end();
}
}  // namespace

std::span<const Model> Models() { return kModels; }
const Model* FindModel(uint32_t hash) {
  auto it = std::find_if(std::begin(kModels), std::end(kModels), [=](const Model& m) { return m.hash == hash; });
  return it == std::end(kModels) ? nullptr : it;
}
bool Matches(const Model& model, std::string_view query) {
  if (Contains(model.name, query) || Contains(model.internal, query)) return true;
  if (query.starts_with("0x") || query.starts_with("0X")) query.remove_prefix(2);
  std::array<char, 8> text;
  for (unsigned i = 0; i < text.size(); ++i) text[i] = "0123456789abcdef"[(model.hash >> (28 - i * 4)) & 15];
  return Contains({text.data(), text.size()}, query);
}
std::span<const Weapon> Weapons() { return kWeapons; }
bool KnownWeapon(uint32_t hash) {
  return std::any_of(std::begin(kWeapons), std::end(kWeapons), [=](const Weapon& w) { return w.hash == hash; });
}
bool ValidPreset(const Preset& p) {
  return ValidText(p.name, 40) && !p.pieces.empty() && p.pieces.size() <= BuildManager::kPresetPieces &&
      std::all_of(p.pieces.begin(), p.pieces.end(), [](const SavedPiece& piece) {
        return FindModel(piece.model) && piece.relative.Valid() && piece.relative.at.Valid(100.f);
      });
}
bool ValidFavorites(std::span<const uint32_t> hashes) {
  if (hashes.size() > BuildManager::kFavorites) return false;
  for (auto it = hashes.begin(); it != hashes.end(); ++it)
    if (!FindModel(*it) || std::find(hashes.begin(), it, *it) != it) return false;
  return true;
}
bool Placement::Valid() const {
  return std::abs(lens.x) <= 1.f && std::abs(lens.y) <= 1.f && lens.aspect >= .5f && lens.aspect <= 4.f &&
      distance >= 2.f && distance <= 50.f && std::abs(height) <= 12.f && std::abs(yaw) <= 360.f &&
      (grid_cm == 0 || grid_cm == 25 || grid_cm == 50 || grid_cm == 100 || grid_cm == 200) &&
      (angle == 15 || angle == 45 || angle == 90);
}
Pose Relative(Pose anchor, Pose world) { return {Rotate(world.at - anchor.at, -anchor.yaw), Yaw(world.yaw - anchor.yaw)}; }
Pose Compose(Pose anchor, Pose relative) { return {anchor.at + Rotate(relative.at, anchor.yaw), Yaw(anchor.yaw + relative.yaw)}; }
std::optional<Pose> Project(WorkshopGame& game, const Ray& ray, const Placement& opts) {
  if (!opts.Valid() || !ray.origin.Valid() || !ray.direction.Valid(1.f) ||
      std::abs(ray.direction.Dot(ray.direction) - 1.f) > .03f) return {};
  // An adapter with a verified LOS probe supplies the surface hit. TU27 otherwise uses a
  // deterministic camera projection; no guessed shape-test dispatch or fake surface hits.
  Vec3 pos = game.RayHit(ray, opts.distance).value_or(ray.origin + ray.direction * opts.distance);
  if (opts.grid_cm) {
    float grid = opts.grid_cm / 100.f;
    pos.x = std::round(pos.x / grid) * grid;
    pos.y = std::round(pos.y / grid) * grid;
    pos.z = std::round(pos.z / grid) * grid;
  }
  if (opts.ground) {
    const auto ground = game.Ground(pos);
    if (!ground) return {};
    pos.z = *ground;
  }
  pos.z += opts.height;
  Pose pose{pos, Yaw(std::round(opts.yaw / float(opts.angle)) * float(opts.angle))};
  return pose.Valid() ? std::optional(pose) : std::nullopt;
}

const char* Label(BuildResult result) {
  switch (result) {
    case BuildResult::Okay: return "Done";
    case BuildResult::Queued: return "Queued on the game thread";
    case BuildResult::Denied: return "Not allowed by the Workshop authority";
    case BuildResult::Invalid: return "Invalid or unavailable for this build";
    case BuildResult::Limit: return "Build or queue limit reached";
    case BuildResult::Busy: return "Another build action is still in progress";
    case BuildResult::Missing: return "No eligible entity or selection";
    case BuildResult::Failed: return "The game did not complete the action";
    case BuildResult::Timeout: return "Streaming expired after 300 game frames";
    case BuildResult::Cancelled: return "Cancelled (access, world or selection changed)";
  }
  return "Unknown";
}
BuildManager::BuildManager(std::vector<uint32_t> favorites, std::vector<Preset> presets) {
  if (ValidFavorites(favorites)) favorites_ = std::move(favorites);
  for (auto& p : presets) {
    if (presets_.size() >= kPresets) break;
    if (ValidPreset(p) && !std::any_of(presets_.begin(), presets_.end(), [&](const Preset& q) { return p.name == q.name; }))
      presets_.push_back(std::move(p));
  }
}
std::span<const uint32_t> BuildManager::Recent(PlayerId actor) const {
  auto it = histories_.find(actor);
  return it == histories_.end() ? std::span<const uint32_t>{} : it->second.recent;
}
const Piece* BuildManager::Find(PieceId id) const {
  auto it = std::find_if(pieces_.begin(), pieces_.end(), [=](const Piece& p) { return p.id == id; });
  return it == pieces_.end() ? nullptr : &*it;
}
size_t BuildManager::Count(PlayerId owner) const {
  return std::count_if(pieces_.begin(), pieces_.end(), [=](const Piece& p) { return p.owner == owner; });
}
PieceId BuildManager::Aimed(WorkshopGame& game, const Ray& ray, float max) {
  PieceId hit = 0;
  // Cheap intersection against periodically refreshed piece bounds. Untracked map/ambient
  // entities are never returned for mutation.
  for (const auto& p : pieces_) {
    const float r = p.spec.kind == Kind::Vehicle ? 2.2f : p.spec.kind == Kind::Ped ? 1.f : 1.4f;
    const Vec3 from = p.spec.pose.at - ray.origin;
    const float t = from.Dot(ray.direction);
    const Vec3 d = from - ray.direction * t;
    if (t <= 0 || t >= max || d.Dot(d) > r*r || !game.Exists(p.entity)) continue;
    max = t;
    hit = p.id;
  }
  return hit;
}
bool BuildManager::CanUndo(PlayerId actor, bool redo) const {
  const auto it = histories_.find(actor);
  return it != histories_.end() && !(redo ? it->second.redo : it->second.undo).empty();
}
bool BuildManager::ToggleFavorite(uint32_t model) {
  if (!FindModel(model)) return false;
  if (std::erase(favorites_, model)) return true;
  if (favorites_.size() >= kFavorites) return false;
  favorites_.push_back(model);
  return true;
}
bool BuildManager::SavePreset(std::string name, PlayerId owner, PieceId anchor, WorkshopGame& game) {
  const auto* piece = Find(anchor);
  if (!piece || piece->owner != owner || !game.Exists(piece->entity)) return false;
  Preset preset{std::move(name), {}};
  const auto origin = game.Locate(piece->entity);
  if (!origin) return false;
  // Store the aimed piece first so the ghost previews the actual anchor even when it
  // was not the first object built. The other pieces are relative to this pose.
  preset.pieces.push_back({piece->spec.model, {}, piece->spec.locked});
  for (const auto& p : pieces_) {
    if (p.owner != owner || p.id == anchor) continue;
    const auto coarse = p.spec.pose.at - origin->at;
    if (coarse.Dot(coarse) > 26.f*26.f) continue;
    const auto coords = game.Locate(p.entity);
    if (!coords) continue;
    const auto delta = coords->at - origin->at;
    if (delta.Dot(delta) <= 25.f*25.f) preset.pieces.push_back({p.spec.model, Relative(*origin, *coords), p.spec.locked});
    if (preset.pieces.size() > kPresetPieces) return false;
  }
  if (!ValidPreset(preset)) return false;
  for (auto& old : presets_) if (old.name == preset.name) { old = std::move(preset); return true; }
  if (presets_.size() >= kPresets) return false;
  presets_.push_back(std::move(preset));
  return true;
}
bool BuildManager::DeletePreset(std::string_view name) {
  return std::erase_if(presets_, [&](const Preset& p) { return p.name == name; }) != 0;
}
void BuildManager::NoteRecent(PlayerId actor, uint32_t model) {
  auto& recent = histories_[actor].recent;
  recent.insert(recent.begin(), model);
  if (recent.size() > kRecent) recent.pop_back();
}
void BuildManager::SaveHistory(PlayerId actor, Entry entry, Commit commit) {
  auto& h = histories_[actor];
  auto& to = commit == Commit::Undo ? h.redo : h.undo;
  if (commit == Commit::New) h.redo.clear();
  if (commit == Commit::Undo && !h.undo.empty()) h.undo.pop_back();
  if (commit == Commit::Redo && !h.redo.empty()) h.redo.pop_back();
  to.push_back(std::move(entry));
  if (to.size() > kHistory) to.pop_front();
}
BuildResult BuildManager::Place(PlayerId actor, std::vector<SpawnSpec> specs, unsigned limit, EntitySpawnQueue& queue,
                                uint64_t frame, std::function<bool()> allowed) {
  if (specs.empty() || specs.size() > kPresetPieces || next_ > UINT32_MAX - specs.size()) return BuildResult::Invalid;
  Entry entry{Edit::Add, {}};
  for (const auto& spec : specs) {
    const auto* model = FindModel(spec.model);
    if (!model || model->kind != spec.kind || !spec.pose.Valid() || spec.ghost) return BuildResult::Invalid;
    entry.pieces.push_back({next_++, actor, spec, {}});
  }
  return Spawn(actor, std::move(entry), Commit::New, limit, queue, frame, std::move(allowed));
}
BuildResult BuildManager::Spawn(PlayerId actor, Entry entry, Commit commit, unsigned limit, EntitySpawnQueue& queue,
                                uint64_t frame, std::function<bool()> allowed) {
  if (Busy()) return BuildResult::Busy;
  if (!allowed || !allowed()) return BuildResult::Denied;
  if (Count(actor) + entry.pieces.size() > limit || pieces_.size() + entry.pieces.size() > 160) return BuildResult::Limit;
  batch_ = Batch{actor, std::move(entry), commit, 0, {}, BuildResult::Okay};
  for (auto piece : batch_->entry.pieces) {
    if (Find(piece.id) || !queue.Push(piece.spec, frame,
        [this, allowed] { return batch_ && batch_->result == BuildResult::Okay && allowed(); },
        [this, piece](SpawnResult r, Entity e) mutable {
          auto& batch = *batch_;
          if (r == SpawnResult::Created) { piece.entity = e; batch.made.push_back(std::move(piece)); }
          else if (batch.result == BuildResult::Okay) {
            batch.result = r == SpawnResult::Timeout ? BuildResult::Timeout :
                           r == SpawnResult::Cancelled ? BuildResult::Cancelled : BuildResult::Failed;
          }
          if (--batch.outstanding) return;
          if (batch.result == BuildResult::Okay) {
            for (const auto& p : batch.made) { pieces_.push_back(p); NoteRecent(batch.actor, p.spec.model); }
            SaveHistory(batch.actor, std::move(batch.entry), batch.commit);
          } else {
            for (const auto& p : batch.made) reap_.push_back(p.entity);
          }
          result_ = Completion{batch.result, batch.result == BuildResult::Okay && !batch.made.empty()
                                                ? batch.made.back().entity : Entity{}};
          batch_.reset();
        })) {
      batch_->result = BuildResult::Limit;
      break;
    }
    ++batch_->outstanding;
  }
  if (!batch_->outstanding) { batch_.reset(); return BuildResult::Limit; }
  return BuildResult::Queued;
}
bool BuildManager::RemovePiece(PieceId id, WorkshopGame& game) {
  const auto* p = Find(id);
  if (!p) return true;
  if (!game.Destroy(p->entity)) return false;
  std::erase_if(pieces_, [=](const Piece& old) { return old.id == id; });
  return true;
}
BuildResult BuildManager::Delete(PlayerId actor, PieceId id, WorkshopGame& game) {
  if (Busy()) return BuildResult::Busy;
  const auto* p = Find(id);
  if (!p) return BuildResult::Missing;
  if (p->owner != actor) return BuildResult::Denied;
  auto copy = *p;
  if (auto live = game.Locate(p->entity)) copy.spec.pose = *live;
  if (!RemovePiece(id, game)) return BuildResult::Failed;
  SaveHistory(actor, {Edit::Remove, {copy}}, Commit::New);
  return BuildResult::Okay;
}
BuildResult BuildManager::Lock(PlayerId actor, PieceId id, bool locked, WorkshopGame& game) {
  if (Busy()) return BuildResult::Busy;
  auto it = std::find_if(pieces_.begin(), pieces_.end(), [=](const Piece& p) { return p.id == id; });
  if (it == pieces_.end()) return BuildResult::Missing;
  if (it->owner != actor) return BuildResult::Denied;
  if (it->spec.locked == locked) return BuildResult::Okay;
  if (!game.Exists(it->entity) || !game.Anchor(it->entity, locked)) return BuildResult::Failed;
  it->spec.locked = locked;
  if (auto pose = game.Locate(it->entity)) it->spec.pose = *pose;
  SaveHistory(actor, {locked ? Edit::Anchor : Edit::Unanchor, {*it}}, Commit::New);
  return BuildResult::Okay;
}
BuildResult BuildManager::Undo(PlayerId actor, bool redo, unsigned limit, WorkshopGame& game, EntitySpawnQueue& queue,
                               uint64_t frame, std::function<bool()> allowed) {
  if (Busy()) return BuildResult::Busy;
  if (!allowed || !allowed()) return BuildResult::Denied;
  if (!CanUndo(actor, redo)) return BuildResult::Missing;
  auto& h = histories_.at(actor);
  auto entry = (redo ? h.redo : h.undo).back();
  const auto commit = redo ? Commit::Redo : Commit::Undo;
  if ((entry.edit == Edit::Add && redo) || (entry.edit == Edit::Remove && !redo))
    return Spawn(actor, std::move(entry), commit, limit, queue, frame, std::move(allowed));
  if (entry.edit == Edit::Add || entry.edit == Edit::Remove) {
    for (auto& p : entry.pieces) {
      if (const auto* live = Find(p.id)) {
        p = *live;
        if (auto pose = game.Locate(p.entity)) p.spec.pose = *pose;
      }
      if (!RemovePiece(p.id, game)) return BuildResult::Failed;
    }
  } else {
    auto it = std::find_if(pieces_.begin(), pieces_.end(), [&](const Piece& p) { return p.id == entry.pieces.front().id; });
    if (it == pieces_.end()) return BuildResult::Missing;
    const bool locked = (entry.edit == Edit::Anchor) == redo;
    if (!game.Exists(it->entity) || !game.Anchor(it->entity, locked)) return BuildResult::Failed;
    it->spec.locked = locked;
  }
  SaveHistory(actor, std::move(entry), commit);
  return BuildResult::Okay;
}
BuildResult BuildManager::Cleanup(PlayerId actor, std::function<bool()> allowed) {
  if (Busy()) return BuildResult::Busy;
  if (!allowed || !allowed()) return BuildResult::Denied;
  cleanup_ = CleanupJob{actor, std::move(allowed)};
  return BuildResult::Queued;
}
void BuildManager::Forget(PlayerId actor) {
  histories_.erase(actor);
  for (const auto& p : pieces_) if (p.owner == actor) reap_.push_back(p.entity);
  std::erase_if(pieces_, [=](const Piece& p) { return p.owner == actor; });
}
void BuildManager::Tick(WorkshopGame& game, bool ready) {
  if (cleanup_ && (!ready || !cleanup_->allowed())) {
    cleanup_.reset();
    result_ = Completion{BuildResult::Cancelled, {}};
  }
  // Rollback/retirement is internal cleanup, not an actor editing another player's pieces.
  for (unsigned budget = 2; budget && !reap_.empty(); --budget) {
    if (!game.Destroy(reap_.back())) break;
    reap_.pop_back();
  }
  if (!ready) return;
  if (cleanup_) {
    const auto owner = cleanup_->owner;
    for (unsigned budget = 2; budget; --budget) {
      auto it = std::find_if(pieces_.begin(), pieces_.end(), [=](const Piece& p) { return p.owner == owner; });
      if (it != pieces_.end() && !RemovePiece(it->id, game)) {
        cleanup_.reset(); result_ = Completion{BuildResult::Failed, {}}; break;
      }
      // Keep history on revocation or failure before the first deletion. Once we have
      // removed anything (or the build was already empty), cleanup is irreversible.
      if (auto h = histories_.find(owner); h != histories_.end()) { h->second.undo.clear(); h->second.redo.clear(); }
      if (!Count(owner)) { cleanup_.reset(); result_ = Completion{BuildResult::Okay, {}}; break; }
    }
  }
  // Budget validity/pose refresh rather than querying all entities every frame.
  for (unsigned i = 0; i < 2 && !pieces_.empty(); ++i) {
    probe_ %= pieces_.size();
    if (auto at = game.Locate(pieces_[probe_].entity)) pieces_[probe_++].spec.pose = *at;
    else if (!game.Exists(pieces_[probe_].entity)) pieces_.erase(pieces_.begin() + probe_);
    else ++probe_;
  }
}
std::optional<BuildManager::Completion> BuildManager::TakeResult() { return std::exchange(result_, {}); }
}  // namespace gtav::workshop
