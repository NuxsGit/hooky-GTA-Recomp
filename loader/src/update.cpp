#include "update.h"

#include <array>
#include <charconv>
#include <optional>
#include <utility>

namespace loader {
namespace {

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
constexpr std::string_view kPackage = "gtav-recomp-windows-x64.zip";
#elif defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
constexpr std::string_view kPackage = "gtav-recomp-macos-arm64.zip";
#elif defined(__linux__) && defined(__x86_64__)
constexpr std::string_view kPackage = "gtav-recomp-linux-x64.tar.gz";
#else
constexpr std::string_view kPackage{};
#endif

using Version = std::array<unsigned, 3>;

std::optional<Version> ParseVersion(std::string_view text) {
  Version version{};
  for (size_t i = 0; i < version.size(); ++i) {
    const size_t dot = text.find('.');
    if ((i < 2) != (dot != text.npos)) return std::nullopt;
    auto part = text.substr(0, dot);
    if (part.empty() || (part.size() > 1 && part.front() == '0')) return std::nullopt;
    auto end = part.data() + part.size();
    auto [ptr, ec] = std::from_chars(part.data(), end, version[i]);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    if (i < 2) text.remove_prefix(dot + 1);
  }
  return version;
}

// Read just the fields used by the launcher, skipping release notes and unknown values. We only
// compare strings with fixed ASCII keys, package names and version syntax; escapes are validated
// but not decoded. Never take a URL (or a command) from the response.
class Json {
 public:
  explicit Json(std::string_view input) : rest_(input) {}

  template <typename Visit>
  bool Object(Visit field) {
    return List('{', '}', [&] {
      std::string_view key;
      return String(&key) && Take(':') && field(key);
    });
  }

  template <typename Visit>
  bool Array(Visit element) {
    return List('[', ']', element);
  }

  bool String(std::string_view* value) {
    if (!Take('"')) return false;
    for (size_t i = 0; i < rest_.size(); ++i) {
      const unsigned char c = rest_[i];
      if (c < 0x20) return false;
      if (c == '"') {
        *value = rest_.substr(0, i);
        rest_.remove_prefix(i + 1);
        return true;
      }
      if (c != '\\') continue;
      if (++i == rest_.size()) return false;
      if (rest_[i] == 'u') {
        for (int n = 0; n < 4; ++n) {
          if (++i == rest_.size() || std::string_view("0123456789abcdefABCDEF").find(rest_[i]) == rest_.npos)
            return false;
        }
      } else if (std::string_view("\"\\/bfnrt").find(rest_[i]) == rest_.npos) {
        return false;
      }
    }
    return false;
  }

  bool Boolean(std::optional<bool>* value) {
    if (value->has_value()) return false;  // reject duplicate fields
    if (Word("false")) *value = false;
    else if (Word("true")) *value = true;
    else return false;
    return true;
  }

  bool Skip(int depth = 0) {
    if (depth >= 32) return false;
    Space();
    if (rest_.empty()) return false;
    switch (rest_.front()) {
      case '{': return Object([&](std::string_view) { return Skip(depth + 1); });
      case '[': return Array([&] { return Skip(depth + 1); });
      case '"': {
        std::string_view unused;
        return String(&unused);
      }
      case 't': return Word("true");
      case 'f': return Word("false");
      case 'n': return Word("null");
      default: return Number();
    }
  }

  bool Done() { Space(); return rest_.empty(); }

 private:
  void Space() {
    while (!rest_.empty() && std::string_view(" \r\n\t").find(rest_.front()) != rest_.npos)
      rest_.remove_prefix(1);
  }

  bool Take(char c) {
    Space();
    return Char(c);
  }

  bool Char(char c) {
    if (rest_.empty() || rest_.front() != c) return false;
    rest_.remove_prefix(1);
    return true;
  }

  bool Word(std::string_view text) {
    Space();
    if (!rest_.starts_with(text)) return false;
    rest_.remove_prefix(text.size());
    return true;
  }

