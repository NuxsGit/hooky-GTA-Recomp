# Campaign Co-op (3 joueurs)

**Objectif** : jouer la campagne à 3, un joueur par protagoniste (Michaël, Franklin, Trevor), dès le début, avec sauvegarde isolée et serveur hébergé localement (IP/port éditables dans le launcher).

## Modes

- **Campaign solo** : `data/user` — sauvegarde historique classique, inchangée.
- **Campaign co-op** : `data/user/campaign_coop/profile` — nouveau profil isolé. N’écrase jamais les saves solo.
  - Un fichier marqueur `campaign_unlock.cfg` est créé dans ce profil ; le runtime natif le détecte pour déverrouiller les 3 personnages au lancement (aucune mission préalable requise).
  - Les saves co-op et solo ne se partagent jamais ; supprimer `campaign_coop` remet à zéro la progression co-op uniquement.
- **Sandbox** : `data/user/sandbox/profile` — inchangé.

Le hub `Play` (SDL) expose 4 cartes :

- `Campaign` → `Mode::Campaign` (solo)
- `Sandbox` → `Mode::Solo` (atelier)
- `Campaign Co-op` → `Mode::CampaignCoop` (3 joueurs, verrouillé par personnage, **nécessite `--gtav_native=on`**)
- `Private Session` → `Mode::Multiplayer` (atelier multi 8 joueurs)

`frontend/modes.h:PlayMode()` retourne `CampaignCoop` uniquement si `workshop_enabled` (native renderer) est vrai. Sinon la carte est `UNAVAILABLE`.

## Verrouillage des rôles

- Le salon campagne limite à **3 joueurs max**. L’hôte est toujours `Michael` (slot 0, id 1).
- Le 1er client rejoint → `Franklin` (slot 1), le 2ème → `Trevor` (slot 2). Id déterministe (ordre d’`id` croissant, qui correspond à l’ordre d’arrivée).
- `session.cpp:RunHost` refuse les `Hello` supplémentaires quand `campaign_mode && players.size() >= 3`.
- L’UI (F6 → Session) affiche `ViewCampaign()` : `assignments` + `self_character` + texte « Locked protagonist: X | Saves: campaign_coop (isolated) ».
- HUD : `Campaign Michael | 2/3 [F6] Hosting …` + ligne grisée du verrouillage.

API :

- `gtav/src/sandbox/campaign.h` : `CampaignCharacter` enum, `CharacterLabel()`, `PrepareCampaignWorkspace()`, `EnsureCampaignUnlock()`, `HostCampaign()/JoinCampaign()/ViewCampaign()`, `CampaignHostConfig` persistance (`data/campaign_coop.ini`), capacité `kCampaignMaxPlayers = 3`.

## Isolation des sauvegardes

- `PrepareCampaignWorkspace(user_root)` vérifie `real_root/0000000000000000/545408A7/00000002` (TU27 installé), puis crée `campaign_coop/profile` + `campaign_coop/courses` via `EnsureSeparateDirectory()` corrigé (`create_directories` + `symlink_status` + `weakly_canonical == canonical`). Échoue proprement sur `MissingInstall` / `SeparateFolderRequired`.
- `desktop.cpp:Configure` branche `Mode::CampaignCoop` : crée l’espace, écrit le flag unlock, monte `paths.user_data_root = profile_root` et partage `common_content_root` en lecture seule (pas de copie des 8 Go de packages). Log : `[campaign] coop mode: 3 protagonists unlocked from start, save isolated at …`.

## Réseau & launcher (IP/port éditables)

Même transport que les sessions privées : TCP `kDefaultPort=30127`, invitation 32-hex, invite aléatoire `BCryptGenRandom`/`/dev/urandom`. Aucune découverte, pas de chiffrement — à utiliser en LAN/VPN de confiance.

- **Launcher Windows** (`loader/src/main.cpp`, `loader/src/campaign_config.*`) :
  - Sur la page *Accueil* : champ `IP d’écoute (vide=auto)` + `Port TCP` + bouton `Enregistrer`. Persistance dans `data/campaign_coop.ini` (`address=`, `port=`, `loopback_only=`) via `LoadCampaignHostConfig/SaveCampaignHostConfig` (écriture atomique `.tmp` → rename).
  - `LoadCampaignInputs()` pré-remplit l’edit au `CreateControls Layout` ; `SaveCampaignInputs()` valide (IPv4 pointée, port 1-65535) et log `Campagne host saved: ip:port`.
  - Au `Play`, `extra_args` reçoit `--gtav_campaign_host=...` / `--gtav_campaign_port=...` si non défaut, forwardés à `LaunchWindowsGame` (donc aux cvars `gtav_campaign_host/port`).
  - `WM_CTLCOLOREDIT` et `SetBusy` incluent les nouveaux edits ; la couleur `kField` et l’enable/disable sont cohérents avec les autres champs.
  - `desktop_home` marque `data/campaign_coop.ini` comme `kMutable` → copié lors d’un `OpenDesktopHome` migration.

