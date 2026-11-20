#pragma once

#include <optional>

namespace gtav::frontend {

enum class Mode { Campaign, Solo, Multiplayer, CampaignCoop };
// Two columns (Campaign / Sandbox), with solo above multiplayer in each.
enum class Card { Story, Sandbox, StoryCoop, PrivateSession };
inline constexpr Card kDefaultCard = Card::Sandbox;

constexpr int Column(Card card) { return int(card) % 2; }
constexpr Card SelectColumn(Card card, int column) { return Card(int(card) / 2 * 2 + column); }
constexpr Card SelectRow(Card card, int row) { return Card(row * 2 + Column(card)); }
constexpr Card Cycle(Card card, bool backwards) { return Card((int(card) + (backwards ? 3 : 1)) % 4); }

// CampaignCoop is a 3-player story mode where each player owns one protagonist
// (Michael / Franklin / Trevor) and the save is isolated from the solo profile.
// It reuses the private-session transport but with a 3-slot room and locked roles.
constexpr std::optional<Mode> PlayMode(Card card, bool workshop_enabled) {
  switch (card) {
    case Card::Story: return Mode::Campaign;
    case Card::StoryCoop: if (workshop_enabled) return Mode::CampaignCoop; break;
    case Card::Sandbox: if (workshop_enabled) return Mode::Solo; break;
    case Card::PrivateSession: if (workshop_enabled) return Mode::Multiplayer; break;
  }
  return std::nullopt;
}

}  // namespace gtav::frontend
