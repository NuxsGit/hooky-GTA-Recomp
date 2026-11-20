#include "campaign_config.h"
#include "sources.h"

#include <fstream>
#include <algorithm>
#include <cctype>
#include <charconv>

namespace loader {
namespace fs = std::filesystem;

namespace {
fs::path ConfigPath(const fs::path& root) {
  return root / "data" / "campaign_coop.ini";
}
std::string TrimStr(std::string s) {
  size_t a = 0; while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
  size_t b = s.size(); while (b > a && std::isspace((unsigned char)s[b-1])) --b;
  return s.substr(a, b-a);
}
bool ParsePort(std::string_view text, uint16_t &out) {
  if (text.empty()) return false;
  uint32_t v = 0;
  auto res = std::from_chars(text.data(), text.data()+text.size(), v);
  if (res.ec != std::errc{} || res.ptr != text.data()+text.size() || v==0 || v>65535) return false;
  out = uint16_t(v);
  return true;
}
bool ValidIPv4(std::string_view ip) {
  if (ip.empty() || ip.size()>15) return false;
  int dots=0;
  std::string cur;
  for (char c: ip) {
    if (c=='.') { dots++; if (cur.empty()) return false;
      uint16_t p; if (!ParsePort(cur, p) || p>255) return false;
      cur.clear();
    } else if (c>='0' && c<='9') cur+=c;
    else return false;
  }
  if (cur.empty()) return false;
  uint16_t p; if (!ParsePort(cur, p) || p>255) return false;
  return dots==3;
}
}

CampaignHostConfig LoadCampaignHostConfig(const fs::path& root) {
  CampaignHostConfig cfg;
  std::error_code ec;
  auto path = ConfigPath(root);
  if (!fs::exists(path, ec) || ec) return cfg;
  std::ifstream in(path);
  if (!in) return cfg;
  std::string line;
  while (std::getline(in, line)) {
    line = TrimStr(line);
    if (line.empty() || line[0]=='#' || line[0]==';') continue;
    auto eq = line.find('=');
    if (eq==line.npos) continue;
    std::string key = TrimStr(line.substr(0,eq));
    std::string val = TrimStr(line.substr(eq+1));
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key=="address" || key=="ip" || key=="host") {
      if (val.empty() || val=="0.0.0.0" || val=="auto") cfg.address.clear();
      else if (ValidIPv4(val)) cfg.address = val;
    } else if (key=="port") {
      uint16_t p; if (ParsePort(val, p)) cfg.port = p;
    } else if (key=="loopback_only" || key=="loopback") {
      std::transform(val.begin(), val.end(), val.begin(), ::tolower);
      cfg.loopback_only = (val=="1" || val=="true" || val=="on" || val=="yes");
    } else if (key=="host_string" || key=="endpoint") {
      auto parsed = ParseCampaignHostString(val);
      if (parsed) cfg = *parsed;
    }
  }
  return cfg;
}

bool SaveCampaignHostConfig(const fs::path& root, const CampaignHostConfig& config) {
  std::error_code ec;
  fs::create_directories(root / "data", ec);
  if (ec) return false;
  auto path = ConfigPath(root);
  auto tmp = path; tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << "# Campaign coop host - editable in launcher (IP / port)\n";
    out << "# address: IPv4 for hosting (empty = auto). port: TCP 1-65535.\n";
    out << "address=" << (config.address.empty() ? "" : config.address) << "\n";
    out << "port=" << config.port << "\n";
    out << "loopback_only=" << (config.loopback_only ? "1" : "0") << "\n";
    if (!out.good()) return false;
  }
  fs::rename(tmp, path, ec);
  if (ec) { fs::remove(tmp, ec); return false; }
  return true;
}

std::string FormatCampaignHost(const CampaignHostConfig& cfg) {
  if (cfg.address.empty()) return std::to_string(cfg.port);
  return cfg.address + ":" + std::to_string(cfg.port);
}

std::optional<CampaignHostConfig> ParseCampaignHostString(std::string_view text) {
  text = std::string_view(TrimStr(std::string(text)));
  if (text.empty()) return std::nullopt;
  auto colon = text.find(':');
  std::string ip;
  uint16_t port = 30127;
  if (colon != text.npos) {
    ip = std::string(text.substr(0, colon));
    auto port_str = text.substr(colon+1);
    if (!ParsePort(port_str, port)) return std::nullopt;
  } else {
    // Could be pure port or pure ip. Detect digits only?
    bool all_digits = !text.empty() && std::all_of(text.begin(), text.end(), [](char c){return c>='0'&&c<='9';});
    if (all_digits) {
      if (!ParsePort(text, port)) return std::nullopt;
    } else {
      ip = std::string(text);
      if (!ValidIPv4(ip)) return std::nullopt;
    }
  }
  if (!ip.empty() && !ValidIPv4(ip)) return std::nullopt;
  CampaignHostConfig cfg;
  cfg.address = ip;
  cfg.port = port;
  return cfg;
}

} // namespace loader