  bool Digits() {
    bool found = false;
    while (!rest_.empty() && rest_.front() >= '0' && rest_.front() <= '9') {
      rest_.remove_prefix(1);
      found = true;
    }
    return found;
  }

  bool Number() {
    Char('-');
    if (!Char('0') && !Digits()) return false;
    if (Char('.') && !Digits()) return false;
    if (Char('e') || Char('E')) {
      if (!Char('+')) Char('-');
      return Digits();
    }
    return true;
  }

  template <typename Visit>
  bool List(char open, char close, Visit visit) {
    if (!Take(open)) return false;
    if (Take(close)) return true;
    do {
      if (!visit()) return false;
      if (Take(close)) return true;
    } while (Take(','));
    return false;
  }

  std::string_view rest_;
};

std::string UpdateInstructions() {
#ifdef __APPLE__
  return "Quit the game, replace GTAV.app with the new one, and keep the same game folder. "
         "Recompile if prompted.";
#else
  return "Quit the game and extract the download to a new folder. Copy your data folder, "
         "locally built game library and settings into it (see README). Recompile if prompted.";
#endif
}

}  // namespace

UpdateResult EvaluateRelease(int status, std::string_view body, std::string_view current,
                             std::string_view package) {
  if (status == 404) return {UpdateStatus::NoRelease, "No stable release has been published for this fork yet.", {}};
  if (status != 200)
    return {UpdateStatus::Error, "GitHub update check failed (HTTP " + std::to_string(status) + "). Try again later.", {}};
  if (body.size() > kMaxReleaseBytes)
    return {UpdateStatus::Error, "The GitHub response is too large.", {}};

  Json json(body);
  std::string_view tag;
  std::optional<bool> draft, prerelease;
  bool assets = false, compatible = false;
  const bool parsed = json.Object([&](std::string_view key) {
    if (key == "tag_name") return tag.empty() && json.String(&tag) && !tag.empty();
    if (key == "draft") return json.Boolean(&draft);
    if (key == "prerelease") return json.Boolean(&prerelease);
    if (key != "assets") return json.Skip();
    if (assets) return false;
    assets = true;
    return json.Array([&] {
      std::string_view name, state;
      if (!json.Object([&](std::string_view asset_key) {
            if (asset_key == "name") return name.empty() && json.String(&name) && !name.empty();
            if (asset_key == "state") return state.empty() && json.String(&state) && !state.empty();
            return json.Skip();
          })) return false;
      compatible |= name == package && state == "uploaded";
      return true;
    });
  });
  const auto bad_release = UpdateResult{UpdateStatus::Error, "GitHub did not return valid stable release metadata.", {}};
  if (!parsed || !json.Done() || !draft.has_value() || !prerelease.has_value() || !assets || tag.empty())
    return bad_release;
  if (*draft || *prerelease)
    return {UpdateStatus::NoRelease, "No stable update is available from this fork.", {}};
  const auto installed = ParseVersion(current);
  const auto latest = tag.starts_with('v') ? ParseVersion(tag.substr(1)) : std::nullopt;
  if (!latest || !installed) return bad_release;
  if (*latest <= *installed)
    return {UpdateStatus::Current, "Launcher v" + std::string(current) + " is up to date (latest stable: " +
                                       std::string(tag) + ").", {}};
  if (!compatible)
    return {UpdateStatus::NoPackage, std::string(tag) + " is published, but the package for this system is not available.", {}};
  return {UpdateStatus::Available, "Update available: " + std::string(tag) + " (installed: v" + std::string(current) +
                                      ").\nDownload " + std::string(package) + " from GitHub.\n\n" + UpdateInstructions(),
          std::string(kReleasePage) + "/tag/" + std::string(tag)};
}

UpdateResult CheckForUpdate() {
  if (kPackage.empty()) return {UpdateStatus::Error, "This system has no supported desktop package.", {}};
  int status = 0;
  std::string body, error;
  if (!detail::FetchRelease(&status, &body, &error)) return {UpdateStatus::Error, std::move(error), {}};
  return EvaluateRelease(status, body, kLauncherVersion, kPackage);
}

}  // namespace loader
