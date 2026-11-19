#include "GamemodeManager.h"

namespace gtav::workshop {
Mode GamemodeManager::Effective(PlayerId id) const { return Override(id).value_or(rules_.mode); }
std::optional<Mode> GamemodeManager::Override(PlayerId id) const {
  const auto it = overrides_.find(id);
  return it == overrides_.end() ? std::nullopt : std::optional(it->second);
}
bool GamemodeManager::SetDefault(Role actor, Mode mode) {
  if (!PermissionManager::AdminOS(actor) || mode > Mode::Survival) return false;
  rules_.mode = mode;  // A deliberate per-player override remains in force.
  return true;
}
bool GamemodeManager::SetOverride(Role actor, Role target_role, PlayerId target, bool self, std::optional<Mode> mode) {
  if (!target || !PermissionManager::Target(actor, target_role, self) || (mode && *mode > Mode::Survival)) return false;
  if (mode) overrides_.insert_or_assign(target, *mode);
  else overrides_.erase(target);
  return true;
}
bool GamemodeManager::SetRule(Role actor, Rule rule, bool enabled) {
  if (!PermissionManager::AdminOS(actor) || rule >= Rule::Count) return false;
  const uint32_t bit = 1u << unsigned(rule);
  rules_.enabled = enabled ? rules_.enabled | bit : rules_.enabled & ~bit;
  return true;
}
}  // namespace gtav::workshop
