# Optimisations du rendu pour N100 — Lenovo Y (8 Go, Intel N100, GPU intégré)

> Objectif : viser 60 fps stables sur la machine du joueur. Ces leviers vivent dans le
> renderer natif (`gtav/src/native/backend.cpp` + `graphics_policy.h`) — aucune donnée de jeu,
> aucun shader, aucune image livrée ici.
> Date : 2026-11-20 · Aucun A/B in-game exécuté : la validation se fait sur la machine N100.

## 1. Ce qui est déjà fait (inventaire)

Ces leviers sont actifs, testés asset-free (`graphics_policy_tests.cpp`) ou in-game. Aucun
n'est spécifique au N100 par accident — ils sont volontairement partagés par les préréglages
« Potato » / « Extra Potato » / « N100 Ultra ».

| Levier | Où | Effet attendu (N100) |
|---|---|---|
| Scene scale 0.25 (N100 Ultra), 0.333 (Extra), 0.5 (Potato) | `graphics_policy.h` / `SceneScale` | ÷4 à ÷2 les fragments 3D ; le floater `ScaledTexels` garde les cibles ≤ 320×180 à 0.25. |
| 1 slot de frame (`kMaxFrameSlots`→1), budgets textures 96/96/192 MiB, buffers 32/32/64 MiB, chunk upload 8/8/16 MiB | `graphics_policy.h` `MakeGraphicsMemoryPolicy` | Moins de mémoire vidéo, moins d'uploads CPU, 1 image à la fois (latence). Ultra passé de 64/16/4 à 96/32/8 le 2026-11-21 pour corriger artefacts noirs + têtes sur vitres. |
| Saut de mip (min LOD) : +2 pour Ultra, +1 pour Extra, +1 skip material-mip | `backend.cpp` (Sampler) | -2 niveaux de mip = ~4× moins de bande passante de texture par fetch pour les tex mipmapped eligible. |
| Aniso cap 1× pour Ultra/Extra (16× standard) | `backend.cpp` `sampler_anisotropy_cap` | Évite le filtre anisotrope coûteux. |
| Fog strip 3D omis (`ReduceFog`) | préréglage Extra/Ultra | Retire une passe coûteuse de fog à distance. |
| Contact rays 0 (préréglage Ultra) | `contact_rays` | Retire la passe d'occlusion de contact en écran. |
| FSR 1 (préréglage Ultra) | `Fsr1` | Upscaling léger à la place d'un rendu 3D net — ce qui compte vite à 0.25. |
| `defer_writebacks`, `defer_clears`, `resolve_discard`, `fetch_resolves`, `skip_depth_restore`, `skip_resolves` (diagnostic) | `backend.cpp` | Moins de passes graphiques et moins de passages de fond en sortie de passé. |
| 1 slot GPU (pas `slot_targets`) pour les préréglages lean | memory policy | Évite la duplication des cibles par image in-flight (8 Go : de la mémoire). |

## 2. Nouveauté de cette session : échantillonnage paresseux des textures (CPU N100)

Sur la machine N100, le render thread est CPU-limité (8 Go, CPU intégré 4 cœurs, mémoire
partagée). Tout cache de grande texture faisait un **hash échantillonné à chaque frame**.
Nouveau défaut (cvar non défini = défaut) — **corrigé 2026-11-21 pour stabilité** :

- **N100 Ultra** : sample toutes les **2** frames (était 3 → aliasing : tête Franklin sur vitre voiture) ;
- **Extra Potato / Potato** : sample toutes les **2** frames ;
- **Standard** : toutes les frames (comportement historique).

Le cache sampled passe désormais de 16×256 (4 Ko) à 24×256 (6 Ko) + vérif même dans la fenêtre N (backend.cpp) pour éviter les faux positifs. L'ancien skip pur temporel est retiré.

En cas de définition explicite (`--gtav_native_texture_sample_frames=N`), la valeur est
**conservée**. Le hash complet d'une grande texture reste toutes les 30 frames
(`texture_recheck_frames`), donc un changement de texture est détecté dans ce budget.

Fichier touché : `graphics_policy.h` (`TextureSampleFrames()`), `backend.cpp`
(`s.texture_sample_frames` — associé au point de consommation des textures), tests
`graphics_policy_tests.cpp`. **Non exécuté in-game** : la différence sera visible dans
`logs/native` via les compteurs de `textures`/`hash`/`ms` par frame.

## 3. Culling / LOD / autre — limites de l'architecture

**Frustum culling et LOD par distance : NON IMPLEMENTÉS et non implémentables ici.**

