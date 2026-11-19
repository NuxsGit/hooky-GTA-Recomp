GTA V Xbox 360 recompilation - Windows
=======================================

This package contains no game. It holds a launcher, an Xbox 360 runtime and the tools that turn
your own copy of Grand Theft Auto V for Xbox 360 into a native Windows build on your computer.
No Rockstar Games or Microsoft game code, data, shaders or title-update files are included.

Do not share what you build
---------------------------
Everything this package makes from your discs is Rockstar Games' copyrighted work: the game
folder (data/), the recompiled game library (gtav_game.*), the generated C++, the translated
shaders (data/spirv, shader_cache), and the GTAV.ipa and Android pack made by "Build for iPhone"
and "Build for Android". They are for your own devices only. Never upload, share or link them,
and never add them to a fork or a release. The GPL license of this project covers its own code
only; it gives no one the right to share Rockstar's. Rules for forks and downstream releases:
https://github.com/andrewnakas/gtav-recomp#keeping-it-clean-rules-for-forks-and-downstream-releases

What you need
-------------
- Your own Xbox 360 discs of GTA V: Disc 1 (Install) and Disc 2 (Play), as ISO images or as
  extracted folders.
- Title Update 27 for GTA V from your own console: the TU file (tu00000009_00000000) or a folder
  with disc002\default.xexp and disc002\update.rpf.
- Windows 10/11 x64, a CPU with AVX2 (Intel Haswell / AMD Zen or newer), a GPU with current Vulkan drivers.
- A keyboard and mouse, or a gamepad recognized by SDL.
- About 25 GB free on this drive during setup (about 17 GB once installed), 8 GB RAM or more.

First run
---------
1. Unzip this folder somewhere with enough space and run gtav_launcher.exe.
2. Open Installation, pick Disc 1, Disc 2 and the title update, then press Installer et compiler.
3. The launcher checks every file against known-good hashes, copies them into data\, generates
   C++ from your game executable, compiles it into gtav_game.dll, and keeps a local compilation cache in data\build.
   This takes 15-30 minutes depending on your CPU.
4. Press Jouer. The first minutes of play translate shaders in the background; some objects
   can be missing until their shader is ready. They are kept in data\spirv for later runs.

Later runs open Accueil. Use the sidebar for Installation, Mises à jour or Journaux.
Use --play in a shortcut if you prefer to start the game directly.

Desktop Workshop (experimental)
--------------------------------
Jouer opens a mode selector before the game starts: Sandbox solo (default), Private multiplayer,
or Campaign. Sandbox uses a separate profile: data\user\sandbox\profile. On the first sandbox
run, complete the original game’s first-run screens and prologue before using free-roam tools.
Press F6 in free roam: build and save layouts with props, set up checkpoint races or hunts,
and spawn a test vehicle. In Private multiplayer, Host here opens a TCP listener (default 30127);
friends with the same Workshop build join with your reachable IPv4, port and code. Nearby friends
appear as ghost pedestrians or test vehicles. Use a trusted LAN/VPN. No encryption, automatic
port forwarding, GTA Online, terrain editing or synced world physics. Details: docs/SANDBOX.md.
Skip the selector for story mode with: gtav_launcher.exe --play -- --gtav_mode=campaign

Recompile and update
--------------------
Choose Recompiler (or run the launcher with --rebuild) to check the existing game data and build
the game library without recopying the discs. Use Installer et compiler when the data is missing or
damaged. Recompile leaves your game data, saves and settings in place.

In Mises à jour, Stable is recommended. Nightly contains experimental builds and requires
confirmation to select from Stable. Automatique checks the chosen GitHub feed and downloads and
installs a verified ZIP while this launcher is open and the game is closed. Turn it off to check and
install on demand (Rechercher, Installer maintenant). ZIP local verifies a downloaded ZIP against
the chosen feed. Run with --no-auto-update to pause automatic behavior for one launch.
The launcher checks the asset size and SHA-256, updates package files only and restarts.
A compatible runtime update requires no recompilation; otherwise existing verified objects are
reused and only affected C++ files are compiled. The first rebuild from an older launcher may
have no cache to reuse. Logs: data\logs\install.log and data\logs\update.log.
https://github.com/NuxsGit/hooky-GTA-Recomp/releases

For offline, older or differently laid-out packages: unpack to a new folder and keep a backup.
Copy data/, gtav_game.dll and your settings (gtav.toml, gtav_settings.ini, gamecontrollerdb.txt,
args.txt) if present, but not the old runtime or tools. Recompile if asked.

