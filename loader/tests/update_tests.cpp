#include "update.h"

#include <iostream>
#include <string>

using namespace loader;

// No HTTP client or network needed to test release discovery policy.
bool loader::detail::FetchRelease(int*, std::string*, std::string*) { return false; }

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

UpdateResult Check(std::string_view body, std::string_view current = "0.3.9",
                   std::string_view asset = kWindows) {
  return EvaluateRelease(200, body, current, asset);
}
}  // namespace

int main() {
  const auto available = Check(Release("v0.3.10"));
  Expect(available.status == UpdateStatus::Available, "numeric patch ordering");
  Expect(available.message.find(kWindows) != std::string::npos, "correct Windows artifact");
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
  return failures ? 1 : 0;
}
