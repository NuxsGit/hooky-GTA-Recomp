# Guide A/B : brouillard lointain et chargements

Ce guide en français explique comment comparer **sans biais** le rendu d'origine et le
brouillard réduit, puis comment vérifier que les chargements ne régressent pas.

## 1. Brouillard réduit (PS 9DFE7F549A326A57)

### Ce que fait l'option
- **F1 → Realism → Reduce distant fog** (`gtav_native_reduce_fog`).
- **Off (défaut)** : rendu d'origine. **On** : ignore seulement le strip lointain
  TU27 `9DFE7F549A326A57` quand il dessine avec `RB_COLOR_MASK` ≤ 0xF, `RB_MODECONTROL` = 4,
  sans écriture profondeur, sans écriture stencil et hors requête d'occlusion.
- Les compteurs par frame `suppressed` / `protected` s'affichent dans **Realism** et dans
  `logs/native_trace/frame_*.txt` (`fog=reduced` ou `original`). `suppressed` ne compte pas
  comme une pipeline perdue ; `recorded=false` d'un draw reste distinct.

### A/B en jeu (même lancement, bascule live)
1. Choisir une scène de jour avec collines lointaines visibles (ex. au nord de la carte,
   crête au-dessus de la ville) à hauteur des yeux, puis depuis les hauteurs.
2. Ouvrir **F1 → Realism**, noter `F2 / Esc` inchangé, puis basculer la case.
   Vérifier que le changement est **live** (pas de redémarrage demandé, la case n'est pas
   marquée *Pending restart*).
3. Comparer **original → reduced → original** sans bouger la caméra. Le strip ne doit
   disparaître que en `reduced`, sans trous dans le ciel (bandes `DCFC61F75589ED79`) ni
   dans la météo.
4. Appuyer sur **F9** avec `--gtav_native_trace_only=true` dans les deux états.
   Dans `frame_*.txt`, `fog=original` doit avoir `suppressed=0`, `fog=reduced` doit avoir
   `suppressed>0` ou `protected>0` selon la frame. Joindre la trace si le voile reste.

### A/B entre deux lancements (retail vs recomp, préréglages)
- La TU Xbox 360 retail et les lancements séparés évitent le biais de cache :
  1. Retail TU27 : même caméra, capturer **original**.
  2. Cette version avec `gtav_native_reduce_fog=false` : vérifier **match** (même image que retail).
  3. Même version avec `true` : vérifier **non-match contrôlé** (seul le strip manque).
  4. Revenir à `false` : vérifier la **réversion live** (le strip revient sans redémarrer).
- Pour les préréglages avec redémarrage, comparer deux lancements complets séparés, pas
  `--gtav_native_ab` sur un réglage `restart` (le menu l'avertit et l'A/B conseille deux runs).

### A/B automatique (option développeur)
- `--gtav_native_ab=gtav_native_reduce_fog=false|true` alterne le réglage toutes les 60
  images et journalise les moyennes toutes les 1200 frames. Ne pas utiliser pour un réglage
  `restart` ; préférer deux exécutions distinctes.

## 2. Chargements et compilations

### Objectif de cette série
Réduire les à-coups au démarrage et au premier survol sans changer la vitesse de jeu.

### Ce qui a été mesuré en suivi
- Le cache Vulkan `shader_cache/vk_pipelines.bin` (≈ ms au chargement) est désormais lu en
  tâche de fond pendant l'initialisation de la guest et le driver est échantillonné sans
  bloquer l'écriture précédente. Le journal `pipelines.bin` (32 MiB max) n'est plus vidé
  draw par draw : les nouvelles entrées sont coalescées (~25 ms ou 64 KiB) par un thread
  dédié, avec réparation du suffixe tronqué à la prochaine ouverture.
- Le préchargement SPIR-V et le réchauffage des pipelines restent désactivés en **Potato**
  (un seul worker). Hors Potato, le préchargement démarre **en même temps** que le réchauffage,
  avant la première recherche de shader.
- L'attente des pipelines compilées en arrière-plan est bornée par
  `--gtav_native_pipeline_wait_ms` (40 ms par frame par défaut) avec réveil par
  `condition_variable` au lieu du poll `200 µs`. Un draw qui dépasse le budget est ignoré
  une frame, sans compter comme perte persistante. Le menu **Performance** affiche
  `Background pipeline wait` (ms), `pending` et `warmed`.
- Les écritures de préférences de **F1** sont coalescées : la valeur live du cvar change
  immédiatement, le fichier n'est écrit qu'à l'idle (~350 ms), au changement d'onglet ou à
  la fermeture (F1/Esc/B, switch de panneau, fermeture de fenêtre). Les drafts pendant un
  glissement ne font plus de `fsync` à 60 Hz.

### Comment vérifier
- **Avant / après** sur le même PC et le même disque (de préférence HDD 5400 tr/min pour le
  cas sévère), avec un `shader_cache` existant et un autre vidé :
  1. `rm -rf shader_cache` puis premier lancement → mesurer le temps jusqu'à l'interactif.
  2. Deuxième lancement (cache chaud) → comparer le log `[native] pipeline cache: … KB loaded
     (prefetch; … ms on render thread)` et `[native] hitch … background pipeline wait … ms`.
  3. En jeu, ouvrir **F1 → Performance** et noter `warmed`/`pending`/`wait`.
  4. Déplacer un curseur (par ex. **Contact strength** ou **Texture recheck**) et vérifier
     dans les logs que le fichier `data/config/graphics.ini` n'est écrit qu'à la fin du
     glissement.
- Comparer **Standard** vs **Potato** (le Potato ne doit pas précharger) et vérifier que
  `--gtav_native_pipeline_wait_ms=0` ne bloque jamais (trous d'une frame attendus) tandis que
  la valeur par défaut évite la plupart des trous visibles.
- Vérifier en **540p** (fenêtre `960×540`) que tous les onglets restent lisibles et que
  les combos ne débordent pas (largeur 56 % max).

Aucun gain de gameplay n'est promis sans cache préalable ; les synthèses upload/retention
`64→16 Mio Potato` restent une mesure synthétique hors ligne.
