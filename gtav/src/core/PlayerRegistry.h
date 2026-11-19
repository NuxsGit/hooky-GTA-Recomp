#pragma once

#include "PermissionManager.h"

namespace gtav::workshop {
struct Player {
  PlayerId id{};
  std::string identity, name;
  Seat seat = Seat::Remote;
  Entity entity;
  bool frozen = false;
  uint32_t wallet = 0;  // Workshop credits; never a story save stat.
};

class PlayerRegistry {
 public:
  explicit PlayerRegistry(std::string host_name = "You");
  // The transport must bind a verified identity to a connection before calling Admit. The v2
  // invite/pose protocol does not establish this identity and must not use Admit for its peers.
  PlayerId Admit(const PermissionManager& access, Seat seat, std::string_view identity, std::string_view name);
  bool Remove(PlayerId id);
  bool Bind(PlayerId id, Entity handle);
  const Player* Find(PlayerId id) const;
  Player* Find(PlayerId id);
  const Player* FindIdentity(std::string_view identity) const;
  std::span<const Player> All() const { return players_; }
 private:
  std::vector<Player> players_;
  PlayerId next_ = kLocalOwner + 1;
};
}  // namespace gtav::workshop
