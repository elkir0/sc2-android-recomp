# StarCraft II on Android — Static Recompilation Toolkit

**[Version française 🇫🇷](README.fr.md)**

[![legal-scan](https://github.com/elkir0/sc2-android-recomp/actions/workflows/legal-scan.yml/badge.svg)](https://github.com/elkir0/sc2-android-recomp/actions/workflows/legal-scan.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![No game content](https://img.shields.io/badge/contains-no%20game%20code%20or%20assets-green)

**From a 15.7 MB x86-32 Windows executable to a playable, signed Android APK with a
native Vulkan renderer — via AI-assisted static recompilation.** One week of work took
StarCraft II: Wings of Liberty (2010) from "it boots" to "the first campaign mission
plays end-to-end at 28 FPS on a phone". This repository is the complete method and
toolchain, validated end-to-end, so you can do the same to **your** D3D9-era game.

> **It contains no game code and no assets — never will.** Everything derived from the
> game is generated locally, from your own legitimate copy, by the tools published here.
> A CI scanner enforces this on every commit. Offline / single-player only.
> StarCraft® and Blizzard Entertainment® are trademarks of Blizzard Entertainment, Inc.
> Independent, non-commercial, unaffiliated project. Details: [LEGAL.md](LEGAL.md).

## Does it work? (October 2026)

| | | |
|---|---|---|
| ![Campaign menu](docs/menu-glue-single-f1000-campagne.png)<br/>Campaign menu, Vulkan | ![Menu + 3D scene](docs/menu-echap-f2500-scene-sans-ui.png)<br/>Menu & 3D scene | ![Mission loading](docs/launch-f3600-liberation-day-chargee.png)<br/>Mission loading on device |
| ![Mission selection](docs/mission-selection-f2400-liberation-day.png)<br/>Mission-selection console | ![Cinematic](docs/ok-normal-f1700-tstory01-raynor.png)<br/>In-engine cinematic | ![Engine dashboard](docs/tumor-b-f1500-mainui-dashboard.png)<br/>Engine UI driven from the runtime |

*Captured from our own legitimate copy, for documentation purposes only.*

| At a glance | |
|---|---|
| Functions lifted to portable C | **86,653** (420 MB of C, builds on x86-64 Linux, Apple Silicon, aarch64 Android) |
| Certified against a hardware-reference emulator | **76,650** functions, **0 true divergence** |
| First campaign mission | **played end-to-end on a Galaxy Z Fold 8** (BT keyboard + mouse, live HUD, victory) |
| Performance in active play | **28.1 FPS** (from 4.1 at the first playable build — ×6.9), GPU never limiting |
| Audio | mission soundtrack + voice reconstructed **bit-exact** |
| Video | intro cinematic decodes **in real time on-device** (the game's own lifted Theora decoder) |
| Shaders translated | 2,856 of the 75,797 in the game's cache (growing — see roadmap) |

## Project status

| Subsystem | State | Notes |
|---|---|---|
| Static recompilation (x86-32 → portable C) | ✅ done | CPU-struct reentrant model; generic lifter patches published (atomics, MMX, SSE lanes) |
| Vulkan renderer (D3D9 → Vulkan) | ✅ core complete | title, menus, 3D scenes, terrain, HUD, minimap, units; shadows opt-in; MRT & sRGB pending |
| Audio (FMOD Ex → software mixer → AAudio) | ✅ | bit-exact reconstruction, proof by WAV entropy |
| Video (Theora) | ✅ | real-time on device; optional H.264 hardware path |
| Input | ✅ | touch → Win32 mouse; BT keyboard/mouse; DeX |
| Android packaging | ✅ | reproducible signed APK (NDK r28, NativeActivity) |
| Performance | 🟡 in progress | 28.1 FPS, CPU-bound in the lifted code (~19 ms of a ~35 ms frame) |
| Shader coverage | 🟡 in progress | 2,856 / 75,797; full pack generator & loader under construction |
| Campaign progression | 🔴 next | saves wiped at launch; persistence is the next work item |
| Campaign beyond mission 1 | 🟡 | mission 2 loads; automated sweep of all missions planned |

The full dated milestone log (35+ entries with the measurement behind each claim):
[docs/MILESTONES.md](docs/MILESTONES.md).

## Roadmap

| # | Milestone | Why it matters |
|---|---|---|
| 1 | **Full SPIR-V pack** — translate all 75,797 cached shaders into an mmap pack (~2 GB, format specced in [docs/PACK-SPIRV.md](docs/PACK-SPIRV.md)) | any unplayed scene currently loses draws (stale render targets); this closes shader coverage forever |
| 2 | **Save & progression persistence** | the emulated `C:\` is wiped at launch — campaign progress doesn't survive a reboot |
| 3 | **Automated campaign sweep** — cheat-code-driven playthrough of every mission at the bench | turns "does mission N work?" into a table of missing functions, per mission |
| 4 | **Real campaign menu entry** (no forced `-run`) | the proper player journey, fullscreen |
| 5 | **Remaining CPU levers** — constant-block range upload (only the span the shader reads), SSE as locals | the last ~6 MB/frame of `memmove` and the path to 60 FPS |

## Port YOUR D3D9 game — the recipe

Everything above generalizes. The full step-by-step recipe — decision tree, inventory,
static analysis, lift, host runtime, the D3D9 device, shader translation, presentation,
the correctness oracle, the performance levers *in the order that paid*, effort
calibration and a pitfall checklist — is in
**[docs/D3D9-PORTING-GUIDE.md](docs/D3D9-PORTING-GUIDE.md)**. The short version:

1. Hash & extract your own copy; prune A/B to find the minimal dataset.
2. Ghidra headless: boot map + the import table as your API contract.
3. Lift with pcrecomp (CPU-struct model); apply the generic patches we published.
4. Host runtime: fibers, guest arena, traced IAT — **refuse, don't lie**.
5. D3D9 device in three tiers; prove the plumbing with a degenerate triangle first.
6. Translate the game's shader cache offline (decoder + translator here); validate
   every SPIR-V; hash bytecode by FNV everywhere.
7. Present headless → native bench → device; SUBOPTIMAL discipline; two frames in flight.
8. Difftest every function against a hardware-reference emulator; fingerprint frames; A/B/A.
9. Optimize in measured order: -O2 lift, O(1) dispatch, no TLS, fences, ring, locals.

## What's in this repository

| Path | What it is |
|---|---|
| `runtime/` | the **D3D9-on-Vulkan backend & host runtime** (original work): fake-COM device in the guest arena, guest-free Vulkan backend, presentation, software FMOD Ex, Theora/AMediaCodec video |
| `tools/difftest/` | the **per-function oracle**: lifted function vs Unicorn hardware reference, every register/flag/byte diffed |
| `tools/d3d9-shaders/` | **SM2/3 bytecode → SPIR-V**: decoder, translator, shader-cache extractor, frame metric |
| `tools/pcrecomp-*/` | generic lifter patches: atomics, MMX, `movhps`/`movhpd`, `movhlps`/`movlhps`, disasm32 memory |
| `tools/lift/` | incremental closure, dead-flag elimination, MMX inventory, relift guard |
| `tools/mpq/`, `tools/audio/` | size-checked MPQ extract/repack; MSVC-decorated import decoder; WAV proof checker |
| `scripts/` | measurement & optimization: native bench (`banc-mac.sh`), on-device A/B/A (`fold-ab*`), cinematic bench, lift transforms (locals, direct calls) |
| `android/` | the port: NativeActivity glue, bionic fibers, swapchain presenter, reproducible APK chain, validation benches |
| `interfaces/` | generated interface facts (PE layout, 755-slot IAT) + lift counters |

## Documentation

| Doc | Content |
|---|---|
| [docs/D3D9-PORTING-GUIDE.md](docs/D3D9-PORTING-GUIDE.md) | **the recipe to port your own D3D9 game** |
| [docs/RENDU-D3D9-VERS-VULKAN.md](docs/RENDU-D3D9-VERS-VULKAN.md) 🇫🇷 | the complete D3D9→Vulkan translation method (state model, descriptors, textures, shaders, shadows, presentation, transfers, 15 counter-intuitive decisions) |
| [docs/RETROSPECTIVE-RECOMPILATION.md](docs/RETROSPECTIVE-RECOMPILATION.md) 🇫🇷 | the 7-day engineering retrospective: milestones, 14 major difficulties, measurement discipline, measured dead ends |
| [docs/METHOD.md](docs/METHOD.md) | the end-to-end pipeline in one page |
| [docs/MILESTONES.md](docs/MILESTONES.md) | the full dated milestone log |
| [docs/BRICKS.md](docs/BRICKS.md) | build bricks: Theora decoder cross-compile, local video transcode |
| [docs/PACK-SPIRV.md](docs/PACK-SPIRV.md) 🇫🇷 | the SPIR-V pack format |
| [docs/BANC-MAC.md](docs/BANC-MAC.md) 🇫🇷 | the native bench method |

## License & legal

MIT for the tooling in this repository — original work, no Blizzard content. Built on
[pcrecomp](https://github.com/sp00nznet/pcrecomp) (MIT); see [LEGAL.md](LEGAL.md) for
the full policy (what is published, what never will be, third-party credits) and the
CI guard that enforces it.
