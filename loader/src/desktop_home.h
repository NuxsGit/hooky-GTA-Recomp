// Desktop install location and one-time import. Updates keep the existing game home;
// this code is only used before opening it, not on every rebuild or play.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace loader {

struct DesktopPaths {
  std::filesystem::path home;
  std::filesystem::path migrate_from;
  bool portable = false;
};

std::filesystem::path DefaultGameHome();  // per-user Games/GTAV Recomp, or empty if unavailable
bool HasLocalState(const std::filesystem::path& home);
bool SameFolder(const std::filesystem::path& a, const std::filesystem::path& b);
#ifdef _WIN32
inline constexpr char kDesktopLauncher[] = "gtav_launcher.exe";
#else
inline constexpr char kDesktopLauncher[] = "gtav_launcher";
#endif

#ifndef __APPLE__
// A fresh ZIP/tarball seeds the chosen home. A portable install or an existing home is reused.
// If the returned path differs from package, start that folder's launcher before doing any work.
bool OpenDesktopHome(const std::filesystem::path& package, const DesktopPaths& options,
                     std::filesystem::path* result, std::wstring* error,
                     const std::function<bool()>& cancelled = {});
#else
// The signed app and its tools stay where they are. Import only local files to an empty home.
bool ImportMacHome(const std::filesystem::path& from, const std::filesystem::path& home,
                   std::wstring* error, const std::function<bool()>& cancelled = {});
#endif

}  // namespace loader
