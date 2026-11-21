# Renderer natif — analyse approfondie (M6/M7/M8)

> Document de travail pour le 2e agent. Complète `docs/d3d_map.md` (adresses XDK) et
> `NEXT_SESSION_PLAN.md` (journal de session Linux). Rédigé le 2026-11-20 sur le commit
> `7cf544cb29e6674357a3b5ca421d5de73bdc443d`, **sans données de jeu ni console** :
> tout ce qui dépend du rendu in-game est repris des journaux/docs, pas re-validé ici.

## 1. Vue d'ensemble du pipeline

```
 guest threads (runtime ReXGlue)
    │  hooks d3d_hooks.cpp (SetPending, SetShader, Begin/EndVertices, Draw, Swap…)
    ▼
 capture.cpp  ── OnDrawIndexed / OnDraw / OnResolve / …
    │   lit le register-shadow du D3D device (d3d_map.h)
    │   construit un FramePacket : Commands[], DrawCommand[], ClearCommand[],
    │                             ResolveCommand[], QueryCommand[], ByteArena
    ▼
 renderer.cpp (RenderThread) — Submit() / TakeSpare() — backed par une file bornée
    │   backpressure : quand la file est pleine (queue limit), le guest y attend
    ▼
 backend.cpp (Backend::Execute) — replay Plume
    │   pipelines, RT, resolves, textures, vertex packing, constants, depth, blits
    ▼
 plume (Vulkan / Metal) → fenêtre native   +   SDK swap faké (--gpu_skip_rendering=true)
```

- **Mode off** : aucun capture/hook (défaut). **Shadow** : SDK affiche la fenêtre, le natif
  rejoue dans une 2e fenêtre (comparaison visuelle). **On** : le SDK ne rend plus
  (`--gpu_skip_rendering=true`), le natif possède le swapchain.
- Le thread de rendu est **unique** et détient tous les objets plume. Le guest n'écrit que
  dans le packet (arena) — jamais directement vers plume.
- **FramePacket** (frame.h) = 2 allocations : un tableau de commandes + une byte arena
  pour les register-shadows/constants, adressées par offset (même idée que le Guest CommandBuffer).
- **Fences / fence_arena** : les copies faites à la fin de frame (ApplyFenceCopies) vivent
  dans une seconde arena, adressée par le bit `kFenceBit = 0x80000000`.
- **Backpressure** : `Submit` attend si la file est pleine ; `TakeSpare` réutilise un packet
  vide (TakeSpare) — jamais de drop pour la vitesse (le commentaire du code est explicite).

## 2. Invariants critiques (pièges habituels)

Ces invariants sont documentés dans le code mais méritent d'être rappelés avant de toucher
n'importe quel chemin :

1. **ABI des constantes shader** : les shaders guest sont lus par XenosRecomp ; en `on` mode,
   les constantes sont écrites dans des storage buffers/push constants à des adresses fixées
   (textures s0-s31 → (dim*32+slot)*4, samplers 384+slot*4, g_Booleans 512, etc.).
   Une erreur d'ABI = un shader entier fausse sans crash.
2. **Resolve variants** : un même guest dest peut servir plusieurs formats (8888/gamma,
   10:10:10:2, depth-pack D24S8, MSAA (D24S8 packed)). La variante choisie est keyée par
   la taille du fetch (d3d_map.h / backend.cpp). Erreur = textures qui ne comptent pas et
   SSAO noire (bug historique résolu via `RB_STENCILREFMASK = 0x210D` / `_BF = 0x210C`).
3. **Résolution des destinations physiques** : certaines destinations de resolves sont
   des adresses physiques dans la fourchette 0xE0000000 (+0x1000 vers le guest). Le capture
   réécrit le fetch base. Les vertex/index buffers ont aussi des headers CPU virtuels
   convertis par GetPhysicalAddress (capture.cpp).
4. **VRAM / hotspots** : le ping d'image — resolves de taille réelle (GTA utilise du
   tiling predicated ; les draws arrivent une fois untiled). Des copies rect-limités sont
   faites à la place. C'est pourquoi on utilise des resolves complets (full-size) + resolves
   par rect pour les cleares de rects.
5. **Readback CPU (exposure feedback)** : GTA calcule sa constante d'exposition (PS c18)
   sur CPU à partir d'un resolve 1x1 (luminance). Sans cela, l'exposition vaut 0.25 et la
   scène est sombre. Les resolves petits (≤128×128) sont écrits en guest memory après le fence.
6. **Indirect vertex fetch** : 44 VS fetch avec index calculé (data tilée lumière, skinning).
   Les enregistrements vfetchIndirect sont écrits dans les constantes partagées
   (768+loc*32), une entrée par location. Le shader marker `vs_HASH.indirect` est marqué
   par `shader_census.py`.
