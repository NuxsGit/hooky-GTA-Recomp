#include "update.h"

#include <iostream>
#include <string>

using namespace loader;

// No HTTP client or network needed to test release discovery policy.
bool loader::detail::FetchRelease(UpdateChannel, int*, std::string*, std::string*) { return false; }

namespace {
constexpr std::string_view kWindows = "gtav-recomp-windows-x64.zip";
constexpr std::string_view kLinux = "gtav-recomp-linux-x64.tar.gz";
constexpr std::string_view kMac = "gtav-recomp-macos-arm64.zip";
int failures = 0;

void Expect(bool condition, const char* name) {
  if (condition) return;
  std::cerr << "Failed: " << name << '\n';
  ++failures;
}

std::string Release(std::string_view tag, std::string_view name = kWindows,
                    std::string_view state = "uploaded", bool prerelease = false, bool draft = false) {
  // Include nested lookalikes, escapes, numbers and release text like a real API response.
  return R"({"id": 567, "body": "A { release } with \"quotes\" and \u00e9",)"
         R"("author":{"tag_name":"v800.0.0", "extras":[-1.2e2, null, true]}, "tag_name":")" +
         std::string(tag) + "\", \"draft\":" + (draft ? "true" : "false") +
         ", \"prerelease\":" + (prerelease ? "true" : "false") + ", \"assets\":[{\"name\":\"" +
         std::string(name) + "\", \"state\":\"" + std::string(state) +
         R"(", "browser_download_url": "https://example.invalid/ignore-this"}]})";
}

std::string Signed(std::string release) {
  release.insert(release.find("\"state\""), "\"size\":12345, \"digest\":\"sha256:" + std::string(64, 'a') + "\", ");
  return release;
}

UpdateResult Check(std::string_view body, std::string_view current = "0.3.9",
                   std::string_view asset = kWindows) {
  return EvaluateRelease(200, body, current, asset);
}
}  // namespace

