#pragma once

#include "install.h"
#include "update.h"

#include <cstdint>

namespace loader {

// Prepare while the launcher is running. On success the caller must close: the helper holds the
// per-installation lock, waits for its parent to exit, swaps package assets and starts the launcher.
bool PrepareWindowsUpdate(const std::filesystem::path& root, const std::filesystem::path& archive,
                          const UpdateAsset& asset, InstallUi& ui, std::wstring* error);
bool HasPendingWindowsUpdate(const std::filesystem::path& root);
bool StartWindowsUpdateRecovery(const std::filesystem::path& root, std::wstring* error);
int FinishWindowsUpdate(const std::filesystem::path& helper, uintptr_t parent, uintptr_t lock, bool recover);

namespace detail {
// Internal stages. Callers hold the setup lock; the release smoke test uses a private fixture folder.
bool StageUpdate(const std::filesystem::path& root, const std::filesystem::path& archive,
                 const UpdateAsset& asset, InstallUi& ui, std::wstring* error);
bool JournalUpdate(const std::filesystem::path& root, std::string_view version, std::wstring* error);
bool ApplyUpdate(const std::filesystem::path& root, std::wstring* error);
bool RollbackUpdate(const std::filesystem::path& root, std::wstring* error);
}

}  // namespace loader
