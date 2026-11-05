# 01 — Install the toolchains and get the verification gate green

This guide takes you from a fresh clone of `sc2-android-recomp` to a green
`scripts/verifier.sh`: which tools each host actually needs (with the versions and
default paths the scripts expect), where build outputs land, and what each of the
gate's checks does. Read [AGENTS.md](../../AGENTS.md) first — it sets the scope and
the hard rules every change must keep — and [LEGAL.md](../../LEGAL.md) for what this
public repository distributes (original tooling only) and what never enters it
(anything derived from the game).

## 1. What the repository builds, on which host

| Host | Role | Needed for the gate |
|---|---|---|
| macOS, Apple Silicon | development machine, native bench, macOS app (`mac/`) | yes (all 33 host tests) |
| Linux, x86-64 or arm64 | build machine, headless Vulkan runs | yes (host tests; `test_win32_compat` runs in full here) |
| Android phone / emulator | target of the port (`android/`) | no — checks that need one report SKIP |

The gate itself builds **only original code**: the runtime written by hand in
`runtime/`, the pure-C parts of `android/jni/` and `mac/`. It never compiles lifted
game code, never reads game data, and never touches a phone.

## 2. Install the tools

### 2.1 macOS (Apple Silicon)

1. Xcode command-line tools (Apple `clang`, `git`, `perl`), then Homebrew packages
   used by every script: `cmake` (≥ 3.20, the minimum of `tests-hote/CMakeLists.txt`),
   `ninja`, `python3`.
2. Vulkan headers and MoltenVK — required for the real-draw tests:
   `brew install vulkan-headers molten-vk`, then `brew pin molten-vk` (the pin freezes
   the sha256 that `scripts/paquet-mac.sh` checks; see the Prérequis table in
   [../BANC-MAC.md](../BANC-MAC.md)). Without the headers, `test_d3d9v` and
   `test_d3dx_surface` are configured as SKIP; without MoltenVK at run time,
   `test_d3d9v` and `test_presentation_mac` exit 77 (SKIP).
3. The Android NDK, only if you want gate check 2 to compile: unpack r28 at
   `~/tools/android-ndk-r28` (the default of `ANDROID_NDK` in `scripts/verifier.sh`),
   or export `ANDROID_NDK=<path>`.

Two more checkouts are needed **only** for the full game build (the bench binary),
never for the gate: StormLib at `~/tools/StormLib` and the libogg/libtheora sources
under `~/tools/theora` (see [../BANC-MAC.md](../BANC-MAC.md), Prérequis).

### 2.2 Linux (build machine)

1. `cmake` ≥ 3.20, `ninja` (optional — CMake falls back to the default generator),
   `gcc` or `clang`, `python3`, `bash`, `perl`.
2. Vulkan headers (`/usr/include/vulkan/vulkan.h`) so `test_d3d9v` compiles; the
   software Lavapipe driver if you want headless rendering (the method's dev-machine
   setup: [../METHOD.md](../METHOD.md) §5).
3. The NDK for check 2 and for the APK chain (same default `~/tools/android-ndk-r28`).

### 2.3 Android toolchain (for `android/`, not for the gate)

| Tool | Version / default | Where it is read |
|---|---|---|
| NDK | r28; `r28c (28.2.13676358)` is the tested version; default `$ANDROID_NDK`, then `~/tools/android-ndk-r28` | `scripts/verifier.sh`, `scripts/valider-apk.sh`, `android/build-apk.sh` |
| SDK | build-tools `35.0.0` pinned by `android/build-apk.sh`; `android/build-apk-mac.sh` and `scripts/valider-apk.sh` take the newest; default `$ANDROID_SDK_ROOT`, then `~/tools/android-sdk` | same scripts |
| JDK | `$JAVA_HOME` wins where it is read (`scripts/valider-apk.sh`, `android/build-apk.sh`, which falls back to a `jdk-21*` under `$SC2_TOOLS`); `android/build-apk-mac.sh` hardcodes Homebrew `openjdk@17` | same scripts |
| adb | from platform-tools | every device script |

`SC2_TOOLS` (default `~/tools`) is the common root the scripts fall back to.

## 3. Clone and look around

```sh
git clone https://github.com/elkir0/sc2-android-recomp
cd sc2-android-recomp
ls runtime/ tests-hote/ mac/ android/ scripts/
```

No submodules, no downloads at clone time. Two layout notes that save an hour:

- Several scripts and headers keep the paths of the private working tree
  (`recomp-sc2/...` there is `runtime/` here); the README section "What's in this
  repository" says so explicitly. When a script mentions `recomp-sc2/`, read it as
  the private side that each user generates locally.
- Build directories (`build-tests/`, APK builds, `.app` bundles) are never committed;
  `git status` should stay clean apart from the files you edit.

## 4. Where outputs go — and what never goes anywhere

