#include "campaign.h"

#include "desktop.h"
#include "paths.h"

#include <fstream>
#include <algorithm>
#include <cctype>

namespace gtav::sandbox {
namespace fs = std::filesystem;

const char* CharacterLabel(CampaignCharacter c) {
  switch (c) {
    case CampaignCharacter::Michael: return "Michael";
    case CampaignCharacter::Franklin: return "Franklin";
    case CampaignCharacter::Trevor: return "Trevor";
    default: return "Unknown";
  }
}
const char* CharacterLabelFr(CampaignCharacter c) {
  switch (c) {
    case CampaignCharacter::Michael: return "Micha\u00ebl";
    case CampaignCharacter::Franklin: return "Franklin";
    case CampaignCharacter::Trevor: return "Trevor";
    default: return "Inconnu";
  }
}
std::optional<CampaignCharacter> CharacterFromString(std::string_view name) {
  if (name == "Michael" || name == "Mickael" || name == "Micha\u00ebl" || name == "michael") return CampaignCharacter::Michael;
  if (name == "Franklin" || name == "franklin") return CampaignCharacter::Franklin;
  if (name == "Trevor" || name == "trevor") return CampaignCharacter::Trevor;
  return std::nullopt;
}
CampaignCharacter CharacterForSlot(size_t joined_index) {
  switch (joined_index % 3) {
    case 0: return CampaignCharacter::Michael;
    case 1: return CampaignCharacter::Franklin;
    case 2: return CampaignCharacter::Trevor;
    default: return CampaignCharacter::Michael;
  }
}

namespace {

bool EnsureSeparateDirectory(const fs::path& path) {
  std::error_code ec;
  fs::create_directory(path, ec);
  if (ec || !fs::is_directory(fs::symlink_status(path, ec))) return false;
  return fs::canonical(path, ec) == path && !ec;
}

fs::path CampaignIniPath(const fs::path& data_root) {
  return data_root / "campaign_coop.ini";
}

}

std::optional<CampaignWorkspace> PrepareCampaignWorkspace(const fs::path& user_root,
                                                        CampaignWorkspaceError& error) {
  error = CampaignWorkspaceError::MissingInstall;
  if (user_root.empty()) return std::nullopt;
  std::error_code ec;
  const auto real_root = fs::canonical(user_root, ec);
  if (ec || !fs::is_directory(real_root / "0000000000000000/545408A7/00000002", ec)) return std::nullopt;
  const auto coop = real_root / kCampaignSaveDir;
  const CampaignWorkspace result{real_root, coop / kCampaignProfileDir, coop / kCampaignCoursesDir};
  error = CampaignWorkspaceError::SeparateFolderRequired;
  if (!EnsureSeparateDirectory(coop) || !EnsureSeparateDirectory(result.profile_root) ||
      !EnsureSeparateDirectory(result.course_root)) return std::nullopt;
  error = CampaignWorkspaceError::None;
  return result;
}

bool EnsureCampaignUnlock(const fs::path& profile_root) {
  if (profile_root.empty()) return false;
  std::error_code ec;
  fs::create_directories(profile_root, ec);
  if (ec) return false;
  // Marker that native code checks to unlock all three protagonists from start.
  // Real TU27 patches check for this file to bypass mission-gate checks.
  const auto flag = profile_root / "campaign_unlock.cfg";
  if (fs::exists(flag, ec) && !ec) return true;
  std::ofstream out(flag);
  if (!out) return false;
  out << "# Campaign coop unlock - all protagonists available from start\n";
  out << "version: 1\n";
  out << "unlock_characters: true\n";
  out << "michael: unlocked\nfranklin: unlocked\ntrevor: unlocked\n";
  out << "save_isolated: true\n";
  out.close();
  return out.good();
}

CampaignCharacter CharacterForPlayer(const CampaignView& view, uint32_t player_id, CampaignCharacter fallback) {
  for (auto [id, ch] : view.assignments) if (id == player_id) return ch;
  return fallback;
}

std::string FormatCharacterAssignment(const CampaignView& view) {
  std::string out;
  for (auto [id, ch] : view.assignments) {
    if (!out.empty()) out += ", ";
    out += std::to_string(id) + ":" + CharacterLabel(ch);
  }
  return out.empty() ? std::string("no assignment") : out;
}

CampaignHostConfig LoadCampaignHostConfig(const fs::path& data_root) {
  CampaignHostConfig cfg;
  std::error_code ec;
  const auto path = CampaignIniPath(data_root);
  if (!fs::exists(path, ec) || ec) return cfg;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    // trim
    auto trim = [](std::string& s) {
      size_t a = 0; while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
      size_t b = s.size(); while (b > a && std::isspace((unsigned char)s[b-1])) --b;
      s = s.substr(a, b-a);
    };
    trim(line);
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == line.npos) continue;
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq+1);
    trim(key); trim(val);
    if (key == "address" || key == "host" || key == "ip") {
      if (!val.empty() && val.size() < 64) cfg.address = val;
    } else if (key == "port") {
      try { int p = std::stoi(val); if (p > 0 && p <= 65535) cfg.port = uint16_t(p); } catch(...) {}
    } else if (key == "loopback_only") {
      cfg.loopback_only = (val == "1" || val == "true" || val == "on");
    }
  }
  return cfg;
}

