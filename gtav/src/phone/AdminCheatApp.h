#pragma once

namespace gtav::workshop {
struct DesktopView;
struct PlayerView;
// Admin (Cheats) panel body. Always draws something: the full cheat menu when the local player
// holds Admin-OS authority, otherwise a clear locked state — never a blank page.
void DrawAdminCheatApp(const DesktopView&, const PlayerView& local, float scale = 1.f);
}  // namespace gtav::workshop