Controls
--------
Gamepads and keyboard/mouse are enabled by default. SDL handles mapped USB/Bluetooth gamepads
(including Xbox, PlayStation, Switch and compatible third-party models; support varies by OS).
The game uses Xbox 360 button prompts. Hot-plugged pads and keyboard/mouse control the same player.
Default bindings (physical positions):
- WASD (ZQSD on AZERTY) = left stick; mouse or IJKL = right stick.
- Shift or Enter = A; R or Backspace = B; Space = X; F = Y.
- Right click or Q = LT; left click or E = RT; Ctrl = L3; middle click or C = R3.
- 1 or Mouse4 = LB; 3 or Mouse5 = RB; arrows = D-pad; Tab = Back; Esc = Start.
F1 opens/closes the native settings overlay. Esc closes it when open. On an SDL gamepad use
RB + Back to toggle it, D-pad and A to navigate, and B to close.
F1 > Display (Windows native renderer): choose 1080p, 1440p, 4K or your desktop resolution.
Restart after changing Output or Scene scale. Output size is not native scene rendering size.
AMD FSR 1 spatial upscaling can be toggled live, or left on Linear for less GPU work.
To report a rendering problem without dumping textures, start from PowerShell with:
  .\gtav_launcher.exe --play -- --gtav_native_trace_only=true
Press F9 when the problem appears; share logs\native_trace\frame_*.txt and the scene/settings.
F2 (Fn + F2 on some keyboards) opens Debug: live logs and experimental offline story tools.
Use a spare save. Closing it releases the camera and stops fly mode.
If a pad isn't mapped, try the community gamecontrollerdb.txt from
https://github.com/mdqinc/SDL_GameControllerDB next to the game executable. Customize keybind_*
options and mnk_sensitivity in gtav.toml in the same folder. Set mnk_mode = false to opt out.
For a native DualSense over USB or Bluetooth, keep input_backend = "sdl" in gtav.toml.
If an older install was set to XInput, change it back to SDL and restart the game. Test without
Steam Input or DS4Windows if the pad is not detected directly. For an XInput-compatible pad only,
input_backend = "xinput" is a fallback (use F1 for the settings overlay in this mode).

iPhone (experimental)
---------------------
Play GTA V on an iPhone 12 or newer (iOS 17+). The phone cannot build the game itself, so this
PC does it once (about 15 minutes); the phone then installs the game files from your discs.
1. Set up and play the game here for a while first (the phone gets the shaders the PC has seen:
   drive around, go indoors).
2. In Installation, press "Construire pour iPhone". When it is done,
   Explorer shows iphone\GTAV.ipa.
3. Drag GTAV.ipa into Sideloadly (sideloadly.io) with the iPhone plugged in, and sign in with
   your Apple ID. (A free Apple ID has to re-sign every 7 days.)
4. Copy your Disc 1 ISO, Disc 2 ISO and the TU file onto the phone: Apple Devices (or iTunes) >
   your iPhone > Files > GTA V, or put them on a USB stick and plug that into the phone.
5. Open GTA V on the phone and press Install. It copies and checks about 16 GB, then starts.
Objects missing on the phone? Copy the phone's GTA V > shader_cache folder to iphone\shader_cache
here, build for iPhone again (a few minutes) and reinstall GTAV.ipa; the game files stay.

Android (experimental)
----------------------
Play GTA V on an Android phone with a recent Snapdragon (tested: Galaxy S23 FE, Snapdragon 8 Gen 1)
and about 40 GB free during setup. The app has no game in it: this PC builds your game code for
the phone once (about 15-30 minutes), and the phone installs the game files from your discs.
1. Set up and play the game here for a while first (the phone gets the shaders the PC has seen).
2. In Installation, press "Construire pour Android". When it is done,
   Explorer shows android\gtav-android-pack.zip (android\GTAV.apk is next to it).
3. Copy GTAV.apk, gtav-android-pack.zip, your Disc 1 ISO, Disc 2 ISO and the TU file to the
   phone (for example its Download folder).
4. On the phone, open GTAV.apk to install it (allow installing apps from that source).
5. Open GTA V, choose the four files and press Install. It copies and checks about 16 GB, then
   starts the game. The disc images and the pack can be deleted afterwards.
Objects missing on the phone? Copy the phone's Android\data\com.gtavrecomp.app\files\shader_cache
folder to android\shader_cache here, press "Build for Android" again (a few minutes) and choose
the new pack on the phone; the game files stay.

Everything made from your files stays in this folder (data\ and gtav_game.dll). Do not share it:
it contains Rockstar Games' copyrighted work.

This project is free software under the GNU GPL, version 3 or later (LICENSES\gtav-recomp-GPL-3.0.txt).
Source code: https://github.com/NuxsGit/hooky-GTA-Recomp. The complete source of every program in
this package that is under the GPL or LGPL (this project, the ReXGlue SDK runtime with the FFmpeg
it links statically, XenosRecomp, plume) is in gtav-recomp-src-<version>.tar.xz on the same
release page.
Licenses for the bundled open-source components are in LICENSES\.
Grand Theft Auto V is a trademark of Take-Two Interactive Software. This project is not affiliated
with or endorsed by Rockstar Games, Take-Two Interactive or Microsoft.