- **Launcher Linux** (`loader/src/main_linux.cpp`) : `AppendCampaignArgs()` lit le même `campaign_coop.ini` et injecte les mêmes `--gtav_campaign_*` avant chaque `LaunchGame` (Setup, Play, Rebuild path). Pas de GTK supplémentaire nécessaire ; édition manuelle du ini possible.

- **Jeu** (`gtav/src/sandbox/desktop.cpp`) : `REXCVAR_DEFINE_STRING(gtav_campaign_host)` / `REXCVAR_DEFINE_INT(gtav_campaign_port)` lus pour pré-remplir l’UI F6 `Host campaign here`. Hôte/Joindre appellent `Party().HostCampaign/JoinCampaign` (qui set `campaign_mode_ = true` sous `mutex_` avant de lancer le worker).

## Flux UI

- Hub : `StoryCoop` carte montre `3 PLAYERS` + description verrouillage, art `story_` teinté chaud (subtil wash) pour distinguer du solo.
- In-game F6 : onglet `Session` commun Sandbox/Campagne. En campagne, texte `3 slots locked to Michael/Franklin/Trevor. Saves are isolated.` + décompte `Players: 2/3` + liste des protagonistes verrouillés + `ShowCampaignStatus()` (save path, unlock, self character).

## Bugs corrigés au passage

- `paths.cpp:EnsureSeparateDirectory` : `create_directory` → `create_directories`, check `symlink_status` avant `canonical`, comparaison `weakly_canonical == canonical` (évite faux-négatif sur `/` terminant et rejets des junctions Windows). Évite `profile` qui échoue si `sandbox/` manquant.
- `session.cpp` : `Host/Join` trim l’`address` avant `ParseEndpoint`, `HostCampaign/JoinCampaign` séparent proprement le mode (`campaign_mode_` sous `mutex_` + `lifecycle_` lock), `StopLocked` reset `campaign_mode_`, `Fail` ne le clear pas (affiche le bon message d’échec), `RunHost` vérifie la limite 3 avant `room_.Add` (rejette sans allouer d’`id`), verrou `mutex_` pris avant `room_.View`.
- `desktop.cpp` : branche `campaign-coop`/`campaign_coop` alias, gestion `SecureFolderRequired` vs `MissingInstall` distincte, `EnsureCampaignUnlock` création atomique, `workshop::Start` seed même pour campagne (évite crash `BuildAllowed` null), `DrawWorkshop`/`DrawHud` branchent sur `Mode::CampaignCoop` (actor counts, HUD « Campaign Michael »).
- `play_hub.cpp` : tuile `UNAVAILABLE` → `3 PLAYERS`, description verrouillage, art réutilisé + wash, `playable ? description` simplifiée (l’ancien `|| focus==StoryCoop` masquait l’exigence native), `kTiles` labels story-coop corrigés.
- `loader/desktop_home` : `data/campaign_coop.ini` ajouté à `kMutable`, migration conserve l’IP/port éditée.
- `loader/main.cpp` : `kIdCampaignIp/Port/Save` + handles `g_campaign_*`, coloration `WM_CTLCOLOREDIT`, `SetBusy` enable/disable, `LoadCampaignInputs` au démarrage, `SaveCampaignInputs` avec validation IPv4 stricte (4 octets 0-255, port 1-65535, pas d’injection).

## Lancement

- Depuis le launcher : `Accueil → IP/Port campagne → Enregistrer → Jouer → choisir Campaign Co-op`.
- CLI : `gtav_launcher -- --gtav_mode=campaign-coop --gtav_campaign_host=192.168.1.10 --gtav_campaign_port=30127`
- Test loopback : cocher `This PC only (testing)` puis partager `127.0.0.1:port code` ou laisser l’IP vide.

## Limites

- Free-roam campagne uniquement (pas de synchro missions/script). Les missions solo restent jouées en solo ; le co-op suit la carte ouverte et les marqueurs. Progression mission partagée = à implémenter via `Tu27Workshop` RPC futur.
- 3 joueurs max, pas de spectateurs au-delà.
- Trafic chiffré plaintext, invite 16 octets aléatoires non authentifié — LAN/VPN privé uniquement.
