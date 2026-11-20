# HANDOFF — validation du renderer natif (M7) pour la machine du joueur

> Destiné à la machine qui lance réellement le jeu (pas à la CI). Objectif : valider le
> renderer natif dans les scènes, avec le rythme et les flags documentés ici.
> Date : 2026-11-20 · Commit : `ad644a3` (tests + doc uniquement — aucune logique de rendu
> modifiée par ce commit).

## 0. État connus des travaux (repris de NEXT_SESSION_PLAN.md, 2026-10-25)

- On mode OK : `--gpu_skip_rendering=true` + `--gtav_native=on` ; le natif possède le
  swapchain ; la fenêtre du SDK est cachée (`SDL_HideWindow`, **non vérifié sur Wayland**).
- Cap FPS OK : `--gtav_fps_limit=30` (défaut) / `0` (uncapped) / `N`. Cap matériel : ~62 fps
  uncapped ; 60 à limit 60. Conclusion du journal : probablement un timer guest 60 Hz
  (KeSetTimer) qui rythme la boucle de update — non investigué.
- Prologue room : 5 scènes passent (player, guard, room) ; correspond à l'emulator.
- F2 debug overlay + F1 settings OK.

## 1. Commande de base

```sh
cd ~/gtav-linux/build/gtav   # ou build/gtav selon votre layout
export GTAV_NATIVE=on        # shadow | on | off — on = Plume posséde le swapchain
export GTAV_FPS=0          # 0 = uncapped, 30 = retail, N = à vous
export GTAV_CP_LAG=6       # lagms le command processor suit le CPU
./play.sh mon_log
# équivalent Windows : .\gtav_launcher.exe --play -- --gtav_native=on --gtav_fps_limit=0 --vsync=false --gtav_native_vsync=false
```

Flags de profil/debug utiles (passer après `--`) :
- `--gtav_native=shadow` — native rejoue en 2e fenêtre ; SDK affiche la fenêtre — pour comparer.
- `--gtav_native_dump_frame=N` — écrit logs/native_dump/frame_N (PNG de chaque resolve + commands.txt).
- `--gtav_native_trace_only=true` — F9 capture la frame en text (logs/native_trace/frame_*.txt).
- `--gtav_native_ab=setting=value` — alternance live (F9 pour forcer une capture).
- `--gtav_native_fps` — counter FPS on-screen (native).
- `--gpu_skip_rendering=true` — SDK ne rend plus (on mode).
- `--vsync=false --gtav_native_vsync=false` — désancre (uncapped).
- `--gtav_native_vsync` — vsync natif (revient par défaut ON).

Env : `GTAV_SHOT_DIR=logs/shots GTAV_SHOT_SECS=10` pour des PPMs périodiques ;
`GTAV_VULKAN_DEVICE=1` (RTX 4050) ; `GTAV_LOG_LEVEL=debug` pour debug.

## 2. Checklist de validation (dans l'ordre)

1. **Démarrage vers free roam** : `logs/<logname>.log`. Attendu : lignes `[native]` ~30 fps
   stable (ou ~62 si uncapped). Aucune frame avec 0 draws en cours de scène.
2. **Prologue + room** (la référence) : comparer la fenêtre native vs emulator (ou
   shadow mode en côte à côte). Le journal attend lit "logs/native91" comme correspondance.
3. **Shadow maps** (R16G16) : dans une scène avec ombres portées — non noir.
4. **HDR / lighting chain** (037+ non-black) : le journal attend "lumière non-zero".
   Inspection possible avec `--gtav_native_dump_frame=N` → logs/native_dump/frame_N/*.png.
5. **SSAO** : non noir (le bug SSAO résolu via RB_STENCILREFMASK = 0x210D / 0x210C).
6. **Minimap** : non-box noire (confirmé fixé au moment du journal).
7. **Exposure feedback CPU** : non invisible en clavier/mouse; les low-latency resolves
   (1x1) ne doivent pas laisser expo à 0.25 — surveiller les logs/native_dump.
8. **Modes non-prologue** : extérieur, eau, particules, cutscenes (à valider;
   le journal note des pauses aux cutscenes).
9. **FPS cap** : `--gtav_fps_limit=30` → ~30 fps stable ; `0` → ~62 (ou cap trouvé).
   Déterminer si le cap 60 Hz guest (KeSetTimer) limite toujours.
10. **Fenêtre SDK hidden** : en on mode, vérifier qu'on ne voit que la fenêtre native.
    Wayland non vérifié → tester et signaler.

## 3. Outils de capture / comparaison

- `--gtav_native=shadow` : PPMs PPMs côte à côte via `GTAV_SHOT_DIR` (voir aussi le journal
  `logs/native_trace/` pour le texte).
- `--gtav_native_dump_frame=N` → `logs/native_dump/frame_N/` (tous les resolves +
  `commands.txt` avec état par draw).
- `tools/native/compare_values.py` — compare N résouts (native vs emulated) — nécessite
  une capture emulator (`--emu_resolve_dump_frame=F --emu_draw_dump_window=K` avec
  `--readback_resolve=full` → logs/emu_dump/frame_*/).
- `tools/native/compare_dump.py`, `compare_constants.py`, `compare_raw.py` — comparaisons
  de draws/constants.
- F9 / F10 dans la fenêtre : F9 = dump frame, F10 = screenshot.
- `--gtav_native_ab=setting=value` — alterne les settings 60 frames par alternance (pour
  A/B brouillard réduit etc.).

## 4. Si quoi que ce soit échoue

Joignez : le log (`logs/<logname>.log`), éventuellement `--gtav_native_trace_only=true`
(logs/native_trace/frame_*.txt), le hardware (GPU, driver, fenêtre SDL) et la capture
`--gtav_native_dump_frame`. Ne pas commiter logs/native_dump (data game-derived).

## 5. Ce qu'on sait ne PAS avoir changé (à ne pas re-tester)

- Aucun fichier contenant le jeu (ISO, XEX, XEXP, RPF, dlls) ne doit être créé,
  commité, ni copié. Éviter de commiter `data/`, `logs/`, `derive/native/`.
- Les commits de cette session (cd38020, ad644a3) ne contiennent que des tests et docs
  — aucune logique de rendu n'a été modifiée. Toute régression est inattendue et devrait
  être signalée comme un bug de fond.