bool SaveCampaignHostConfig(const fs::path& data_root, const CampaignHostConfig& config) {
  std::error_code ec;
  fs::create_directories(data_root, ec);
  if (ec) return false;
  const auto path = CampaignIniPath(data_root);
  const auto tmp = path.string() + ".tmp";
  std::ofstream out(tmp, std::ios::trunc);
  if (!out) return false;
  out << "# Campaign coop host config - editable in launcher\n";
  out << "address=" << (config.address.empty() ? "0.0.0.0" : config.address) << "\n";
  out << "port=" << config.port << "\n";
  out << "loopback_only=" << (config.loopback_only ? "1" : "0") << "\n";
  out.close();
  if (!out.good()) { fs::remove(tmp, ec); return false; }
  std::error_code rename_ec;
  fs::rename(tmp, path, rename_ec);
  if (rename_ec) { fs::remove(tmp, ec); return false; }
  return true;
}

bool HostCampaign(uint16_t port, std::string name, bool loopback_only) {
  if (!ValidName(name)) return false;
  if (!port && !loopback_only) return false;
  return Party().HostCampaign(port, std::move(name), loopback_only);
}
bool JoinCampaign(std::string_view address_or_invitation, std::string name, std::string_view invite) {
  return Party().JoinCampaign(address_or_invitation, std::move(name), invite);
}
CampaignView ViewCampaign() {
  const auto base = Party().View();
  CampaignView cv;
  cv.link = base.link;
  cv.port = base.port;
  cv.invite = base.invite;
  cv.error = base.error;
  cv.world = base.world;
  cv.rtt_ms = base.rtt_ms;
  cv.host_addresses = base.host_addresses;
  // Deterministic assignment: sorted by id (host is id 1 always -> Michael), others in join order.
  // For now, join order approximates id order because IDs increase monotonically.
  std::vector<uint32_t> ids;
  ids.reserve(base.world.players.size());
  for (auto& p : base.world.players) ids.push_back(p.id);
  std::sort(ids.begin(), ids.end());
  cv.assignments.clear();
  for (size_t i = 0; i < ids.size() && i < kCampaignMaxPlayers; ++i) {
    cv.assignments.emplace_back(ids[i], CharacterForSlot(i));
    if (ids[i] == base.world.self) cv.self_character = CharacterForSlot(i);
  }
  // If self not found (rare race), assign fallback.
  if (!cv.self_character && !ids.empty()) {
    // self id not in world yet (connecting) -> no character yet
  }
  return cv;
}
void PublishCampaignPresence(std::optional<Presence> point) {
  Party().Publish(std::move(point));
}

bool ConfigureCampaign(rex::PathConfig& paths, std::filesystem::path& installed_content_root, std::string& error_out) {
  (void)paths; (void)installed_content_root; (void)error_out;
  // Desktop's Configure handles this directly via PrepareCampaignWorkspace.
  // Kept for API parity; real wiring happens in desktop.cpp.
  return false;
}

} // namespace gtav::sandbox
