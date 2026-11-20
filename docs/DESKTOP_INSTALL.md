# Desktop installation, saves and mode hub

## One game folder

| Platform | New installations | What to open afterwards |
| --- | --- | --- |
| Windows | `%USERPROFILE%\Games\GTAV Recomp` | `gtav_launcher.exe` in that folder, or **GTA V Recomp** in Start after setup |
| Linux | `~/Games/GTAV Recomp` | `gtav_launcher` in that folder, or **GTA V recomp** in the applications menu |
| macOS | `~/Games/GTAV Recomp` | `GTAV.app` (normally in `/Applications`) |

On Windows and Linux the extracted download installs the launcher, runtime and tools once, then
opens the installed launcher. Its extraction directory is no longer needed. Do not delete your
original discs/TU27; setup may need them to repair missing or damaged data. Opening another fresh
extraction finds the existing game folder; it does **not** silently apply that version or undo a
newer installed version. Use the update procedure below.

On macOS the signed bundle keeps the compiler and launcher. The game folder contains the generated
library, user data, and only the runtime files that need to sit next to the library. Runtime copies
are refreshed as needed. Keep the same **Game folder** when replacing the app.

- **Already installed?** An older portable folder with local files stays usable in place. macOS
  still honors a saved Game folder or detects an existing `~/Library/Application Support/GTAV Recomp`.
  Nothing large is copied or deleted just by upgrading an existing installation.
- **Want a different drive?** `--home` takes an absolute game folder on all three platforms; on
  macOS you can also use the folder picker. On Windows/Linux continue to use the launcher **inside**
  that folder. `--portable` (Windows/Linux) keeps the launcher next to the extracted files, useful
  for side-by-side candidates; it does not share another install's profile.

## Put an older installation under Games

**Without a second copy:** close the game, setup, updater and launcher. Move the **whole** old
Windows/Linux game folder to the desired empty `Games/GTAV Recomp` path, then apply an update to that
folder. On macOS move the writable game folder (not `GTAV.app`), then select it in setup (hold Option
while opening the app). Moving within the same volume avoids reimporting or recopying the discs.
Use the launcher in the new folder after moving it; a shortcut to the old path can be removed.

**With a preserved original:** from a new extracted package (or the new app's executable on macOS),
run one of the following commands before setting up the destination:

```powershell
.\gtav_launcher.exe --migrate-from "D:\Mon ancien jeu\GTAV Recomp"
```

```sh
# Linux
./gtav_launcher --migrate-from "$HOME/old-gtav"
# macOS
/Applications/GTAV.app/Contents/MacOS/GTAV --migrate-from "$HOME/Library/Application Support/GTAV Recomp"
```

This makes a one-time staged copy of the local game files, saves, Workshop layouts, settings and
build/shader caches into the default Games folder (or add `--home /absolute/new/path`). Close the
old game/launcher before importing. It requires room for the copy, does not merge with an existing
non-empty destination and leaves the source intact. A failed import can be retried; a hard kill may
leave a `.gtav-incoming-*` staging directory beside the destination, which can be removed with all
launchers closed. Keep your original until you have checked both profiles in the new location.

## Updating without losing progress

- **Windows, published Stable/Nightly:** use **Mises à jour** in the installed launcher. The verified
  ZIP swaps only package-owned files, keeps one version of the package for rollback, and restarts the
  same launcher. If the local build ID changed, it starts Recompile. The install lock covers the
  mode hub and runtime as well as setup, so an update waits until you quit.
- **Linux and manual/offline Windows packages:** quit the game and launcher. Use a trusted archive
  for this OS and architecture and replace the **package files only** in the existing game folder;
  do not delete the game folder or sync it with `--delete` / `/MIR`. On Linux, for the standard
  archive with one `gtav-recomp-linux-x64/` wrapper, this is:
  `tar -xf ~/Downloads/gtav-recomp-linux-x64.tar.gz --strip-components=1 -C "$HOME/Games/GTAV Recomp"`.
  On Windows copy the **contents** of the extracted package into the existing home and confirm
  replacing package files. For published Windows ZIPs prefer the verified update UI instead.
- **macOS:** quit the game and replace the whole signed `GTAV.app`. The saved home is not in the
  bundle, so do not copy any game data into it.
- **Recompile (all platforms):** open setup and choose **Recompile** (or run the launcher with
  `--rebuild`). A compatible update uses the existing game library; a changed build ID reuses
  the disc files and eligible objects already on disk. No new Disc 1/Disc 2/TU27 import is necessary
  unless the required data is missing or damaged.

Campaign and Workshop keep their own profiles inside `data/user/`; Workshop uses
`data/user/sandbox/profile/` and stores layouts in `data/user/sandbox/courses/`. `data/user/` also
contains the large, read-only Disc 1 install partitions under `0000000000000000/545408A7/00000002`.
The mode selector and F1 share `data/config/graphics.ini`, outside both save profiles. Migration
copies it when present; update and Recompile leave the player profiles and settings in place.
These files are local, not synced or backed up against deletion, drive failure or a bad save.
`data/build/` can be removed with the game and setup closed if you need space; the next compile
will be slower, but it does not remove saves. Never distribute your game data or build outputs.

## Starting the game

**Choisir le mode / Play** opens the illustrated mode hub before retail startup. Click a card, or navigate
with arrows, Tab or D-pad. **Enter / A / Play** confirms; **Esc / B** closes without loading a save.
On Windows `gtav_launcher.exe --play` and on Linux/macOS normal launch of a ready install go directly
to this screen, bypassing setup. The hub uses original artwork, and follows French/English locale.
Open **Graphismes / Graphics** (F1 or Y) to set graphics before loading a profile. Esc or B leaves
that tab without exiting the hub. See [options and driver support](WINDOWS_GRAPHICS.md).

| Card | What it does |
| --- | --- |
| Campaign · Solo | Opens your existing story profile, offline. |
| Campaign co-op | Unavailable: the engine does not replicate story missions or progress. |
| Sandbox · Solo | Opens isolated Workshop free roam, no network. |
| Private session | Opens Sandbox multiplayer. Use **F6** in free roam to host or join a friend's session. No GTA Online or public matchmaking. |

Explicit game arguments still work, e.g. `gtav_launcher.exe --play -- --gtav_mode=campaign`.
See [Desktop Workshop](SANDBOX.md) for profiles and the network limitations. The new hub is desktop
only; mobile startup is unchanged.
