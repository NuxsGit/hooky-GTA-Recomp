# Benchmark prototype — Lenovo N100 (Intel N100, 4× Gracemont, UHD, 8 Go)

Prototype asset-free pour valider le preset **N100 Ultra 25%** (v0.8.8+) avant run sur le vrai Lenovo Ideapad / Mini PC N100.

## Pourquoi N100 Ultra 25% ?

| Preset | Scene | Texture | Buffer | Chunk | Slots | GPU | Notes |
|---|---|---|---|---|---|---|---|
| Standard | 100% | 1024 MiB | — | 64 MiB | 2 | — | Desktop 16 Go+ |
| Potato | 50% | 192 MiB | 64 MiB | 16 MiB | 1 | — | 640×360, saccades-free |
| Extra Potato | 33% | 96 MiB | 32 MiB | 8 MiB | 1 | — | 426×240, GTA IV faces, 3× rien |
| **N100 Ultra** | **25%** | **64 MiB** | **16 MiB** | **4 MiB** | **1** | **FSR1, 1× aniso, no contact, fog off** | **480×270 interne → 10× moins texels, Cyberpunk-low, 6W TDP, UHD partagée** |

N100 = Gracemont sans AVX2, SSE4.2 seulement. Le build par défaut (`GTAV_X86_64_V3=OFF`) target `SSE4.1` donc pas d'`Illegal Instruction`. Scene 25% = `kMinSceneScale = kUltraPotatoSceneScale = 0.25`, `kMaxFrameSlots=1`, `MakeGraphicsMemoryPolicy(true,true,true) -> 64/16/4 MiB`. Workers shader = 1 en Ultra (vs 4-6 standard) pour éviter spikes sur 6W.

## Fichiers

- `gtav/tests/n100_benchmark.cpp` — exe `gtav_n100_benchmark`, inclut `graphics_policy.h`, 5 micro-bench + détection N100
- `gtav/tests/CMakeLists.txt` — target + `ctest -R n100_prototype`
- `tools/benchmark_n100.py` — helper shell/python pour collecter `lscpu`, `/proc/meminfo`, `vulkaninfo` si présent (optionnel)

## Compiler & lancer (sans retail data)

```bash
# tests légers (comme CI)
pip install --break-system-packages cmake ninja -q
/usr/local/bin/cmake -S gtav/tests -B /tmp/n100 -G Ninja
/usr/local/bin/cmake --build /tmp/n100
ctest --test-dir /tmp/n100 -R n100_prototype --verbose
# ou direct
/tmp/n100/gtav_n100_benchmark --quick
/tmp/n100/gtav_n100_benchmark --json > n100_result.json
```

Sur **Lenovo N100 réel** (Ubuntu/Windows) :

```bash
git clone https://github.com/NuxsGit/hooky-GTA-Recomp
cd hooky-GTA-Recomp
cmake -S gtav/tests -B build-n100 -G Ninja
cmake --build build-n100 --target gtav_n100_benchmark
./build-n100/gtav_n100_benchmark        # run complet ~10s
./build-n100/gtav_n100_benchmark --quick  # ~2s pour CI
```

Python helper (optionnel, collecte sysinfo) :

```bash
python3 tools/benchmark_n100.py --quick --json n100.json
```

## Sortie attendue (exemple N100, --quick)

```
============================================================
 Lenovo N100 - Benchmark prototype GTA Recomp (N100 Ultra 25%)
============================================================
 CPU: Intel(R) N100
 AVX2: NO (N100/Gracemont - attendu)
 RAM total: 7860 MiB (8 Go - profil N100)
 Threads: 4 (N100 attendu: 4)
 Preset N100 Ultra: 25% scene, 64 MiB tex / 16 MiB buf / 4 MiB upload, 1 frame slot, FSR1, 1x aniso, fog off
------------------------------------------------------------
Policy        scene       tex MiB   buf MiB   chunk     slots lean
----------------------------------------------------------------------
Standard      100%        1024      0         64 MiB    2     no
Potato        50%         192       64        16 MiB    1     yes
Extra Potato  33%         96        32        8 MiB     1     yes
N100 Ultra    25%         64        16        4 MiB     1     yes
------------------------------------------------------------
 N100 Ultra check: PASS (64/16/4MiB, 1 slot, ultra_potato=true)  kMinSceneScale=0.25 ...
 SceneScale(0.25) -> 0.25 (attendu 0.25)  Scaled 1280x720@0.25 = 320x180
------------------------------------------------------------
Test                      Result      ms        Note
--------------------------------------------------------------------------------
INT add/xor               4.12 Gops/s 48        50M iter
FP mul-add                3.85 Gops/s 51        50M iter
memcpy 64MiB x20           9.40 GiB/s 136       64MiB block
ScaledTexels 25% (N100)    90.00 % saving 2     480x270 vs 1920x1080 = 90% texels saved
Upload chunk N100 4MiB     16.00 x leaner 0      standard 64MiB / ultra 4MiB = 16x
--------------------------------------------------------------------------------
 Score N100: 100/100  ->  Grade A (N100 ready - Ultra 25% fluide)
 Conseil Lenovo N100:
  - Preset recommande: N100 Ultra (25% scene, FSR1, 64/16 MiB) -> 480x270 interne, ~10x moins texels, saccades-free 8Go
  - Workers shader: 1 (ultra) vs 4-6 standard -> evite spike CPU 6W TDP
  - Budget: texture 64MiB + buffer 16MiB -> tient dans UHD partage (vs 1024MiB standard)
  - Si >30 FPS vise: ajouter --gtav_fps_limit=0 + --vsync=false apres warmup
============================================================
N100 prototype benchmark: PASS (asset-free, no GPU required)
```

Si lancé sur autre CPU (AVX2=YES) : grade B/C mais policy PASS — le benchmark reste valide, juste noté *pas N100 réel*.

## Ctest

`n100_prototype` est un test normal : `ctest -R n100 --verbose` doit passer (<30s, exit 0 si Ultra policy ok). En CI il pousse avec `--quick` (50M iter vs 200M) pour rester <5s.

## Interprétation Lenovo N100

- **INT/FP <2 Gops/s** → thermal throttling ou governor powersave → passer en `performance` : `echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`
- **memcpy <3 GiB/s** → single-channel DDR4 bridée (N100 Ideapad 1×8 Go) — attendu 7-12 GiB/s, reste ok pour 4 MiB chunk. Si <2 GiB/s : vérifier BIOS *Memory* single vs dual.
- **Score A** → lancer le jeu en N100 Ultra : `gtav --gtav_native=on` + preset N100 Ultra (F1 → Graphics → Presets). `N100 Ultra check: PASS` garantit pas de `Illegal Instruction` (no AVX2).
- **Score C/D** → essayer Extra Potato 33% d'abord, puis Ultra si <20 FPS en free-roam.

## Prochaines étapes (vrai N100)

1. Lancer le bench `--json` et joindre `n100_result.json` + `vulkaninfo --summary` (si Vulkan dispo) dans l'issue.
2. Run jeu retail + `--gtav_native_fps=true` en N100 Ultra free-roam 5 min, noter FPS log `logs/gtav*.log`.
3. Comparer Standard vs Ultra : attendu +40-60% FPS, VRAM host -90%.

Prototype volontairement sans GPU — pas besoin de `gtav_game.dll` ni de TU27, tourne même dans le sandbox Arena.
