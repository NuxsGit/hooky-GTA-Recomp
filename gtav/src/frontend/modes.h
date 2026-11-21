#pragma once

#include <optional>

namespace gtav::frontend {

enum class Mode { Campaign, Solo, Multiplayer, CampaignCoop, Creator };
// Two columns (Campaign / Sandbox), with solo above multiplayer in each.
// CreatorMAP is the 5th card — full-width bottom row, Unity/Unreal style map & scenario editor.
enum class Card { Story, Sandbox, StoryCoop, PrivateSession, Creator };
inline constexpr Card kDefaultCard = Card::Sandbox;

constexpr int Column(Card card) {
  switch (card) {
    case Card::Story: return 0;
    case Card::Sandbox: return 1;
    case Card::StoryCoop: return 0;
    case Card::PrivateSession: return 1;
    case Card::Creator: return 2;
  }
  return 0;
}
constexpr int Row(Card card) {
  switch (card) {
    case Card::Story: case Card::Sandbox: return 0;
    case Card::StoryCoop: case Card::PrivateSession: case Card::Creator: return 1;
  }
  return 0;
}
constexpr Card CardAt(int row, int col) {
  if (row == 0) return col == 0 ? Card::Story : Card::Sandbox;
  // row 1 has 3 columns
  if (col <= 0) return Card::StoryCoop;
  if (col == 1) return Card::PrivateSession;
  return Card::Creator;
}
constexpr Card SelectColumn(Card card, int column) {
  const int row = Row(card);
  const int max_col = row == 0 ? 1 : 2;
  column = std::clamp(column, 0, max_col);
  return CardAt(row, column);
}
constexpr Card SelectRow(Card card, int row) {
  const int col = Column(card);
  row = std::clamp(row, 0, 1);
  const int max_col = row == 0 ? 1 : 2;
  const int clamped_col = std::min(col, max_col);
  return CardAt(row, clamped_col);
}
constexpr Card Cycle(Card card, bool backwards) { return Card((int(card) + (backwards ? 4 : 1)) % 5); }

// CampaignCoop is a 3-player story mode where each player owns one protagonist
// (Michael / Franklin / Trevor) and the save is isolated from the solo profile.
// It reuses the private-session transport but with a 3-slot room and locked roles.
constexpr std::optional<Mode> PlayMode(Card card, bool workshop_enabled) {
  switch (card) {
    case Card::Story: return Mode::Campaign;
    case Card::StoryCoop: if (workshop_enabled) return Mode::CampaignCoop; break;
    case Card::Sandbox: if (workshop_enabled) return Mode::Solo; break;
    case Card::PrivateSession: if (workshop_enabled) return Mode::Multiplayer; break;
    case Card::Creator: if (workshop_enabled) return Mode::Creator; break;
  }
  return std::nullopt;
}

}  // namespace gtav::frontend
