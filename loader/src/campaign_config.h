#pragma once

#include <filesystem>
#include <string>
#include <optional>

namespace loader {

struct CampaignHostConfig {
  std::string address = ""; // empty = auto (0.0.0.0)
  uint16_t port = 30127;
  bool loopback_only = false;
};

// Persistent launcher-editable campaign host config (IP/port).
// Stored under <game_root>/data/campaign_coop.ini (legacy: launcher.ini extension).
CampaignHostConfig LoadCampaignHostConfig(const std::filesystem::path& root);
bool SaveCampaignHostConfig(const std::filesystem::path& root, const CampaignHostConfig& config);
std::string FormatCampaignHost(const CampaignHostConfig& cfg); // ip:port or empty
std::optional<CampaignHostConfig> ParseCampaignHostString(std::string_view text); // ip[:port]

} // namespace loader
