# Workshop Build and AdminOS (desktop TU27 preview)

These tools use the isolated Workshop profile and the existing native renderer. They are not
available in Campaign or on mobile. Start **Sandbox solo** from Play, load or reach free roam,
and close the initial F6 panel with Esc. No patches, mods, scripts or game assets are downloaded
by these tools. The launcher, updater and Campaign profile do not change.

## Controls and modes

| Input | Action |
| --- | --- |
| **F6** | Existing Workshop sessions, checkpoint layouts, test drive and minigames. Editing/test drive now also require Creative access. |
| **F8** | Open/close the Creative Build overlay when the authority and free-roam checks allow it. |
| **F5** | Open/close the cheats (Admin) panel — a first-class workshop panel, never a phone page. |
| **Shift+F8** | Graphics replay when `--gtav_native_replay=true`. Campaign retains F8 for replay. |
| **P**, **Up** or **D-pad Up** (SDL pad) | Open or navigate the original GTA V phone when gameplay allows it. No Workshop overlay uses this shortcut. |
| **Mouse in Build** | Aim in the game view outside the panel. Left-click to place; right-click to cancel the preview. Wheel rotates, Shift+wheel changes distance, Ctrl+Shift+wheel changes height. |
| **Z / Shift+Z** | Undo / redo in Build, outside text fields. Ctrl+Y also redoes. |
| **ZQSD / Space / Ctrl** | Steer Fly or Noclip after closing Workshop. These are WASD physical positions on QWERTY. Flight is on foot. |

Build and Workshop reuse the existing input gate: raw look deltas are flushed when switching panels,
movement/fire are captured while open, and focus loss cancels placement. The camera is not
replaced. **F1** and **F2** still work, but F2's Story tools are Campaign-only.

The **Owner** seat is assigned by the local/host authority, not by JSON, a nickname, or a TCP
invite. Only Owner can promote or demote Admin. Owner cannot be kicked, banned or frozen.
**Admin** can moderate Player, not Owner or another Admin. **Player** has no AdminOS; Build requires
both effective Creative and the host's explicit `players_build` rule. Only tracked owned pieces
can be edited or removed in Build. Quotas and live-game checks also apply to Owner. Creative is
the initial map default. A deliberate per-player mode override survives a map-default change.
Entering effective Survival closes Build, cancels Creative spawns and stops Creative effects.
Sprint is explicitly restored to `1.0`; super jump is a per-frame request, not a separate writable
TU27 multiplier. It stops being requested on revocation. No movement toggles are rearmed by
respawning or going back to Creative.

## Build and AdminOS

- Build searches the catalog by display name, internal name, or the 32-bit hex hash. Tabs include
  props/architecture, vehicles, NPC/interactive models, favorites/recent, and presets. Models are
  checked against the loaded game before requesting them; candidates can be absent on a given
  install. Favoriting a car also makes it available in the AdminOS quick-vehicle picker.
- A selected model streams into a separate local preview (alpha 160, no collision). Placement
  supports grid and angle snaps, lift and range, and optional auto-anchor. Targeting tests a
  camera ray against tracked piece bounds; it **does not raycast map surfaces**. Without the
  optional ground probe, placement is a projection at the chosen distance. Anchor uses the
  mapped dynamic/gravity flags and stand-still task on owned peds, not an unmapped hard-freeze
  native. Start with a cone on a clear flat surface. Leave vehicles before deleting them.
- **Presets** capture an aimed anchor and up to 15 of your nearby pieces, up to 25 m from it.
  Position and yaw are relative to the anchor. A preset placement is one history action and
  previews the anchor; if one member cannot spawn, the batch rolls back. Recent activity keeps
  the last 30 models spawned, and history retains up to 100 actions per builder. **Clean my
  build** is irreversible and clears undo/redo, not favorites, presets or F6 layouts.
- **F6 Workshop** shows the session controls. Owner/Admin use the dedicated **Cheats (Admin) panel (F5)** — 5 large tabs (MAP / MOVE / GIVE / WORLD / PLAYERS) — never a phone page. The fake-phone Admin was removed because it captured the whole game into a phone-shaped window and made gameplay impossible.
  **F8** is the only shortcut for Creative Build. **P / Up** opens the original GTA V phone (restored, not intercepted).
- **AdminOS → Players & Moderation → Load / respawn three practice players** creates Alex, Noa
  and Sam in Solo. Their rows are labeled simulated. Test mode overrides, nearby on-foot moves
  in both directions, anchoring, give/heal/revive, and kick/ban with a reason. A ban survives a
  restart and blocks respawn until unbanned. Kick is not a ban. Changes to mode, role, ban/kick,
  cleanup and bulk give emit UTC `[ADMIN AUDIT] Actor -> Action -> Target` lines to the game log.