- Le renderer natif **rejoue la liste de draws émise par le guest** (`backend.cpp::Draw`).
  Ni la caméra, ni les volumes de bornes, ni une hiérarchie de scène ne sont présents dans
  la capture — le guest élimine déjà avant d'émettre ses draws. Rejouer un cull de
  notre côté nécessiterait la matrice de vue/projection et les bornes par objet, ce qu'on
  n'a pas — ni dans `frame.h`, ni dans la capture (`DrawCommand` n'a ni bounding box ni
  matrice).
- La « culling » existante de l'architecture par défaut **est le guest** : le renderer
  natif est un rejeu de ce qu'il émet. Les seules sélections disponibles sont des réglages
  de grain fin — `skip_ps`/`skip_vs` (diagnostics), `resolve_discard`, `skip_resolves`,
  `fetch_max_draws`, `coarse_shading` (seulement pour les cibles à ≥ 640 px de large, donc
  **inutilisable à 0.25 scene**) qui passent par les passes GPU, pas par le draw call.
- Les occlusion queries (`QueryCoverage::GuestSamples`) reçoivent le résultat du guest pour
  ses propres occlusion queries — pas un levier de cull possible.
- **LOD par distance = non-implémenté** : aucun mécanisme de type « distant part. / near part »
  n'est présent (pas de `MaterialFilterFloor` type distance, pas de point-LOD). Le seul
  levier de LOD est **le saut de mip** (`texture_min_lod`, déjà activé ultra/extra).

## 4. Leviers manuels à activer dans le jeu (test à faire)

| Levier | Défaut | Recommandation N100 |
|---|---|---|
| Scene scale (`gtav_native_render_scale` / préréglage N100 Ultra) | 1.0 | **0.25 (Ultra)**, 0.333 (Extra). 0.125 est possible mais non testé ici. |
| `gtav_native_fsr1` | off (Windows) | Oui (Ultra préréglé). |
| `gtav_native_max_anisotropy` | 16 | **1** (Ultra préréglé). |
| `gtav_native_contact_rays` | 0 | **0** (Ultra préréglé). |
| `gtav_native_reduce_fog` | off | On (Ultra préréglé). |
| `gtav_native_texture_recheck_frames` | 30 | 60–90 pour moins de hash CPU — **non testé**. |
| `gtav_native_pipeline_wait_ms` | 40.0 | 20–30 à tester si hitchs sur load natifs — non testé. |
| `gtav_native_hold_lost_frames` | true | garder — évite les frames trouées pendant la compilation pipeline. |
| `gtav_fps_limit` | 30 | **60** (le préréglage ne touche ni FPS ni Vsync, volontairement). |
| `gtav_native_vsync` / mode natif | on | VSync **off** avec 60 fps + `--vsync=false` (le swapchain). |

## 5. Checklist in-game (N100, à faire sur la machine du joueur)

1. Lancer le jeu en mode natif : `GTAV_NATIVE=on ./play.cmd` (ou `./play.sh` sous
   Linux) — ou équivalent option F1.
2. Dans F1 : préréglage **N100 Ultra** → appliquer, redémarrer.
3. FPS limit 60 (cvar `gtav_fps_limit=60`), Vsync off (`gtav_native_vsync=false` + `--vsync=false`).
4. Ouvrir un log de `native` : vérifier dans `native_stats`/`logs/native_dump/` que les
   stats `hash`/`drawn`/`skipped` sont proportionnelles (pas de perte de frames).
5. Si hitchs : réduire `pipeline_wait_ms` (ex. 30) et vérifier que les passes graphiques
   n'explosent pas en nombre — `--gtav_native_pass_gpu_times=true` trace les passes par
   type (logs every 300 frames).
6. A/B : `N100 Ultra` vs `Performance` (0.75 scene, 1 slot, FSR 1) pour voir l'écart 30–40% de scene scale
   sur le framerate.

## 6. Tests asset-free exécutés (cette session)

- `gtav/tests/graphics_policy_tests.cpp` — `TextureSampleFrames` pour
  Standard / Potato / Extra / Ultra + garde-fous (défaut utilisé, cvar explicit gardé,
  minimum 1). Compilé avec `g++ -std=c++20` (le bac n'a pas cmake) — **passe**.
- Aucun A/B in-game — voir la checklist 5.

## 7. Limites / non-réalisations

- Vulkan requis : le renderer natif cible Vulkan 1.3, le GPU N100 intégré suit le
  répertoire Vulkan 1.1+ d'Intel (vérifier la capacité exacte avec `vulkaninfo` sur la
  machine du joueur).
- Les préréglages ne modifient pas FPS/Vsync intentionnellement (choix de session volontaire).
- `texture_sample_frames` n'est pas exposé via le hub (cvar live) — pas de path du
  settings enum, juste le défaut de préréglage.
- Non validé visuellement : MIP skip + FSR1 à 0.25 sont
bon marché mais peuvent paraître pires à un cap > 60, à tester.
