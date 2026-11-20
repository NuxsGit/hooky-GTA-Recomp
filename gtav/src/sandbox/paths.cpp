#include "paths.h"

namespace gtav::sandbox {
namespace fs = std::filesystem;
namespace {
bool EnsureSeparateDirectory(const fs::path& path) {
  std::error_code ec;
  // create_directories handles a missing parent tree; create_directory alone would fail
  // if sandbox/ does not yet exist when creating sandbox/profile.
  fs::create_directories(path, ec);
  if (ec) return false;
  // Use symlink_status to detect reparse points / directory junctions before canonical check.
  const auto status = fs::symlink_status(path, ec);
  if (ec || !fs::is_directory(status)) return false;
  // Reject junctions or symlinked folders: canonical must equal the normalized path itself.
  const auto canon = fs::canonical(path, ec);
  if (ec) return false;
  // Normalize both sides for comparison (avoid trailing slash mismatches on Windows).
  return fs::weakly_canonical(path, ec) == canon && !ec;
}
}  // namespace

std::optional<WorkspacePaths> PrepareWorkspace(const fs::path& user_root, WorkspaceError& error) {
  error = WorkspaceError::MissingInstall;
  if (user_root.empty()) return std::nullopt;
  std::error_code ec;
  const auto real_root = fs::canonical(user_root, ec);
  if (ec || !fs::is_directory(real_root / "0000000000000000/545408A7/00000002", ec)) return std::nullopt;
  const auto workshop = real_root / "sandbox";
  const WorkspacePaths result{real_root, workshop / "profile", workshop / "courses"};
  error = WorkspaceError::SeparateFolderRequired;
  if (!EnsureSeparateDirectory(workshop) || !EnsureSeparateDirectory(result.profile_root) ||
      !EnsureSeparateDirectory(result.course_root)) return std::nullopt;
  error = WorkspaceError::None;
  return result;
}
}  // namespace gtav::sandbox