- Give uses an allowlisted base/update armory, checks `IS_WEAPON_VALID`, and processes bulk give
  a weapon per frame. The default give uses a fixed ammo request; optional maximum-ammo lookup
  uses the extended ABI. Infinite clip is a reversible toggle. Quick vehicle can warp the local
  ped into an empty driver's seat and start the engine. Credits are a **Workshop wallet**, not a
  write to a story cash stat. It resets with the session. PvP and fall-damage rules are not yet
  enforced by the TU27 adapter and are shown disabled.

### Experimental TU27 operations

`--gtav_workshop_extended_tu27=true` enables the mapped, **retail-unverified** operations that
use guest output pointers or string arguments, and the Noclip collision toggle. This includes
Build ground snap, waypoint (blip 8) and landmark streaming, weather (including Snow/Halloween
where present), and maximum-ammo lookup. Test on a spare Workshop save. Travel checks ground
progressively while the player is still at the original location; when possible, drivers take
their vehicle. Passengers cannot use it. If ground never resolves within 300 game frames no
move is made. This is separate from the short, same-area on-foot practice roster moves which
work without the opt-in. The handler table and dispatcher check are not a real-world proof of
physics, game-content availability, or safe signatures on any other game revision.

## Persistence and authority boundary

`data/user/sandbox/profile/config/` contains version-1 `permissions.json`, `bans.json`,
`build_favorites.json` and `presets.json`. Map defaults/rules and durable privilege grants are
in permissions; individual mode overrides are session state. Presets store model hashes,
centimeters and millidegrees, not pointers or guest handles. Config is validated and bounded;
unsupported or broken documents are kept unchanged and a storage warning is shown. Writes go
through a single disk worker and same-directory atomic replacements. No changes to Disc 1,
Campaign saves, or the F6 `.gtcourse` format are involved.

`gtav/src/core/WorkshopCore` is an ImGui-free, single-writer authority; it can admit verified
transport identities and accept commands bound to their server-side player ID. `WorkshopGame`
is the game-effects port. `CreativeRuntime` binds the local Owner to a bounded window/render →
guest mailbox and publishes read-only views. Direct native calls are only made on the existing
validated `GET_GAME_TIMER` script thread. The spawn queue uses guest swaps for its frame budget,
never sleeps for a model and releases its streaming reference on success, failure, timeout or
revocation. It does not compete for the same model reference with the older F6 scene streamer.

The shipping **private v2 TCP protocol still syncs poses, room layouts and race state only**.
A v2 guest sees room status in F6 Workshop, not host privileges. The private host can use
Build/AdminOS on the host copy but these commands and entities are not replicated; the simulated
roster is Solo-only. Promotion, durable bans and shared gameplay require a transport that actually
authenticates principals. No changes to the v2 wire format or the original trust model are made
for this preview. A future transport must authorize requests in `WorkshopCore`, not trust the
client's UI or take addresses/handles from the wire.

## Verification

The `workshop_core` test in the [launcher suite](SANDBOX.md#verification) runs without game assets.
It exercises a mock host, access checks and revocation, rollback, presets, driver travel,
streaming, simulation, and versioned JSON under an accented path. `workshop_runtime` runs the
production desktop mailbox and writer against a fake game: stale commands, mode/host revocation
before a loaded model can spawn, focus loss during a tick, and profile flush/restart. The Windows
candidate also checks isolated JSON creation on the actual packaged host with a fixture DLL.
To test the real ImGui widgets without a renderer, pass the pinned SDK ImGui sources (the Windows
candidate does this automatically):

```sh
cmake -S gtav/tests -B build/workshop-ui -DGTAV_IMGUI_SOURCE="$PWD/rexglue-sdk/thirdparty/imgui"
cmake --build build/workshop-ui --parallel
ctest --test-dir build/workshop-ui --output-on-failure
```

Before release, repeat on genuine TU27 data: enter free roam; exercise F8 search, ghost,
place/undo/redo/cleanup, verify that clicking or turning the wheel never fires or jumps the camera,
and switch to Survival while streaming or flying. Test focus loss, return from a cutscene,
controller polling and a ped change. Open the cheats panel (F5) on a fresh and existing Workshop profile; run
practice moderation, check the audit, restart and unban. Test the extended opt-in separately
and confirm invalid models or unresolved travel leave the world unchanged. A two-PC test should
confirm the old v2 room still works, and verify the original phone on both host and guest. Do not treat tests on
synthetic peds or a fake game as evidence of retail native safety or multiplayer physics.
