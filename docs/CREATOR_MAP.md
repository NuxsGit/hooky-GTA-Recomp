# Creator MAP — Éditeur type Unity / Unreal (Mode 05)

> Nouveau mode de jeu **Creator MAP** — un éditeur de maps & scenarios inspiré de Unity et Unreal Engine, puis jouable directement in-game. Isole dans `data/user/creator/` (profil + maps).

## Vue d'ensemble

* **WORKSHOP engine réutilisé** — BuildManager (90+ modèles Prop/Vehicle/Ped), ghost preview, grid/angle snap, ground snap, anchor, undo/redo, Streaming TU27.
* **Isolation** — `data/user/creator/profile/` (saves, permissions, favorites, presets) + `data/user/creator/maps/*.json` (tes maps). Ne touche pas `data/user/sandbox` ni `campaign` ni `campaign_coop`.
* **Jouer dedans** — **▶ Play** spawn tous les objets à leur pose stockée via `WorkshopCore::Place(proposed)` (500 m radius, `IS_VEHICLE_MODEL`/`IS_MODEL_ON_CD` filtré), puis free-roam pour tester triggers/scenario.

## Lancer

1. **PLAY hub** — tuile `05 / CREATOR MAP [EDITOR]` (or, 3ᵉ colonne ligne du bas, à côté de PRIVATE SESSION). Nave : flèches / stick, Entrée/A.
2. **Fallback** — si le hub rate, message box propose aussi `Creator MAP`.
3. **Ligne de commande** — `gtav_launcher --play -- --gtav_mode=creator` ou `creator-map` / `creatormap`.
4. Besoin de `--gtav_native=on` (renderer natif), comme tous les modes Workshop.

## Contrôles

| Raccourci | Action |
|---|---|
| **F7** | Toggle Creator MAP editor fullscreen (Unity-like). Dispo même hors Creator pour prototyper, mais saves toujours dans `creator/`. |
| **F8** | Creative Build overlay (classique) — ghost + placement caméra. |
| **F5** | Cheats Admin (SPAWN Vehicules/Props/Peds, GIVE, etc.) — host-only partagé via Promote, même en Creator. |
| **F6** | Workshop sessions + nouvel onglet **Creator MAP** (aperçu + bouton Open Editor). |
| **F1 / F2** | Settings / Debug. |

Dans l'**editeur F7** :
* **Hierarchy** (gauche) — liste objets (`idx: name [internal] [L]`) + triggers. Clic = selection → Inspector. Tooltip pose + hash.
* **Viewport** (centre) — ghost preview hors panneaux : **clic gauche place**, **molette yaw**, **Shift+molette distance**, **Ctrl+Shift+molette hauteur** (même que F8). Grille `Off/0.25/0.5/1/2m`, angle `15/45/90°`, ground snap, auto-anchor.
* **Inspector** (droite) — transform Unity : **Position xyz (DragFloat3 0.1)**, **Yaw (0-360)**, **Scale xyz (0.1-5)**, Locked, Duplicate/Delete, Snap to ghost. Pour Trigger : Name/Pos/Radius/Type/Script/Delete.
* **Asset Browser** (centre bas) — filtre `All/Props/Vehicles/Peds/Triggers`, recherche `Matches(name/internal/hash)`, clipper list. Clic modèle → ajoute à Hierarchy (pose `{{0,6,0.5},0}` par défaut, éditable) + preview ghost. Pour Trigger : `Add trigger at player (6m ahead)` → `player_start+6m`.
* **Maps on disk** (centre) — liste `creator/maps/*.json`, clic Open, Delete selected map.
* **Toolbar** (haut) — `New` (Untitled), `Open` (first map), `Save`/`Save As`, `▶ Play`/`■ Stop`, `Rename`, dirty `* unsaved` / `saved`, stats `Objects / Triggers`.
* **Scenario** (Inspector bas) — `Scenario name`, `Description`, `Win` (`All Checkpoints / Win Zone / Script`). Triggers Checkpoints comptent pour win.
* **Status bar** (bas) — `F7 toggle | F8 Build | F5 Cheats | isole creator/` + `Ready to play` / raison.

## Modèles & Scénarios

* **Objets** (`CreatorObject`) — `model` (hash Jenkins TU27), `kind` (Prop/Vehicle/Ped), `pose` (world), `scale`, `locked`, `name`, `category`. Limite 240 objets / map (vs 160 Build). Vérif `FindModel` + `Valid()`.
* **Triggers** (`CreatorTrigger`) — `name`, `pos`, `radius 0.5-200`, `type` (`Checkpoint, SpawnPlayer, SpawnVehicle, WinZone, LoseZone, Script`), `script` (string libre, ex `onEnter: giveWeapon RPG`). Limite 64 / map.
* **Scénario** (`CreatorScenario`) — `name`, `description`, `triggers[]`, `win` (enum). Futur : graphe logique, timelines.
* **Map** (`CreatorMap`) — `name` (64, `^[\w \-]+$`), `description` (500), `author`, `version`, `player_start` + `yaw`, `objects[]`, `triggers[]` (legacy flat) + `scenario`. Persist JSON `creator/maps/<name>.json` (writer `EscapeJson`, parser simple scan — pas de lib externe).

