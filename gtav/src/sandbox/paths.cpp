#include "paths.h"

namespace gtav::sandbox {
namespace fs = std::filesystem;
namespace {
bool EnsureSeparateDirectory(const fs::path& path) {
  std::error_code ec;
  fs::create_directory(path, ec);
  if (ec || !fs::is_directory(fs::symlink_status(path, ec))) return false;
  // Also reject directory junctions that can appear as directories on Windows.
  return fs::canonical(path, ec) == path && !ec;
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
