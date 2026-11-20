GTA V Xbox 360 recompilation - macOS
=====================================

This package contains no game. It holds an app with a setup window, an Xbox 360 runtime and the
tools that turn your own copy of Grand Theft Auto V for Xbox 360 into a native Apple silicon build
on your Mac. No Rockstar Games or Microsoft game code, data, shaders or title-update files are
included.

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
  with disc002/default.xexp and disc002/update.rpf.
- A Mac with Apple silicon (M1 or newer) and macOS 15 (Sequoia) or newer. 8 GB RAM works; 16 GB is
  more comfortable. Nothing else to install: the compiler that builds the game is inside the app.
- A keyboard and mouse, or a gamepad recognized by SDL.
- About 25 GB free during setup (about 17 GB once installed) on a normal, case-insensitive volume
  (the macOS default). The internal SSD loads much faster than a USB drive.

First run
---------
1. Unzip the download and move GTAV.app to Applications (or anywhere you like).
2. The app is not notarized by Apple, so macOS blocks it the first time. Either:
     - Open it once, then System Settings > Privacy & Security > "GTAV was blocked..." > Open Anyway
     - or, in Terminal:  xattr -dr com.apple.quarantine /Applications/GTAV.app
3. Open GTAV.app. Pick Disc 1, Disc 2 and the title update with the "Choose..." buttons, and the
   game folder (default for new installs: ~/Games/GTAV Recomp). An existing saved game folder
   or ~/Library/Application Support/GTAV Recomp is reused. Press Install & build.
4. Setup checks every file against known-good hashes, copies them into the game folder, generates
   C++ from your game executable, compiles it into gtav_game.dylib with the bundled clang and
   keeps incremental build objects. This takes about 15-40 minutes. Cancel (or closing the window) stops
   it; installing again continues where it stopped.
5. Press Play. The first minutes of play translate shaders in the background; some objects can be
   missing until their shader is ready. They are kept in the game folder (data/spirv) for later.

Later runs: GTAV.app opens the illustrated mode hub. Campaign solo uses your story saves;
Sandbox solo and Private session have a separate profile. Campaign co-op is unavailable.
Hold Option while opening GTAV.app to get the
setup window again (for example after moving your discs, or to repair the install).
Logs: <game folder>/data/logs (gtav.log for the game, install.log for setup).

Recompile and update
--------------------
Choose Recompile (or run the launcher with --rebuild) to check the existing game data and build
the game library without recopying the discs. Use Install & build when the data is missing or
damaged. Recompile leaves your game data, saves and settings in place.

Choose Check for updates (or --check-updates) to look for a newer stable release of this fork.
When offered, open the GitHub release and download the named archive. No update is applied by
the launcher. https://github.com/NuxsGit/hooky-GTA-Recomp/releases
Quit the game and replace the whole GTAV.app. Do not modify its signed contents. Keep your
existing game folder (data/, game library and settings); back up that folder before updating.
Recompile if the new launcher asks; compatible runtime-only updates can play immediately.

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
If a pad isn't mapped, try the community gamecontrollerdb.txt from
https://github.com/mdqinc/SDL_GameControllerDB next to the game executable. Customize keybind_*
options and mnk_sensitivity in gtav.toml in the same folder. Set mnk_mode = false to opt out.
The game executable and gtav.toml are in your installed game folder, not in GTAV.app.

iPhone (experimental)
---------------------
The Mac builds the iPhone app once from your installed game (iOS cannot compile code or shaders;
Windows and Linux PCs can do this too); the phone then installs the game's files itself from your
discs, like the Mac's first-run setup.
Needs an iPhone 12 or newer (A14+, 4 GB RAM; tested on an iPhone 13 mini) on iOS 17 or newer, and
a sideloading tool that signs apps with your own Apple ID (Sideloadly or AltStore; with a free
Apple ID the app has to be re-signed every 7 days).
1. Install and play the game on the Mac for a while first: the game saves every shader it meets,
   and only those can be translated for the phone (drive around the map, enter interiors).
