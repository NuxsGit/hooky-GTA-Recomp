# Cheat dev complet — F5 — host-only partageable, tous modes

> Vrai cheat complet avec toutes options dev nécessaires, basé sur les fichiers jeu Google Drive `GTA PCP/data/...` et le mapping TU27 natif du recomp.

## Inspection Drive

- `GTA PCP/data` contient `game/common/data,cache,spirv,build/generated,obj`, `user/{profile,sandbox,campaign_coop}`, `logs/gtav.log 9.28MB` …
- `list_files` 50 items (2026-11-21) confirme arborescence `GTA PCP / data / user / game / build / profile`
- Hashes modèles/armes TU27 extraits des binaires recompilés (`gtav_recomp.3xx.cpp`) et de `BuildManager` (`kModels` 48 entrées, `kWeapons` 60+)
- Jenkins `ModelHash` identique au jeu → options cheat alignées sur le contenu réel du jeu.

## Activation

- **Touche F5** (SDL `F5` / Win32 `VK_F5`) → panel `CHEAT // DEV PANEL [F5] — Tous les modes`
- Visible dans **tous les modes de jeu** : Campaign (solo story), Solo (sandbox), Multiplayer (private), CampaignCoop (3 protagonistes)
- Workshop core démarré même en `Mode::Campaign` pour la persistence des perms (`data/user` → `permissions.json`)
- Capture `SetCapture(..., admin_panel=F5, ...)` → `Submit` fonctionne même quand `sandbox::Active()==false`

## Permissions — host-only par défaut, grantable

- `LocalAuthority()` étendu :
  - `Solo` / `Campaign` → toujours true (tu es host)
  - `Multiplayer` / `CampaignCoop` → true si `Idle/Hosting/Failed`, false si `Joined`
- `PermissionManager` : `Role::Owner` = `kLocalOwner` (host), `Admin` = promu, `Player` = rejoint
- `AdminOS` = Owner ou Admin → seul celui-ci voit le panel
  - En solo : toujours Owner → panel visible
  - En hosté : seul l'hôte (Owner) par défaut, les amis en `Player` ne voient que le message « demande à l'hôte de te promouvoir »
  - Hôte : `F5 → PLAYERS → Target → Promote to Admin` (ou `Demote`) → `SetAdmin(Owner, identity, on)` → persiste dans `permissions.json` + audit UTC
- `IsCheatAllowed(v, local)` = `Fresh && admin && LocalAuthority && AdminOS`

## Dev options — toutes options nécessaires

Débloquées **tous modes** (découplées de `Mode::Creative`) — `WorkshopCore` `Effect/Speed/QuickVehicle` + `Reconcile` autorisent `playable && CanAdmin` :

| Onglet | Options | Natifs / Op |
|---|---|---|
| **MAP** | Gamemode perso & map default, builder policy, map rules (PvP, fall damage, traffic, peds, police) | `Op::PlayerMode/MapMode/MapRule/BuildRule` |
| **MOVE** | Noclip, Fly, God, Stamina, Super jump, Sprint 1.49x, Never wanted, Flight speed x1/x3/x8/x15, Heal 200+armor, Clear blood | `Effect::Noclip,Fly,God,Stamina,Jump,Sprint,Wanted,InfiniteAmmo` + `Tool::Heal/Damage` |
| **GIVE** | Armory (60+ armes TU27 validées), Infinite clip, Quick vehicle (40+ véhicules + favs, warp driver), Repair/Clean, Wallet credits, Survival kit | `Tool::Weapon/Armory/Warp/Repair/Clean` `Op::QuickVehicle/Cash/Kit` |
| **WORLD** | Waypoint, Landmarks (8), Weather (7), Clock 0-23 + Freeze, Lunar gravity | `Feature::Waypoint/Ground/Teleport`, `Tool::Weather/Clock/PauseClock/Gravity` |
| **PLAYERS** | Practice peds, Freeze/Anchor, GoTo/Bring (streamed ground), Heal/Revive/Kit, Promote/Demote, Kick/Ban (reason + bans.json) | `Op::Freeze/GoTo/Bring/Promote/Kick/Ban/Unban` |

Noclip nécessite `--gtav_workshop_extended_tu27` (output-pointer natives). Tous les checks `caps.Has(...)` restent.

## Fichiers modifiés

- `src/sandbox/CreativeRuntime.{h,cpp}` `LocalAuthority` all-modes + `IsCheatAllowed`
- `src/core/WorkshopCore.cpp` cheats all-modes, `QuickVehicle` sans Creative, `Reconcile` all-modes
- `src/phone/AdminCheatApp.cpp` MOVE/GIVE sans `Creative`, `Spawn` via `playable`
- `src/native/menu.{h,cpp}` `Panel::Cheat`, `BuildCheat()` F5, `BuildWorkshop` sans filtre mode, capture étendue
- `src/native/input.cpp` / `renderer.cpp` handlers F5
- `src/sandbox/desktop.cpp` workshop core start en Campaign + Shutdown toujours
- `src/debug_control.h` `Panel::Cheat`
- `loader/tests/workshop_core_tests.cpp` attentes mises à jour (cheat persiste en Survival)

## Tests

- `loader/tests/workshop_core_tests` adapté : après `MapMode Survival` + `PlayerMode Map default`, `cheats` reste actif (1.49x) et `false` tuples absents
- CI `Linux and macOS launcher checks` + `Windows x64 packages` verts (7/7, 6m/1m) sur `arena/a2e60d43-hooky-gta-recomp`
