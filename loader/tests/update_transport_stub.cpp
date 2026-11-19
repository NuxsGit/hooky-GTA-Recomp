// Network stand-in linked ONLY into the older launcher used by the Windows package smoke test.
// The shipped launcher always links the WinHTTP transport, with no file-feed switch.
#include "windows_update.h"

#include <windows.h>

#include <fstream>

namespace fs = std::filesystem;

namespace loader::detail {
namespace {
fs::path TestFeedFolder() {
  std::wstring exe(32768, L'\0');
  const auto n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
  return n && n < exe.size() ? fs::path(exe.substr(0, n)).parent_path() / L".test-feed" : fs::path();
}
}  // namespace

bool FetchRelease(UpdateChannel channel, int* status, std::string* json, std::string* error) {
  const auto folder = TestFeedFolder();
  if (folder.empty()) return false;
  const auto name = channel == UpdateChannel::Nightly ? L"nightly.json" : L"stable.json";
  std::ifstream in(folder / name, std::ios::binary);
  if (!in) { *status = 404; return true; }
  json->resize(kMaxReleaseBytes + 1);
  in.read(json->data(), static_cast<std::streamsize>(json->size()));
  json->resize(static_cast<size_t>(in.gcount()));
  if (!in.eof() || in.bad()) { *error = "Could not read the update test feed."; return false; }
  *status = 200;
  return true;
}

bool DownloadWindowsAsset(const UpdateAsset& asset, const fs::path& output, InstallUi& ui, std::wstring* error) {
  const auto folder = TestFeedFolder();
  std::error_code ec;
  if (folder.empty() || !asset || ui.Cancelled() || !fs::copy_file(folder / L"asset.zip", output, ec) || ec) {
    *error = L"Could not copy the exact update fixture ZIP.";
    return false;
  }
  return true;
}
}  // namespace loader::detail
