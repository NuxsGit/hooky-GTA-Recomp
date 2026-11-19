#pragma once

#include <optional>

namespace gtav::frontend {

enum class Mode { Campaign, Solo, Multiplayer };
// Two columns (Campaign / Sandbox), with solo above multiplayer in each.
enum class Card { Story, Sandbox, StoryCoop, PrivateSession };
inline constexpr Card kDefaultCard = Card::Sandbox;

constexpr int Column(Card card) { return int(card) % 2; }
constexpr Card SelectColumn(Card card, int column) { return Card(int(card) / 2 * 2 + column); }
constexpr Card SelectRow(Card card, int row) { return Card(row * 2 + Column(card)); }
constexpr Card Cycle(Card card, bool backwards) { return Card((int(card) + (backwards ? 3 : 1)) % 4); }

constexpr std::optional<Mode> PlayMode(Card card, bool workshop_enabled) {
  switch (card) {
    case Card::Story: return Mode::Campaign;
    case Card::StoryCoop: return std::nullopt;  // No guest mission synchronization.
    case Card::Sandbox: if (workshop_enabled) return Mode::Solo; break;
    case Card::PrivateSession: if (workshop_enabled) return Mode::Multiplayer; break;
  }
  return std::nullopt;
}

}  // namespace gtav::frontend
