#pragma once

#include "WorkshopTypes.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace gtav::workshop {
struct Ban {
  std::string identity, name, reason, utc;
  bool operator==(const Ban&) const = default;
};
struct Permissions {
  std::vector<std::string> admins;
  bool players_build = false;
  unsigned prop_limit = 48;
  bool Valid() const;
};
std::string UtcNow();
// Public authorization predicates are also used to render the panels. Mutations recheck them
// inside the authority. Owner is a single host seat, never a value in permissions.json.
class PermissionManager {
 public:
  static constexpr size_t kMaxRecords = 128;
  PermissionManager(Permissions settings, std::vector<Ban> bans);
  const Permissions& Settings() const { return settings_; }
  const std::vector<Ban>& Bans() const { return bans_; }
  Role Get(PlayerId id, std::string_view identity) const;
  bool IsBanned(std::string_view identity) const;
  bool SetAdmin(Role actor, std::string_view identity, bool admin);
  bool SetBuildRules(Role actor, bool allow, unsigned limit);
  bool AddBan(Role actor, Role target, Ban ban);
  bool Unban(Role actor, std::string_view identity);
  static bool ValidBans(std::span<const Ban> bans);
  static bool AdminOS(Role role) { return role == Role::Owner || role == Role::Admin; }
  static bool Build(Role role, Mode mode, bool players_build) {
    return mode == Mode::Creative && (AdminOS(role) || (role == Role::Player && players_build));
  }
  static bool Target(Role actor, Role target, bool self = false) {
    return actor == Role::Owner || (actor == Role::Admin && (target == Role::Player || self));
  }
  static bool Moderate(Role actor, Role target, bool self = false) {
    return !self && target != Role::Owner && Target(actor, target);
  }
 private:
  Permissions settings_;
  std::vector<Ban> bans_;
};
}  // namespace gtav::workshop
