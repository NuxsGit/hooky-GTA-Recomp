// Native Windows update: authenticate the archive against the fork's GitHub asset digest, then
// update only package-owned files. No scripts, shell command interpretation, or move of data/.
#include "windows_update.h"

#include "win_process.h"
#include "windows_package_layout.h"
#include "sources.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace loader {

bool HashUpdateArchive(const fs::path& path, std::string* digest, uint64_t* size, InstallUi& ui);

namespace {

constexpr wchar_t kWork[] = L".gtav-update", kHelper[] = L"apply.exe", kJournal[] = L"pending.txt";
using windows_package::IsFolder;
using windows_package::kLauncher;
using windows_package::kOwned;

fs::path StagedPackage(const fs::path& work) { return work / L"stage" / PathFromUtf8(std::string(kWindowsPackageStem)); }

bool Error(std::wstring* error, std::wstring message) { *error = std::move(message); return false; }

std::wstring Quote(const fs::path& path) { return L"\"" + PathWide(path) + L"\""; }

enum class PathKind { Missing, File, Folder, Unsafe };
PathKind Kind(const fs::path& path) {
  const DWORD flags = GetFileAttributesW(path.c_str());
  if (flags == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? PathKind::Missing : PathKind::Unsafe;
  }
  if (flags & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return PathKind::Unsafe;
  return flags & FILE_ATTRIBUTE_DIRECTORY ? PathKind::Folder : PathKind::File;
}

bool IsHex(std::string_view value) {
  return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == value.npos;
}

bool ReadLine(const fs::path& file, std::string* text, uintmax_t max_bytes) {
  if (Kind(file) != PathKind::File) return false;
  std::error_code ec;
  if (fs::file_size(file, ec) > max_bytes || ec) return false;
  std::ifstream in(file);
  return bool(std::getline(in, *text)) && in.peek() == std::char_traits<char>::eof();
}

bool ValidatePackage(const fs::path& work, std::string_view version, std::wstring* error) {
  const auto stage = StagedPackage(work);
  if (Kind(work) != PathKind::Folder || Kind(work / L"stage") != PathKind::Folder || Kind(stage) != PathKind::Folder)
    return Error(error, L"The verified archive does not have the expected Windows package folder.");
  std::error_code ec;
  for (auto it = fs::directory_iterator(work / L"stage", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    if (it->path().filename() != stage.filename())
      return Error(error, L"The ZIP contains files outside the expected package folder.");
  }
  if (ec) return Error(error, L"Could not read the update staging folder.");
  for (auto it = fs::directory_iterator(stage, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const auto name = it->path().filename().wstring();
    if (std::find(kOwned.begin(), kOwned.end(), name) == kOwned.end())
      return Error(error, L"This ZIP has a different package layout. Extract it to a new folder instead.");
  }
  if (ec) return Error(error, L"Could not read the staged package.");
  for (const auto* name : kOwned) {
    if (Kind(stage / name) != (IsFolder(name) ? PathKind::Folder : PathKind::File))
      return Error(error, L"Invalid or missing package file: " + std::wstring(name));
  }
  uint64_t bytes = 0;
  unsigned entries = 0;
  for (auto it = fs::recursive_directory_iterator(stage, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const auto type = Kind(it->path());
    if (++entries > 10'000 || (type != PathKind::File && type != PathKind::Folder))
      return Error(error, L"The staged package contains unsafe file entries.");
    if (type != PathKind::File) continue;
    const auto size = it->file_size(ec);
    if (ec || size > (2ull << 30) - bytes) return Error(error, L"The staged package is too large or unreadable.");
    bytes += size;
    auto ext = it->path().extension().wstring();
    for (auto& c : ext) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    if (ext == L".xex" || ext == L".xexp" || ext == L".rpf" || ext == L".iso" || ext == L".obj" || ext == L".spv" ||
        CompareStringOrdinal(it->path().filename().c_str(), -1, L"gtav_game.dll", -1, TRUE) == CSTR_EQUAL)
      return Error(error, L"The ZIP contains local game material and cannot be used for updates.");
  }
  if (ec) return Error(error, L"Could not check the staged package files.");
  std::string marker, build;
  if (!ReadLine(stage / "gtav_release.txt", &marker, 256) ||
      marker != "gtav-recomp release folder " + std::string(version) + " (windows-x64)" ||
      !ReadLine(stage / "tools/game_dll/build-id.txt", &build, 128) || !IsHex(build))
    return Error(error, L"The release version or game build metadata does not match the update.");
  for (const auto* file : {L"tools/game_dll/expected.txt", L"tools/game_dll/codegen.partition.json",
                           L"tools/game_dll/gtav_manifest.toml", L"tools/game_dll/fs/gtav_pch.h",
                           L"tools/game_dll/interface/gtav_game.def", L"tools/llvm/bin/clang++.exe",
                           L"tools/llvm/bin/lld-link.exe"}) {
    if (Kind(stage / file) != PathKind::File) return Error(error, L"Missing required update input: " + std::wstring(file));
  }
  return true;
}

bool Move(const fs::path& from, const fs::path& to, bool replace = false) {
  return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0));
}

struct Journal {
  std::string version;
  std::array<bool, kOwned.size()> old{};
};

bool ReadJournal(const fs::path& work, Journal* journal) {
  if (Kind(work) != PathKind::Folder || Kind(work / "backup") != PathKind::Folder || Kind(work / "stage") != PathKind::Folder ||
      Kind(StagedPackage(work)) != PathKind::Folder) return false;
  std::string text;
  if (!ReadLine(work / kJournal, &text, 128) || !text.starts_with("gtav-update-1 ")) return false;
  const auto split = text.find(' ', 14);
  if (split == text.npos) return false;
  journal->version = text.substr(14, split - 14);
  if (!ParseBuildVersion(journal->version) || text.size() != split + 1 + kOwned.size()) return false;
  for (size_t i = 0; i < kOwned.size(); ++i) {
    const char bit = text[split + 1 + i];
    if (bit != '0' && bit != '1') return false;
    journal->old[i] = bit == '1';
  }
  return true;
}

bool WriteJournal(const fs::path& work, const std::string& text) {
  const auto temp = work / L"pending.new";
  win::Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  DWORD written = 0;
  if (!file || !WriteFile(file.Get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
      written != text.size() || !FlushFileBuffers(file.Get())) return false;
  file.Close();
  return Move(temp, work / kJournal);
}

bool SafeOwnedPath(const fs::path& path) {
  const auto type = Kind(path);
  return type == PathKind::Missing || type == (IsFolder(path.filename().wstring()) ? PathKind::Folder : PathKind::File);
}

bool StartHelper(const fs::path& root, bool recover, HANDLE lock, std::wstring* error) {
  const auto work = root / kWork;
  const auto helper = work / kHelper;
  HANDLE parent = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent,
                       SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0))
    return Error(error, L"Could not prepare the launcher restart.");
  win::Handle owned_parent(parent);
  HANDLE inherit[] = {lock, parent};
  const auto command = Quote(helper) + (recover ? L" --recover-update " : L" --commit-update ") +
      std::to_wstring(reinterpret_cast<uintptr_t>(parent)) + L" " + std::to_wstring(reinterpret_cast<uintptr_t>(lock));
  return (Kind(helper) == PathKind::File && win::Spawn(helper, command, work, 0, nullptr, inherit)) ||
         Error(error, L"Could not start the update helper. Close the launcher and try again.");
}

DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER,
                            DWORD, DWORD, HANDLE, HANDLE, LPVOID userdata) {
  return static_cast<InstallUi*>(userdata)->Cancelled() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

bool Extract(const fs::path& archive, const fs::path& stage, InstallUi& ui, std::wstring* error) {
  wchar_t system[MAX_PATH];
  const UINT length = GetSystemDirectoryW(system, MAX_PATH);
  if (!length || length >= MAX_PATH) return Error(error, L"Could not find the Windows system directory.");
  const auto tar = fs::path(system) / "tar.exe";
  win::Handle process;
  if (!win::Spawn(tar, Quote(tar) + L" -xf " + Quote(archive) + L" -C " + Quote(stage), stage,
                  CREATE_NO_WINDOW, &process)) return Error(error, L"Windows tar.exe is required to install ZIP updates.");
  for (int tries = 0; tries < 1200 && !ui.Cancelled(); ++tries) {
    const DWORD wait = WaitForSingleObject(process.Get(), 150);
    if (wait == WAIT_TIMEOUT) continue;
    DWORD status = 1;
    return (wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.Get(), &status) && status == 0) ||
           Error(error, L"Windows could not unpack the verified ZIP. Check available disk space.");
  }
  if (TerminateProcess(process.Get(), 1)) WaitForSingleObject(process.Get(), INFINITE);
  return Error(error, ui.Cancelled() ? L"Cancelled." : L"Timed out unpacking the update.");
}

}  // namespace

namespace detail {

bool JournalUpdate(const fs::path& root, std::string_view version, std::wstring* error) {
  const auto work = root / kWork;
  const auto backup = work / L"backup";
  if (!ParseBuildVersion(version) || Kind(work / kJournal) != PathKind::Missing || !ValidatePackage(work, version, error)) {
    if (error->empty()) *error = L"A previous update still needs recovery.";
    return false;
  }
  std::string journal = "gtav-update-1 " + std::string(version) + ' ';
  for (const auto* name : kOwned) {
    if (!SafeOwnedPath(root / name)) return Error(error, L"The update won't replace linked or unexpected files: " + std::wstring(name));
    journal += Kind(root / name) == PathKind::Missing ? '0' : '1';
  }
  std::error_code ec;
  if (!journal.ends_with('1')) return Error(error, L"The original launcher is missing.");
  if (Kind(backup) == PathKind::Unsafe) return Error(error, L"The backup path is unsafe.");
  fs::remove_all(backup, ec);
  if (ec || !fs::create_directory(backup, ec) || ec)
    return Error(error, L"Could not create the update backup folder.");
  fs::remove(work / L"pending.new", ec);
  return (!ec && WriteJournal(work, journal + '\n')) || Error(error, L"Could not write the recovery journal.");
}

bool ApplyUpdate(const fs::path& root, std::wstring* error) {
  const auto work = root / kWork, stage = StagedPackage(work), backup = work / L"backup";
  Journal journal;
  if (!ReadJournal(work, &journal) || !ValidatePackage(work, journal.version, error)) {
    if (error->empty()) *error = L"The prepared update journal is missing or invalid.";
    return false;
  }
  for (size_t i = 0; i < kOwned.size(); ++i) {
    const auto* name = kOwned[i];
    const auto old = root / name, saved = backup / name;
    if (!SafeOwnedPath(old) || (Kind(old) != PathKind::Missing) != journal.old[i] || Kind(saved) != PathKind::Missing)
      return Error(error, L"The runtime changed during preparation: " + std::wstring(name));
    if (name == kLauncher) {
      // Keep an executable at the original path at all times, including after power loss.
      const auto temporary = backup / L"launcher.part";
      if (!CopyFileW(old.c_str(), temporary.c_str(), TRUE) || !Move(temporary, saved) || !Move(stage / name, old, true))
        return Error(error, L"Could not replace gtav_launcher.exe.");
    } else if ((journal.old[i] && !Move(old, saved)) || !Move(stage / name, old)) {
      return Error(error, L"Could not replace " + std::wstring(name) + L". Close any game or tools using this folder.");
    }
  }
  if (!DeleteFileW((work / kJournal).c_str())) return Error(error, L"Could not commit the update journal.");
  return true;
}

bool RollbackUpdate(const fs::path& root, std::wstring* error) {
  const auto work = root / kWork, backup = work / L"backup", stage = StagedPackage(work);
  Journal journal;
  if (!ReadJournal(work, &journal)) return Error(error, L"The update recovery journal is invalid.");
  // Idempotent after an interruption. Rename only the owned paths listed in the original journal.
  for (size_t i = kOwned.size(); i-- > 0;) {
    const auto* name = kOwned[i];
    const auto target = root / name, incoming = stage / name, saved = backup / name;
    auto fail = [&] { return Error(error, L"Could not restore " + std::wstring(name) + L". Close the game and reopen the launcher to retry."); };
    if (!SafeOwnedPath(target) || !SafeOwnedPath(incoming) || !SafeOwnedPath(saved)) return fail();
    const bool has_backup = Kind(saved) != PathKind::Missing;
    if (journal.old[i] && !has_backup) {
      if (Kind(target) == PathKind::Missing) return fail();
      continue;
    }
    if (name == kLauncher && has_backup) {
      if (!Move(saved, target, true)) return fail();
      continue;
    }
    if (Kind(target) != PathKind::Missing && Kind(incoming) == PathKind::Missing && !Move(target, incoming)) return fail();
    if (has_backup && !Move(saved, target)) return fail();
  }
  return DeleteFileW((work / kJournal).c_str()) || Error(error, L"Could not clear the update recovery journal.");
}

}  // namespace detail

UpdatePreferences LoadWindowsUpdatePreferences(const fs::path& root) {
  const auto data = root / "data";
  const auto directory = Kind(data);
  constexpr UpdatePreferences fallback{UpdateChannel::Stable, false};
  if (directory != PathKind::Missing && directory != PathKind::Folder) return fallback;
  const auto path = data / "launcher-updates.txt";
  const auto kind = Kind(path);
  const auto installed = ParseBuildVersion(kLauncherVersion);
  if (kind == PathKind::Missing) return {installed ? installed->Channel() : UpdateChannel::Stable, true};
  if (kind != PathKind::File) return fallback;
  std::ifstream in(path, std::ios::binary);
  char text[64];
  in.read(text, sizeof(text));
  if (!in.eof() || in.bad()) return fallback;
  return ParseUpdatePreferences(std::string_view(text, size_t(in.gcount()))).value_or(fallback);
}

bool SaveWindowsUpdatePreferences(const fs::path& root, UpdatePreferences prefs) {
  const auto data = root / "data", path = data / "launcher-updates.txt";
  std::error_code ec;
  fs::create_directories(data, ec);
  if (ec || Kind(data) != PathKind::Folder || !SafeOwnedPath(path)) return false;
  const auto temp = data / (L"launcher-updates-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64()) + L".new");
  win::Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) return false;
  const auto text = SerializeUpdatePreferences(prefs);
  DWORD written = 0;
  const bool saved = WriteFile(file.Get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                     written == text.size() && FlushFileBuffers(file.Get());
  file.Close();
  if (saved && Move(temp, path, true)) return true;
  fs::remove(temp, ec);
  return false;
}

bool WindowsUpdateIdle(const fs::path& root) {
  std::error_code ec;
  fs::create_directories(root / "data", ec);
  if (ec) return false;
  win::Handle lock(win::OpenSetupLock(root / "data/setup.lock"));
  return bool(lock) && !HasPendingWindowsUpdate(root);
}

bool HasPendingWindowsUpdate(const fs::path& root) { return Kind(root / kWork / kJournal) != PathKind::Missing; }

bool detail::StageUpdate(const fs::path& root, const fs::path& archive, const UpdateAsset& asset,
                         InstallUi& ui, std::wstring* error) {
  if (!asset) return Error(error, L"There is no newer official Windows ZIP with a published SHA-256 digest.");
  const auto work = root / kWork;
  std::error_code ec;
  fs::create_directory(work, ec);
  if (ec || Kind(work) != PathKind::Folder || HasPendingWindowsUpdate(root))
    return Error(error, L"An update is already pending or the update folder is unavailable.");

  // Verify/extract only a private copy, even if a local ZIP is replaced during preparation.
  const auto zip = work / L"verified.zip", stage = work / L"stage";
  if (!SafeOwnedPath(zip) || Kind(stage) == PathKind::Unsafe)
    return Error(error, L"The update staging folder or ZIP path is unsafe.");
  fs::path source;
  if (!archive.empty()) {
    if (Kind(archive) != PathKind::File || fs::file_size(archive, ec) != asset.size || ec)
      return Error(error, L"The selected file does not match the size of the latest official Windows ZIP.");
    // Never remove a file that the player selected from inside our staging area.
    source = fs::canonical(archive, ec);
    if (ec) return Error(error, L"Could not read the selected ZIP path.");
    const auto destination = fs::canonical(work, ec);
    if (ec) return Error(error, L"Could not resolve the update staging folder.");
    for (auto parent = source.parent_path(); !parent.empty(); parent = parent.parent_path()) {
      if (CompareStringOrdinal(parent.c_str(), -1, destination.c_str(), -1, TRUE) == CSTR_EQUAL)
        return Error(error, L"Choose a ZIP outside the installer's .gtav-update folder.");
      if (parent == parent.parent_path()) break;
    }
  }
  const auto cleanup = [&] {
    std::error_code ignored;
    fs::remove(zip, ignored);
    fs::remove_all(stage, ignored);
  };
  cleanup();
  if (source.empty()) {
    if (!DownloadWindowsAsset(asset, zip, ui, error)) { cleanup(); return false; }
  } else if (!CopyFileExW(source.c_str(), zip.c_str(), CopyProgress, &ui, nullptr, COPY_FILE_FAIL_IF_EXISTS)) {
    cleanup();
    return Error(error, ui.Cancelled() ? L"Cancelled." : L"Could not copy the selected ZIP for verification.");
  }
  ui.Stage(L"Verifying the official ZIP (SHA-256)...");
  ui.Progress(.67);
  std::string digest;
  uint64_t size = 0;
  if (!HashUpdateArchive(zip, &digest, &size, ui) || size != asset.size || digest != asset.sha256) {
    cleanup();
    return Error(error, ui.Cancelled() ? L"Cancelled." : L"SHA-256 mismatch. Download the official Windows ZIP again.");
  }
  ui.Progress(.82);
  ui.Stage(L"Unpacking the verified update (your data is not being changed)...");
  if (!fs::create_directory(stage, ec) || ec || !Extract(zip, stage, ui, error) || ui.Cancelled() ||
      !ValidatePackage(work, asset.version, error)) {
    cleanup();
    if (error->empty()) *error = ui.Cancelled() ? L"Cancelled." : L"Could not create the staging directory.";
    return false;
  }
  fs::remove(zip, ec);
  return true;
}

UpdatePreparation PrepareWindowsUpdate(const fs::path& root, const fs::path& archive, const UpdateAsset& asset,
                                       InstallUi& ui, std::wstring* error) {
  using enum UpdatePreparation;
  error->clear();
  const auto data = root / "data", work = root / kWork;
  std::error_code ec;
  fs::create_directories(data / "logs", ec);
  if (ec) {
    *error = L"Could not create the log folder. Check write permissions.";
    return Failed;
  }
  win::Handle lock(win::OpenSetupLock(data / "setup.lock", true));
  if (!lock) {
    const auto reason = GetLastError();
    if (reason == ERROR_SHARING_VIOLATION || reason == ERROR_LOCK_VIOLATION) {
      *error = L"Close the game and any other setup before applying an update.";
      return Deferred;
    }
    *error = L"Could not lock the game folder. Check write permissions.";
    return Failed;
  }
  std::ofstream log(data / "logs/update.log", std::ios::app);
  log << "Preparing verified update v" << asset.version << '\n';
  if (!detail::StageUpdate(root, archive, asset, ui, error)) {
    log << Narrow(*error) << '\n';
    return Failed;
  }
  const auto cleanup = [&] {
    std::error_code ignored;
    fs::remove_all(work / L"stage", ignored);
  };
  const auto helper = work / kHelper;
  if (Kind(helper) == PathKind::Unsafe || !fs::copy_file(root / kLauncher, helper, fs::copy_options::overwrite_existing, ec) || ec ||
      ui.Cancelled() || !detail::JournalUpdate(root, asset.version, error)) {
    cleanup();
    if (error->empty()) *error = ui.Cancelled() ? L"Cancelled." : L"Could not prepare the restart helper.";
    log << Narrow(*error) << '\n';
    return Failed;
  }
  if (ui.Cancelled() || !StartHelper(root, false, lock.Get(), error)) {
    // No runtime file moved yet. Leave the journal intact if removing it fails.
    if (DeleteFileW((work / kJournal).c_str())) cleanup();
    else *error = L"Could not remove the update journal. Reopen the launcher to recover.";
    if (error->empty()) *error = L"Cancelled.";
    log << Narrow(*error) << '\n';
    return Failed;
  }
  log << "Verified and staged; waiting for launcher to exit\n";
  ui.Progress(1.0);
  return Ready;
}

bool StartWindowsUpdateRecovery(const fs::path& root, std::wstring* error) {
  if (Kind(root / kWork) != PathKind::Folder) return Error(error, L"The update recovery folder is inaccessible.");
  win::Handle lock(win::OpenSetupLock(root / "data/setup.lock", true));
  if (!lock) return Error(error, L"The installation is busy. Close the game and try again.");
  // Another helper may have committed between startup and acquisition of the lock.
  if (!HasPendingWindowsUpdate(root)) return Error(error, L"The previous update finished. Open the launcher again.");
  return StartHelper(root, true, lock.Get(), error);
}

int FinishWindowsUpdate(const fs::path& helper, uintptr_t parent, uintptr_t lock, bool recover) {
  if (helper.filename() != kHelper || helper.parent_path().filename() != kWork || !parent || !lock || parent == lock)
    return 1;
  win::Handle parent_handle(reinterpret_cast<HANDLE>(parent)), setup_lock(reinterpret_cast<HANDLE>(lock));
  const auto pid = GetProcessId(parent_handle.Get());
  if (!pid || pid == GetCurrentProcessId() || WaitForSingleObject(parent_handle.Get(), 60'000) != WAIT_OBJECT_0) return 1;
  const auto root = helper.parent_path().parent_path();
  std::wstring error, recovery_error;
  const bool updated = !recover && detail::ApplyUpdate(root, &error);
  const bool restored = !updated && detail::RollbackUpdate(root, &recovery_error);
  {
    std::ofstream log(root / "data/logs/update.log", std::ios::app);
    if (!error.empty()) log << Narrow(error) << '\n';
    if (!recovery_error.empty()) log << Narrow(recovery_error) << '\n';
    log << (updated ? "Installed\n" : restored ? "Previous version restored\n" : "Recovery required\n");
  }
  if (updated || restored) {
    std::error_code ec;
    fs::remove_all(helper.parent_path() / "stage", ec);
  }
  setup_lock.Close();
  if (!updated) {
    const auto text = error + (restored ? L"\nThe previous version has been restored." : L"\n" + recovery_error);
    MessageBoxW(nullptr, text.c_str(), L"GTA V Recomp | Update", restored ? MB_ICONINFORMATION : MB_ICONERROR);
  }
  const auto launcher = root / kLauncher;
  if (updated || restored) {
    if (win::Spawn(launcher, Quote(launcher) + (updated ? L" --updated --portable" : L" --setup --no-auto-update --portable"), root, 0)) return 0;
    MessageBoxW(nullptr, L"Open gtav_launcher.exe to continue.", L"GTA V Recomp | Update", MB_ICONINFORMATION);
  }
  return 1;
}

}  // namespace loader