7. **Reverse Z / stencil** : la reverse Z (GEQUAL, clear 0), flags de Clear bits0-3 = RT0-3,
   0x10 Z, 0x20 stencil. Les resolve depths sont écrites avec du copy shader → R32F.
8. **MSAA** : MSAA sources passent par un scratch 1-sample ; les mip chains viennent du
   fetch dword4 (mip_max_level) ; dest face/level (r8/r9) spécifiques.
9. **Pipeline cache** : `vk_pipelines.bin` lu au démarrage (PrefetchPipelineCache),
   restreint par kDriverCacheLimit. Les pipeline loads en arrive-t-il au frame start ?
   PipelineCompletion coordonne les workers de compil qui signalent un job prêt.

## 3. Composants (fichiers & rôles)

| Composant | Rôle | Fichier |
|---|---|---|
| Hooks D3D | SetPending / SetShader / BeginVertices / Draw / Resolve / Clear / Query / Swap | `d3d_hooks.cpp` |
| Capture | Mémorisation du FramePacket, register shadow, physical address, fence copies | `capture.cpp` |
| Frame packet | Commands + ByteArena + fence_arena + front_buffer | `frame.h` |
| Renderer | Thread unique, queue bornée, NoteGuestSwap/WaitForGuestSwap, ping-pong slots | `renderer.cpp` |
| Backend | Replay Plume (pipelines, RT, resolves, blits, occlusion queries, dump) | `backend.cpp` (6810 lignes) |
| Shaders | Shartuer XenosRecomp/compil de shader guest (3617 shaders première fois) | `shaders.cpp`, `shader_compiler.cpp` |
| Textures | Décodage textures (port Skate 3 decoder) + writeback | `textures.cpp` |
| Déferred | Batches deferred, MSAA | `deferred_list.cpp` (Plume RenderCommandList custom) |
| Input | SDL gate, pads/clavier/touch + `input_pacing.h` (AMD anti-lag Windows) | `input.cpp`, `touch_controls.cpp` |
| Menu | F1 overlay settings (ImGui), prefs persistées `data/config/graphics.ini` | `menu.cpp`, `graphics_menu_ui.cpp`, `graphics_preferences.*` |
| Stats | Compteurs frames/draws/GPU-W | `stats.cpp` |
| Write watch | Suivi des écritures guest (debug) | `write_watch.cpp` |
| Fog filter | Filtering du strip de brouillard lointain (F1 → Realism) | `fog_filter.h` |
| Contact rays | Ray-cast screen-space contact shadow | `contact_rays.h`, `contact_rays_pass.h`, `native_contact_rays.hlsl` |
| Settings | F1 → prefs runtime/live (preset, potato, shader_cache) | `settings_runtime.cpp`, `settings_runtime.h` |
| Frame trace | Trace de frame (pour diag) | `frame_trace.h` |
| Adreno fix | Correctifs Android | `adreno_shader_fix.cpp` |

## 4. État milestone (repris de NEXT_SESSION_PLAN.md, 2026-10-25)

- **M0/M1** : map D3D + probe (0x837F0000-0x83840000 → 2 896 fns), renderer shadow mode.
- **M2** : shadow mode OK (2e fenêtre, RTX 4050, 30 fps cap).
- **M3** : compilation shaders OK (3617 shaders → SPIR-V via XenosRecomp + DXC).
- **M5** : backend replay ~880 draws/frame, full-size RTs keyed by EDRAM base/format/pitch,
  resolves → textures, vertex repack per element, inline begin-vertices, screen-space draws.
  **Résultat** : visuel cohérent (G-buffer, HUD, lights); lighting pass noire à l'époque.
- **M6** : shadow maps (R16G16 black avant), minimap/HUD fix, reflections, point-light shadows,
  occlusion queries (PublishQueryResult), frame cap OK (--gtav_fps_limit), on mode OK (~62 fps
  uncapped). Prologue room matches emulator. **NEXT (M7/M8)** : outdoors/water/particles/cutscenes,
  null GPU plugin, PSO precompile, perf, cube/slice resolves, MSAA, point sprites.

## 5. Ce qui reste ouvert (et où le travail se fait)

M6 (`make passes correct one by one`) : shadow maps (R16G16 black), lighting/HDR,
minimap, depth resolves, rect lists, point sprites — d'après le journal, déjà corrigées.

