#pragma once

#include <string>
#include <string_view>

namespace gtav::sandbox {

// GTA Online style death adapted to campaign co-op at 3.
// Open world (free roam, not in mission): solo death - only the dead player gets Wasted,
// respawns at nearest hospital, loses 2% cash (max $5000), others keep playing.
// Missions (story): shared - if one dies without revive in 30s, Mission Failed for all (host authoritative).
enum class DeathMode { SoloOnline, SharedMission };

DeathMode CampaignDeathMode(bool in_mission);
bool IsCampaignSoloDeath(bool in_mission); // true = open world solo (GTA Online style)
std::string DeathNotification(std::string_view player_name, bool in_mission);
const char* DeathModeLabel(DeathMode mode);

// GTA Online adapted economy: 2% cash penalty, capped at $5000, hospital respawn (solo).
inline constexpr double kSoloHospitalPenaltyRate = 0.02;
inline constexpr int kSoloHospitalPenaltyMax = 5000;
inline constexpr int kSharedMissionReviveWindowMs = 30'000;
int HospitalPenalty(int cash);
int ApplyHospitalPenalty(int cash); // returns cash after penalty (clamped)

// Mission state tracking for failure effect routing (updated from debug_tools PlayablePed tick).
void SetCampaignMissionState(bool in_mission);
bool GetCampaignMissionState();
bool CurrentDeathIsSolo(); // uses GetCampaignMissionState()

} // namespace gtav::sandbox
