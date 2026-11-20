#pragma once

#include "PermissionManager.h"

#include <map>
#include <optional>

namespace gtav::workshop {
enum class Rule : uint8_t { PvP, FallDamage, Traffic, Peds, Police, Count };
struct MapRules {
  Mode mode = Mode::Creative;
  uint32_t enabled = 0b11110;  // No shared PvP policy by default.
  bool Get(Rule rule) const { return rule < Rule::Count && ((enabled >> unsigned(rule)) & 1u); }
  bool Valid() const { return mode <= Mode::Survival && enabled < (1u << unsigned(Rule::Count)); }
};
class GamemodeManager {
 public:
  explicit GamemodeManager(MapRules rules) : rules_(rules.Valid() ? rules : MapRules{}) {}
  Mode Effective(PlayerId id) const;
  std::optional<Mode> Override(PlayerId id) const;
  bool SetDefault(Role actor, Mode mode);
  bool SetOverride(Role actor, Role target_role, PlayerId target, bool self, std::optional<Mode> mode);
  bool SetRule(Role actor, Rule rule, bool enabled);
  void Forget(PlayerId id) { overrides_.erase(id); }
  const MapRules& Rules() const { return rules_; }
 private:
  MapRules rules_;
  std::map<PlayerId, Mode> overrides_;
};
}  // namespace gtav::workshop