M7/M8 (d'après la dernière ligne du journal) :
- « Null GPU plugin, Plume owns the swapchain, no PM4 » — phase "on" complète.
- Queries & fences (occlusion queries passent par PublishQueryResult ; GetData override).
- PSO precompile (warm-up du pipeline avant la première utilisation).
- Perf : device-local buffers, pipeline cache, upload arena.
- Scenes à valider : outdoors, water, particles, cutscenes (semi-figées encore).

Note : le null GPU plugin remplace PM4/PM4 queue — c'est le plus gros chantier.
Aucun de ces points ne s'est validé in-game dans cette session.

## 6. Tests asset-free existants (+ nouveaux)

`gtav/tests/` : 9 tests CI de base + 4 nouveaux cette session.

| Test | Fichier | Couvre |
|---|---|---|
| input_pacing_gate | `input_pacing_tests.cpp` | input_pacing.h (gating de input) |
| presentation_policy | `presentation_tests.cpp` | presentation.h (présentation F1) |
| graphics_preset_policy | `graphics_policy_tests.cpp` | graphics_policy.h (Ultra/potato/standard) |
| contact_rays_policy | `contact_rays_tests.cpp` | contact_rays.h |
| native_upload_arena | `upload_arena_tests.cpp` | upload_arena.h |
| fog_strip_filter | `fog_filter_tests.cpp` | fog_filter.h |
| pipeline_cache_io | `pipeline_cache_tests.cpp` | pipeline_cache_io.h (read/repair) |
| resolve_replay_policy | `resolve_tests.cpp` | resolve_pm4.h + resolve_policy.h |
| graphics_preferences | `graphics_preferences_tests.cpp` | graphics_preferences.cpp |
| pipeline_completion | `pipeline_completion_tests.cpp` | pipeline_completion.h **(nouveau)** |
| byteswap_simd | `byteswap_simd_tests.cpp` | byteswap_simd.h **(nouveau)** |
| frame_data_hash | `frame_hash_tests.cpp` | frame.h (DrawDataHash + ByteArena) **(nouveau)** |
| safe_read | `safe_read_tests.cpp` | safe_read.h **(nouveau)** |

À ne PAS implémenter sans GPU/game data : tests GPU (vertex_layout, textures, deferred_list,
backend). `guest_texture_tests.cpp` et `contact_rays_vulkan_tests.cpp` sont skippés sans
SDK/Plume ; ils existent mais ne sont pas branchés en CI par défaut.

## 7. Sécurité / dangers

- **backend.cpp est énorme** (6810 lignes, 485 usages de méthodes) : toute modification
  doit être accompagnée de validation en jeu ou de dumps de frame (`--gtav_native_dump_frame=N`
  + `logs/native_dump/frame_N/` + comparaison vs emulator avec `tools/native/compare_dump.py`
  et `compare_values.py`).
- **frame.h DrawDataHash** mélange les slices d'indices même quand le walk échoue
  (0xFFFF all-invalid) — le hash n'est pas reset. Behavior observé ; documenter avant de
  modifier.
- **DrawCommand workspace** : snap_first/snap_vertices/snap_stream/snap_index sont des copies
  arena-offsetées — ne pas les toucher sans vérifier l'offset effectif.
- Le renderer natif **met le guest à 30 fps par défaut** ; le cap matériel n'est dépassé
  qu'en `--gtav_fps_limit=0` — ne pas invoquer un tests avec des flags non contrôlés.

## 8. Comment valider (avec game data)

Voir `play.sh` / `play_mac.sh` / `play.ps1`. Aucune donnée de jeu ici :

1. `build/tmp-gtav-tests` (asset-free) : `ctest`.
2. `--gtav_native=on` + `--gpu_skip_rendering=true` : natif possède le swapchain.
3. Compare native vs emulator : `--gtav_native=shadow` + `GTAV_SHOT_DIR` → PPMs.
4. Dump frame : `--gtav_native_dump_frame=N` → `logs/native_dump/frame_N/` ✓ (PNG + commands.txt).
5. Comparaison de valeurs : `tools/native/compare_values.py` (RT native vs emulator).

## 9. Recommandations pour le 2e agent

1. **Ne touche pas backend.cpp sans dump de frame** disponible (outils `--gtav_native_dump_frame`,
   `--gtav_native_trace_only=true` et `--gtav_native_ab`).
2. **Commence par les zones où la CI asset-free et les tests existent** (pipeline_completion,
   resolve, frame_hash, fog, upload_arena, byteswap). Elles ont retrouvé leur valeur de test.
3. **Ne pas forcer de test GPU/game data** dans la CI — il n'y a pas de data retail en CI.
4. **Doc** : tout changement de renderer doit être accompagné de mise à jour de `d3d_map.md`
   ou d'un HANDOFF_* document.
5. **Manifest / shader census** : ne pas toucher au stencil magic / vairable. Voir
   `tools/native/shader_census.py`.

---
*Fin du document. Prochain step : valider M7 (null GPU plugin) — requires a GPU + game data.*