2. Hold Option while opening GTAV.app and press "Build for iPhone" (about 15-25 minutes). The game
   folder then has iphone/GTAV.ipa (the game code and shaders for the phone; not the game files),
   and Finder shows it.
3. Drag GTAV.ipa into Sideloadly (sideloadly.io) with the iPhone plugged in, or use AltStore.
4. Put your Disc 1 and Disc 2 ISO images and the TU27 file (tu00000009_00000000) where the phone
   can read them: in GTA V's folder (Finder > your iPhone > Files > GTA V, or the Files app), or
   on a USB drive plugged into the phone.
5. Open GTA V. Its setup screen finds the files in its folder (or pick them with Choose), copies
   and checks about 16 GB (keep the app open; an interrupted install continues where it stopped),
   offers to delete the disc images from its folder, and starts the game. Touch controls appear
   on screen; a Bluetooth controller works too.
Free space on the phone: about 17 GB, plus the disc images while installing if you copy them
there (about 16 GB; not needed with a USB drive).
Shaders the phone has no translation for are skipped (missing objects) and saved on the phone in
GTA V's folder, shader_cache. To fix them: copy that folder to <game folder>/iphone/shader_cache
on the Mac, press "Build for iPhone" again (only the shaders are redone, a few minutes) and
install the new GTAV.ipa over the old one (the game files on the phone stay).

Android (experimental)
----------------------
Play GTA V on an Android phone with a recent Snapdragon (tested: Galaxy S23 FE, Snapdragon 8 Gen 1)
and about 40 GB free during setup. The app has no game in it: this Mac builds your game code for
the phone once (about 15-25 minutes), and the phone installs the game files from your discs.
1. Install and play the game on the Mac for a while first (the phone gets the shaders the Mac has
   seen: drive around, go indoors).
2. Hold Option while opening GTAV.app and press "Build for Android". Finder then shows the
   game folder's android/ folder with GTAV.apk and gtav-android-pack.zip.
3. Copy GTAV.apk, gtav-android-pack.zip, your Disc 1 ISO, Disc 2 ISO and the TU file to the phone
   (for example its Download folder; Android File Transfer or a USB stick).
4. On the phone, open GTAV.apk to install it (allow installing apps from that source).
5. Open GTA V, choose the four files and press Install. It copies and checks about 16 GB, then
   starts the game. Keep the app open while it installs. The disc images and the pack can be
   deleted afterwards.
Objects missing on the phone? Copy the phone's Android/data/com.gtavrecomp.app/files/shader_cache
folder to <game folder>/android/shader_cache, press "Build for Android" again (a few minutes) and
choose the new pack on the phone (long-press the app icon, "Run setup again"); the game files stay. A newer GTAV.apk needs a new pack (the app
says so).

Everything made from your files stays in the game folder (data/, gtav_game.dylib, iphone/GTAV.ipa
and android/gtav-android-pack.zip). Do not share any of it: it contains Rockstar Games' copyrighted
work. (GTAV.apk as downloaded holds no game and is fine to share.) To uninstall, delete GTAV.app and the game folder.

This project is free software under the GNU GPL, version 3 or later (LICENSES/gtav-recomp-GPL-3.0.txt).
Source code: https://github.com/NuxsGit/hooky-GTA-Recomp. The complete source of every program in
this package that is under the GPL or LGPL (this project, the ReXGlue SDK runtime with the FFmpeg
it links statically, XenosRecomp, plume) is in gtav-recomp-src-<version>.tar.xz on the same
release page.
Bundled tools: clang and lld from LLVM 19.1.7 (official release build, Apache-2.0 with LLVM
exception), DirectX Shader Compiler (NCSA), MoltenVK and the Vulkan loader (Apache-2.0), simde
(MIT). tools/game_dll/libSystem.tbd is a list of five C library function names (this project) that
lets the bundled linker build gtav_game.dylib without Apple's SDK.
Licenses for the bundled open-source components are in LICENSES/.
Grand Theft Auto V is a trademark of Take-Two Interactive Software. This project is not affiliated
with or endorsed by Rockstar Games, Take-Two Interactive, Microsoft or Apple.
