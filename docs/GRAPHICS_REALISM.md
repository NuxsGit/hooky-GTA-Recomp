# Réalisme : filtrage des matériaux et rayons de contact

Dans le **rendu natif sur ordinateur**, ouvre **F1 → Realism** (manette SDL : **RB + Back**).
Les réglages sont enregistrés dans `data/config/graphics.ini` comme les autres options graphiques.
Aucun préréglage ne modifie ces choix explicites.

| Option | Réglage et effet |
| --- | --- |
| **Material filtering** | **Follow preset** garde le comportement précédent : Ultra demande au moins 8× sur les matériaux éligibles. **Game filtering** retire ce renforcement, même avec Ultra. **2× / 4× / 8× / 16× minimum** rendent les surfaces vues en biais, par exemple la route, plus nettes sans dégrader un échantillonnage déjà supérieur. Redémarrage requis. |
| **Contact rays** | **Off** par défaut. **Low / Medium / High** : respectivement 4×3, 4×5 ou 8×5 échantillons de rayon par pixel, plus les lectures nécessaires pour estimer la pente du récepteur. Plus le réglage est élevé, plus il peut coûter de temps GPU. Effet expérimental. |
| **Contact strength** | Intensité de 0 à 100 %, 50 % par défaut. **0 %** ou **Off** ne lance aucune passe. Réglable en jeu. |

Le filtrage ne concerne que les textures 2D compressées par blocs avec plusieurs mipmaps, en
répétition U/V et filtrage trilinéaire. Les résolutions intermédiaires, la profondeur, les tables et
les échantillonnages volontairement ponctuels ne sont pas renforcés. Le plafond du pilote et du
moteur reste applicable (2× par défaut sous Potato) ; le menu affiche la valeur de cette session.
**Aucune texture ni aucun modèle n'est remplacé.** La qualité de l'eau d'Ultra reste un réglage
de rendu distinct.

## Technologie et limites

- **Rayons courts dans le tampon de profondeur** : comme le principe des
  [ombres de contact de Unity HDRP](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.high-definition/Documentation~/Override-Contact-Shadows.md),
  le shader cherche des occultants visibles tout près du pixel. Ici c'est une approximation de
  contact multidirectionnelle en profondeur projetée : pas de direction du soleil, de mesure en
  mètres ou de rebonds. Un biais, une épaisseur finie et une atténuation évitent une partie des
  halos ; le ciel et les pixels sans profondeur valide restent inchangés. Le shader est propre au
  projet ; il ne charge pas le SDK Unity ou FidelityFX CACAO.