| Path | Content |
|---|---|
| `build-tests/` | the gate's CMake build of `tests-hote/` and the ctest run |
| `build-tests/verifier/` | one log per check: `cmake.log`, `build.log`, `ctest.log`, `10a-cc.log`, `python.log`, `matin-*`, `verrou-*`, `variables.log`, `apk.log` |

Everything derived from the game — lifted C, extracted archives, shader caches and
SPIR-V packs, built APKs and `.app` bundles, captures, logs quoting game data — is
produced **locally, from your own legitimate copy, and stays on your machine**
([LEGAL.md](../../LEGAL.md)). The `legal-scan` CI (`.github/workflows/legal-scan.yml`)
fails the push on what it can detect: game extensions, executables, files over 20 MB,
images outside the curated set, personal data (home paths, IPs, e-mails).

## 5. Run the gate

```sh
scripts/verifier.sh --rapide    # while iterating: skips checks 2 and 3
scripts/verifier.sh             # the gate — the command that must end green
```

- One `[OK]` / `[SKIP]` / `[ÉCHEC]` line per check; last line
  `verifier : N OK, M SKIP, K ÉCHEC en S s`; exit code 0 only with zero `ÉCHEC`.
- `scripts/verifier.sh --help` prints the header comment (every script documents
  itself this way — read it before running any script in `scripts/`).
- Two gates in the same tree serialize on `build-tests/.verifier.verrou`.
- `JOBS` sets the compile parallelism (default 8); `ANDROID_NDK` the NDK path.
- Runtime: under two minutes, CMake cache hot or cold.

### What each check does

| # | Check | Meaning | Without a phone / emulator |
|---|---|---|---|
| 1 | host tests | builds `tests-hote/` (-O0, no lifted code) into `build-tests/`, runs `ctest --output-on-failure`; one line per test | runs — no device involved |
| 2 | `test_presentation_10a` | compiles the Android presentation test with the NDK clang, then runs it on the AVD `emulator-5554` **only if** `adb devices` lists it (the emulator is never launched here) | compile needs the NDK (SKIP if absent); the run SKIPs without the emulator |
| 3 | Python unit tests | `python3 -m unittest` over the `test_*.py` suite of `scripts/` (212 tests at last count) | runs |
| 4 | `bash -n` | syntax check of every `.sh` of the tree (excluding `build*`, `extraction`, `.venv`, `tools`) | runs |
| 5 | verdict fixtures | `scripts/verdict-fold.sh` over `scripts/verdict-fixtures`; expected verdicts live in the `VERDICTS` table of `scripts/verifier.sh` | runs |
| 6 | `test-matin` in `ADB_FAKE` | the morning-test path on a **fake** adb: delivered, not delivered, stale second `sc2.env` | runs — a fake `adb` at the head of `PATH` refuses every real call (no phone touched) |
| 7 | lock discipline | `scripts/verrou.sh`: a second simultaneous take is refused (75), served after a bounded wait; real locks in `~/.sc2-verrous` are never held | runs |
| 8 | variables | `scripts/variables-sc2.sh verifier`: every `SC2_*` variable read by the code must be documented in [../VARIABLES.md](../VARIABLES.md) | runs |
| 9 | `bar-fold` in `ADB_FAKE` | the shortcut-bar bench against fixtures | runs (same fake-adb guard) |
| 10 | `seance-fold` in `ADB_FAKE` | a full scripted phone session against fixtures | runs (same fake-adb guard) |
| 11 | `--apk <file>` | `scripts/valider-apk.sh` on an APK you built (pinned signing key, lift not `-O0`…) | not run by default — it requires an APK |

Checks 6, 9 and 10 are the reason the gate is safe on a machine with a phone
attached: the fake `adb` exits 97 on any real call, and a script that tried one
fails its check.

On macOS, two tests are expected to report SKIP with a reason: `test_win32_compat`
(four known failures, listed one by one in `tests-hote/CMakeLists.txt`, wrapped by
`tests-hote/echecs-connus.sh` so that only those failures — and only them — mean
SKIP), and the Vulkan tests of §2.1 when MoltenVK is missing.

## 6. What the gate does not build

The full game binary (lift, SPIR-V shader table, game data) is generated on the
private side of each user's machine and is not part of this repository; the same
goes for obtaining the game's files in the first place. Out of scope for this
repository — see [AGENTS.md](../../AGENTS.md#scope).

## Verify

```sh
scripts/verifier.sh --rapide          # expect: 0 ÉCHEC, a few SKIP with reasons
ctest --test-dir build-tests --output-on-failure -R test_documents   # one test alone
git status --porcelain                # only the files you meant to touch
```

A green `--rapide` run plus a clean `git status` is the baseline every task starts
from; the full gate is what you hand back.

**Next:** [02-runtime.md](02-runtime.md) — find your way in `runtime/`, keep the
Win32 layer truthful, add a host test.