```json
{
  "name": "MyStuntPark",
  "description": "Unity-like park — 12 ramps + win zone",
  "author": "Player",
  "version": 1,
  "player_start": {"x":0,"y":6,"z":0.5,"yaw":0},
  "objects": [
    {"model":123456,"kind":3,"x":10,"y":20,"z":0.3,"yaw":45,"sx":1,"sy":1,"sz":1,"locked":false,"name":"Stunt ramp","cat":"Ramps"}
  ],
  "triggers": [
    {"name":"Checkpoint 1","x":15,"y":25,"z":0.3,"r":5,"type":0,"script":""}
  ],
  "scenario": {"name":"Race","desc":"","win":0}
}
```

## Jouer la map

1. Dans F7, `Save` → `▶ Play` (ou bouton `Test Play (spawn all)` dans Viewport). Chaque objet est `Submit({Op::Place, value=model, proposed=pose, on=locked}, build_panel=true)` via CreativeRuntime queue (4/cmd/frame, 2s timeout, epoch/world_epoch guards). Distance max 500 m (vs 90 m normal) pour grandes maps.
2. Roam, test triggers (HUD Creator affiche `Nearest trigger: name [type] dist`). Triggers sont volumes invisibles — futur : détection `DistanceSquared < r²` + script.
3. `■ Stop` (TODO `Cleanup`) ou `F8 → Clean my build` pour effacer tes pièces. Undo/redo gardent 100 history.
4. Alternative rapide : `F5 SPAWN` (generic QuickVehicle) ou `F8 Build` ghost pour placer à la volée — même roster 90+ modèles.

## Fichiers

* `gtav/src/frontend/modes.h` — `Mode::Creator`, `Card::Creator`, `Row/Column/CardAt` (2 cols row0, 3 cols row1), `Cycle` %5, `PlayMode`.
* `gtav/src/frontend/play_hub.cpp` — 5 tuiles (60,284 650,251 / 730,284 … / 60,555 426,163 / 507,555 … / 954,555 Creator gold tint), `kAccents` 5, `art[5]`, `TabForFocus`, `Horizontal/Vertical` clamp, `Fallback` creator.
* `gtav/src/sandbox/CreatorManager.{h,cpp}` — `PrepareCreatorWorkspace`, `CreatorObject/Trigger/Scenario/Map`, JSON save/load/list/delete, `CreatorManager` (New/Open/Save/SaveAs, Add/Remove/Duplicate, ValidForPlay).
* `gtav/src/sandbox/CreatorEditorUI.{h,cpp}` — `DrawCreatorEditor` (toolbar + 3 panels Unity : Hierarchy/Inspector/Asset browser + viewport gizmo + maps list + scenario), `UpdateCreatorPreview`.
* `gtav/src/sandbox/desktop.{h,cpp}` — `Mode::Creator` dans `Configure` (`--gtav_mode creator*`), `CreatorWorkspace` isolé, `GetCreatorManager()/IsCreatorMode()`, `DrawWorkshop` onglet Creator MAP + header, `DrawHud` Creator (map name, objects, nearest trigger), `LocalAuthority` true pour Creator.
* `gtav/src/sandbox/CreativeRuntime.cpp` — `LocalAuthority` inclut Creator.
* `gtav/src/core/WorkshopCore.cpp` — `Build` accepte `proposed` même pour Local (editor pose), distance 500 m si `proposed`.
* `gtav/src/native/menu.{h,cpp}` — `Panel::Creator`, `BuildCreator()` fullscreen, `ToggleCreator()` F7, capture `build+admin`, `Frame` case Creator, `TextPanel`.
* `gtav/src/native/input.cpp` — F7 `ToggleCreator` (SDL + VK_F7).
* `gtav/WorkshopCore.cmake`, `gtav/CMakeLists.txt` — ajout `CreatorManager.cpp`, `CreatorEditorUI.cpp`.
* `loader/VERSION` — bump `0.8.17` → `0.9.0` (nouveau mode).

## Vérif

* Hub : 5 tuiles, navigation flèches respecte 3 cols ligne basse, TabForFocus garde Creator dans Workshop, art tints, fallback button id 4 → PlayMode Creator.
* Mode Creator : `PrepareCreatorWorkspace` crée `creator/profile` + `maps`, `workshop::Start(..., true)`, `mode=Creator`, isole saves, `Active()` true.
* Editeur F7 : Hierarchy/Inspector/Asset browser fluides, ghost preview suit `placement` + selected model, Search `Matches`, filtre kind, favorites via `Has(ForKind)`, Save JSON valide, List/Open/Delete, Dirty flag, Rename.
* Play : `Submit Place(proposed)` 4/frame queue, `BuildManager::Place` OK si <500 m + caps Has(ForKind) + valid pose, rollback si fail/timeout, HUD montre `Ready to play`.
* Regression : Solo/Multi/Campaign/CampaignCoop inchangés, F5 cheats + F8 Build + F6 Workshop verts, `workshop_core` + `play_hub` tests pass (mock game, 5 cards).
* Isolé : `data/user/creator` séparé, `Stop()` reset manager, `VERSION 0.9.0`.

## Suite (roadmap)

* Terrain/heightmap sculpt, spline roads, lighting/weather per map, minimap.
* Visual scripting (triggers → graph `OnEnter → SpawnPed / GiveWeapon / Win`).
* Multiplayer Creator (host sync via Session, invite share like Private).
* Import/export preset → map, thumbnail auto-capture, workshop share code.
* Play cleanup/Stop + respawn + failure reset + checkpoints streaming (Course-like).
