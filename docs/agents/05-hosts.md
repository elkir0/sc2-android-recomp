# 05 — The hosts: macOS app and Android port

Two hosts run the recompiled game: a macOS Cocoa app (`mac/`) and an Android
NativeActivity app (`android/`). Both are thin platform layers over the same runtime:
window/surface, presentation to a swapchain, audio output, input, and a launcher
that writes `sc2.env`. This guide maps both trees, gives the build and test commands
grounded in `docs/BANC-MAC.md` and the build scripts, and states the rule that
matters most: a built APK or `.app` contains recompiled game code and is never
published or shared ([LEGAL.md](../../LEGAL.md) §4; [AGENTS.md](../../AGENTS.md),
hard rule 2).

What this guide does not cover: obtaining or preparing the game executable, and
launching the game itself. Out of scope for this repository — see
[AGENTS.md](../../AGENTS.md#scope).

## 1. The macOS host (`mac/`)

### 1.1 One binary, two modes

`recomp-sc2` is both the historical bench and the app; the mode is decided once in
`main()` (`mac/sc2_mac_main.m`), and the wiring is `mac/mac.cmake`, included by the
game build under `APPLE` and `SC2_APP_MAC`:

- `SC2_FENETRE` absent or `0` — **bench**: `sc2_poc_main` on the main thread, the
  runtime's `exit(code)` kept, no AppKit call; every bench script works unchanged.
- `SC2_FENETRE=1` — **app** (windowed host): set by the `LSEnvironment` of
  `mac/Info.plist.in`, so any launch through the Finder or `open` is the app, while
  running `Contents/MacOS/recomp-sc2` directly is still the bench.

The Android platform layer is compiled **in place** on macOS
(`android/jni/sc2_android_hooks.c`, `sc2_trace_present.c`, and the presenter
`android/jni/sc2_vk_presente.c`), with target-definition renames standing in for
Android's `--wrap` (ld64 has none) — see the rules at the top of `mac/mac.cmake`.
`-DSC2_APP_MAC=OFF` gives back the pre-V3 bench binary.

### 1.2 Map of `mac/`

| File | Role (from its header comment) |
|---|---|
| `mac/sc2_mac_main.m` | `main()` of the executable: chooses bench or window mode, exit hook |
| `mac/sc2_mac_hote.c` | the C side of the host: bench mode, the `sc2-jeu` thread, `[FPS]` line, weak stubs for later lots; no AppKit call |
| `mac/sc2_mac_fenetre.m` | the Cocoa host: NSApplication, window, `SC2Vue` on a `CAMetalLayer`, game thread started after the surface is handed to the presenter |
| `mac/sc2_entrees_mac.m` | Cocoa input (NSEvent) routed to the runtime input API; keyboard via `mac/sc2_clavier_mac.c`, mouse/wheel via `mac/sc2_souris_mac.c` |
| `mac/sc2_clavier_mac.c`, `mac/sc2_souris_mac.c` (headers beside them) | VK mapping and characters (AZERTY tested with simulated layouts), buttons, wheel — pure C, host-tested |
| `android/jni/sc2_audio_android.c` (`__APPLE__` branch) | CoreAudio output (AUHAL) fed by the ring of `SC2_MAC_SON`; enabled by the `SC2_MAC_SON_COREAUDIO` line of `mac/mac.cmake` |
| `mac/sc2_lanceur_mac.m` | launcher menus (« Partie », « Missions », « Réglages »): write `sc2.env` and relaunch via NSWorkspace — never two engines in one process |
| `mac/sc2_catalogue_mac.c` + `mac/sc2_catalogue_mac.h` | the launcher catalogue in pure C (33 campaign maps, 63 melee maps, presets), mirroring the Android Java catalogue |
| `mac/sc2_config_mac.c` + `mac/sc2_config_mac.h` | app configuration in pure C: `sc2.env` reading (never overriding the environment), `SC2.exe` check by size and sha256, support folder |
| `mac/sc2_mac_evenements.c` + `mac/sc2_mac_evenements.h`, `mac/sc2_mac_evenements_ns.m` | synthetic event replay (`SC2_MAC_EVENEMENTS`): pure-C core and AppKit synthesis, one primitive = one NSEvent |
| `mac/sc2_mac_sortie.m` | the window-mode exit path (exit hook, fault-safe `_exit`), so `exit()` never runs on the game thread |
| `mac/Info.plist.in` | the app bundle plist (`LSEnvironment`, `NSPrincipalClass`) |
| `mac/tests/` | host tests: catalogue, keyboard, mouse, config, events, replay, sound ring, platform order — all in the fast gate |

Vulkan on macOS is **Metal through MoltenVK**. The backend's explicit load order
(comment at the top of the loader in `runtime/sc2_d3d9v.c`): `SC2_VK_LIB`, then
MoltenVK next to the executable (embedded bundle), then the Khronos loader, then
Homebrew MoltenVK, then `libMoltenVK.dylib` from the search path — MoltenVK exports
`vkGetInstanceProcAddr` itself, so no loader is required. The Homebrew dylib is
pinned by sha256 in `scripts/paquet-mac.sh`; every `SC2_MAC_*` knob is in
[../VARIABLES.md](../VARIABLES.md), Mac section.

### 1.3 Build and test

The full build (bench + app) needs the private side: lift, SPIR-V table, StormLib,
Theora sources — see Prérequis in [../BANC-MAC.md](../BANC-MAC.md). The commands:

```sh
cmake -S recomp-sc2 -B recomp-sc2/build-mac-O2 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      "-DSC2_LIFTED_OPT=-O2;-fno-strict-aliasing;-fwrapv;-g0;-fno-stack-protector"
ninja -C recomp-sc2/build-mac-O2 recomp-sc2
scripts/banc-mac.sh defaut  recomp-sc2/build-mac-O2/recomp-sc2 /tmp/mac-defaut   # 600 frames, fingerprints
ninja -C recomp-sc2/build-mac-O2radx recomp-sc2 sc2partout-app    # dev bundle, target outside ALL
```

The gate covers the original host code without any of that (all of `mac/tests/`):

```sh
scripts/verifier.sh --rapide
```

Replay a scenario in the app without hands (`scripts/app-mac.sh` header for the full
contract — it launches through LaunchServices so `LSEnvironment` applies, isolates
the run, and kills the app at the deadline):

```sh
scripts/app-mac.sh <app> <dossier> <scénario.txt> [délai_s=600] [VAR=valeur…]
```

Hands-off launcher trial: `SC2_MAC_LANCEUR_CHOIX=menu|carte:<nom>|escarmouche:<carte>:<race>:<raceIA>:<diff>`
([../BANC-MAC.md](../BANC-MAC.md), last section).

### 1.4 The self-contained bundle (for your own machine)

`scripts/paquet-mac.sh` assembles a self-contained `SC2Partout.app` from a Release
build, for your own machine. It refuses (named exit codes in its header) a destination outside
`~/code/sc2-apk/`, a lift compiled `-O0`, a missing SPIR-V table, or a MoltenVK
whose sha256 differs from the pinned one:

```sh
scripts/paquet-mac.sh recomp-sc2/build-mac-O2radx --dest ~/code/sc2-apk/mac-essai
codesign --verify --deep --strict ~/code/sc2-apk/mac-essai/SC2Partout.app
plutil -lint ~/code/sc2-apk/mac-essai/SC2Partout.app/Contents/Info.plist
```

The bundle is **private**: ad hoc signature, for the maintainer's machines, never in
git, never outside `~/code/sc2-apk/` — it contains recompiled game code.

## 2. The Android port (`android/`)

### 2.1 Shape of the port

`android/CMakeLists.txt` builds `libSC2Partout.so` for aarch64-linux-android from the
runtime sources **unmodified**; the Android deltas all live in the glue:

| Piece | File | What it does |
|---|---|---|
| NativeActivity glue | `android/jni/sc2_android_main.c` | `android_main` on the glue thread (window, touch → `sc2_input`, back key), the 16 MB-stack `sc2-jeu` thread with the relocated guest arena, the `sc2-log` thread (stdout → logcat + file); picks `SC2_INSTALL` |
| Link-time hooks | `android/jni/sc2_android_hooks.c` | `--wrap=mmap` (relocated guest zone), `--wrap=dlopen` (`libvulkan.so`), present hook — the runtime itself is untouched |
| bionic fibers | `android/jni/sc2_ucontext.c`, `android/jni/sc2_ucontext_aarch64.S`, `android/jni/sc2_ucontext_offsets.h` | `makecontext`/`swapcontext` equivalents for bionic, which ships no `<ucontext.h>`; generic copies documented in `tools/android-porting/README.md` |
| Guest-arena delta | `tools/android-porting/cpu_delta.cmake` | generates a patched `cpu.h` whose atomics go through `GPTR` once the arena is relocated (delta mode) — the ART heap occupies the low 4 GB |
| Swapchain presenter | `android/jni/sc2_vk_presente.c` | `--wrap=dlsym` interposition: adds the surface extensions, builds a swapchain on the `ANativeWindow`, blits the engine's back buffer (aspect preserved, FIFO), `[VKP]` logs; compile-time option `SC2_SWAPCHAIN`, run-time `SC2_SWAPCHAIN=1` |
| Gestures | `android/jni/sc2_gestes.h` | pure computation: 1 finger = left click / box select, 2 fingers = right click, with the wait/threshold rules in the header |
| Shortcut bar | `android/jni/sc2_barre.h` | shortcut keys drawn in the letterbox band (control groups, F1/F2, Tab, modifiers, orders…); pure layout/hit-test/Raster |
| Geometry | `android/jni/sc2_geo.h` | where the game image lands in the window, and the exact inverse mapping of a touch to game client pixels |
| Audio output | `android/jni/sc2_audio_android.c` | AAudio ring + callback subscribed to the mixer's output API |
| Java launcher | `android/src/fr/sc2partout/` (`LanceurActivity.java`, `Catalogue.java`, `EtatInstall.java`, `Options.java`, `Prereglages.java`, `Progression.java`) | the title-screen launcher the icon opens: campaign cards, `sc2.env`, install check, options; the game itself stays the exported `:jeu` NativeActivity (`android/AndroidManifest.xml`) |
| Bench without a phone | `android/sim/` | the same hooks and thread layout under Linux/qemu (`sc2_sim_main.c`, `android/sim/CMakeLists.txt`), with a fake ART heap |
| Phases | `android/jni/sc2_phases.h` | run phases (start → title → menu → loading) parsed from the runtime log, to measure cadence per phase |

### 2.2 The APK chain

Two scripts drive the build, both refusing an APK whose lift is stale
(`scripts/garde-lift.sh` — see the v7 lesson quoted in its header):

- `android/build-apk-mac.sh` — the fully local macOS chain (this is the one you run
  on a Mac): CMake + NDK r28 → `libSC2Partout.so`, aapt2 resources and assets
  (`inits-stricts.txt`, Liberation fonts), `javac` + `d8` for the Java launcher,
  packaging with the `.so` **stored** (uncompressed, 16 KB-aligned), zipalign,
  debug signing, then manifest checks. Output: `artifacts-mac/sc2-partout-mac.apk`
  (suffixes for variants: `SC2_APK_LIFT_OPT`, `SC2_APK_API`, `SC2_APK_LDFLAGS`,
  `SC2_APK_APPELS_DIRECTS`, `SC2_APK_REGISTRES_LOCAUX`…).
- `android/build-apk.sh` — the Linux-server chain (same guards), then re-packs the
  native lib as `Stored` and re-signs; see its header for the exit contract.

Paths they expect: `android/build-apk.sh` reads `$ANDROID_SDK_ROOT` / `$ANDROID_NDK`
(defaults under `$SC2_TOOLS`, i.e. `~/tools`), JDK from `$JAVA_HOME` or a `jdk-21*`
under `$SC2_TOOLS`; `android/build-apk-mac.sh` hardcodes its own defaults
(`~/tools/android-sdk`, `~/tools/android-ndk-r28`, Homebrew `openjdk@17`) — see the
header of each script before running it. Validation of a built APK — do not install
one that fails it:

```sh
scripts/valider-apk.sh <fichier.apk>     # badging, arm64 lib + entry symbols,
                                         # signature/alignment, pinned key (SC2_APK_CLE_SHA256),
                                         # lift proven not -O0 (build-info marker or .symtab proof)
```

The pass-by-pass validation benches (`android/valider-7b.sh` … `valider-8h.sh`) are
historical records of each pass: each header states what it builds and runs; read
one before reusing it, they assume the private tree and a device or emulator.

### 2.3 Game data on the device

What to copy, how much it weighs, where the app looks, and what breaks when a file
is missing is measured and documented in `android/DONNEES-INSTALL.md`, with the
reference list in `android/manifest-install.txt` and the pusher
`android/outils/pousser-donnees-7y.sh` (resumable: it pushes only missing or
size-different files, then grants the all-files access). The settings side
(`/sdcard/SC2Partout/sc2.env`, `Variables.txt` seeding) is in the same document.

### 2.4 Tests without a phone

- Fast gate: the pure-C glue tests (`test_gestes`, `test_geo`, `test_barre`,
  `test_vars_regle`, `test_entrees_8h`, `test_presentation_mac`) all run on the host
  via `scripts/verifier.sh`; `android/jni/tests/test_presentation_10a.c` compiles
  with the NDK in the full gate and runs only if `emulator-5554` is listed by
  `adb devices`.
- Java launcher: `android/test-javac.sh` compiles the launcher sources and runs
  `android/src/test/` (`EtatInstallTest`, `CatalogueTest`, `OptionsTest`); a
  repository checkout is all it needs.
- Phone session, one command: `scripts/test-matin.sh <device-id>` (its header lists
  the variables and the verdict contract); the bench/judge scripts behind it are
  the subject of [06-bench-and-regress.md](06-bench-and-regress.md).

### 2.5 Never publish a build

A built APK or `.app` bundle contains recompiled game code: never commit it, never
share it, never upload it ([LEGAL.md](../../LEGAL.md) §4, "built APKs or any binary
containing recompiled game code"). `scripts/paquet-mac.sh` enforces its side by
refusing any destination outside `~/code/sc2-apk/`, and the `legal-scan` CI fails a
push that adds such a file to the repository.

## Verify

```sh
scripts/verifier.sh --rapide            # mac/ and android/jni host tests green
bash android/test-javac.sh              # Java launcher tests: VERT
scripts/valider-apk.sh <your.apk>       # PASS, for an APK you built locally
```

**Next:** [06-bench-and-regress.md](06-bench-and-regress.md) — measuring, the benches,
and the two-level non-regression.
