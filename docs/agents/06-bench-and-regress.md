# 06 — Bench and non-regression: measuring before changing anything

This guide is the measurement protocol of the project: how to produce a result
that survives review (A/B/A, plateaus, what counts as a result), which bench
script measures what, how the event-driven scenarios in `scripts/entrees/` drive
the game without a player, how the two-level non-regression gate works (smoke and
full), what the graphics presets are for, and how to report a measured result.
The reference documents are `docs/BANC-SCENARIOS.md` (scenarios and
non-regression), `docs/BANC-MAC.md` (the native bench) and §4 "Mesurer juste" of
`docs/RETROSPECTIVE-RECOMPILATION.md`.

All bench scripts run the game against your own local install; their outputs
(frames, captures, run logs) never enter this repository. The non-regression
reference you compare against is **yours** (`scripts/nonreg-ref/README.md`).

1. [The measurement protocol](#1-the-measurement-protocol)
2. [The bench scripts and what each measures](#2-the-bench-scripts-and-what-each-measures)
3. [Event-driven scenarios (`scripts/entrees/`)](#3-event-driven-scenarios-scriptsentrees)
4. [Two-level non-regression](#4-two-level-non-regression)
5. [The fast gate](#5-the-fast-gate)
6. [Graphics presets](#6-graphics-presets)
7. [Reporting a measured result](#7-reporting-a-measured-result)
8. [Verify](#8-verify)

## 1. The measurement protocol

From `docs/RETROSPECTIVE-RECOMPILATION.md` §4 (each rule was learned from a
measurement that lied):

- **A/B/A systematically.** The reference build is replayed *after* the candidate.
  Game content alone is worth ±15 ms of CPU per frame, so a single interleaved
  pair proves nothing: use at least 3 pairs, on a calm machine, with thermals
  watched. On the phone (`fold-ab-auto.sh`), runs wait for thermal status 0
  before starting.
- **Plateaus, not whole runs.** Measure FPS on a window where the scene is
  settled — the mac A/B measures from the moment the tutorial help is closed
  (default 65 s) to the scenario's QUIT (default 125 s), one sample per pump tour
  (a tour = one presented frame, from the `[INPUT] banc` lines). Whole-run averages
  are dominated by startup and are only indicative.
- **Frame fingerprints** (hash of the presented image) are the non-regression
  oracle — on **stable** frames only (the known frame-10 race; frames 1 and
  100–300 are the canon).
- **Three benches, three roles**: a CI/dev machine (software GPU — only CPU
  metrics mean anything), a native bench of the target architecture (the mac M4
  bench: same arch and compiler as the phone, ~6-minute builds, ~90 s per
  mission), and the real device as final judge. "Bench first, hardware last"
  divided iteration cost by ten.
- **Cross-check any sampled profiler** against a kernel counter — a phantom
  "100% libc" was an `ITIMER_PROF` artifact (§3.10 of the retrospective).
- **What counts as a result**: a number (FPS, ms/frame, AE pixels, counter),
  obtained by a script in this repository, on a named plateau, with the reference
  replayed after the candidate, and the delta stated as a percentage of the
  paired medians. "It feels faster" is not a result; a per-mission FPS on a shared
  machine is indicative only (one binary measured −9% to +38% there — which is
  exactly why non-regression judges the **aggregate**).

## 2. The bench scripts and what each measures

All live in `scripts/`; every script's header comment documents its arguments,
variables and exit codes — read it before invoking (`sed -n '1,30p' scripts/<name>`
or open the file). They share `scripts/lib-banc.sh`: a disk guard
(`SC2_DISQUE_MIN_GO` + an estimate before any `rm`), a single bench **lock**
(`~/.sc2-verrous/banc` via `scripts/verrou.sh`, reentrant) so two measurements
never interleave, and post-run **compaction** (PPM frames → `empreintes.txt` +
PNG thumbnails: a mission goes from 1.3 GB to ~30 MB).

| Script | What it measures / does |
|---|---|
| `banc-mac.sh defaut <bin> <out>` | The **canon**: a 600-frame default run with frame fingerprints (image 1, 10, 100–300). This is the byte-stable reference every change is judged against. `mission` mode runs TRaynor01 with a `sample` profile window. |
| `banc-ab.sh A:<bin> B:<bin> [n]` | **A/B of FPS**: n interleaved pairs (A/B/A/B…), a warm-up pair excluded from medians (measured: the shared Metal shader cache gave +48% to B1 on equal binaries), per-label pipeline caches, plateau windows from `BANC_AB_DEBUT`/`FIN`. Depouillage by `nonreg.py ab` (paired deltas, medians, A-B-A triplets against thermal drift). |
| `capture-ab.sh <bin> <out> <map> <scenario> <captures> [n] -- A:… B:… [D:…]` | **A/B of image quality**: same named captures under two settings, interleaved; a diagnostic run D paints the studied zone in magenta to bound the AE; effect vs noise (A1/A2) separated; AE per capture via ImageMagick. |
| `banc-bar.sh <bin> <out> [progression]` | Campaign entry through the bar (hub) without a player; judges that TStory01 then TRaynor02 are read, fallback draws, crashes. |
| `campagne-banc.sh <bin> <out> [map…]` | The **29 campaign missions**, one per run, each won without a player (generic victory scenario + `QUAND:VICTOIRE` detection); per-mission summary by `campagne-rapport.py` (rc, victory, MANQUE addresses, light draws, non-voluntary stops, captured shaders). |
| `marathon-bar.sh <bin> <out> [plan] [progression]` | Several missions **in one game process** from a plan (`marathon-scenario.py` builds the scenario from the bricks of `scripts/entrees/marathon/`); snapshots campaign banks after each victory, verified by `progression-oracle.py`; memory sampled every 10 s. |
| `banc-escarmouche.sh <bin> <out> <scenario> [alarm]` | Offline skirmish vs the AI on one of the install's own melee maps (`SC2_ESCARMOUCHE*` variables, `docs/VARIABLES.md`); scenario captures decide. |
| `banc-options.sh <bin> <out> [preset…]` | The **preset matrix** (see §6) plus the pinned canon (`--canon`); `--controle` validates the preset files off-bench. |
| `fold-ab-run.sh` / `fold-ab-auto.sh` | Single run / A-B series **on the phone**, scripted identical gameplay, `simpleperf` profile window, thermal gate; depouillage by `fold-ab.py` (plateau FPS, CPU ms/frame by category, deltas). |
| `seance-fold.sh <apk\|->` | A **full grouped phone session** (install, bar, A/B/A, Medium-vs-Ultra mission, pause/resume, endurance, 60 vs 120 Hz, cinematic, captures), each step PASS/ÉCHEC/SKIP, user's `sc2.env` and `Variables.txt` saved and restored, report at the end. |
| `verdict-fold.sh <logcat>` | Display verdict from a logcat dump (CPU-OK / SWAPCHAIN-OK / MIXTE / …) — the one-line answer for "did it display on the device". |
| `nonreg.sh` | The non-regression gate (§4). |
| `verifier.sh` | The fast gate (§5). |

Shared environment: game data path `SC2_EXTRACTION` (default
`~/sc2-partout/extraction`, outside this repository), fonts from
`android/assets/polices`, `SC2_FRAMES` capture policy — all in
`docs/VARIABLES.md`, checked by `scripts/variables-sc2.sh verifier`.

## 3. Event-driven scenarios (`scripts/entrees/`)

A recompiled game cannot be tested like a library: most defects only appear after
minutes of menus, loads and play. The bench therefore **plays the game without a
player**: a scenario file injects Win32 inputs into the game loop, and triggers
wait for engine states. The format is defined in the header of
`runtime/sc2_entrees_banc.c` (format 2 is a strict superset of format 1 — proven
identical over all existing scenarios by a host test that embeds a copy of the old
parser).

- **Input events**: `DOWN/UP/CLIC/MOVE` at client coordinates (raw pixels, or
  normalized by `REF:LxH` / `@ui` / `@scene` anchoring), `ESC`, `KEY:vk`,
  combinations via `KEYDOWN`/`KEYUP`, `RCLIC`, `DRAG`, `MOLETTE`, `TEXTE`, `WAIT`,
  named `CAPTURE:<name>` at the next Present, `QUIT` (voluntary stop), `GARDE:<s>`
  (absolute stop, never cancelled).
- **Triggers** open chained segments: `QUAND:VICTOIRE` (the engine wrote its
  victory save during the run — everything after runs in relative `+s` time),
  `QUAND:CARTE:<name>` (a map is read from the archives), `QUAND:FICHIER:<glob>`
  (successful CreateFileW, optionally writes only), `QUAND:PRET`/`QUAND:CHARGEMENT`
  (loading screen end/start), `QUAND:FINPARTIE`, `QUAND:FRAME:<n>`,
  `QUAND:STABLE:<s>` (no archive read for s seconds), `QUAND:GLUE:<m>`; each with
  `MAX:<s> SINON:SUITE|QUIT|SAUTE`. Measured effect: the bar → mission-console →
  next-mission path went from 170 s (fixed instants, failing at 1280×1024) to
  77–82 s at both resolutions.
- **Caps**: 1,024 events, 64 segments; the segment *k* arms only when *k−1*
  triggered; leftovers of the previous segment are cancelled and held keys
  released. Writing rules learned the hard way (each scenario header says what it
  expects): a button that needs hover requires `MOVE` then `CLIC`; no `;` inside a
  comment (it is the separator); a first minimap click after minutes without input
  may not move the camera — double such clicks.

Where the scenarios live: `scripts/entrees/` (generic victory, per-map victories,
bar entries, active-play A/B for mac and Fold, options menus, skirmishes),
`scripts/entrees/marathon/` (bricks + plans for multi-mission marathons),
`scripts/entrees/diag/` (diagnostic one-shots, with their own LISEZMOI),
`scripts/entrees/mac/` (host-level input scenarios).

## 4. Two-level non-regression

`scripts/nonreg.sh <binary|main> <out> [--complet] [--maj-ref] [--ref <dir>] [--seul a,b] [--env VAR=val]`
+ `scripts/nonreg.py` for the verdict. Two levels (`docs/BANC-SCENARIOS.md` §3):

| Level | Controls | Duration (M4 bench, measured) |
|---|---|---|
| **smoke** (default) | canon (`banc-mac.sh defaut`), bar (`banc-bar.sh`), three missions in victory (`campagne-banc.sh`) | ≈ 14 min |
| **full** (`--complet`) | canon, bar, all 29 missions | ≈ 1 h 40 – 2 h |

Per control, against the same-named reference entry:

| Criterion | Requirement |
|---|---|
| `rc` | 0 |
| victory | detected (missions); expected maps read (bar) |
| `MANQUE` | missing-function addresses ≤ reference |
| light draws | fallback-shader draws ≤ reference (absent balance: RÉGRESSION) |
| texture fallbacks | "without data": 0, always; any other new cause: regression |
| stops | non-voluntary stops (signal, DEVICE_LOST) ≤ reference |
| images | canon: exactly 600; timed runs: ≥ 50% of reference (premature stop) |
| fingerprints | canon: frames 1, 10, 100, 200, 300 identical (frame 10 has one tolerated variant) |
| FPS | judged on the **aggregate** of missions (−10% by default, `NONREG_TOL_FPS`); per mission: indicative only |

The aggregate rule exists because the bench is shared: a same binary measured −9%
to +38% across missions. Fine FPS measurement is `banc-ab.sh`'s job, on a calm
machine. Exit codes: 0 PASS, 1 RÉGRESSION, 2 usage, 3 disk guard, 4 refused
reference update.

**The reference is personal**: each user sets their own with `--maj-ref`
(refused unless every control is PASS apart from FPS; one person does it, citing
the change in NOTES) or points `NONREG_REF=<dir>` at it. It contains fingerprints
of their own renders, binary and pack sha256 — it never belongs in the repository
(`scripts/nonreg-ref/README.md`).

**The judge itself is tested** (`docs/BANC-SCENARIOS.md` §3): a build that leaves
the game "in the background" (`SC2_ACTIVATION=0`, −61% FPS) must yield
RÉGRESSION; a binary without SPIR-V must too (21 M fallback draws); and
`scripts/injecter-bugs-v22.sh` applies mutants to a copy of the backend and
requires each to be **killed by its own test** (a non-mutated witness must pass
first, else the judge is false). `scripts/test_nonreg.py` replays synthetic bench
directories through the whole verdict logic.

## 5. The fast gate

Before any bench: `scripts/verifier.sh` (~2 min, no lifted code, no game data) —
the host tests of `tests-hote/` (CMake project, `-O0`, 33 tests, one known macOS
SKIP), the Python unit tests of `scripts/` (the `test_*.py` files listed in §8),
`bash -n` on every shell
script, fixtures-replayed Fold checks (`ADB_FAKE=1`, no phone touched), and the
`SC2_*` variable documentation control. On this repository the host tests are
also runnable directly (§8).

## 6. Graphics presets

`scripts/prereglages/` holds **settings keys only** (no game content):
`low.txt`, `medium.txt`, `high.txt`, `ultra.txt` (Graphics Quality and Texture
Quality at the same level, written by the game itself after "Accept" in its
options menu), `fold.txt` (the phone's default, a copy of `ultra.txt`) and
`canon.txt` (exactly what the game detects and writes alone for the announced
2007 GPU — this one pins the canon). They are seeded via `SC2_VARIABLES` +
`SC2_VARIABLES_FIGE=1` (`docs/VARIABLES.md`); `scripts/prereglages/LISEZMOI.md`
documents the two measured traps:

- The game **re-detects and overwrites** the seeded preset when `lastDeviceId` is
  missing or does not match the announced card — the seeding adds it (1026 or
  1507 according to `SC2_GPU_PROFIL`).
- Texture quality needs **both** the interface key and the effective keys
  (`texQualityLevel`, `targettexeldensity`, `TerrainTextureSize`…); seeding only
  the interface key displays "High" and leaves Low textures.

`scripts/banc-options.sh` runs the matrix (one run per preset on a mission,
Variables.txt frozen, then a neutral accepted change in the options menu so the
game rewrites the file — proof of application), plus the pinned canon
(`--canon`) and the off-bench preset control (`--controle`). A **coverage warning**
measured on the current client (`docs/CLIENT-5.0.md` §5): a 0-missing-shader
bench proves coverage only for its own settings — each quality level must be
harvested.

## 7. Reporting a measured result

A result that survives review states, in this order:

1. **What was measured** and by which script (`scripts/banc-ab.sh`, 3 pairs,
   warm-up excluded; `capture-ab.sh` with the diagnostic zone; a non-regression
   control), on which bench (mac M4 / emulator / Fold, thermal status 0).
2. **The plateau** (window, why it is settled) and the metric (paired median FPS
   delta, ms/frame by category from the profiler, AE pixels per capture, counter
   delta).
3. **The reference replayed after the candidate** (A/B/A), and the noise estimate
   (A1/A2 or the warm-up pair).
4. **The verdict against the gate**: canon fingerprints unchanged (or the reason
   they changed, with the before/after captures compared by
   `scripts/comparer-captures.sh`, never by eye), smoke non-regression PASS
   (and full, for anything touching lift or backend semantics).
5. Absolute numbers with units and the binary (sha256 prefix) — the shape used
   throughout `docs/CLIENT-5.0.md` §7 and the levers table of
   `docs/D3D9-PORTING-GUIDE.md` §10.

Report honestly when something did not run: a skipped step named with its reason
beats a green line nobody can reproduce.

## 8. Verify

These need no game data and no device. From the repository root:

```sh
# 1) Host tests (33, -O0, no lifted code; test_d3d9v SKIPs at 77 without Vulkan)
cmake -S tests-hote -B build-tests -G Ninja && cmake --build build-tests -j8
ctest --test-dir build-tests --output-on-failure

# 2) Unit tests of the bench scripts' verdict and report logic
(cd scripts && python3 -m unittest test_nonreg)                              # non-regression verdict
(cd scripts && python3 -m unittest test_marathon_scenario)                   # scenario generator
(cd scripts && python3 -m unittest test_progression_oracle)                  # bank comparison
(cd scripts && python3 -m unittest test_campagne_rapport)                    # campaign report
(cd scripts && python3 -m unittest test_fold_ab)                             # phone A/B depouillage

# 3) The variables-documentation control (fails naming any SC2_* missing from docs/VARIABLES.md)
scripts/variables-sc2.sh verifier

# 4) Scenario format gate: bash syntax over the bench drivers
bash -n scripts/nonreg.sh && bash -n scripts/banc-ab.sh && bash -n scripts/campagne-banc.sh
```

Expected: step 1 green (known macOS SKIP allowed), steps 2–3 exit 0, step 4
silent. Note that `python3 -m unittest` prints each test's own progress lines
before the `OK` summary — that is the pass condition.

**Next:** [symptoms.md](symptoms.md) — the matching fault-finding index.
