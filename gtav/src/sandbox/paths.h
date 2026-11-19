#pragma once

#include <filesystem>
#include <optional>

namespace gtav::sandbox {
struct WorkspacePaths {
  std::filesystem::path common_content_root;
  std::filesystem::path profile_root;
  std::filesystem::path course_root;
};

enum class WorkspaceError { None, MissingInstall, SeparateFolderRequired };
// The caller's original user root is not a writable sandbox mount. Only its common
// TU27 install packages will be shared, via the SDK's read-only content mapping.
std::optional<WorkspacePaths> PrepareWorkspace(const std::filesystem::path& user_root, WorkspaceError& error);
}  // namespace gtav::sandbox
