# 03 — Rendering: the fake D3D9 device, the Vulkan backend, and the shader chain

This guide is for anyone (human or agent) who has to change, extend or debug the
D3D9-on-Vulkan renderer of a statically recompiled game: where each tier lives, how
state flows, which texture and surface traps have already been paid for, how the
offline shader chain turns the game's precompiled cache into validated SPIR-V, and
how to investigate a rendering bug with counters and frame fingerprints instead of
by eye. Everything here is grounded in files of this repository; the reference
method document (French) is `docs/RENDU-D3D9-VERS-VULKAN.md`, and the announced
capabilities are documented in `docs/CAPS.md`.

Scope reminder: the runtime operates on your own local install; nothing derived from
the game is ever added to this repository (`LEGAL.md`). A shader cache is read from
the user's own install and its translations stay on the user's machine.

1. [The three tiers and where they live](#1-the-three-tiers-and-where-they-live)
2. [The fake-COM D3D9 device](#2-the-fake-com-d3d9-device)
3. [State model: raw at Set, translated at Draw](#3-state-model-raw-at-set-translated-at-draw)
4. [Descriptors, samplers, push constants](#4-descriptors-samplers-push-constants)
5. [Textures and surfaces: the known traps](#5-textures-and-surfaces-the-known-traps)
6. [Caps: announce only what you implement (or what the game reads)](#6-caps-announce-only-what-you-implement-or-what-the-game-reads)
7. [Presentation](#7-presentation)
8. [The offline shader chain, step by step](#8-the-offline-shader-chain-step-by-step)
9. [The SPIR-V pack](#9-the-spirv-pack)
10. [Investigating a rendering bug](#10-investigating-a-rendering-bug)
11. [Verify](#11-verify)

## 1. The three tiers and where they live

A translation-layer port (Wine + DXVK) intercepts a DLL; a static recompilation has
**nothing to intercept** — the recompiled engine calls vtable addresses directly.
The device must therefore live inside the recompiled game's own address space. The
architecture that follows keeps three strictly separated tiers
(`docs/RENDU-D3D9-VERS-VULKAN.md` §1):

| Tier | File | Role | Game-dependent? |
|---|---|---|---|
| 1. Fake-COM D3D9 device | `runtime/sc2_d3d9.c` | COM objects in the guest arena, raw D3D9 state tracking, guest memory copies | Knows the COM/x86 ABI, not the game |
| 2. Pure Vulkan backend | `runtime/sc2_d3d9v.c` | Targets, samplers, pipelines, transfers — pure C, **zero guest state** | No — testable standalone |
| 3. Presentation | `android/jni/sc2_vk_presente.c` | Swapchain, blit, queues (Android); the mac bench loads MoltenVK directly | No — interposed at the loader |

Tier-2 purity is what makes the renderer maintainable: `runtime/tests/test_d3d9v.c`
runs the backend on Lavapipe/MoltenVK with **no game and no lifted code**, and the
same binary runs on Lavapipe (CI), MoltenVK (mac bench) and Adreno (device). The
instancing translation plan is a standalone header (`runtime/sc2_d3d9_inst.h`), and
resource release is deferred per frame slot (`runtime/sc2_d3d9_file.h`).

## 2. The fake-COM D3D9 device

`runtime/sc2_d3d9.c` (header comment: everything is "fake but coherent"). Engine
D3D9 objects are `[vtbl, ref, data…]` blocks allocated **in the guest arena** at
synthetic addresses; when lifted code calls `pDev->lpVtbl->Release(pDev)`, the
dispatch recovers object and method by subtraction, pops the stdcall arguments
word by word, and routes. Consequences:

1. An unknown method **stops the run naming itself** (interface, vtable number,
   caller) — "nothing is invented in silence". A fake success on a critical method
   costs hours of diagnosis; a named stop costs one log line
   (`docs/RENDU-D3D9-VERS-VULKAN.md` §0).
2. Deliberately no-op methods are counted `m_ok` and logged, never silent.
3. The API surface to implement is the **intersection** of the import table and
   what the trace shows the engine actually calls (§2 of the same doc). The
   game's IAT facts live in `interfaces/sc2_imports.h` (755 slots for the 2010
   client).

## 3. State model: raw at Set, translated at Draw

Founding choice of `runtime/sc2_d3d9.c` (state tables) + `runtime/sc2_d3d9v.c`
(translation), described in `docs/RENDU-D3D9-VERS-VULKAN.md` §3:

- The device stores **raw D3D9 state**: render states `rs[256]`, sampler states,
  texture stages, transforms, VS/PS constants with generation counters. Thousands
  of `Set*State` per frame become plain table writes — no Vulkan pipeline is
  touched, no synchronization, and the engine's `Get*State` read back the truth.
- Translation to Vulkan happens **only at draw time**, into a throwaway POD
  (`d3dv_dessin`: blend, colour mask, Z, cull, attribute formats, topology…).
- The **pipeline key is a whole struct hashed FNV over exactly the bytes compared
  by memcmp** — a state change gives another key, another pipeline; the hash
  cannot lie about state equality. Viewport and scissor are the only dynamic
  states.
- The `VkPipelineCache` is **persisted to disk** (atomic periodic writes), so
  pipeline compilation hiccups only happen on first launch.
- Fixed-function (transforms, lights, TSS) is memorized and served back at `Get*`
  but never translated: this game is 100% SM2/3 shader-driven. Check that fact for
  *your* game before committing to this size of project (§3, last note).

## 4. Descriptors, samplers, push constants

From `docs/RENDU-D3D9-VERS-VULKAN.md` §4 and the interface contract documented in
the header of `tools/d3d9-shaders/d3d9-vers-spirv.py`:

- Primary path: `VK_KHR_push_descriptor`. One set: bindings 0/1 = VS/PS constant
  UBOs, 2+k = sampler k, 18+k = comparison sampler (shadows). Updated per draw by
  `vkCmdPushDescriptorSetKHR` — zero allocation per frame.
- Fallback (extension missing — Lavapipe, Android emulator GPUs, or deliberate
  cutoff): one set per draw from the frame-slot pool, reset when the slot returns
  after its fence. Measured trap: in the fallback **all** bindings must be valid
  (Lavapipe dereferences the null view) — hence dummy white textures for unused
  samplers.
- Samplers: cache of compacted keys (filters, bias quantized to 1/16, maxmip,
  aniso, border reduced to the 3 canonical Vulkan colours).
- Push constants carry the two states translated on the fly in the shaders: the
  D3D9 half-pixel offset (VS) and the alpha test (PS).

## 5. Textures and surfaces: the known traps

`docs/RENDU-D3D9-VERS-VULKAN.md` §5 is the checklist; the short version, in the
order the traps cost us:

1. **Formats and software BC.** Direct-mapped formats get a table (A8R8G8B8 →
   B8G8R8A8, X8 → swizzle A=ONE, L8/A8L8 by swizzle, R5G6B5, 16/32-bit floats);
   others convert pixel by pixel. DXT1/2/3/4/5 → BC1/2/3 when
   `textureCompressionBC`, else **CPU decoding conformant to the Khronos S3TC
   spec** — the Adreno and emulators do not always have BC, and the software
   decoder (tested against the GPU to ±8) unlocked the full title screen where
   others see a flat colour.
2. **`LockRect(pRect)` is a feature, not a detail.** The glyph atlas is filled
   glyph by glyph with rectangled `LockRect`s; a device that ignores `pRect`
   smashes every glyph at (0,0) — a black UI with zero errors. `pBits` must point
   at the rectangle's top-left (containing 4×4 block for DXTn). The runtime
   counts these (`[BILAN] [TEX] LockRect/LockBox avec sous-rectangle` in
   `runtime/sc2_d3d9.c`).
3. **`GetRenderTargetData` must really read back.** The initial no-op silently
   blocked unit-sprite composition: the engine draws units *from* its own read-back
   render targets. Implementation: GPU RGBA readback → BGRA rewrite in guest
   memory (bounded 4096²).
4. **`D3DXLoadSurfaceFromMemory`**: a standalone-testable conversion core
   (`runtime/tests/test_d3dx_surface.c`) plus fault-safe guest reads via
   `process_vm_readv` on itself (EFAULT instead of SIGSEGV on a dubious guest
   pointer — tested against a `PROT_NONE` page).
5. **TGA**: the engine's D3DX layer only sees DDS; the map minimap (TGA, all
   sub-formats, RLE) is decoded to DDS in memory. Without it: black minimap, no
   error.
6. **Mips, cubes, volumes**: all levels uploaded by one multi-region
   `vkCmdCopyBufferToImage`, the chain bounded to levels actually present (game
   DDS files sometimes lie about the count); `AUTOGENMIPMAP` serves level 0 only
   (traced).

## 6. Caps: announce only what you implement (or what the game reads)

`docs/CAPS.md` is the authoritative table of the `D3DCAPS9` the device announces
(`caps_ecrire` in `runtime/sc2_d3d9.c`). The rule that made it safe:

1. A bit the game actually **reads** keeps its historical value (1), implemented
   or not — changing it would change the game's code path. Its real state and the
   work item that would implement it are written down per bit (e.g.
   `MaxUserClipPlanes`, `NumSimultaneousRTs`, `TextureAddressCaps` BORDER).
2. Every other bit states the **real** implementation (1 if the runtime or Vulkan
   backend does what the bit promises, 0 otherwise — including restrictions such
   as POW2-only, which were accidentally announced before).
3. Exploratory format refusals are env-driven (`SC2_FORMATS_REFUS`, see
   `docs/VARIABLES.md`): refusing a format is how you discover the engine's
   fallback paths — the R32F refusal revealed the shadow path.

Also in `docs/CAPS.md`: the DirectDraw adapter used for video-memory probing and
its GPU profiles (`SC2_GPU_PROFIL`: `historique` 8600 GT / `dx10` GTX 285), with
the measured effect on the settings the game writes.

## 7. Presentation

`docs/RENDU-D3D9-VERS-VULKAN.md` §7; the presenter is `android/jni/sc2_vk_presente.c`.

- **SUBOPTIMAL discipline.** On Android in native-portrait-held-landscape,
  `vkQueuePresentKHR` returns `VK_SUBOPTIMAL_KHR` on *every* frame. Recreating on
  SUBOPTIMAL starves the compositor (measured: 4,809 swapchain creations → black
  screen). Recreate only on `VK_OUT_OF_DATE`, SUBOPTIMAL **with a size change**,
  or a platform resize; ignore SUBOPTIMAL at constant size (logged once); bound
  the degradation — repeated failures fall back definitively to CPU copy.
  Presentation may never spin.
- **Two frames in flight.** Per-slot fences instead of `vkQueueWaitIdle`; binary
  semaphores consumed exactly once, including by a deliberate empty submission.
  This single lever took the game from 12.5 to 21.3 FPS. `SC2_VK_SYNCHRONE=1`
  (`docs/VARIABLES.md`) restores the old synchronous behaviour for A/B tests.
- **mac bench**: the same backend loads MoltenVK directly
  (`VK_KHR_portability_subset` enabled with a log of its limits: no `triangleFans`,
  `samplerMipLodBias` clamped to 0). Presentation is interposed at the loader
  (`--wrap=dlsym` on Android), never inside the backend.

## 8. The offline shader chain, step by step

You will likely never see HLSL: the game ships or builds a **precompiled shader
cache**; all you ever get is D3D9 SM2/3 **bytecode**. The chain
(`tools/d3d9-shaders/README.md`, `docs/RENDU-D3D9-VERS-VULKAN.md` §6.1):

```
game cache ──extraire-shaders.py──▶ bytecodes .bin
bytecode ──d3d9sm.py (SM2/3 decoder)──▶ instructions
instructions ──d3d9-vers-spirv.py──▶ GLSL 450
GLSL ──glslangValidator -V + spirv-val──▶ validated SPIR-V
SPIR-V + interface metadata ──▶ embedded table (C header) / pack (mmap binary)
```

| # | Script (in `tools/d3d9-shaders/`) | Input | Output | Notes |
|---|---|---|---|---|
| 1 | `extraire-shaders.py` | the game's precompiled cache blob (from your own install) | `cache/{vs,ps}_<hash>.bin` per distinct bytecode + `index-cache.tsv` (hash, type, version, bytes, offset, occurrences, instructions) | Scans for vs/ps version tokens at any position, validates token by token, dedupes by hash. `--boot <dump>` cross-checks the shaders created at boot against the cache (the runtime dumps them with `SC2_SHADERS_DUMP`, see `docs/VARIABLES.md`). |
| 2 | `d3d9sm.py` | one bytecode | decoded instructions (version, dcl/def/defi/defb, parameters with mask/swizzle/modifiers/relative addressing, CTAB) | Pure decoder, Python stdlib only. |
| 3 | `d3d9-vers-spirv.py` | a directory of `{vs,ps}_<hash>.bin` | the generated C header with the embedded SPIR-V table + per-shader interface metadata; optional `--glsl <dir>` keeps the intermediate GLSL, `--ombres <list>` also emits the shadow variant | Translates to GLSL 450, compiles with `glslangValidator -V` and validates with `spirv-val`. **Only validator-passing SPIR-V enters the table** — a broken shader never ships silently. An unhandled instruction (predicates, `call`/`label`, `oFog`/`oPts`, `t` registers in ps_3) leaves the shader untranslated and listed in the report; at runtime that draw falls back to the light shader and is counted. |
| 4 | `ppm-blocs.py` | rendered PPM frames | per-image 16×16 block statistics (mean RGB, non-black blocks, luminance min/max/stddev, count of **distinct** block means — a flat colour has 1) | Render-regression detector; `--avant <dir>` counts blocks changed vs a reference run. |
| 5 | `ppm2png.py` | PPM (P6) | PNG | Pure stdlib (`zlib` + `struct`), optional brightness gain. |

Identity everywhere is the **FNV-1a 64 of the bytecode, token by token, comments
skipped** — the same hash at runtime (when the engine creates the shader), in the
translator and in the pack. Interface metadata (VS input declarations, sampler
types) is stored *next to* the SPIR-V, so the backend matches D3D9 declaration ↔
VS inputs at draw time **without re-parsing the bytecode**.

The frozen interface contract (before generating anything — every divergence costs
a full table regeneration; see the header of `d3d9-vers-spirv.py`):

- **Y flipped** in the VS output (D3D9 renders "top-down"), **Z [0,1] kept**;
- **16 distinct `vec4` varyings, no arrays** — the Adreno refuses array↔scalar
  matching between VS/PS stages (a paid hardware lesson);
- half-pixel (VS) and alpha-test (PS) via **push constants**, not recompilation;
- shadow variant as a **specialization constant** (ID 100+k): one PS binary serves
  both paths, and the default reproduces the previous output byte for byte.

**The generated table is a local artifact.** `sc2_shaders_spv.h` is produced on
each user's machine from their own cache (the runtime includes it via
`#if __has_include`, see `runtime/sc2_d3d9.c`); neither the bytecodes nor the
SPIR-V ever enter this repository (`LEGAL.md`). Coverage strategy: translate what
the played scenes need, count every fallback draw, capture more bytecodes as you
reach new scenes.

## 9. The SPIR-V pack

When the table grows to the whole cache, move to an **mmap pack**:
`docs/PACK-SPIRV.md` (format v1) — 64-byte header (magic `SC2SPV01`), a sorted
96-byte-per-entry index (FNV hash, `ps` flag, lengths, offsets, 16 VS-input
entries, 16 sampler types), 16-byte-aligned SPIR-V blobs, uncompressed. The
loader is `runtime/sc2_spv_pack.c` (fallback of the embedded table, path from
`SC2_SPIRV_PACK` or next to `SC2_INSTALL` — `docs/VARIABLES.md`);
`runtime/tests/test_spv_pack.c` builds conformant and corrupted packs and checks
both opening paths. `docs/PACK-SPIRV.md` credits the pack generation to the
translator; the published copy in `tools/d3d9-shaders/` documents its own
options in its header — read it before generating.

## 10. Investigating a rendering bug

The discipline (`docs/RENDU-D3D9-VERS-VULKAN.md` §0.3 and the case study §6.6):
**instrument first, compare by number, never by eye**.

1. **State counters before screenshots.** The runtime prints a `[BILAN]` block at
   the end of every run (`runtime/sc2_d3d9.c`): draws real vs "light" (fallback),
   per-state draw counts (`[BILAN] [ETATS]`), instancing (`[BILAN] [INST]`), pack
   service (`[BILAN] [SPV]`), texture fallbacks (`[BILAN] samplers des draws
   réels`), most-used shaders. A well-placed fallback counter found the bar-glass
   bug (57% of draws skipped → targeted capture of 355 bytecodes → fallback draws
   from 18,387 to 278) where six visual hypotheses had failed.
2. **Frame fingerprints.** Hash of the presented image, compared across runs —
   `scripts/comparer-captures.sh` gives a numbered AE/PSNR per image pair
   (`--max-ae`/`--min-ae` to assert a change or its absence), and
   `tools/d3d9-shaders/ppm-blocs.py` (or `scripts/ppm-blocs.py`) tells geometry
   from flat colour and counts blocks changed against a reference run. Only
   compare **stable** frames (a known frame-10 race taught us to pick frames that
   do not depend on thread timing).
3. **A/B captures.** `scripts/capture-ab.sh` replays the same scenario under two
   settings with interleaved runs, computes per-capture AE in the zone painted by
   a diagnostic run, and separates effect from noise. Device-side, A/B/A is the
   rule (guide 06).
4. **Two GPUs, two truths.** A bug invisible on the mac bench (fresh zeroed
   memory) can be visible on Adreno (recycled memory) — the same bar-glass story.
   When the bench says "fine" and the device does not, suspect memory
   initialization, then test on hardware.
5. **2D renders, 3D black?** Suspect the lift's matrix math, not the renderer —
   one wrong SSE-lane character in `movhps` lifting did exactly this (guide 04,
   `tools/pcrecomp-movhps/README.md`).
6. **Engine runs, screen black?** Check audio init: an engine without its sound
   may loop on re-init and never present (`docs/D3D9-PORTING-GUIDE.md` §12).
7. **When all is green and it still fails**, the missing piece may be *content*
   absent from your install, not a bug (§12.10 of the same guide).

## 11. Verify

None of these need game data. Run them from the repository root:

```sh
# 1) Host tests of the pure backend and the pack (test_d3d9v SKIPs at 77 without Vulkan)
cmake -S tests-hote -B build-tests -G Ninja && cmake --build build-tests -j8
ctest --test-dir build-tests --output-on-failure

# 2) Standalone backend test (needs a Vulkan loader; writes test_d3d9v.ppm)
./build-tests/test_d3d9v

# 3) Shader-chain scripts self-check (stdlib only): print their usage contract
python3 tools/d3d9-shaders/extraire-shaders.py   # usage (exit 2 without args)
python3 tools/d3d9-shaders/ppm-blocs.py          # column header (exit 0)

# 4) Bench-input module tests (drives the state-machine the renderer consumes)
(cd scripts && python3 -m unittest test_nonreg)

# 5) Syntax gate over every shell script you may have touched
bash -n scripts/comparer-captures.sh && bash -n scripts/capture-ab.sh
```

Expected: step 1 green (a known macOS SKIP is allowed, see
`tests-hote/echecs-connus.sh`); step 3 prints each script's argument contract;
steps 2, 4 and 5 end with `OK` / exit 0.

**Next:** [04 — Lift and oracle](04-lift-and-oracle.md) — pcrecomp, the CPU-struct
model, the lifter patches and the per-function differential oracle.
