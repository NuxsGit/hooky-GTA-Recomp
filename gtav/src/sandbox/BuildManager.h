#pragma once

#include "../core/WorkshopGame.h"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace gtav::workshop {
struct Model { std::string_view name, internal, category; Kind kind; uint32_t hash; };
std::span<const Model> Models();
const Model* FindModel(uint32_t hash);
bool Matches(const Model&, std::string_view query);
struct Weapon { std::string_view name, internal; uint32_t hash; };
std::span<const Weapon> Weapons();
bool KnownWeapon(uint32_t hash);

struct SavedPiece { uint32_t model{}; Pose relative; bool locked{}; };
struct Preset { std::string name; std::vector<SavedPiece> pieces; };
bool ValidPreset(const Preset&);
struct Placement {
  Lens lens;
  float distance = 6.f, height = 0.f, yaw = 0.f;
  unsigned grid_cm = 0, angle = 15;
  bool ground = false, lock = false;
  bool Valid() const;
};
bool ValidFavorites(std::span<const uint32_t> hashes);
std::optional<Pose> Project(WorkshopGame&, const Ray&, const Placement&);
Pose Relative(Pose anchor, Pose world);
Pose Compose(Pose anchor, Pose relative);

struct Piece {
  PieceId id{};
  PlayerId owner{};
  SpawnSpec spec;
  Entity entity;
};
enum class BuildResult { Okay, Queued, Denied, Invalid, Limit, Busy, Missing, Failed, Timeout, Cancelled };
const char* Label(BuildResult result);
class BuildManager {
 public:
  static constexpr size_t kHistory = 100, kRecent = 30, kPresetPieces = 16, kPresets = 24, kFavorites = 64;
  BuildManager(std::vector<uint32_t> favorites, std::vector<Preset> presets);
  const std::vector<uint32_t>& Favorites() const { return favorites_; }
  const std::vector<Preset>& Presets() const { return presets_; }
  std::span<const uint32_t> Recent(PlayerId actor) const;
  const Piece* Find(PieceId id) const;
  PieceId Aimed(WorkshopGame&, const Ray&, float max_distance);
  size_t Count(PlayerId owner) const;
  size_t Total() const { return pieces_.size(); }
  bool Busy() const { return batch_.has_value() || !reap_.empty() || cleanup_.has_value(); }
  bool CanUndo(PlayerId actor, bool redo = false) const;
  bool ToggleFavorite(uint32_t model);
  bool SavePreset(std::string name, PlayerId owner, PieceId anchor, WorkshopGame&);
  bool DeletePreset(std::string_view name);
  BuildResult Place(PlayerId actor, std::vector<SpawnSpec> specs, unsigned limit, EntitySpawnQueue&,
                    uint64_t frame, std::function<bool()> allowed);
  BuildResult Delete(PlayerId actor, PieceId id, WorkshopGame&);
  BuildResult Lock(PlayerId actor, PieceId id, bool locked, WorkshopGame&);
  BuildResult Undo(PlayerId actor, bool redo, unsigned limit, WorkshopGame&, EntitySpawnQueue&, uint64_t frame,
                   std::function<bool()> allowed);
  BuildResult Cleanup(PlayerId owner, std::function<bool()> allowed);
  void Tick(WorkshopGame&, bool ready);
  void Forget(PlayerId actor);
  struct Completion { BuildResult result; Entity last_created; };
  std::optional<Completion> TakeResult();
 private:
  enum class Edit { Add, Remove, Anchor, Unanchor };
  enum class Commit { New, Undo, Redo };
  struct Entry { Edit edit; std::vector<Piece> pieces; };
  struct History { std::deque<Entry> undo, redo; std::vector<uint32_t> recent; };
  struct Batch {
    PlayerId actor;
    Entry entry;
    Commit commit;
    size_t outstanding;
    std::vector<Piece> made;
    BuildResult result = BuildResult::Okay;
  };
  BuildResult Spawn(PlayerId actor, Entry, Commit, unsigned, EntitySpawnQueue&, uint64_t, std::function<bool()>);
  void SaveHistory(PlayerId actor, Entry, Commit);
  bool RemovePiece(PieceId id, WorkshopGame&);
  void NoteRecent(PlayerId actor, uint32_t model);
  std::vector<Piece> pieces_;
  std::vector<Entity> reap_;
  std::map<PlayerId, History> histories_;
  std::optional<Batch> batch_;
  struct CleanupJob { PlayerId owner; std::function<bool()> allowed; };
  std::optional<CleanupJob> cleanup_;
  std::optional<Completion> result_;
  PieceId next_ = 1;
  size_t probe_ = 0;
  std::vector<uint32_t> favorites_;
  std::vector<Preset> presets_;
};
}  // namespace gtav::workshop
