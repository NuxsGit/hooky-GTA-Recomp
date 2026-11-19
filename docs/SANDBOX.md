# Desktop Workshop preview

The desktop launcher’s **Play** button starts the game and opens the mode selector **before** the
TU27 guest boots. **Sandbox solo** is the default (Enter); **Private multiplayer** and **Campaign**
are the other choices. Escape closes the selector without booting the game. This is a feature of the
new runtime + launcher, not of previously released ZIPs. Phone builds still use their existing startup.

- **Campaign** uses the existing story profile. It does not start the Workshop or open a listener.
- **Sandbox solo** runs the Workshop without a network connection.
- **Private multiplayer** starts the same isolated world. Only **Host here** opens a listener;
  selecting the mode or joining a friend does not open an incoming port.

Both Workshop modes boot your local GTA V Xbox 360 + TU27 data. They do not boot GTA Online or
contact Rockstar. They currently use the original GTA free-roam world, including its normal first-run
screens and prologue when there is no free-roam save yet. Play through those screens on the sandbox
profile first. The Workshop does not skip the prologue, import saves, or interrupt missions.

## Building and playing a checkpoint race

1. In free roam, press **F6** for **Workshop**. Use the keyboard and mouse for text fields.
2. In **Course builder**, name the course and use **Add checkpoint here**. Close the panel with
   **F6** or **Esc**, move to another point, and add the next checkpoint. Use at least 15 m between
   consecutive checkpoints. The course can contain 2 to 24 points for a race.
3. **Save course** saves that layout by name. **Load course** reads a local layout with that exact
   name. **Undo last** or **Clear layout** edits the active course without deleting a saved file.
   Names use letters A–Z, digits, spaces, hyphens and underscores (max 32 characters).
4. **Start checkpoint race** closes the host’s panel, gives a three-second countdown, and starts
   timing. Close F6 on any joining PCs to drive. Follow the yellow marker in checkpoint order;
   approach each point within 6 m. The HUD shows progress and distance; **Race results** shows times.
5. In **Test drive**, select a stock vehicle and request it nearby while on foot in free roam.
   Model streaming is time-limited. Only the local test vehicle is spawned, not copies on other PCs.

The host owns the course and starts/resets the race. Clients see the host's layout without it being
written to their disks. Players still loading, past the freshness limit, or joining after the
countdown starts are spectators until the next race. Blue map markers show the last fresh position
of friends. This is a checkpoint layout and one minigame on the existing map, not an editor for
terrain, props or scripts. This version does not render remote avatars or synchronize NPCs, damage,
vehicle physics or world changes. The race relies on positions reported by clients, not anticheat.

## Hosting for friends

1. Everyone needs their own verified TU27 installation and a compatible Workshop build. Choose
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
  Named checkpoint layouts are under `data/user/sandbox/courses/`. There is no automatic save import.
- The Disc 1 installation remains in `data/user/0000000000000000/545408A7/00000002/`. The patched SDK
  mounts GTA V's common packages from there read-only in Workshop sessions. Other content paths use
  the selected profile. Game/update data and the shader cache can be reused without copying gigabytes.
- To skip the selector from the launcher: `gtav_launcher --play -- --gtav_mode=campaign`
  (use `gtav_launcher.exe` on Windows; `solo` and `multiplayer` are also accepted). Direct developer
  runs keep the campaign default; use `--gtav_mode=menu --gtav_native=on` with the usual game paths
  to opt into the selector, or specify a mode directly. `--headless=true` still applies only to
  guest XAM prompts, not the startup selector. Use an explicit mode for automated runs.
- **F1** settings and **F2** debug still work (**Fn+F2** on media-key keyboards). Story cheat actions
  are disabled in private multiplayer. Intentional mission-failure slow motion is unchanged.

## Verification

```sh
cmake -S loader -B build-launcher -DGTAV_LAUNCHER_TESTS=ON
cmake --build build-launcher
ctest --test-dir build-launcher --output-on-failure
```

The assets-free `sandbox_loopback` test exercises the codec, bounded course I/O, storage layout,
countdown and ordered checkpoints, local server with two clients, capacity, invalid invites,
stale positions, disconnection and restart. The Linux launcher fixture checks the normal startup
flag and its explicit campaign override. These are not retail-game tests.

For a retail validation, use two installations with their own TU27 data. Check keyboard and mouse
navigation and quitting from the selector, then boot all three modes and confirm the campaign saves
remain unchanged after saving in Workshop. In free roam, host and join, drive through a round, test
vehicle spawn, switch in and out of missions/pause, and verify markers are removed on disconnect.
Test the F2 panel and a deliberate campaign mission failure as separate regression checks.
