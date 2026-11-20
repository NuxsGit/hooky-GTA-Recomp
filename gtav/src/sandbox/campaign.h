#pragma once

#include "session.h"
#include "world.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace gtav::sandbox {

// Three protagonists, one per connected player. The assignment is host-owned
// and stable for the room (first join -> Michael, second -> Franklin, third -> Trevor).
enum class CampaignCharacter : uint8_t { Michael = 0, Franklin = 1, Trevor = 2, Count = 3 };
const char* CharacterLabel(CampaignCharacter character);
const char* CharacterLabelFr(CampaignCharacter character);
std::optional<CampaignCharacter> CharacterFromString(std::string_view name);
CampaignCharacter CharacterForSlot(size_t joined_index); // 0->Michael etc, wraps safely

inline constexpr unsigned kCampaignMaxPlayers = 3; // one per protagonist
inline constexpr const char* kCampaignSaveDir = "campaign_coop"; // under data/user/
inline constexpr const char* kCampaignProfileDir = "profile"; // profile inside campaign_coop
inline constexpr const char* kCampaignCoursesDir = "courses"; // not used but reserved

// Isolated save folder for campaign co-op. Returns root paths or nullopt on failure.
struct CampaignWorkspace {
  std::filesystem::path common_content_root; // same as sandbox: data/user canonical
  std::filesystem::path profile_root;        // data/user/campaign_coop/profile
  std::filesystem::path course_root;         // unused, kept for parity
};
enum class CampaignWorkspaceError { None, MissingInstall, SeparateFolderRequired };
std::optional<CampaignWorkspace> PrepareCampaignWorkspace(const std::filesystem::path& user_root,
                                                        CampaignWorkspaceError& error);
bool EnsureCampaignUnlock(const std::filesystem::path& profile_root);

// Lightweight replicated campaign session view. Wraps the generic Session but
// enforces 3-player cap and character binding.
struct CampaignView {
  Link link = Link::Idle;
  uint16_t port = 0;
  std::string invite;
  std::string error;
  World world; // for presence/UI reuse
  std::optional<uint32_t> rtt_ms;
  std::vector<Endpoint> host_addresses;
  // Campaign-specific: locked character per player id, host's view of assignments.
  std::optional<CampaignCharacter> self_character;
  // id -> character (only for players present in world.players)
  std::vector<std::pair<uint32_t, CampaignCharacter>> assignments;
  bool Owner() const { return link == Link::Idle || link == Link::Hosting || link == Link::Failed; }
};

// Character assignment is owned by the host Room; joining order determines lock.
// Exposed helpers for UI and replication.
CampaignCharacter CharacterForPlayer(const CampaignView& view, uint32_t player_id, CampaignCharacter fallback = CampaignCharacter::Michael);
std::string FormatCharacterAssignment(const CampaignView& view);

// Config persistence for launcher-editable IP/port (campaign host address).
struct CampaignHostConfig {
  std::string address = "0.0.0.0"; // editable host IP (empty means auto)
  uint16_t port = kDefaultPort;    // editable TCP port
  bool loopback_only = false;
};
CampaignHostConfig LoadCampaignHostConfig(const std::filesystem::path& data_root);
bool SaveCampaignHostConfig(const std::filesystem::path& data_root, const CampaignHostConfig& config);

// Host/Joined helpers that enforce campaign limits and isolate world.
// These are thin facades over the global Party() Session; they set up
// the campaign workspace and character mapping. The underlying Session
// remains the authoritative transport.
bool HostCampaign(uint16_t port, std::string name, bool loopback_only);
bool JoinCampaign(std::string_view address_or_invitation, std::string name, std::string_view invite = {});
CampaignView ViewCampaign();
void PublishCampaignPresence(std::optional<Presence> point);

namespace rex { struct PathConfig; }
// Called by desktop Configure when Mode::CampaignCoop is selected.
bool ConfigureCampaign(rex::PathConfig& paths, std::filesystem::path& installed_content_root,
                       std::string& error_out);

} // namespace gtav::sandbox
