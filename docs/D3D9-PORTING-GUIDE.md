# Porting a D3D9 game to native ARM64/Vulkan — the recipe we validated

*This is the step-by-step recipe we validated on StarCraft II: Wings of Liberty
(2010, 15.7 MB x86-32 MSVC executable, 86,653 discovered functions, ~7.5 GiB of
data): from the disc image to a playable, signed Android APK with a Vulkan
renderer — in seven calendar days of AI-assisted work. Every tool referenced here
is in this repository; every rule below was learned the hard way (see the
[retrospective](RETROSPECTIVE-RECOMPILATION.md) for the war stories behind each).*

**The one rule that cannot bend:** work on your own legitimate copy only. The lift,
the translated shaders, the repacked data — everything derived from the game is
generated locally on your machine and never distributed. This repo ships method
and tools, nothing else.

## 0. Is static recompilation right for your game?

| Question | Answer that makes this recipe fit |
|---|---|
| Architecture | x86-32 executable (x86-64: pcrecomp has a 64-bit disassembler too) |
| Graphics | D3D9, shader-driven (SM2/3) with little fixed-function use |
| Target | Offline / single-player capable (you decide what the game really needs at boot) |
| Ground truth | The original runs on a machine you control, for differential testing |
| Size | A ~15 MB executable lifted to ~420 MB of C compiled fine for us; plan RAM accordingly |

If your game is fixed-function-heavy (pre-~2002) or online-mandatory, a translation
layer (Wine + DXVK + Turnip on Android) is dramatically cheaper — that was our own
plan B, documented for memory. Static recompilation pays off when you want a
*native* binary: no translation overhead, full control of the rendering stack,
and a per-game optimization surface (we went 4.1 → 28.1 FPS by optimizing the
lifted code itself, something no translation layer can do).

## 1. Inventory (day 0)

1. **Hash everything** before touching it (ISO, executables, extracted files) —
   you will need stable identities all along, and it is your legal hygiene.
2. Identify the container format: 2008-2013 Blizzard titles use stream-encrypted
   MPQ (`.MPQE`) whose keys the publisher itself published (see StormLib's
   `FileStream.cpp`); other engines use CASC, bigfiles, loose installs.
3. Extract engine binaries + data with **size-checked** tools — beware the
   partial-block EOF trap (a naive `while (SFileReadFile(...))` loop silently
   truncates; our `tools/mpq/` handles it).
4. **Prune A/B to find the minimal dataset**: remove one dataset, observe the
   exact breakage, repeat. We proved the offline campaign needs ~7.5 GiB and
   never reads the launcher/matchmaking content — each removal broke precisely
   the expected feature. This method is cheap and its output is your install
   list for the device.

**Deliverable:** a reproducible extraction script + an inventory with hashes.

## 2. Static analysis (day 0-1)

1. Import the executable into **Ghidra headless** with bounded memory
   (`Xmx12g` + cgroup). Triage mechanically: entry point, CRT, import table,
   strings, section map.
2. Build the **boot map**: entry → CRT init → WinMain → fiber/game main → staged
   init. Strings like "Must run from the launcher" tell you exactly where the
   launch-chain checks live.
3. **The IAT is your API contract.** List every import; the graphics/audio/win32
   surface you must reimplement is exactly this list, no more. Ours: 755 imports,
   of which just 18 for d3d9/d3dx9 — the D3D9 device is a small, knowable target.

**Deliverable:** boot map + the API contract list (see `interfaces/` for ours).

## 3. Lift to portable C (day 1)