int main() {
  const auto available = Check(Release("v0.3.10"));
  Expect(available.status == UpdateStatus::Available, "numeric patch ordering");
  Expect(available.message.find(kWindows) != std::string::npos, "correct Windows artifact");
  Expect(!available.asset, "a download link alone cannot authorize installing a ZIP");
  const auto tagged = Signed(Release("v0.4.0"));
  const auto verified = Check(tagged);
  Expect(verified.asset && verified.asset.version == "0.4.0" && verified.asset.sha256 == std::string(64, 'a') &&
         verified.asset.size == 12345, "verified Windows artifact metadata");
  Expect(!Check(tagged, "0.4.0").asset, "never authorize reinstall/downgrade");
  auto malformed_digest = tagged;
  malformed_digest[malformed_digest.find("sha256:") + 7] = 'z';
  Expect(!Check(malformed_digest).asset, "malformed digest is not installable");
  auto huge_zip = tagged;
  huge_zip.replace(huge_zip.find("12345"), 5, "1000000000");
  Expect(!Check(huge_zip).asset, "oversized ZIP is not installable");
  auto duplicate_digest = tagged;
  duplicate_digest.insert(duplicate_digest.find("\"digest\""), "\"digest\":null,");
  Expect(Check(duplicate_digest).status == UpdateStatus::Error, "duplicate digest rejected");
  Expect(available.url == std::string(kReleasePage) + "/tag/v0.3.10", "only trusted release URL");
  Expect(Check(Release("v0.4.0", kLinux), "0.3.99", kLinux).status == UpdateStatus::Available, "Linux artifact");
  Expect(Check(Release("v1.0.0", kMac), "0.99.99", kMac).status == UpdateStatus::Available, "Mac artifact");
  Expect(Check(Release("v0.3.9")).status == UpdateStatus::Current, "current");
  Expect(Check(Release("v0.3.2")).url.empty(), "no downgrade link");
  Expect(Check(Release("v1.0.0", kLinux)).status == UpdateStatus::NoPackage, "wrong platform");
  Expect(Check(Release("v1.0.0", kWindows, "starter")).status == UpdateStatus::NoPackage, "unfinished upload");
  Expect(Check(Release("v1.0.0-rc1", kWindows, "uploaded", true)).status == UpdateStatus::NoRelease, "prerelease");
  Expect(Check(Release("v1.0.0", kWindows, "uploaded", false, true)).status == UpdateStatus::NoRelease, "draft");
  Expect(EvaluateRelease(404, {}, kLauncherVersion, kWindows).status == UpdateStatus::NoRelease, "empty fork");
  Expect(EvaluateRelease(403, {}, kLauncherVersion, kWindows).status == UpdateStatus::Error, "rate limit");
  Expect(EvaluateRelease(302, {}, kLauncherVersion, kWindows).status == UpdateStatus::Error, "unexpected redirect");
  Expect(Check(Release("v2.03.0")).status == UpdateStatus::Error, "leading zero rejected");
  Expect(Check(Release("v42949672960.0.0")).status == UpdateStatus::Error, "integer overflow rejected");
  Expect(Check(Release("v1.0.0/other")).status == UpdateStatus::Error, "unsafe tag rejected");
  Expect(Check(Release("v1.0.0") + "{}").status == UpdateStatus::Error, "trailing content rejected");
  Expect(Check(R"({"tag_name":"v9.0.0","tag_name":"v1.0.0","draft":false,"prerelease":false,"assets":[]})")
                 .status == UpdateStatus::Error, "duplicate relevant field rejected");
  Expect(Check(std::string(kMaxReleaseBytes + 1, ' ')).status == UpdateStatus::Error, "body limit");
  Expect(Check("{\"nested\":" + std::string(128, '[') + std::string(128, ']') + "}").status == UpdateStatus::Error,
         "nesting limit");

  constexpr auto stable = UpdateChannel::Stable, nightly = UpdateChannel::Nightly;
  const auto night = [](std::string_view number) {
    return Signed(Release("v0.6.0-nightly." + std::string(number), kWindows, "uploaded", true));
  };
  const auto list = "[" + night("9") + "," + Signed(Release("v100.0.0")) + "," + night("100") + "," + night("10") +
      "," + Release("v8.0.0-nightly.5", kWindows, "uploaded", true, true) + "," +
      Release("v90.0.0-nightly.4", kWindows, "uploaded", false) + "," + Release("v99.0.0-rc1", kWindows, "uploaded", true) + "]";
  const auto selected = EvaluateRelease(200, list, "0.6.0", kWindows, nightly);
  Expect(selected.asset && selected.asset.channel == nightly && selected.latest == "0.6.0-nightly.100",
         "explicit Nightly opt-in selects highest immutable version");
  Expect(selected.url == std::string(kNightlyReleasePage) + "/tag/v0.6.0-nightly.100", "separate pinned nightly page");
  Expect(WindowsAssetPath(selected.asset) == "/" + std::string(ReleaseRepository(nightly)) +
         "/releases/download/v0.6.0-nightly.100/gtav-recomp-windows-x64.zip", "only official binary path");
  Expect(WindowsAssetPath({"0.6.0-nightly.100", std::string(64, 'a'), 12345, stable}).empty(), "channel/tag consistency");
  Expect(WindowsAssetPath({"0.7.0/extra", std::string(64, 'a'), 12345}).empty(), "unsafe download path refused");
  Expect(EvaluateRelease(200, list, "0.6.0-nightly.99", kWindows, nightly).asset.version == "0.6.0-nightly.100", "nightly serial order");
  Expect(EvaluateRelease(200, list, "0.6.0-nightly.100", kWindows, nightly).status == UpdateStatus::Current, "no nightly reinstall");
  Expect(EvaluateRelease(200, list, "0.6.0-nightly.101", kWindows, nightly).status == UpdateStatus::Current, "no nightly downgrade");
  Expect(EvaluateRelease(200, list, "0.7.0", kWindows, nightly).status == UpdateStatus::OlderChannel, "nightly cannot downgrade stable base");
  Expect(Check(Signed(Release("v0.6.0")), "0.6.0-nightly.100").asset.version == "0.6.0", "promote to same stable base");
  Expect(Check(Signed(Release("v0.5.0")), "0.6.0-nightly.100").status == UpdateStatus::OlderChannel, "cannot downgrade on stable switch");
  Expect(Check(night("42")).status == UpdateStatus::NoRelease, "stable ignores published prereleases");
  Expect(Check(Release("v0.6.0-nightly.42")).status == UpdateStatus::Error, "invalid stable tag rejected");
  Expect(EvaluateRelease(200, "[]", "0.6.0", kWindows, nightly).status == UpdateStatus::NoRelease, "no nightly releases");
  Expect(EvaluateRelease(200, "[" + night("42") + "," + night("42") + "]", "0.6.0", kWindows, nightly)
             .status == UpdateStatus::Error, "ambiguous nightly rejected");
  Expect(!ParseBuildVersion("0.7.0-nightly.0") && !ParseBuildVersion("0.7.0-nightly.01") &&
         !ParseBuildVersion("0.7.0-nightly.1234567890123456789") && !ParseBuildVersion("0.7.0-nightly.1/evil"),
         "nightly serial validation");
  Expect(detail::ReleaseApi(stable).ends_with("/releases/latest") && detail::ReleaseApi(nightly).ends_with("/releases?per_page=20"),
         "stable and nightly use independent endpoints");
  for (auto channel : {stable, nightly}) {
    for (bool auto_on : {false, true}) {
      const UpdatePreferences prefs{channel, auto_on};
      Expect(ParseUpdatePreferences(SerializeUpdatePreferences(prefs)) == prefs, "update preference round-trip");
    }
  }
  Expect(!ParseUpdatePreferences("gtav-updates-1\nnightly\non\nhttps://other.net"), "no URL override in settings");
  Expect(!ParseUpdatePreferences("gtav-updates-1\nunknown\non\n"), "invalid channel rejected");
  return failures ? 1 : 0;
}
