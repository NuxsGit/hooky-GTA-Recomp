# Desktop Workshop preview

The desktop launcher opens the illustrated mode hub **before** the TU27 guest boots. Select
with the mouse, arrows or D-pad, confirm with Enter/A, or close with Esc/B. **Sandbox solo** is
the default. **Campaign co-op** is shown as unavailable; the title's missions and story progress
are not synchronized. Phone builds still use their existing startup.

- **Campaign** uses the existing story profile. It does not start the Workshop or open a listener.
- **Sandbox solo** runs the Workshop without a network connection.
- **Private multiplayer** starts the same isolated world. Only **Host here** opens a listener;
  selecting the mode or joining a friend does not open an incoming port.

Both Workshop modes boot your local GTA V Xbox 360 + TU27 data. They do not boot GTA Online or
contact Rockstar. They currently use the original GTA free-roam world, including its normal first-run
screens and prologue when there is no free-roam save yet. Play through those screens on the sandbox
profile first. The Workshop does not skip the prologue, import saves, or interrupt missions.

## Build overlay and AdminOS

In Workshop free roam, **F8** opens the permission-gated Creative Build overlay; **P / Up** opens
the phone. Use **F6** for the original checkpoint editor and private sessions. See the
[Build, phone, controls, and runtime limits](WORKSHOP_CREATIVE.md) for the new tools. They are
local to the host copy of the game; the private v2 room does not replicate them.

## Building a layout and playing

1. In free roam, press **F6 → Map builder**. Use the keyboard and mouse for the editor. Type a
   layout name (A–Z, digits, spaces, hyphens and underscores, up to 32 characters).
2. On foot, use **Place props** to choose a cone, barrier, crate or ramp. **Place 3 m ahead** places
   it in the direction you're facing using the rotation and height sliders. Ground height is not
   snapped automatically; use **Undo prop** and adjust the height if needed. Up to 24 placed props
   are stored per layout. Props are fixed local scenery, not shared moving physics objects.
3. **Add checkpoint here** records your position. Close F6, move, and repeat. A minigame needs 2–24
   checkpoints. Keep consecutive points at least 15 m apart for **Checkpoint race**; for
   **Checkpoint hunt**, all pairs must be at least 15 m apart.
4. **Save layout** keeps the props, checkpoints and selected minigame on this PC. **Load layout**
   reads a saved layout by its exact name. Undo buttons, **Clear layout** and **Return to editing**
   don't delete any saved files. Earlier checkpoint-only saves still load.
5. **Start minigame** closes the host's panel and starts a three-second countdown. Close F6 on any
   joining PCs to drive. For a race, visit the points in order; for a hunt, visit them in any order
   (the yellow marker points to the nearest unvisited one). Approach a point within 6 m to collect
   it. A round ends once every racer completes it or after 20 minutes. See times in **Results**.
6. With Creative build access, **Test drive** spawns a stock vehicle nearby from free roam while on foot.
   Model streaming is time-limited, and only your local test vehicle is driveable.

The host owns the layout and controls the minigame. The layout is shared in the room; a guest's
saved files aren't modified. Players whose positions are stale, who are still loading, or who join
after countdown begins are spectators until the next round. Blue map markers show friends' fresh
positions. Within 150 m, the game also attempts to stream their local translucent ghosts. On foot
or in an unrecognized vehicle, that is a stock pedestrian. For one of the test vehicles it is a
matching ghost vehicle. The ghosts show position and heading, not appearance, animations, collision,
damage, occupants or driving physics. If the scene natives cannot be used, markers and minigames
remain available. This is not a terrain or script editor, and it does not synchronize NPCs, weather
or traffic. Progress relies on client-reported positions, not anticheat.

## Hosting for friends