Use [pcrecomp](https://github.com/sp00nznet/pcrecomp)'s pipeline:
`pe_analyze` → `disasm32` (recursive descent) → `lift32_cpu`.

- The **CPU-struct model** (explicit `CPU *c` state, all guest memory accesses
  through `rd*/wr*` helpers) is reentrant and multi-thread friendly — the single
  most important architectural choice of the whole project.
- Expect **~1 line of C per byte of machine code** (86,653 functions → 420 MB).
  Compile the result as a normal host program: the same C builds on x86-64
  Linux, Apple-Silicon macOS and aarch64 Android.
- Lifter gaps are normal; ours needed six generic patches, all reusable:
  [atomics](../tools/pcrecomp-atomics/) (`xchg`, `lock` RMW, `cmpxchg8b`),
  [MMX](../tools/pcrecomp-mmx/) (mm0-mm7 as CPU fields, ~50 mnemonics),
  [movhps/movhpd](../tools/pcrecomp-movhps/) (the high SSE lane — get this
  wrong and every 4×4 matrix product degenerates while 2D keeps working),
  [movhlps/movlhps](../tools/pcrecomp-movhlps/), the
  [disasm32 memory patch](../tools/pcrecomp-disasm32/) for >15 MB binaries,
  and rsqrt semantics (`tools/lift/`).

**Deliverable:** a binary that runs and dies on the first unimplemented import —
that crash is your runtime's first work item.

## 4. Host runtime — the "fake Win32" (day 1-2)

Implement what the engine actually calls, nothing else (the full design lives in
`runtime/`, directly reusable):

- **Fibers** (host `ucontext`; a bionic-compatible variant with aarch64 asm in
  `android/jni/sc2_ucontext*`), a 32-bit handle table, TLS/TEB.
- **The guest-arena trick for hosts without fixed low mappings** (macOS,
  Android): reserve one anonymous host region for the whole guest address space
  and funnel every guest-VA access through `GPTR(va) = base + va`. Because the
  CPU-struct model already funnels all memory through one pair of helpers, this
  is a ~30-line change.
- **IAT dispatch** with traced stubs: every import call is logged, arity-known
  (decode MSVC-decorated C++ imports mechanically — our
  [decoder](../tools/audio/decode-msvc-audio-imports.py) reconstructs a whole
  FMOD Ex API table from decorated names).
- **Faute-safe guest reads** via `process_vm_readv` on yourself (EFAULT instead
  of SIGSEGV on a dubious guest pointer).
- **Refuse, don't lie:** an unimplemented import or COM method must stop the run
  *naming itself*. A fake success costs hours; a named refusal costs one log line.

**Deliverable:** boots to the game's main loop on the host.

## 5. The D3D9 device (day 2-4)

The complete method is [RENDU-D3D9-VERS-VULKAN.md](RENDU-D3D9-VERS-VULKAN.md)
(FR) — the architecture in brief:

- **Three tiers, strictly separated**: (1) a fake-COM D3D9 device living in the
  guest arena (objects are `[vtbl, ref, data]` blocks at synthetic addresses the
  runtime decodes arithmetically), (2) a pure-C Vulkan backend with **zero**
  guest state — testable standalone on Lavapipe/MoltenVK/Adreno, (3) a
  presentation layer interposed at the loader (`--wrap=dlsym`), never inside the
  backend.
- **Prove the plumbing before the pixels**: the "light mode" draws a real
  pipeline with a degenerate off-screen triangle — all the Vulkan plumbing runs
  and is measurable while zero pixels can lie to you.
- The traps that cost us the most, in order: `LockRect(pRect)` ignored (a glyph
  atlas silently smashed into (0,0) → unreadable UI), `GetRenderTargetData` as a
  no-op (the engine composites unit sprites from its own render targets →
  missing units), D3DX accepting only DDS (the TGA minimap silently dropped),
  no-BC GPUs needing a software DXTn decoder tested against the spec.
- Announce a **coherent D3DCAPS9** (we claimed a 2007 GeForce) and keep
  exploratory format refusals behind an env var — refusing a format is how you
  discover the engine's fallback paths (ours revealed the shadow path).

**Deliverable:** first real pixels, headless (Lavapipe), measurable
("N non-black pixels" is a milestone you can diff).

## 6. Shaders — translate the cache, never the source (day 2-5)

You will likely never see HLSL: the game ships or builds a **shader cache**
(ours: `baselineCache.bin`, 75,797 distinct SM2/3 bytecodes).

1. **Extract** the cache (`tools/d3d9-shaders/extraire-shaders.py`).
2. **Decode** SM2/3 bytecode (`d3d9sm.py`).
3. **Translate** to GLSL 450 then SPIR-V via `glslangValidator -V` +
   `spirv-val` (`d3d9-vers-spirv.py`). Only validator-passing SPIR-V enters
   the table — a broken shader must never ship silently.
4. **Freeze the interface contract before generating anything**: y-flip, z
   range, 16 distinct `vec4` varyings (arrays break on Adreno), half-pixel
   offset and alpha-test via push constants, and interface metadata (VS input
   declarations, sampler types) stored *next to* the SPIR-V.
5. **Identity = FNV-1a 64 of the bytecode, token by token, comments skipped** —
   the same hash at runtime, in the translator and in the pack.
6. Coverage strategy: translate what the played scenes need (ours: 2,856 of
   75,797), count every draw that falls back ("light draws"), capture more
   bytecodes as you reach new scenes. **A skipped draw leaves a stale render
   target** — on some GPUs that shows as ghosts of previous scenes (the
   bar-glass story in the retrospective).
7. When the table grows, move to an **mmap pack** (format:
   [PACK-SPIRV.md](PACK-SPIRV.md)).

**Deliverable:** the title screen renders with the game's own shaders.

## 7. Presentation (day 4-5)

- Headless first; then the same backend on a **native bench** (MoltenVK on a
  Mac gave us the Fold's architecture and compiler at 10× the iteration speed);
  Android last.
- The **SUBOPTIMAL discipline**: on Android, `vkQueuePresentKHR` returns
  `VK_SUBOPTIMAL_KHR` on *every* frame in native-portrait-held-landscape.
  Recreating on SUBOPTIMAL starves the compositor (4,809 swapchain creations →
  black screen). Recreate only on `VK_OUT_OF_DATE`, on SUBOPTIMAL *with a size
  change*, or a platform resize. Bounded degradation: repeated failures fall
  back to CPU copy *definitively* — presentation may never spin.
- **Two frames in flight**: per-slot fences instead of `vkQueueWaitIdle`;
  binary semaphores consumed exactly once, empty submissions included. This one
  lever took us from 12.5 to 21.3 FPS.

**Deliverable:** the game on a phone screen, one swapchain creation per session.

## 8. Audio and video (parallel tracks)

- Reconstruct the engine's audio API from decorated import names, implement a
  software mixer, and **prove it bit-exact**: dump the mix to WAV and check
  duration/entropy/RMS (a real soundtrack is neither flat nor uniform noise).
  Ours reconstructs the mission soundtrack byte-identically.
- Video: don't assume you must transcode — after lift optimization the game's
  *own* Theora decoder ran in real time on the phone (24.9 fps for 24). Keep a
  hardware path optional (`scripts/video-transcode.sh` + `SC2_VIDEO_AMC=1`).

## 9. Prove correctness continuously — the oracle

This is what lets you change the lift fearlessly:

- **Per-function differential testing** (`tools/difftest/`): run any lifted
  function with a seeded random CPU/memory state, replay the *original bytes*
  in Unicorn as the hardware reference, diff every register, flag and written
  byte. We swept **76,650 functions**; every initial mismatch was a harness
  artifact (simulated return frame, nulled IAT); after fixes, **0 true
  divergence**. Every lifter patch above was certified this way.
- **Frame fingerprints** (hash of the presented image) as the non-regression
  oracle — on *stable* frames only (our frame 10 depended on a thread race;
  the queue advanced in wall time).
- **A/B/A**: replay the reference build after the candidate — game content
  alone is worth ±15 ms of CPU per frame.
- **Three benches, three roles**: CI/dev machine (software GPU — only CPU
  metrics mean anything), a native bench of the target architecture (6-minute
  builds, honest memory costs), the real device as final judge. Profile on
  device with `simpleperf`; cross-check any sampler against
  `/proc/<pid>/task/<tid>/schedstat` (we chased a phantom "100 % libc" that was
  an `ITIMER_PROF` artifact).

## 10. Performance — the levers, in the order that paid

| Lever | Effect (measured, on device) |
|---|---|
| Compile the lifted code optimized (-O2+) | 4.1 → 7.8 FPS |
| O(1) dispatch + PLT reduction | → 9.8 |
| State in the CPU struct, no TLS on the dispatch path | → 12.9 |
| Two frames in flight (fences per slot) | → 21.3 |
| Staging ring sized one frame per slot (was overflowing mid-frame) | → 22.5 |
| Persistent Vulkan pipeline cache | → ~24 |
| Guest registers as locals + direct calls (both, in that order) | → 25.0 |
| x87 stack as locals + volatile-range cache | → **28.1** |

Measured dead ends: lowering render resolution (CPU-bound frame), direct calls
*alone* (−15 % — they only win after locals), unguarded static-buffer caching
(the engine regenerates more than you think), interprocedural flag liveness
(provably correct, no dynamic gain). **The order of transformations matters as
much as the transformations.**

## 11. What a week bought us (your calibration)

| Metric | Value |
|---|---|
| Calendar time (AI-assisted, heavy parallelism) | 7 days |
| Functions lifted / tested by the oracle | 86,653 / 76,650 (0 true divergence) |
| Generated C | 420 MB, 132 files |
| Shaders translated / available in cache | 2,856 / 75,797 |
| First pixels → mission fully played | day 1 → day 4 |
| On-device performance | 4.1 → 28.1 FPS (×6.9), GPU never limiting |

## 12. Pitfall checklist

1. 2D renders, 3D black → suspect the lift's matrix math (one wrong SSE-lane
   character did this to us), not the renderer.
2. Engine runs, screen black → check audio init: an engine without sound may
   loop on re-init and never present.
3. A no-op you "temporaries" away (GetRenderTargetData, pRect) is a missing
   feature — the engine depends on more of the API than it seems.
4. A bug invisible on the bench can be visible on device (fresh-zeroed vs
   recycled memory).
5. Every `SUBOPTIMAL`/recreate loop needs a bound and a definitive fallback.
6. Missing shaders are not just missing pixels — they leave stale render
   targets that *other* shaders refract.
7. A translation layer host lies about memory costs (memmove was 0.3 ms on an
   M4, 5-8 ms on the phone) — measure on the target.
8. When the compiler gains rights (locals, hoisting), guest memory needs
   barriers back (a "held by accident" concurrency story).
9. Fibers + Metal/`backtrace()` → leave a null frame above the fiber's initial
   SP.
10. When everything is green and it still doesn't work, the missing piece may
    be *content* absent from your install, not a bug (our main menu was the
    Battle.net client's dashboard, not on the disc).

## 13. Reuse map — what applies directly to your game

| Component | Reusability |
|---|---|
| `runtime/sc2_d3d9.c` + `sc2_d3d9v.c` | High — a complete D3D9-on-Vulkan backend for recomp projects (device tier carries arena/COM assumptions, the Vulkan tier is guest-free) |
| `tools/difftest/` | High — oracle harness, game-agnostic |
| `tools/d3d9-shaders/` | High — SM2/3 decoder + SPIR-V translator + cache extractor, any D3D9 title |
| `tools/pcrecomp-*` patches | High — generic lifter fixes |
| `tools/mpq/`, `tools/audio/` | Medium — MPQ (StormLib) and MSVC-import decoding |
| `scripts/` bench + lift transforms | Medium — measurement discipline and lift optimization, adaptable |
| `android/` | Medium — NativeActivity glue, bionic fibers, swapchain presenter, APK chain |

**Legal reminder:** all of this operates on a copy you own, offline, and
distributes nothing derived. See [LEGAL.md](../LEGAL.md).
