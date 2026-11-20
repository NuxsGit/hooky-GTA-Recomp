#include "PermissionManager.h"

#include <cstdio>
#include <ctime>
#include <utility>

namespace gtav::workshop {
const char* Label(Role role) {
  switch (role) {
    case Role::Owner: return "Owner";
    case Role::Admin: return "Admin";
    case Role::Player: return "Player";
  }
  return "Unknown";
}
const char* Label(Mode mode) { return mode == Mode::Survival ? "Survival" : "Creative"; }

bool ValidIdentity(std::string_view text) {
  return !text.empty() && text.size() <= 64 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == ':' || c == '_' || c == '-' || c == '.';
  });
}

std::string_view Trim(std::string_view text) {
  const auto start = text.find_first_not_of(' ');
  return start == text.npos ? std::string_view{} : text.substr(start, text.find_last_not_of(' ') - start + 1);
}

// Single-line UTF-8 for both ImGui and audit records. Reject overlong, surrogate, terminal control
// and line separator code points. Do not split a multibyte sequence to enforce the byte limit.
bool ValidText(std::string_view text, size_t limit) {
  if (text.size() > limit || Trim(text).empty()) return false;
  for (size_t i = 0; i < text.size();) {
    const uint8_t c = uint8_t(text[i++]);
    uint32_t point = c;
    if (c >= 0x80) {
      unsigned n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
      if (c < 0xc2 || c > 0xf4 || n > text.size() - i) return false;
      point = c & (0x7f >> (n + 1));
      for (unsigned j = 0; j < n; ++j) {
        const uint8_t b = uint8_t(text[i++]);
        if ((b & 0xc0) != 0x80) return false;
        point = (point << 6) | (b & 63);
      }
      if ((n == 2 && point < 0x800) || (n == 3 && point < 0x10000) || point > 0x10ffff ||
          (point >= 0xd800 && point <= 0xdfff)) return false;
    }
    if (point < 32 || (point >= 127 && point <= 159) || (point >= 0x2028 && point <= 0x202e) || (point >= 0x2066 && point <= 0x2069)) return false;
  }
  return true;
}

std::string UtcNow() {
  const auto now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  if (gmtime_s(&tm, &now)) return "1970-01-01T00:00:00Z";
#else
  if (!gmtime_r(&now, &tm)) return "1970-01-01T00:00:00Z";
#endif
  char utc[32];
  return std::strftime(utc, sizeof utc, "%Y-%m-%dT%H:%M:%SZ", &tm) ? utc : "1970-01-01T00:00:00Z";
}

bool Permissions::Valid() const {
  if (prop_limit < 1 || prop_limit > 128 || admins.size() > PermissionManager::kMaxRecords) return false;
  for (auto it = admins.begin(); it != admins.end(); ++it)
    if (!ValidIdentity(*it) || it->starts_with("local:") || std::find(admins.begin(), it, *it) != it) return false;
  return true;
}

bool PermissionManager::ValidBans(std::span<const Ban> bans) {
  if (bans.size() > kMaxRecords) return false;
  for (auto it = bans.begin(); it != bans.end(); ++it) {
    if (!ValidIdentity(it->identity) || it->identity.starts_with("local:") || !ValidText(it->name, 40) ||
        !ValidText(it->reason, 160) || it->utc.size() != 20 || it->utc[4] != '-' || it->utc[7] != '-' ||
        it->utc[10] != 'T' || it->utc[13] != ':' || it->utc[16] != ':' || it->utc[19] != 'Z' ||
        std::find_if(bans.begin(), it, [&](const Ban& old) { return old.identity == it->identity; }) != it) return false;
    for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18})
      if (it->utc[i] < '0' || it->utc[i] > '9') return false;
  }
  return true;
}

PermissionManager::PermissionManager(Permissions settings, std::vector<Ban> bans) {
  if (settings.Valid()) settings_ = std::move(settings);
  if (ValidBans(bans)) bans_ = std::move(bans);
}
Role PermissionManager::Get(PlayerId id, std::string_view identity) const {
  if (id == kLocalOwner) return Role::Owner;
  return std::find(settings_.admins.begin(), settings_.admins.end(), identity) != settings_.admins.end()
             ? Role::Admin : Role::Player;
}
bool PermissionManager::IsBanned(std::string_view identity) const {
  return std::any_of(bans_.begin(), bans_.end(), [&](const Ban& b) { return b.identity == identity; });
}
bool PermissionManager::SetAdmin(Role actor, std::string_view identity, bool admin) {
  if (actor != Role::Owner || !ValidIdentity(identity) || identity.starts_with("local:") || IsBanned(identity))
    return false;
  auto& list = settings_.admins;
  auto it = std::find(list.begin(), list.end(), identity);
  if (admin && it == list.end()) {
    if (list.size() == kMaxRecords) return false;
    list.emplace_back(identity);
  } else if (!admin && it != list.end()) list.erase(it);
  return true;
}
bool PermissionManager::SetBuildRules(Role actor, bool allow, unsigned limit) {
  if (!AdminOS(actor) || limit < 1 || limit > 128) return false;
  settings_.players_build = allow;
  settings_.prop_limit = limit;
  return true;
}
bool PermissionManager::AddBan(Role actor, Role target, Ban ban) {
  if (!Moderate(actor, target) || !ValidBans(std::span(&ban, 1)) || IsBanned(ban.identity) || bans_.size() >= kMaxRecords)
    return false;
  bans_.push_back(std::move(ban));
  return true;
}
bool PermissionManager::Unban(Role actor, std::string_view identity) {
  return Moderate(actor, Get(0, identity)) && std::erase_if(bans_, [&](const Ban& b) { return b.identity == identity; }) != 0;
}
}  // namespace gtav::workshop
