# Analyse & documentation — dépôt Hooky GTA Recomp

> Document de travail pour le 2e agent (et pour quiconque s'y connecte) : cartographie
> complète du dépôt, architecture, flux de build, état des travaux, repères de navigation.
> Synthèse de l'analyse réalisée le 2026-11-20 sur le commit `7cf544cb29e6674357a3b5ca421d5de73bdc443d`
> (branche `main`, fusion du PR n°14). Aucun fichier de jeu, aucune donnée protégée n'a été
> consulté ni ajouté.

---

## 1. Ce que fait ce projet

**Hooky GTA Recomp** recompile *Grand Theft Auto V* (Xbox 360, TU27) depuis son exécutable
PowerPC : le code est traduit en C++ (~811 fichiers générés par ReXGlue), recompilé en native
(clang + lld), et exécuté sur un runtime Xbox 360 (ReXGlue SDK v0.10.0) avec un renderer
Vulkan (« native renderer », plume) qui rejoue les commandes D3D/PM4 de la console.
Le but : jouer à GTA V sur PC (Windows x64, Linux x64, macOS arm64) et, expérimentalement,
sur iPhone et Android.

**Règle d'or (clean-room)** : le dépôt ne contient *jamais* de fichier du jeu — ni ISO,
ni XEX/XEXP, ni RPF, ni DLL/SO/dylib du jeu, ni C++ généré, ni shaders traduits. Chaque
utilisateur compile sa propre copie depuis ses disques. Voir « Keeping it clean »
(`README.md`) et `docs/RELEASE_CLEANROOM.md`.

**Fork** : ce dépôt est le fork `NuxsGit/hooky-GTA-Recomp` du projet upstream `andrewnakas/gtav-recomp`.
Il ajoute : un lanceur/installateur Windows complet, le mode **Desktop Workshop**
(Sandbox solo / privé / Campaign), des builds mobiles, un renderer natif avancé
(SSAO, contacts rays, FSR1, post FX), et un outillage de release (tirage stable/nightly).

---

## 2. En un coup d'œil (état du commit analysé)

| Indicateur | Valeur |
|---|---|
| Fichiers suivis | ~400 (7,7 Mio) |
| Code C++ / Java / Objective-C | 186 fichiers (~80 650 lignes sources, non-documentation) |
| Documentation | 29 fichiers Markdown, ~3 900 lignes (`docs/`) |
| Tests | 26 fichiers de tests C++ (loader + gtav) |
| Manifeste d'entrées | `gtav/gtav_manifest.toml` (TU27, 438 lignes) + base v9 (139 lignes) |
| Patches aux upstream | 7 fichiers .patch (~405 ko) |
| CI | 2 workflows GitHub (`.github/workflows/`) |
| Loader | `loader/VERSION = 0.8.8`, `loader/GAME_ABI = 1` |
| Licence | GPL-3.0-or-later (`LICENSE`) |

---

## 3. Architecture en couches

```
 ┌────────────────────────────────────────────────────────────────┐
 │  release / packaging          tools/release/**                 │  packagers, scan de fuite
 │  ├─ Windows ZIP + source  .github/workflows/windows-candidate  │
 │  └─ Linux/macOS builds     tools/release/make_release_*.py     │
 ├────────────────────────────────────────────────────────────────┤
 │  lanceur / installateur    loader/**  (gtav_launcher)          │  Win32 / GTK3-term / AppKit
 │  ├─ install (discs+TU27)   loader/src/install.cpp + sources.cpp │  hash, copie, codegen, build
 │  ├─ mise à jour Stable/Nightly update.cpp / windows_update.cpp │  GitHub releases, atomique
 │  └─ tests & CI              loader/tests/**  + CI workflow     │
 ├────────────────────────────────────────────────────────────────┤
 │  gtav (hôte du jeu)        gtav/**  (gtav_fast / gtav_game)    │
 │  ├─ main.cpp + gtav_app.h  rex::ReXApp (hooks ReXGlue)         │
 │  ├─ core/**                Workshop (sessions, permissions,    │
 │  │                         player registry, spawn queue)       │
 │  ├─ sandbox/**             mode Campaign/Sandbox/private       │
 │  ├─ frontend/**            hub « Mode », assets, modes.h       │
 │  ├─ native/**              RENDERER NATIF (Plume, D3D hooks,..) │
 │  ├─ phone/**               AdminCheatApp (panel F5 Admin)      │
 │  └─ fs/**                  headers freestanding du DLL du jeu  │
 ├────────────────────────────────────────────────────────────────┤
 │  codegen (ReXGlue)         gtav_manifest.toml + partition      │  811 fichiers .cpp générés
 │  └─ compilé par LLVM       → gtav_game.dll (DLL/SO/dylib)      │  clang + lld, fs/ headers
 ├────────────────────────────────────────────────────────────────┤
 │  upstream (patchés)        rexglue-sdk (Linux/Win patches)      │
 │                            XenosRecomp (traducteur shaders)     │
 │                            plume (renderer Vulkan)              │
 │                            + sdl3, ffmpeg, LLVM (bundled)      │
 └────────────────────────────────────────────────────────────────┘
```

Le jeu n'existe pas dans ce dépôt : il est **généré à la première installation** (codegen)
et **compilé à chaque install** par le loader, depuis les disques de l'utilisateur.
Le dépôt ne livre que l'hôte (`gtav_fast`), le launcher, le runtime, les outils de
codegen et les shims mobiles (sans aucune donnée de jeu).

---

## 4. Cartographie dossier par dossier

### 4.1 `gtav/` — le projet du jeu (~40 000 lignes, le cœur actif)

| Fichier / dossier | Rôle |
|---|---|
| `CMakeLists.txt` | Build Win/Linux/macOS/iOS/Android ; options `GTAV_GAME_DLL` (DLL séparée au runtime), `GTAV_DESKTOP_WORKSHOP`, phone targets |
| `CMakePresets.json` | Presets Debug/Release/RelWithDebInfo × (windows/linux/mac) × (amd64/arm64) |
| `gtav_manifest.toml` | Points d'entrée du codegen (TU27) — **438 lignes**, `#` CRLF connu (voir §10) |
| `gtav_manifest.base_v9.toml` | Manifeste v9 (build de base, base du TU) — 139 lignes |
| `WorkshopCore.cmake` | CMake pour les moteurs « Workshop » (core + tests) |
| `src/main.cpp` | Point d'entrée → `REX_DEFINE_APP(gtav, GtavApp::Create)` ; `#if GTAV_GAME_DLL` → image config exportée par la DLL |
| `src/gtav_app.h` | `GtavApp : rex::ReXApp` (hooks : fenêtre, dtor, FPS, shots, dumps, phone setup) |
| `src/fs/` | Headers freestanding avec lesquels la DLL est compilée : `gtav_pch.h` (généré), `fs_crt.cpp` (memcpy/memset/memcmp), `malloc.h`/`stdlib.h` shims, `layout_check.h` (static_asserts PPCContext / PPCImageInfo) |
| `src/core/` | État de Workshop partagé : `WorkshopCore` (sessions, layout), `GamemodeManager`, `PermissionManager`, `PlayerRegistry`, `EntitySpawnQueue`, `CheatController`, `WorkshopGame.h`/`WorkshopTypes.h` |
| `src/frontend/` | Hub « Choisir le mode » (`play_hub.cpp` ~720 lignes, `modes.h`), assets embarqués (Barlow, PNG) via `embed_assets.py` |
| `src/native/` | **Renderer natif** — voir §6 |
| `src/phone/` | `AdminCheatApp.cpp` (menu cheats / Admin, panel F5 dédié) |
| `src/sandbox/` | Modes Campaign/Sandbox/Private : `desktop.cpp` (hub → configure), `session.cpp` (hôte/invitation TCP), `world.cpp`, `scene.cpp`, `protocol.cpp`, `courses.cpp`, `campaign.cpp`, `BuildManager.cpp`, `Tu27Workshop.cpp`, `BuildMenuUI.cpp`, `CreativeRuntime.cpp` |
| `src/utils/ConfigStorage.cpp` | Persistance de config (settings localisées) |
| `tests/` | 14 tests CI (voir §8) |

### 4.2 `loader/` — lanceur / installateur (~10 500 lignes)

| Fichier | Rôle |
|---|---|
| `src/install.cpp` | **Setup central** (~2547 lignes, retenu par tous les OS) : vérifie discs ISO/dossiers + TU27 (XDVDFS, `sources.cpp`), copie dans `data/` avec hachage vs `expected.txt`, appelle le codegen (`rexgluerd codegen`), compile 407 TUs (clang + lld, **fs/**), lie `gtav_game.dll/.so/.dylib`, écrit `installed.txt` |
| `src/sources.cpp` | Lecteur XDVDFS / ISO (Disc 1+2), TU27 en `tu00000009_00000000` ou dossier `disc002/` |
| `src/main.cpp` | **Windows** : fenêtre Win32 + `install.cpp`, channel Stable/Nightly (`windows_update.cpp` — GitHub releases, rollback atomique, `--no-auto-update`) |
| `src/main_linux.cpp` | **Linux** : GTK 3 (via dlopen) + fallback terminal ; politique d'install (`--setup`, `--play`, `--check-updates`) |
| `src/main_mac.mm` | **macOS** : `GTAV.app` AppKit ; `--setup`, gestion des migrations |
| `src/update.{cpp,h}` | Mises à jour Stable/Nightly (Linux/mac), téléchargement + vérif SHA-256 + atomicité |
| `src/windows_update.cpp` | Stable/Nightly Windows ZIP (`.zip` vs `expected.txt`), channels pinned au repo `NuxsGit/hooky-GTA-Recomp` |
| `src/campaign_config.cpp`, `desktop_home.cpp` | Session/home directory « desktop home », migration portable → `%USERPROFILE%\Games\GTAV Recomp` |
| `src/setup_lock.h` | Lock d'install atomique |
| `VERSION` / `GAME_ABI` | `0.8.8` / `1` — bump GAME_ABI quand le DLL ABI change (sinon rebuild du jeu forcé) |
| `tests/` | 9 tests : `desktop_home_tests`, `runtime_policy_tests`, `sandbox_tests`, `update_tests`, `workshop_*`, + `rebuild_test.py` (fixture complète fake-data, Linux x64) |

### 4.3 `tools/` — dev & release (~50 000 lignes, dont beaucoup de .txt de données)

| Dossier | Contenu |
|---|---|
| `tools/game_dll/` | `build_game_dll.py` (215 lignes, compile + lier la DLL locale), `gen_interface.py` (188 lignes, interface EXE↔DLL : 2930 fonctions hookées déclarées dans exe, exportées par DLL ; `game_bridge.cpp` = 9 `gtavfs_*`), `codegen.partition.json` (amorce de split DLL stable), `libSystem*.tbd` (iOS) |
| `tools/release/` | Packagers Windows (`make_release.py` + `build_windows_candidate.py`), Linux (`make_release_linux.py` + `build_release_linux.sh`), macOS (`make_release_macos.py` + `build_release_macos.sh`), `taint_scan.py` (32 octets content-defined vs disque), `expected.txt` (taille/hachages), `compat/` (Docker Linux portable), `test_windows_release.py` |
| `tools/native/` | **Diagnostic du renderer natif** : `compare_*.py` (dump vs emulator), `shader_census.py` (3617 shaders), `genasm.py`/`fnsim.py` (fonctions D3D par empreinte), `gen_probe.py` (générateur d'ABI), `ucode_disasm.cpp`, `bench.sh`, `headless_run.sh`, `metal_pass_bench.mm` |
| `tools/android/` | Shell Android : `build_apk.sh`, `build_game.sh`, `build_llvm.sh`, `build_sdk.sh`, `make_shell.sh`, `check_shader_fix.sh`, `shader_fix/` (cli C++), `lower_int64.py`/`lower_header.py` (ABI), `symbolize_crash.py`, `ubo_constants.py`, `GameActivity.java`/`SetupActivity.java` |
| `tools/ios/` | Shell iPhone : `make_app.sh`/`make_shell.sh`/`make_spirv.sh`, `deploy.sh`, `install_ipa_dev.sh`, `Info.plist` |
| `tools/scripttools/` | Outillage de rétro-ingénierie des scripts (GTA) : `scan.c`/`bk.c`/`dis.py`/`jo.py`/`gm.py`/`st.py`/`strs.py`/`off2g.py`, adresses/natives (`fn2name.txt`, `ptrs.txt`, `handlers.txt`, `buckets.txt`, `pro1.natives`) |
| `tools/input_tests/` | Tests de pads SDL (USB/Bluetooth, README) |
| `tools/old_traces/` | Traçage temporaire cutscene (gel de lecture) — gardé hors SVN |
| `tools/fn_match.py` | Base → TU27 : comparaison de fonctions par empreinte (fn handshake) |

### 4.4 `scripts/` — manifeste & extraction (659 lignes)

| Script | Rôle |
|---|---|
| `derive_entries.py` | Génère le manifeste suivant (stubs orphelins, `find_ptr_targets`, `find_hidden_entries`), append au manifest |
| `find_ptr_targets.py` / `normalize_manifest.py` / `find_hidden_entries.py` | Analyse des entrées manquantes / nettoyage de manifest |
| `extract_basefile.py` | Extraction du base file (v9) |
| `stfs_extract.py` | Extraction STFS (install packages Disc 1) |
| `xiso.py` / `xiso_adb.py` | ISO XDVDFS + lecture via ADB |

### 4.5 `docs/` — documentation (29 fichiers, ~3 900 lignes)

| Fichier | Contenu |
|---|---|
| `RELEASE_CLEANROOM.md` (381) | Build/release Windows, scan de fuite, scrub d'historique (2026-10-29), procédure Linux |
| `RELEASE_v0.3.0.md` | Notes de release v0.3.0 (build Windows) |
| `d3d_map.md` (71) | Map D3D XDK TU27 (adresses, layout CDevice) — **référence renderer natif** |
| `SANDBOX.md` (127) / `WORKSHOP_CREATIVE.md` (129) | Desktop Workshop : modes, Build overlay, AdminOS panel |
| `WINDOWS_GRAPHICS.md` (267) / `WINDOWS_PERF_2026-10-29.md` | Post FX, FSR1, contact rays, fog, perf Windows |
| `GRAPHICS_REALISM.md` (105) | Options F1 (material filter, rays) |
| `DEBUG_AND_UPDATES.md` (85) | F2 overlay, story tools, tests lanceur/mise à jour |
| `DESKTOP_INSTALL.md` (109) | Installation desktop, migration portable → Games |
| `CAMPAIGN_COOP.md` (78) | Campaign co-op : consensus limité, unavailable |
| `ANDROID_STATUS.md` (154) / `IOS_PORT.md` (217) | Ports mobiles (état, validation, déploiement) |
| `HANDOFF_*.md` (6 fichiers) | Handoffs inter-sessions par OS (Linux 10-27, Mac 10-29, Mac 10-30 ×2, Windows 10-29, Windows ↔ Mac) |
| `NEXT_SESSION_*.md` (3 fichiers) | Prochaines sessions : 10-28, 10-29, Android |
| `GRAPHIQUES_BROUILLARD_AB.md` (85) | A/B brouillard réduit (FR) |
| `ARM64_NOTES_FOR_ANDROID.md` (157) | Points clés Android ARM64 |
| `TREE_ORBS_INVESTIGATION.md` (117) | Diagnostic arbre/orb (F11) |
| `TEMPORAL_UPSCALING.md` (81) | Spatial upscaling (FSR1) |
| `MAC_THEN_ANDROID_PLAN.md` (151) | Plan Mac → Android |
| `HANDOFF_ANDROID_PC_BUILD.md` (161) / `HANDOFF_WINDOWS_PHONES.md` (81) | Builds mobiles depuis PC |
| `HANDOFF_WINDOWS_MAC_2026-10-29.md` (92) | Handoff Windows ↔ Mac |
| `HANDOFF_LINUX_2026-10-27.md` (211) | Handoff Linux (dernière session sur ce poste) |
| `HANDOFF_WINDOWS_MAC.md` (156) | Handoff Windows ↔ Mac |
| `NEXT_SESSION_ANDROID.md` (51) | Session Android |
| `SANDBOX.md` | voir ci-dessus |

### 4.6 Racine — patches et scripts de dev

| Fichier | Rôle |
|---|---|
| `rexglue-sdk-gtav-linux.patch` (195 ko) | Patch SDK v0.10.0 (Linux) — SDL DLL shared, XMA, kernel, etc. |
| `rexglue-sdk-gtav-windows.patch` (42 ko) | Patch SDK (Windows) — SDL3 shared, D3D12, etc. |
| `xenosrecomp-gtav.patch` (46 ko) / `xenosrecomp-win-before-linux-1029.patch` (39 ko) | XenosRecomp (traducteur shaders) — textures 0-31, FETCH, dedup, literal lists |
| `plume-gtav.patch` (64 ko) | Plume (renderer Vulkan) — multi-sample, mip views, addressable buffers, etc. |
| `sdl3-gtav.patch` (2 ko) | SDL3 patchers |
| `ffmpeg-gtav.patch` (2 ko) | FFmpeg (runtime) |
| `derive_gaps.toml` (32) / `derive_ptr.toml` (562) | Entrées manquantes dérivées (scripts) — **données de développement**, pas livrables |
| `derive/manifest_try.toml` (437) | Scratch local de dérivation (non versionné par convention) |
| `play.sh` / `play_mac.sh` / `play.cmd` / `play.ps1` / `play_compare.cmd` | Lance l'exécutable TU27 + options (native renderer ON, fps, logs) |
| `build.cmd` / `build_launcher.cmd` / `build_release_exe.cmd` / `build_sdk.cmd` / `build_xr.cmd` / `codegen.cmd` | Scripts de build dev Windows (chemins hardcodés `E:\gtav-rex`) |
| `audio_capture.sh/.cmd` | Audio capture |
| `hangdump.sh` / `night_*.sh` / `play_mac.sh` | Outillage nocturn (perf, comparação) |
| `NEXT_SESSION_PLAN.md` (18 ko) | Journal des sessions Linux : état natif renderer, bugs, TODO |

---

## 5. Le pipeline complet

```
 Disc 1+2 (ISO/dossier) + TU27
        │
        ▼  loader/src/sources.cpp (XDVDFS)
  Passe de vérification (hashes vs expected.txt)
        │
        ▼  loader/src/install.cpp
  data/game + data/update + data/user
        │
        ▼  codegen (rexgluerd codegen) — manifest TU27
  811 fichiers générés (~407 TUs) — hashes vs expected.txt
        │
        ▼  clang + lld (freestanding, fs/ headers)
  gtav_game.dll / .so / .dylib          ← build.cmd / tools/game_dll/build_game_dll.py
        │
        ▼  gtav_fast.exe (GTAV_GAME_DLL=ON) — exe + DLL
  gtav.exe + runtime ReXGlue + renderer native
        │
        ▼  F1 → acquérir shaders (XenosRecomp + dxc, data/spirv)
  jeu jouable
```

**Release** : `tools/release/` → packagers par OS → scan de fuite (`taint_scan.py`) → ZIP/tar.gz
→ `.github/workflows/windows-candidate.yml` (nightly + draft staging + source archive).

**Phones** : `tools/ios/make_shell.sh` (ipa) / `tools/android/make_shell.sh` (apk) —
shaders + pack générés côté PC, livrés avec le shell. Le téléphone ne contient que l'app.

---

## 6. Renderer natif (le cœur des travaux récents)

Référence détaillée : `docs/d3d_map.md` (XDK D3D TU27) + `NEXT_SESSION_PLAN.md`.

Le renderer natif **remplace l'émulation de rendu** du SDK par un rendu Vulkan/Metal
direct via *plume*. Les hooks intercepte les appels D3D de la console dans le runtime
ReXGlue via `gtav/src/native/d3d_hooks.cpp`, capture chaque frame dans `capture.cpp`,
et un thread de rendu (`renderer.cpp`) rejoue le frame packet via `backend.cpp`.

Composantes principales (~15 400 lignes dans `src/native/`) :

| Composant | Rôle |
|---|---|
| `capture.cpp` (~1366) | Hooks D3D/PM4 : retro-capture des draws, constants, resolves (`--gtav_native=on|shadow|off`) |
| `renderer.cpp` (~1808) | Thread de rendu dédié, initialisation plume |
| `backend.cpp` (**6810**) | Replay des draws : pipelines, RT, resolves, textures, ordering, batches, MSAA, depth pack, MSAA mip |
| `shaders.cpp` (~181) | Chargement / compilation partagée des shaders guest → SPIR-V (via XenosRecomp) |
| `shader_compiler.cpp` | Compile une shader XenosRecomp au premier usage, cache `data/spirv` (~3617 shaders/load) |
| `textures.cpp` (~574) | Décodeur textures guest (port Skate 3) + copie/flush |
| `deferred_list.cpp` (~693) | Batches deferred (indirect draws) |
| `d3d_hooks.cpp` (~370) | Hooks SetPending / SetPixelShader / BeginVertices / etc. |
| `menu.cpp` (~424) | F1 Settings overlay (presets, Memory, Display, Realism, Performance) |
| `graphics_menu_ui.cpp` | Rendu de l'UI F1 (ImGui), prefs persistence |
| `input.cpp` + `touch_controls.cpp` | SDL input (pads/clavier/tactile) + gating (F2, F6, F8, F1) |
| `write_watch.cpp` (~508) | Suivi des écritures guest (debug) |
| `stats.cpp` | Compteurs (draws/frame, GPU W) |
| `frame.h` / `frame_trace.h` | Structures/diagnostic par frame |
| `resolve_policy.h`, `pipeline_cache_io.h`, `pipeline_completion.h` | Policy resolve, cache pipelines (`vk_pipelines.bin`) |
| `adreno_shader_fix.*` | Correctifs Adreno (Android) |
| `fog_filter.h` / `contact_rays.h` / `shader_compiler.h` / `tu27_call.h` / `vertex_layout.h` / `xenos_regs.h` / `upload_arena.h` / `resolve_pm4.h` | Helpers internes |
| `shaders/` | HLSL : `native_blit.hlsl`, `native_clear.frag`, `native_memory.hlsl`, `native_menu.hlsl`, `native_rectlist.hlsl`, `native_contact_rays.hlsl`, `native_fsr1.hlsl`, `native_resolve_compute.comp`, `native_resolve_fetch.*`, `fsr1/` (AMD ffx_a/fsr1) |
| Outillage debug | `write_watch`, `d3d_probe.cpp` (probe de fonctions D3D — 2 896 fonctions), `--gtav_native_trace_only=true`, `--gtav_native_dump_frame=N` |

Options temps-réel notées en `NEXT_SESSION_PLAN.md` :
- **On mode** : `--gpu_skip_rendering=true` → SDK ne rend plus, le renderer natif possède le swapchain.
- **FPS cap** : `gtav_fps_limit=0|N|30` + `--vsync` / `--gtav_native_vsync`.
- **Frame capture** : PPMs via `GTAV_SHOT_DIR` / `GTAV_SHOT_SECS`.
- **Shadow → On** : shadow mode (native + SDK en 2e fenêtre) puis on mode.

Le backend doit gérer : resolves (rect, MSAA, cube, mip-chains), vertex buffers (inline, indexed, indirect),
runtime writes (RESOLVED memory via `native_memory.hlsl`), etc.

---

## 7. Desktop Workshop / multi-mode

Deux faces distinctes :

### 7.1 Hub « Mode » (`gtav/src/frontend/`)
Avant le boot du guest, `play_hub.cpp` affiche les cartes « Campaign solo », « Sandbox solo »,
« Private session » (Campaign co-op affiché mais indisponible — pas de sync). Choix par souris
/ clavier / D-pad. Deux profils Sandbox séparés ; F6 pour checkpoint/build/minigames/host.

### 7.2 Sandbox / Workshop (`gtav/src/sandbox/` + `gtav/src/core/`)
Modes Campaign/Sandbox/Private, mode Private = session TCP multi-joueur (`session.cpp`, `protocol.cpp`).
« Build overlay » F8 et « AdminOS » panel dédié F5 (voir `docs/WORKSHOP_CREATIVE.md`). Le téléphone base GTA (P/Haut) est restauré, non intercepted.
`GTAV_DESKTOP_WORKSHOP=ON` compile ce code ; sinon `core/sandbox/phone` sont absents.

---

## 8. Tests & CI

### 8.1 Tests — `gtav/tests/` (14 tests, CI)

| Test | File | Ce qu'il couvre |
|---|---|---|
| `input_pacing_gate` | `input_pacing_tests.cpp` | Gate de pacing input |
| `presentation_policy` | `presentation_tests.cpp` | Politique présentation (F1 Display) |
| `graphics_preset_policy` | `graphics_policy_tests.cpp` | Préréglages presets Ultra/potato/standard |
| `contact_rays_policy` | `contact_rays_tests.cpp` | Contact rays : politique + shader |
| `native_upload_arena` | `upload_arena_tests.cpp` | Arena upload Vulkan |
| `fog_strip_filter` | `fog_filter_tests.cpp` | Fog strip reduce (F1 Realism) |
| `pipeline_cache_io` | `pipeline_cache_tests.cpp` | Cache pipelines `vk_pipelines.bin` |
| `resolve_replay_policy` | `resolve_tests.cpp` | Politique resolve (rect, MSAA, mip) |
| `graphics_preferences` | `graphics_preferences_tests.cpp` | Persistance / changement live des préférences |
| `pipeline_completion` | `pipeline_completion_tests.cpp` | Concurrence Wait/Signal/budget (**nouveau**) |
| `byteswap_simd` | `byteswap_simd_tests.cpp` | Swap16Bytes unit=2/4 (**nouveau**) |
| `frame_data_hash` | `frame_hash_tests.cpp` | DrawDataHash + ByteArena (**nouveau**) |
| `safe_read` | `safe_read_tests.cpp` | SafeReadMemory (**nouveau**, non-Windows) |
| `guest_texture_layout` | `guest_texture_tests.cpp` | Décodeur textures guest (SDK + Plume) |
| `frame_trace` | `frame_trace_tests.cpp` | Trace frame |
| `workshop_overlay` | `workshop_overlay_tests.cpp` | F8 Build overlay / Admin OS imgui |
| `graphics_menu` | `graphics_menu_tests.cpp` | F1 overlay (ImGui) |
| `play_hub` | `play_hub_tests.cpp` | Hub de mode (SDL injection) |

`gtav/tests/CMakeLists.txt` utilise des `GTAV_IMGUI_SOURCE` / `GTAV_TEXTURE_SDK_SOURCE` /
`GTAV_TEXTURE_PLUME_SOURCE` / `GTAV_FMT_SOURCE` opcionaux (sinon certains tests sont exclus).

### 8.2 Tests — `loader/tests/` (9 tests)

`desktop_home_tests.cpp`, `runtime_policy_tests.cpp`, `sandbox_tests.cpp`, `update_tests.cpp`,
`update_transport_stub.cpp`, `windows_update_tests.cpp`, `workshop_core_tests.cpp`,
`workshop_fake_game.h` + `workshop_runtime_tests.cpp`, `rebuild_test.py`
(installeur fake-data, Linux x64, AVX2, ≥3 Go disque tmp).

### 8.3 CI (`.github/workflows/`)

- `desktop-launcher.yml` — sur `main`/`arena/**` push (paths loader/gtav/tests…) :
  Ubuntu+macOS : cmake+ctest pour le lanceur et le policy graphics.
- `windows-candidate.yml` — schedule 03:17 UTC + push `arena/**` :
  Metadata (channel stable/nightly, serial) → build Windows x64 (LLVM 19.1.5, SDL3 shared,
  SDK RelWithDebInfo) → source archive → draft staging/publish nightly.

**Aucune CI n'exécute le jeu.** Les tests sont tous asset-free (pas de retail data).

---

## 9. Règles non négociables (clean-room)

Voir `docs/RELEASE_CLEANROOM.md` et `README.md § Keeping it clean`.

- **Jamais** dans le dépôt : ISO/folders, XEX/XEXP, RPF, assets extraits, manifest du jeu
  en tant que contenu, gtav_game.dll/so/dylib, shaders traduits, data/, saves, logs avec
  mémoire de jeu, GTAV.ipa, Android pack.
- Manifeste + partition + expected.txt : **nos analyses** (adresses/sizes), ok à commit.
- Shader traduits / HLSL générés : **jamais** — history re-scrubbed le 2026-10-29 → clones
  pré-scrub interdits de push/merge.
- Ne pas utiliser le leaked Rockstar source, même pour noms.

---

## 10. Points de vigilance / état actuel

- **Manifeste TU27** : `gtav/gtav_manifest.toml` a CRLF (noter dans `.gitattributes`).
- **Branches Windows native** : la branche `win-native` (pas ici) a été scrubbed d'un shader
  traduit le 2026-10-29 (ps_D258EBE32001470C_tiles.hlsl). Tout commit d'avant scrub est à
  éviter. Vérifier `git log --all -- '*ps_D258EBE32001470C*'` n'imprime rien.
- `NEXT_SESSION_PLAN.md` (dernière actualisation 2026-10-25) cite : on-mode natif OK,
  frame cap OK, `~62 fps` uncapped, mais scene skinning + outdoors à valider. M6/M7/M8 en cours.
- `derive_ptr.toml` / `derive_gaps.toml` : données de dev — nettoyage de manifest possible.
- Tests `gtav/tests` nécessitent SDK + plume sources optionnels ; ne pas forcer en CI.

---

## 11. Guide pour le 2e agent

### 11.1 Ordre de lecture recommandé
1. `README.md` (install vs modif, pas de game)
2. `docs/RELEASE_CLEANROOM.md` (règles + scrub d'historique)
3. `NEXT_SESSION_PLAN.md` (état + TODO du renderer natif)
4. `docs/d3d_map.md` (renderer natif)
5. `docs/SANDBOX.md` + `docs/WORKSHOP_CREATIVE.md` (mode hub & Workshop)
6. `loader/src/install.cpp` + `loader/src/update.cpp` (flux d'install)
7. `gtav/src/gtav_app.h` + `gtav/src/native/renderer.h` + `backend.h` (renderer natif)

### 11.2 Comment travailler ici
- **Branche de travail** : ne pas toucher à `main`. Travail = commits sur la branche arena
  de départ de la session. Ne pas push sans demande explicite.
- **Tests non-retail** :
  ```sh
  cmake -S loader -B build/launcher-test -DGTAV_LAUNCHER_TESTS=ON
  cmake --build build/launcher-test
  ctest --test-dir build/launcher-test --output-on-failure
  # et tests graphics:
  cmake -S gtav/tests -B build/graphics-policy -DCMAKE_BUILD_TYPE=Release
  cmake --build build/graphics-policy
  ctest --test-dir build/graphics-policy --output-on-failure
  ```
- **Jamais de game data** dans un commit, même temporairement.
- **Manifeste** : toujours valider avec `tools/fn_match.py` / scripts `find_*` avant release.
- Docs inter-sessions : HANDOFF_* / NEXT_SESSION_* conventions. Le 2e agent doit lire la
  dernière handoff de son OS avant de s'engager.

### 11.3 Ce qu'il ne faut PAS oublier
- Renderer natif : backend.cpp (6810 lignes) est le plus gros fichier du repo. Le toucher
  avec prudence.
- `loader/src/install.cpp` est le pont entre discs, codegen, build et Play.
- `tools/release/expected.txt` : hachages connus-goûts — jamais éditer sans décision.
- Les `.patch` aux upstream sont les seules modifs de SDK — garder minimal.
- CI windows-candidate.yml : ne pas push nightly sans confirmation.

---

## 12. Glossaire rapide

- **TU27** : Title Update 27 du GTA V Xbox 360 — version cible (TU file `tu00000009_00000000`).
- **ReXGlue SDK v0.10.0** : runtime Xbox 360 (kernel, fs, audio) + codegen de compiling.
- **gtav_game.dll/.so/.dylib** : la DLL du jeu compilée à la volée (jamais dans le dépôt).
- **Plume** : renderer Vulkan (fork avec patches).
- **XenosRecomp** : traducteur de shaders HLSL → SPIR-V.
- **TU27 manifest** : liste d'entrées (adresses, tailles, stubs) pour le codegen.
- **F1/F2/F8** : overlay F1 settings, F2 debug, F8 build (Workshop).
- **Shadow/on modes** : native renderer shadow (comparable) vs on (remplace le SDK).
- **GTAV_DESKTOP_WORKSHOP** : define CMake pour le Workshop (sandbox/creative overlay).

---

## 13. Annexe — tailles des fichiers les plus gros

| Fichier | Lignes |
|---|---|
| `gtav/src/native/backend.cpp` | 6810 |
| `gtav/src/native/shaders/fsr1/ffx_a.h` | 2656 |
| `loader/src/install.cpp` | 2547 |
| `gtav/src/native/renderer.cpp` | 1808 |
| `gtav/src/native/capture.cpp` | 1366 |
| `loader/src/main.cpp` | 1358 |
| `gtav/src/native/shaders/fsr1/ffx_fsr1.h` | 1199 |
| `gtav/fs/gtav_pch.h` (freestanding) | 1035 |
| `loader/src/main_linux.cpp` | 897 |
| `gtav/src/debug_tools.cpp` | 819 |
| `gtav/src/frontend/play_hub.cpp` | 723 |
| `loader/src/main_mac.mm` | 628 |
| `loader/src/sources.cpp` | 589 |
| `gtav/src/native/adreno_shader_fix.cpp` | 589 |
| `gtav/src/native/textures.cpp` | 574 |
| `gtav/src/sandbox/session.cpp` | 680 |
| `gtav/src/native/write_watch.cpp` | 508 |
| `gtav/src/core/WorkshopCore.cpp` | 484 |
| `gtav/src/sandbox/desktop.cpp` | 485 |
| `loader/src/windows_update.cpp` | 465 |
