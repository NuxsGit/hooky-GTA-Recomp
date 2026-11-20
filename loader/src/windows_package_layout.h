#pragma once

#include <array>
#include <string_view>

namespace loader::windows_package {
inline constexpr wchar_t kLauncher[] = L"gtav_launcher.exe";
// Verified ZIP and first-install distribution. The updater installs the launcher last.
inline constexpr std::array<const wchar_t*, 17> kOwned = {
    L"tools", L"LICENSES", L"gtav_fast.exe", L"rexruntimerd.dll", L"rexgpu-xenosrd.dll",
    L"TracyClientrd.dll", L"SDL3rd.dll", L"rexgluerd.exe", L"msvcp140.dll", L"msvcp140_1.dll",
    L"msvcp140_2.dll", L"msvcp140_atomic_wait.dll", L"vcruntime140.dll", L"vcruntime140_1.dll",
    L"README.txt", L"gtav_release.txt", kLauncher,
};
inline bool IsFolder(std::wstring_view name) { return name == L"tools" || name == L"LICENSES"; }
}  // namespace loader::windows_package
