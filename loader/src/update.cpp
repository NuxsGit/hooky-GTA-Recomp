#include "update.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <utility>

namespace loader {
namespace {

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
constexpr std::string_view kPackage = kWindowsPackageName;
#elif defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
constexpr std::string_view kPackage = "gtav-recomp-macos-arm64.zip";
#elif defined(__linux__) && defined(__x86_64__)
constexpr std::string_view kPackage = "gtav-recomp-linux-x64.tar.gz";
#else
constexpr std::string_view kPackage{};
#endif

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

  bool OptionalDigest(std::string_view* value) {
    Space();
    return Word("null") || String(value);
  }

  bool Unsigned(uint64_t* value) {
    Space();
    if (rest_.empty() || rest_.front() < '0' || rest_.front() > '9') return false;
    const char* begin = rest_.data();
    auto [end, error] = std::from_chars(begin, begin + rest_.size(), *value);
    if (error != std::errc{} || (end - begin > 1 && *begin == '0')) return false;
    rest_.remove_prefix(end - begin);
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

struct Release {
  std::string_view tag, digest;
  std::optional<bool> draft, prerelease;
  uint64_t size = 0;
  bool compatible = false;
};

bool ReadRelease(Json& json, std::string_view package, Release& release) {
  bool assets = false;
  return json.Object([&](std::string_view key) {
    if (key == "tag_name") return release.tag.empty() && json.String(&release.tag) && !release.tag.empty();
    if (key == "draft") return json.Boolean(&release.draft);
    if (key == "prerelease") return json.Boolean(&release.prerelease);
    if (key != "assets") return json.Skip();
    if (std::exchange(assets, true)) return false;
    return json.Array([&] {
      std::string_view name, state, digest;
      uint64_t size = 0;
      bool has_digest = false, has_size = false;
      if (!json.Object([&](std::string_view field) {
            if (field == "name") return name.empty() && json.String(&name) && !name.empty();
            if (field == "state") return state.empty() && json.String(&state) && !state.empty();
            if (field == "digest") return !std::exchange(has_digest, true) && json.OptionalDigest(&digest);
            if (field == "size") return !std::exchange(has_size, true) && json.Unsigned(&size);
            return json.Skip();
          })) return false;
      if (name == package && state == "uploaded") {
        if (std::exchange(release.compatible, true)) return false;
        release.digest = digest;
        release.size = size;
      }
      return true;
    });
  }) && assets && !release.tag.empty() && release.draft.has_value() && release.prerelease.has_value();
}

std::optional<BuildVersion> TaggedVersion(std::string_view tag) {
  return tag.starts_with('v') ? ParseBuildVersion(tag.substr(1)) : std::nullopt;
}

std::string UpdateInstructions() {
#ifdef __APPLE__
  return "Quit the game, replace GTAV.app with the new one, and keep the same game folder. Recompile if prompted.";
#elif defined(_WIN32)
  return "Open Updates in the launcher to install the verified ZIP. Your saves, game files and settings are kept.";
#else
  return "Quit the game and extract the download to a new folder. Copy your data folder, "
         "locally built game library and settings into it (see README). Recompile if prompted.";
#endif
}

}  // namespace

UpdateResult EvaluateRelease(int status, std::string_view body, std::string_view current,
                             std::string_view package, UpdateChannel channel) {
  const bool nightly = channel == UpdateChannel::Nightly;
  const std::string channel_name = nightly ? "nightly" : "stable";
  const UpdateResult none{UpdateStatus::NoRelease, "No " + channel_name + " release has been published for this channel yet.", {}};
  if (status == 404) return none;
  if (status != 200)
    return {UpdateStatus::Error, "GitHub update check failed (HTTP " + std::to_string(status) + "). Try again later.", {}};
  if (body.size() > kMaxReleaseBytes) return {UpdateStatus::Error, "The GitHub response is too large.", {}};
  const UpdateResult invalid{UpdateStatus::Error, "GitHub did not return valid " + channel_name + " release metadata.", {}};
  const auto installed = ParseBuildVersion(current);
  if (!installed) return invalid;

  Json json(body);
  Release newest;
  std::optional<BuildVersion> latest;
  if (nightly) {
    // The rolling list is small. Pick the highest parsed version, not the order of API records
    // or the release name. Stable, drafts and unrelated previews never enter the nightly feed.
    std::array<BuildVersion, 20> seen;
    auto seen_end = seen.begin();
    unsigned count = 0;
    if (!json.Array([&] {
          Release item;
          if (++count > seen.size() || !ReadRelease(json, package, item)) return false;
          if (*item.draft || !*item.prerelease) return true;
          auto version = TaggedVersion(item.tag);
          if (!version || version->Channel() != channel) return true;
          if (std::find(seen.begin(), seen_end, *version) != seen_end) return false;
          *seen_end++ = *version;
          if (!latest || CanUpdate(*latest, *version)) { latest = version; newest = item; }
          return true;
        }) || !json.Done()) return invalid;
    if (!latest) return none;
  } else {
    if (!ReadRelease(json, package, newest) || !json.Done()) return invalid;
    if (*newest.draft || *newest.prerelease) return none;
    latest = TaggedVersion(newest.tag);
    if (!latest || latest->Channel() != channel) return invalid;
  }
  const std::string tag(newest.tag), version = tag.substr(1);
  if (!CanUpdate(*installed, *latest)) {
    if (installed->base > latest->base && installed->Channel() != channel) {
      return {UpdateStatus::OlderChannel, "This channel is older than your installed build. To switch, use a separate installation.",
              {}, {}, version};
    }
    return {UpdateStatus::Current, "Launcher v" + std::string(current) + " is up to date (latest " + channel_name + ": " + tag + ").",
            {}, {}, version};
  }
  if (!newest.compatible)
    return {UpdateStatus::NoPackage, tag + " is published, but the package for this system is not available.", {}, {}, version};
  UpdateAsset asset;
  if (newest.digest.starts_with("sha256:")) {
    UpdateAsset candidate{version, std::string(newest.digest.substr(7)), newest.size, channel};
    if (candidate) asset = std::move(candidate);
  }
  return {UpdateStatus::Available, "Update available: " + tag + " (installed: v" + std::string(current) +
                                      ").\nDownload " + std::string(package) + " from GitHub.\n\n" + UpdateInstructions(),
          std::string(ReleasePage(channel)) + "/tag/" + tag, std::move(asset), version};
}

UpdateResult CheckForUpdate(UpdateChannel channel) {
  if (kPackage.empty()) return {UpdateStatus::Error, "This system has no supported desktop package.", {}};
  int status = 0;
  std::string body, error;
  if (!detail::FetchRelease(channel, &status, &body, &error)) return {UpdateStatus::Error, std::move(error), {}};
  return EvaluateRelease(status, body, kLauncherVersion, kPackage, channel);
}

std::string WindowsAssetPath(const UpdateAsset& asset) {
  if (!asset) return {};
  return "/" + std::string(ReleaseRepository(asset.channel)) + "/releases/download/v" + asset.version + "/" +
         std::string(kWindowsPackageName);
}

std::optional<UpdatePreferences> ParseUpdatePreferences(std::string_view text) {
  constexpr std::string_view magic = "gtav-updates-1\n";
  if (!text.starts_with(magic)) return std::nullopt;
  text.remove_prefix(magic.size());
  const auto newline = text.find('\n');
  if (newline == text.npos) return std::nullopt;
  const auto channel = text.substr(0, newline);
  const auto automatic = text.substr(newline + 1);
  if ((channel != "nightly" && channel != "stable") || (automatic != "on\n" && automatic != "off\n")) return std::nullopt;
  return UpdatePreferences{channel == "nightly" ? UpdateChannel::Nightly : UpdateChannel::Stable, automatic == "on\n"};
}

std::string SerializeUpdatePreferences(UpdatePreferences prefs) {
  return std::string("gtav-updates-1\n") + (prefs.channel == UpdateChannel::Nightly ? "nightly\n" : "stable\n") +
         (prefs.automatic ? "on\n" : "off\n");
}

}  // namespace loader
