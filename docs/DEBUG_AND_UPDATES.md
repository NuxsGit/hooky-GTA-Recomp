# Windows launcher, incremental builds and TU27 Debug

## Testing the launcher and updates

- Open `gtav_launcher.exe` at normal and high DPI. Tab through Overview, Source setup and
  Activity log. Check the game folder and logs buttons, Play, input file/folder pickers,
  install/cancel/resume, rebuilding, and the `--play`, `--setup`, `--unattended` and `--rebuild`
  switches. Play should refuse a stale library or another running copy in the same folder.
- Install the standard ZIP from a **newer, stable** release of the fork using **Install update ZIP**.
  This is a verified import of the original downloaded ZIP, not an auto-downloader. Drafts,
  prereleases, older builds, archives without a GitHub asset digest, modified ZIPs and optional
  mobile package layouts require manual installation (see the README). When GitHub has no
  stable release, expect the launcher to say so and change no runtime files.
- Check that saves, `gtav.toml`, `gtav_settings.ini`, `args.txt`, custom pad mappings, local game DLL,
  shader caches, and `data/build/` survive. Check that a second launcher or running game blocks
  the update. With a compatible game build ID, the new launcher should say **No recompilation
  needed**. For a changed ID, inspect the number of C++ units reused and compiled in `install.log`.
- Updates stage an authenticated ZIP using the Windows system `tar.exe` and swap only the
  standard package's owned paths. The helper waits for the old launcher to exit and holds the
  installer lock until the swap or rollback is complete. The previous package files are retained
  in `.gtav-update/backup`; the next update replaces this backup. If interrupted mid-swap,
  reopen `gtav_launcher.exe` to recover. Keep the backup and journal if recovery reports an error.
- A clean `data/build/` compile can take as long as before. The cache lives on the player's
  PC and is never part of a release. XEX/XEXP and the generated reference files always pass
  strict SHA-256 checks on a rebuild. Non-code verification receipts reuse only previously
  matching size/timestamp/reference entries; delete `data/verified-assets.txt` to rehash every asset.
  The compiler key hashes the exact flags, Clang and bundled headers and each unit's inputs;
  objects must also match their recorded hash. Link-only changes don't invalidate objects.
  Keep `GAME_ABI` in sync when changing compilation semantics outside the packaged input files.

## Mission failure — retail play-test still required

1. Use the same TU27 save/checkpoint and record the mission, exact fail condition and elapsed
   wall time until **Mission Failed / Retry**. Check death outside missions separately (**Wasted**
   and respawn). Keep cheats disabled. Try at least one mission failure without dying.
2. Repeat at the stock 30 FPS default and at 60/uncapped (F1). Test switching back to 30 without
   restarting. The fast modes used to leave the guest interval at zero and force the audio wait
   fallback; returning to stock now restores the prior guest values. During the death/failure
   camera effect, use stock pacing for up to 20 seconds, or until the effect clears. Neither the
   effect nor the guest time scale is removed or overridden. `gtav_fail_stock_pacing=false` is an
   A/B option, not a proposed general fix for slow motion.
3. Check `data/logs/gtav.log` for `[mission]` lines (camera state, pacing, time-scale requests and
   guest script timer). If it still stalls, report the mission/checkpoint, FPS selection and these
   lines. A progressing render FPS does not prove that gameplay scripts or audio have advanced.
   The earlier delay was observed with rendering still active; its precise root cause remains open.

## F2 (Fn + F2 on keyboards with media keys)

The native overlay has **Live logs** and **Story tools**. F1 still opens settings; F2, Esc or B
closes Debug. Gameplay input is captured while the panel is open. Enable Story tools only in offline
free roam on a spare save. Presets are **Give pistol + ammo**, **Teleport 10 m forward** (local,
on foot, slightly raised), **Fly**, and **Freecam** (close-range camera; does not move the player).
Fly applies short velocity changes, not god mode or permanent gravity/collision edits.

- **Move:** physical WASD / ZQSD. **Rise / descend:** E / Q (E / A on AZERTY). **Freecam look:** IJKL.
- Start and stop each mode; check the camera returns on F2/Esc, F1, focus loss and on entering a
  mission or cutscene. Test loading a different character, dying, pausing, in a vehicle, and a
  reconnect of the gamepad. No command should survive closing the panel or a stale guest tick.
- Check that one physical F2 press/release toggles once (hold F2 down, too), that the SDL and Win32
  native windows don't send the key into gameplay, and that F1/RB+Back and DualSense still work.
- Commands are allowlisted and consumed only inside the game's `GET_GAME_TIMER` wrapper, on the
  live guest script thread. The context/return pointer is checked against guest memory and a second
  scalar timer call is compared before mutating anything. The scratch area and PPC registers are
  isolated from the real VM call. Checks block actions while online, paused, dead, switching, in a
  mission/cutscene or fading. Camera cleanup targets only the camera the tool created.

ABI references: TU27 handler addresses/hashes in `tools/scripttools/{handlers,fn2name}.txt`,
[`scrNativeCallContext` on 360](https://blog.lms-dev.com/reverse-engineering/vs-native-invocation/),
and Xbox 360 [TU27 call layout](https://github.com/xxHASTERxx/GTA-V-Mod-Menu-TU27/blob/master/GTAVNatives.h).
The Xbox context uses 32-bit big-endian pointers/words, not the PC 64-bit native ABI. A working
scalar ABI probe doesn't replace a real play-test of vector results, physics and camera ownership.

Asset-free checks: configure `loader` with `-DGTAV_LAUNCHER_TESTS=ON`, build, then run `ctest`.
Fixtures cover update discovery/digest policy, Windows staging and rollback, pacing and request
state, and Linux cold/incremental/corrupted/cancelled builds. Run the native Windows candidate and
controller smoke workflow before sharing a ZIP. These tests do not run retail TU27.
