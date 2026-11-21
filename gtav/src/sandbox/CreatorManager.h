#pragma once

#include "../core/WorkshopTypes.h"
#include "../core/WorkshopGame.h"
#include "BuildManager.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gtav::sandbox {

// CreatorMAP — Unity/Unreal style editor for building maps & scenarios
// then playing them. Isolated profile under data/user/creator/
// Maps are JSON under data/user/creator/maps/*.json

inline constexpr const char* kCreatorSaveDir = "creator";
inline constexpr const char* kCreatorProfileDir = "profile";
inline constexpr const char* kCreatorMapsDir = "maps";

struct CreatorWorkspace {
  std::filesystem::path common_content_root; // data/user canonical
  std::filesystem::path profile_root;        // data/user/creator/profile
  std::filesystem::path maps_root;           // data/user/creator/maps
};

enum class CreatorWorkspaceError { None, MissingInstall, SeparateFolderRequired };
std::optional<CreatorWorkspace> PrepareCreatorWorkspace(const std::filesystem::path& user_root,
                                                        CreatorWorkspaceError& error);

// Extended object for Creator — beyond BuildManager::Piece (adds Unity-like fields)
struct CreatorObject {
  uint32_t model = 0;
  workshop::Kind kind = workshop::Kind::Prop;
  workshop::Pose pose{}; // world pose (not relative)
  workshop::Vec3 scale{1.f, 1.f, 1.f};
  bool locked = false;
  std::string name; // user label, default = Model name
  std::string category; // e.g. "Walls", "Gameplay", "Trigger"
  // Future: script/behavior string (Lua-like), color, physics flags, etc.
  bool operator==(const CreatorObject&) const = default;
};

enum class TriggerType : uint8_t { Checkpoint = 0, SpawnPlayer, SpawnVehicle, WinZone, LoseZone, Script, Count };

struct CreatorTrigger {
  std::string name = "Trigger";
  workshop::Vec3 pos{};
  float radius = 5.f;
  TriggerType type = TriggerType::Checkpoint;
  std::string script; // optional logic snippet (e.g. "onEnter: giveWeapon RPG")
  bool operator==(const CreatorTrigger&) const = default;
};

struct CreatorScenario {
  std::string name = "Scenario";
  std::string description;
  std::vector<CreatorTrigger> triggers;
  // Win condition: All checkpoints, or script
  enum class WinCondition : uint8_t { AllCheckpoints, EnterWinZone, Script } win = WinCondition::AllCheckpoints;
  bool operator==(const CreatorScenario&) const = default;
};

struct CreatorMap {
  std::string name = "Untitled";
  std::string description = "Creator MAP — Unity/Unreal style";
  std::string author = "Player";
  uint32_t version = 1;
  workshop::Vec3 player_start{0, 0, 0};
  float player_yaw = 0.f;
  std::vector<CreatorObject> objects;
  std::vector<CreatorTrigger> triggers; // legacy flat triggers (pre-scenario)
  CreatorScenario scenario;
  // World rules (reuses Workshop MapRules compatible subset)
  bool Valid() const;
  bool operator==(const CreatorMap&) const = default;
};

// JSON persistence (no external lib — simple writer/parser for this bounded schema)
bool SaveCreatorMap(const std::filesystem::path& maps_root, const CreatorMap& map, std::string& error_out);
std::optional<CreatorMap> LoadCreatorMap(const std::filesystem::path& maps_root, std::string_view name,
                                         std::string& error_out);
bool DeleteCreatorMap(const std::filesystem::path& maps_root, std::string_view name);
std::vector<std::string> ListCreatorMaps(const std::filesystem::path& maps_root);

// Runtime manager : holds loaded map, unsaved edits, spawns into WorkshopCore via BuildManager
class CreatorManager {
 public:
  explicit CreatorManager(std::filesystem::path maps_root);
  const std::filesystem::path& MapsRoot() const { return maps_root_; }
  const CreatorMap& Current() const { return current_; }
  CreatorMap& Mutable() { dirty_ = true; return current_; }
  bool Dirty() const { return dirty_; }
  void MarkClean() { dirty_ = false; }
  // File ops
  bool New(const std::string& name);
  bool Open(const std::string& name, std::string& error);
  bool Save(std::string& error);
  bool SaveAs(const std::string& name, std::string& error);
  // Object ops
  void AddObject(const CreatorObject& obj);
  bool RemoveObject(size_t index);
  bool DuplicateObject(size_t index);
  void ClearObjects();
  // Trigger ops
  void AddTrigger(const CreatorTrigger& t);
  bool RemoveTrigger(size_t index);
  // Validation
  bool ValidForPlay(std::string& why) const;
 private:
  std::filesystem::path maps_root_;
  CreatorMap current_;
  bool dirty_ = false;
};

} // namespace gtav::sandbox
