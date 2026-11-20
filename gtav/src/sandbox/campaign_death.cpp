#include "campaign_death.h"

#include <algorithm>
#include <atomic>

namespace gtav::sandbox {

DeathMode CampaignDeathMode(bool in_mission) {
  return in_mission ? DeathMode::SharedMission : DeathMode::SoloOnline;
}

bool IsCampaignSoloDeath(bool in_mission) {
  return CampaignDeathMode(in_mission) == DeathMode::SoloOnline;
}

std::string DeathNotification(std::string_view player_name, bool in_mission) {
  if (IsCampaignSoloDeath(in_mission)) {
    return std::string(player_name) + " is Wasted (solo) - respawning at hospital - others continue (GTA Online style)";
  }
  return std::string(player_name) + " is Wasted - Mission Failed in 30s if not revived (shared)";
}

const char* DeathModeLabel(DeathMode mode) {
  return mode == DeathMode::SoloOnline ? "Solo Online (open world)" : "Shared (missions)";
}

int HospitalPenalty(int cash) {
  if (cash <= 0) return 0;
  int loss = static_cast<int>(cash * kSoloHospitalPenaltyRate);
  return std::clamp(loss, 0, kSoloHospitalPenaltyMax);
}

int ApplyHospitalPenalty(int cash) {
  int loss = HospitalPenalty(cash);
  return std::max(0, cash - loss);
}

namespace {
std::atomic_bool g_campaign_in_mission{false};
}

void SetCampaignMissionState(bool in_mission) {
  g_campaign_in_mission.store(in_mission, std::memory_order_relaxed);
}

bool GetCampaignMissionState() {
  return g_campaign_in_mission.load(std::memory_order_relaxed);
}

bool CurrentDeathIsSolo() {
  return IsCampaignSoloDeath(GetCampaignMissionState());
}

} // namespace gtav::sandbox
