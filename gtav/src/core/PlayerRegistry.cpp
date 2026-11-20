#include "PlayerRegistry.h"

#include <utility>

namespace gtav::workshop {
PlayerRegistry::PlayerRegistry(std::string host_name) {
  players_.push_back({kLocalOwner, "local:workshop", ValidText(host_name, 40) ? std::move(host_name) : "You", Seat::Local, {}});
}
PlayerId PlayerRegistry::Admit(const PermissionManager& access, Seat seat, std::string_view identity, std::string_view name) {
  if ((seat != Seat::Simulated && seat != Seat::Remote) || players_.size() >= 16 || !next_ ||
      !ValidIdentity(identity) || identity.starts_with("local:") || identity.starts_with("sim:") != (seat == Seat::Simulated) ||
      !ValidText(name, 40) || FindIdentity(identity) || access.IsBanned(identity)) return 0;
  players_.push_back({next_, std::string(identity), std::string(name), seat, {}});
  return next_++;
}
bool PlayerRegistry::Remove(PlayerId id) {
  return id != kLocalOwner && std::erase_if(players_, [=](const Player& p) { return p.id == id; }) != 0;
}
bool PlayerRegistry::Bind(PlayerId id, Entity entity) {
  auto* p = Find(id);
  if (!p || (entity && (entity.kind != Kind::Ped || std::any_of(players_.begin(), players_.end(), [&](const Player& other) {
        return other.id != id && other.entity == entity;
      })))) return false;
  p->entity = entity;
  if (!entity) p->frozen = false;
  return true;
}
const Player* PlayerRegistry::Find(PlayerId id) const {
  auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& p) { return p.id == id; });
  return it != players_.end() ? &*it : nullptr;
}
Player* PlayerRegistry::Find(PlayerId id) { return const_cast<Player*>(std::as_const(*this).Find(id)); }
const Player* PlayerRegistry::FindIdentity(std::string_view identity) const {
  auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& p) { return p.identity == identity; });
  return it != players_.end() ? &*it : nullptr;
}
}  // namespace gtav::workshop
