# Windows launcher, incremental builds and TU27 Debug

## Testing the launcher and updates

- Open `gtav_launcher.exe` at normal and high DPI. Tab through **Accueil**, **Installation**,
  **Mises à jour** and **Journaux**. Check the game/log folder buttons, **Choisir le mode**, file/folder pickers,
  setup/cancel/resume, recompilation, and `--play`, `--setup`, `--unattended`, `--rebuild`.
- With published releases, test Stable and Nightly independently. A Stable launcher never offers
  a prerelease unless Nightly is selected and confirmed. Channel and **Automatique** survive a
  restart. Check that **Rechercher** is nonblocking, that **Installer maintenant** stays disabled
  without an uploaded, digest-bearing ZIP, and that an offline or empty feed leaves Play usable.
  An older base version is never installed automatically on a channel change.
- Test **Automatique** on a copy of a working installation with a newer official ZIP. With the game
  running, leave a launcher open on Accueil: the update must wait for the game to exit. Cancel once
  during download, make sure no runtime file moved, then retry on demand. Check that **ZIP local…**
  accepts the exact ZIP for the selected feed and rejects a modified one. Use `--no-auto-update`
  to suppress the automatic check during testing. Windows CI drives the real automatic update flow
  against a local test feed with the exact ZIP. It checks waiting for the game/setup lock, applying
  the ZIP, restarting, preserving data and switching preferences. It also boots the managed Games layout and smokes the illustrated mode hub without a retail DLL.
  The shipped launcher uses WinHTTP; the test-only executable is never packaged.
- Saves, Workshop layouts, `gtav.toml`, `gtav_settings.ini`, `args.txt`, custom pad mappings, local game
  DLL, shader caches and `data/build/` survive. A compatible build ID needs no recompilation. For a
  changed build ID, inspect the number of reused/compiled C++ units in `data/logs/install.log`.
- The installer opens verified ZIPs with Windows `tar.exe` and swaps only the standard package’s
  owned paths. The helper waits for the old launcher and holds the install lock through swap or
  rollback. The previous package files remain in `.gtav-update/backup` until the next update. If
  interrupted mid-swap, reopen `gtav_launcher.exe` to recover. Keep the backup and journal if
  recovery reports an error.
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
closes Debug. Gameplay input is captured while the panel is open. Story tools are Campaign-only.
Enable them in offline free roam on a spare save. In Workshop use the [AdminOS phone](WORKSHOP_CREATIVE.md). Presets are **Give pistol + ammo**, **Teleport 10 m forward** (local,
on foot, slightly raised), **Fly**, and **Freecam** (close-range camera; does not move the player).
Fly applies short velocity changes, not god mode or permanent gravity/collision edits.

- **Move:** physical WASD / ZQSD. **Rise / descend:** E / Q (E / A on AZERTY). **Freecam look:** IJKL.
- Start and stop each mode; check the camera returns on F2/Esc, F1, focus loss and on entering a
  mission or cutscene. Test loading a different character, dying, pausing, in a vehicle, and a
  reconnect of the gamepad. No command should survive closing the panel or a stale guest tick.
- Check that one physical F2 press/release toggles once (hold F2 down, too), that the SDL and Win32
  native windows don't send the key into gameplay, and that F1/RB+Back and DualSense still work.
- Commands are allowlisted and consumed only inside the game's `GET_GAME_TIMER` wrapper, on the
  live guest script thread. In log-only mode, the hook only samples the existing call. When Story
  tools are armed, it checks the context/return pointer and compares a second scalar timer call
  before mutating anything. The scratch area and PPC registers are
  isolated from the real VM call. Checks block actions while online, paused, dead, switching, in a
  mission/cutscene or fading. Camera cleanup targets only the camera the tool created.

ABI references: TU27 handler addresses/hashes in `tools/scripttools/{handlers,fn2name}.txt`,
[`scrNativeCallContext` on 360](https://blog.lms-dev.com/reverse-engineering/vs-native-invocation/),
and Xbox 360 [TU27 call layout](https://github.com/xxHASTERxx/GTA-V-Mod-Menu-TU27/blob/master/GTAVNatives.h).
The Xbox context uses 32-bit big-endian pointers/words, not the PC 64-bit native ABI. The TU27
examples read the entity and gameplay-camera results as three contiguous floats. A working scalar
probe or these examples don't replace a real play-test of vector results, physics and camera ownership.

Asset-free checks: configure `loader` with `-DGTAV_LAUNCHER_TESTS=ON`, build, then run `ctest`.
Fixtures cover update discovery/digest policy, Windows staging and rollback, pacing and request
state, managed home selection, conflicting/failed migration, and Linux cold/incremental/corrupted/cancelled builds. The native Windows candidate also
feeds its exact ZIP through the updater's SHA-256 and system `tar.exe` staging path. These tests do
not run retail TU27 or validate controllers over physical USB/Bluetooth.