- **Emplacement** : l'essai TU27 vise l'alpha du masque solaire *avant* l'éclairage, pas la
  couleur finale avec le HUD. Il prend comme indices le couple copie/ombre documenté dans
  [les captures historiques](HANDOFF_LINUX_2026-10-27.md), les états de mélange et le viewport.
  La copie doit avoir initialisé le même masque RGBA8 pendant l'image courante, sans autre
  changement d'alpha entre les deux dessins. La profondeur R32F liée à ce passage doit être fraîche,
  sans MSAA ni décalage, entièrement couverte (copie entière ou bandes contiguës d'une même source)
  et avec un sens de profondeur cohérent.
  Il ne suffit pas que deux adresses ou deux tailles se ressemblent. Le shader multiplie seulement
  l'alpha du masque, sans modification de ses canaux RGB, du stencil ni de l'ordre des dessins du jeu.
- **Repli** : si ces conditions ne sont pas reconnues, ou si le shader/pipeline est indisponible,
  le jeu garde ses ombres. Le menu distingue l'absence de passe, l'absence de profondeur alignée
  et une passe enregistrée. Aucun nouveau tampon d'image, aucune extension Vulkan RT ni historique
  temporel n'est exigé. Les anciens jeux d'en-têtes précompilés qui ne contiennent pas ce shader
  continuent de fonctionner sans l'effet. Les options ne s'affichent pas sur mobile.
- **Ray tracing matériel** : non intégré. Un point de départ pertinent est
  [Vulkan Ray Query](https://github.com/KhronosGroup/Vulkan-Samples/tree/main/samples/extensions/ray_queries)
  (`VK_KHR_ray_query` + `VK_KHR_acceleration_structure`). Avant de proposer ce mode, il faut disposer
  des triangles et transformations de la scène (y compris les objets animés), gérer la construction
  et le recyclage de BLAS/TLAS, prouver l'alignement des rayons et vérifier le matériel et le pilote.
  Les extensions de Plume seules ne fournissent pas ces données. Les rayons de contact ne peuvent
  ni voir derrière un objet ou hors de l'écran, ni ajouter des reflets RT ou un éclairage global.

## Vérifier le résultat

Le test Vulkan hors écran utilise une profondeur synthétique, sans fichiers de GTA. Il exerce le
shader et le mélange de production, vérifie que l'alpha seul varie, teste les deux conventions Z,
les niveaux de qualité, Off, l'intensité, les plans plats/inclinés et un masque demi-résolution.
Les tests sans GPU couvrent aussi l'opt-in, les passes non reconnues, les copies décalées ou
incomplètes et la persistance des réglages. Ils ne valident pas une scène commerciale ni le coût GPU.

Pour lancer le test Vulkan sous Linux, préparer les dépendances du projet (Plume **avec** le patch
et ses contribs, SDL3 partagé, en-têtes Vulkan du SDK et DXC ; sous Linux, indiquer le wrapper DXC
qui configure sa bibliothèque). Puis :

```sh
cmake -S gtav/tests -B build/realism-tests -DCMAKE_BUILD_TYPE=Release \
  -DGTAV_TEXTURE_SDK_SOURCE="$SDK" -DGTAV_TEXTURE_PLUME_SOURCE="$PLUME" \
  -DGTAV_TEST_CONTACT_DXC="$DXC" -DCMAKE_PREFIX_PATH="$SDL3_PREFIX"
cmake --build build/realism-tests -j 4
ctest --test-dir build/realism-tests -R contact_rays --output-on-failure
```

Vérification en jeu encore nécessaire : comparer la même caméra en journée, par exemple près des
pneus ou d'une bordure de trottoir, **Off** puis **Low** à 50 %. Tester aussi l'eau, le téléphone,
les cinématiques, les changements de caméra et Potato/Ultra. Pour vérifier l'accroche sans exporter
les textures, lancer avec `--gtav_native_trace_only=true` puis appuyer sur **F9**. Dans
`logs/native_trace/frame_*.txt`, `contact_rays=recorded` confirme *l'enregistrement CPU* de la passe,
pas son rendu GPU, sa visibilité ou sa qualité. S'il n'y a pas de passe reconnue, joindre cette
trace avec le lieu, le préréglage et le GPU avant d'élargir les critères.

Options pour un essai ponctuel : `--gtav_native_contact_rays=1 --gtav_native_contact_strength=50`.
Le filtrage explicite utilise par exemple `--gtav_native_material_anisotropy=16` au démarrage.
Pour revenir au chemin précédent : **Follow preset** et **Off**. Pour revenir au rendu natif sans
amélioration de préréglage : aussi sélectionner **Standard**, puis redémarrer si indiqué.

## Brouillard lointain (TU27, expérimental)

**F1 → Realism → Reduce distant fog** ou `--gtav_native_reduce_fog=true` (par défaut **false**,
sans redémarrage) omet un seul strip de brouillard lointain : le PS TU27
`9DFE7F549A326A57` qui couvre les collines au loin. Les bandes de ciel, la brume météo et les
autres passes restent visibles ; seuls les dessins correspondant exactement à ce hash, avec un
test de profondeur en lecture seule, sans écriture stencil, sans MRT et hors requête d'occlusion
sont ignorés et comptés dans `suppressed`. Les autres sont comptés en `protected` et conservés.
Désactiver l'option au même endroit restaure immédiatement le rendu d'origine. L'effet est
désactivé sur mobile ; aucune autre couleur, profondeur ou stencil n'est modifiée.

Aucune preuve retail que ce strip supprime toute forme de brouillard. Si un voile persiste,
d'autres passes ou la météo contribuent : comparer côte à côte la même caméra **original /**
**reduced** sur retail TU27 et sur cette version, puis joindre `logs/native_trace/frame_*.txt`
(`fog=reduced suppressed=… protected=…`) et des captures à hauteur des yeux et depuis les hauteurs.
Utiliser aussi `--gtav_native_ab=gtav_native_reduce_fog=false|true` pour alterner toutes les
60 images et comparer deux lancements complets séparés pour les préréglages avec redémarrage.
Voir le guide A/B en `docs/GRAPHIQUES_BROUILLARD_AB.md`.

Options ponctuelles : `--gtav_native_reduce_fog=true` ou `--gtav_native_skip_ps=9DFE7F549A326A57`
(diagnostic générique, sans garde-fou). Pour revenir à l'original : décocher la case ou
`gtav_native_reduce_fog=false`.