1. Everyone needs their own verified TU27 installation and a compatible Workshop build (protocol v2;
   it won't connect to earlier Workshop builds). Choose
   **Private multiplayer** at launch. The host opens **F6 → Session**, enters a nickname, chooses a
   **TCP** port (default **30127**), and presses **Host here**. Leave **This PC only** unchecked for LAN
   play. The listener shows the bound port and a random 32-character invite code.
2. On the same network, give friends the host’s **LAN IPv4**, TCP port and code. For another network,
   a trusted private VPN is recommended. If you choose direct Internet play, you must allow the
   chosen TCP port through the host firewall and forward it to that PC on your router. Share the
   reachable IPv4 with your friends. Carrier-grade NAT can prevent direct inbound play. The game
   does not discover your public IP, configure your router or bypass NAT.
3. Friends open **F6 → Session**, enter the host IPv4, same port and invite and press **Join friend**.
   Nicknames use the same characters as course names, up to 20 characters. At most eight players
   (host included) can be connected. **Stop server** ends the room and revokes its code; **Disconnect**
   returns a client to its local Workshop. The code changes every time the host starts a room.

**Use only with people and networks you trust.** The invite prevents accidental joins; it is not
host authentication or encryption. The code, names, positions and race state travel in plaintext
inside the TCP connection. Don't post the invite or expose the port as a public server. Use the
loopback-only option and `127.0.0.1` for tests, or restrict access using a private LAN/VPN/firewall.
No discovery, relay, updates, accounts, or asset transfers use this protocol.

## Storage and launch flags

- The Workshop profile, including guest saves and achievements, is under `data/user/sandbox/profile/`.
  Named layouts are under `data/user/sandbox/courses/`. There is no automatic save import.
- The Disc 1 installation remains in `data/user/0000000000000000/545408A7/00000002/`. The patched SDK
  mounts GTA V's common packages from there read-only in Workshop sessions. Other content paths use
  the selected profile. Game/update data and the shader cache can be reused without copying gigabytes.
- To skip the selector from the launcher: `gtav_launcher --play -- --gtav_mode=campaign`
  (use `gtav_launcher.exe` on Windows; `solo` and `multiplayer` are also accepted). Direct developer
  runs keep the campaign default; use `--gtav_mode=menu --gtav_native=on` with the usual game paths
  to opt into the selector, or specify a mode directly. `--headless=true` still applies only to
  guest XAM prompts, not the startup selector. Use an explicit mode for automated runs.
- To turn off live props and ghosts if streaming causes trouble, pass `--gtav_workshop_scene=false`
  after the launcher's `--` separator. Checkpoints, map markers, layouts and test vehicles remain.
- **F1** settings and **F2** debug still work (**Fn+F2** on media-key keyboards). Story cheat actions
  are Campaign-only. Intentional mission-failure slow motion is unchanged.

## Verification

```sh
cmake -S loader -B build-launcher -DGTAV_LAUNCHER_TESTS=ON
cmake --build build-launcher
ctest --test-dir build-launcher --output-on-failure
```

The asset-free `sandbox_loopback` test covers both minigames, old/new layout files, bounds, scene
selection by distance and pose freshness, and TCP rooms with multiple clients, capacity, refused
edits, bad invites, disconnect and restart. The `workshop_core` suite tests the authoritative
core, deferred effects and JSON on a fake game. The Linux launcher fixture checks the startup flag and
its explicit campaign override. The Windows packaging smoke checks verified ZIP application,
helper handoff, dashboard relaunch and preservation of player data. It separately starts the
packaged host with a temporary no-game DLL to verify the actual selector and Solo default. Neither
test boots the retail title.

Before publishing, test the same Windows ZIP on a PC with the original discs and TU27. Check the
selector and boot all three modes into the game. Play through the prologue on a fresh Workshop
profile, or use an existing Workshop free-roam save. Save in Workshop and confirm the campaign
saves are unchanged. In free roam, place/rotate/remove each prop, save and reload the layout, spawn
and drive each test vehicle, and test F6, pause and mission transitions. With a second TU27 PC, host
and join, follow another player on foot and in a test vehicle, drive out of range and back, play
both minigames, then disconnect and check that remote actors disappear. Check F2 and deliberate
mission failure in Campaign separately. Check the physical controller over USB and Bluetooth.
