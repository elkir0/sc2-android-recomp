# Symptom index — from what you observe to where the answer lives

Lookup table for debugging this repository's runtime, backend, lift and benches. Every row was
lived at least once on this project (2010 client and current client); each one traces to a
sentence in a cited document.

**How to use it.**

1. Grep this file for the error text, the visual effect or the measured number you see
   (`grep -i <word> docs/agents/symptoms.md`).
2. Open the **Reference** column: it points at the section that explains the cause and the fix.
3. Then open the guide of that subsystem in [docs/agents/README.md](README.md) for the steps and
   the tools.
4. Before any bench, run the fast gate (`scripts/verifier.sh`, about 80 s) so a broken tool never
   poisons a measurement.

If a symptom leads you to game-side content (assets, protections, launching the game, accounts,
online play), stop there: out of scope — see [AGENTS.md](../../AGENTS.md#scope).

---

## Build and gate

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| `disasm32` dies with `MemoryError` or SIGSEGV (rc 139) just after the `Added … branch targets` line, on a >15 MB PE under `ulimit -v` | The final alias loop of `find_functions` keeps every capstone operand in memory; address space runs out (24 MB of margin left) | Use the patched copy in `tools/pcrecomp-disasm32/` (operand release, same output byte for byte); margin becomes ~644 MB | [disasm32 PATCH, Symptôme](../../tools/pcrecomp-disasm32/PATCH.md#symptôme) |
| The lift of a function aborts at its first MMX instruction (`not an xmm operand: mmN`) | mm0–mm7 were not lifted at all; whole functions were left as TODOs | Apply `tools/pcrecomp-mmx/` (mm0–mm7 as CPU fields, ~50 mnemonics), re-lift, re-run the oracle | [pcrecomp-mmx](../../tools/pcrecomp-mmx/README.md#pcrecomp-mmx--mmx-lifting-mm0-mm7-as-64-bit-cpu-fields) |
| Extracted MPQ files smaller than the 64 KiB buffer come out **empty**; non-multiples truncated to the lower multiple; exact multiples intact | `SFileReadFile` returns FALSE + `ERROR_HANDLE_EOF` on the last partial read although it did read bytes; a naive loop drops that block | Use `tools/mpq/mpqget`: writes every byte read, rc 0 only when total equals the catalog size | [mpq README](../../tools/mpq/README.md#mpq-extraction-tools-stormlib--and-the-partial-block-eof-trap) |
| An MPQ listing comes out truncated to about half, silently; whole archives look absent | The `(listfile)` was read into a 2 MiB-capped buffer | Fixed: cap raised to 256 MiB and truncation refused (exit 3, stderr message, no partial listing) | [mpq README, Usage](../../tools/mpq/README.md#usage) |
| `scripts/paquet-mac.sh` refuses with exit 66, 67 or 68 | Named guards: lift built `-O0` (66), SPIR-V table absent from the build (67), MoltenVK sha256 mismatch (68) | Fix the named cause; a different MoltenVK must be assumed explicitly with `--mvk <file> <sha256>` | [BANC-MAC, Binaire unique](../BANC-MAC.md#binaire-unique--banc-et-app-v3-2210) |
| Guard on the Android library by counting a lifter macro in the binary finds ~2 hits and proves nothing | The macro is gone at compile time; only two runtime log strings remain in the `.so` | Guard on the defined symbols of known lifted roots, with a negative test of the guard's logic | [BILAN-8F, libSC2Partout.so](../../android/BILAN-8F.md#libsc2partoutso) |
| A Python helper named `dis.py` breaks unrelated scripts | It shadows the standard `dis` module | Rename the helper | [CLIENT-5.0 §9](../CLIENT-5.0.md#9-pitfalls-worth-sharing) |
| The CI `legal-scan` workflow fails on a file you added | Named reason: forbidden extension, file over 20 MB, image outside the curated set, or a personal-data pattern (home path, IP, e-mail) | Fix the file; never bypass the gate — policy in `LEGAL.md` | [LEGAL.md, Enforcement](../../LEGAL.md#enforcement--application) |

## Boot and host runtime (Win32 layer, threads, fibers)

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| Boot spins with ~1.6 million calls and never finishes; nothing else fails | `CPUID` returned the same values for every leaf, so the leaf-4 loop (`eax & 0x1F` never 0) never ends | Return true per-leaf values; the game's own logs print the values it expects — read them first | [CLIENT-5.0 §2](../CLIENT-5.0.md#2-win32-answers-must-be-true--every-lie-surfaced-later) |
| Crash or corrupted locals after a version query; the caller's saved frame pointer is damaged | `GetVersionExW` filled 0x11C bytes into a 0x114-byte `OSVERSIONINFOW` | Write only the size the caller declared | [CLIENT-5.0 §2](../CLIENT-5.0.md#2-win32-answers-must-be-true--every-lie-surfaced-later) |
| `time()` goes negative, `localtime` returns NULL, `strftime` raises an invalid-parameter fault | `FILETIME` counted from 1970; Windows counts from 1601 | Use the 1601 epoch | [CLIENT-5.0 §2](../CLIENT-5.0.md#2-win32-answers-must-be-true--every-lie-surfaced-later) |
| Every heap release is refused; memory climbs to ~2.9 GB and dies while far less is live | `VirtualQuery` returned one allocation base for everything, so the C allocator refuses all releases | Answer real allocation bases (measured: 1.2 GB stable instead of 2.9 GB) | [CLIENT-5.0 §2](../CLIENT-5.0.md#2-win32-answers-must-be-true--every-lie-surfaced-later) |
| Saves written with the archive header at the wrong offset; some maps fail silently | `WriteFile`/`ReadFile` ignored the `OVERLAPPED` offset | Honour the offset; re-check the maps that had "always" failed | [CLIENT-5.0 §2](../CLIENT-5.0.md#2-win32-answers-must-be-true--every-lie-surfaced-later) |
| "Save Failed" in the menu, or saves of ~1 KB that load nothing | `CreateDirectoryW` refused a path with a trailing separator (and early stubs did not touch the disk); `GetFileAttributesW` called any missing path a directory | Implement the directory/file calls truly and accept the trailing separator | [CLIENT-5.0 §2](../CLIENT-5.0.md#2-win32-answers-must-be-true--every-lie-surfaced-later), [BANC-SCENARIOS §5](../BANC-SCENARIOS.md#5-ce-que-le-banc-a-trouvé-exemples-mesurés) |
| Key-binding labels all display as "Key/F1" | `MapVirtualKeyExW` returned its input | Map virtual keys for real | [CLIENT-5.0 §2](../CLIENT-5.0.md#2-win32-answers-must-be-true--every-lie-surfaced-later) |
| A progress dialog cancels itself | `InitCommonControlsEx` returned 0, read as *Cancel* | Return success | [CLIENT-5.0 §2](../CLIENT-5.0.md#2-win32-answers-must-be-true--every-lie-surfaced-later) |
| Middle-button camera panning does nothing | `GetCursorPos` returned the screen centre and `GetCursorInfo` failed; the game measures the drag from the cursor position | Return the true cursor position (found late: a stub hides for days until its feature is exercised) | [CLIENT-5.0 §2](../CLIENT-5.0.md#2-win32-answers-must-be-true--every-lie-surfaced-later) |
| After an import call the caller's register restores are shifted (`ebx ← edi`, `edi ← esi`, `esi ← extra value`); crash far away | Unknown stdcall arity: one argument left on the stack shifts every restore | An unknown arity must stop the run immediately; decode MSVC-decorated names mechanically | [CLIENT-5.0 §3](../CLIENT-5.0.md#3-calling-conventions-and-import-names) |
| A stub "works" while the heap is bounded, then eats 512 MB per call once large blocks move to the arena | Import misnamed: `HeapSize` taken for `HeapAlloc` — the pointer was passed as a size and failed silently | Treat every import name as a hypothesis until the call site is re-read | [CLIENT-5.0 §3](../CLIENT-5.0.md#3-calling-conventions-and-import-names) |
| A feature is broken but the log shows no refusal | A stub of known arity that returns 0 stops nothing; one file-attribute query ran 257 times unhandled | Read the end-of-run per-import call summary at every new front | [CLIENT-5.0 §3](../CLIENT-5.0.md#3-calling-conventions-and-import-names) |
| Credits and costs are blank in the UI (765 calls returned NULL) | ICU `NumberFormat::createInstance` was unimplemented | Provide the format objects (`SC2_ICU_NOMBRES`) | [VARIABLES](../VARIABLES.md) |
| The run stops with a deadlock report naming fibers | Real critical sections, events, semaphores and mutexes live in guest memory; a genuine cycle is named | Read the listed fibers; the virtual-time scheduler makes the order reproducible run to run | [CLIENT-5.0 §1](../CLIENT-5.0.md#1-threads-one-host-thread-deterministic-order) |
| `lock cmpxchg8b` on a 4-byte-aligned address raises SIGBUS on ARM64 | Translated to a native atomic on an unaligned address | Run all lifted code on one host thread (fibers); `lock`-prefixed ops become plain read-modify-write | [CLIENT-5.0 §1](../CLIENT-5.0.md#1-threads-one-host-thread-deterministic-order) |

## Memory

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| NULL frame pointer on a return, far from any recent change | Allocations climbed into the thread stacks before the guest address space was re-laid out | Separate the ranges: heap, arena, thread stacks | [CLIENT-5.0 §4](../CLIENT-5.0.md#4-memory) |
| A long run dies around the half-hour mark; ~1.8 GB of arena overflow | Fragmentation by large `VirtualAlloc` (hundreds of MB of unusable free extents) plus a surface-object leak (~260 objects/s never freed) | Arena journal per caller; surface cache; two-ended best-fit arena with quarantine — same run then holds at ~390 MB of overflow budget | [Rétrospective §3.9](../RETROSPECTIVE-RECOMPILATION.md#3-les-difficultés-qui-ont-coûté-le-plus-symptôme--mesure--cause--correctif) |
| A "leak" that tracks your own stubs, not the game | Refused releases look exactly like real consumption | Count live blocks **per caller**; never conclude from lowered settings | [CLIENT-5.0 §4](../CLIENT-5.0.md#4-memory) |
| Intermittent crash, ~1 run in 5 on one mission: an empty texture read sends D3DX to guest address 0 | D3DX accepted a null source | Refuse a null source like the real d3dx9; isolated by the full non-regression | [BANC-SCENARIOS §5](../BANC-SCENARIOS.md#5-ce-que-le-banc-a-trouvé-exemples-mesurés) |
| The run dies when the guest passes 2 GB on the current client | The client is large-address-aware (1.2 GB committed for 593 MB allocated on a first mission) | Extend the guest space (documented ceiling 0xF0000000) | [CLIENT-5.0 §4](../CLIENT-5.0.md#4-memory) |

## Lift and oracle

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| 2D and UI render, the whole 3D scene is black | The lifter tested the wrong character of the mnemonic: high-lane SSE loads lifted as low-lane, so every 4×4 matrix product degenerates (rows `a,b,a,b`) | One-line fix (`tools/pcrecomp-movhps/`) + re-lift. When 2D and 3D diverge, suspect the lift's matrix math, not the renderer | [pcrecomp-movhps, Symptom](../../tools/pcrecomp-movhps/README.md#symptôme--symptom) |
| Float-to-integer conversions off by one (grid cells, ticks, table indices), about half the time | `fldcw` was ignored, so `fistp` rounded to nearest instead of truncating toward zero | Model the x87 control word (`PCRECOMP_FPU_CW=1`, helpers in `runtime/cpu.h`); targeted test passes 147/147 on x86-64 and aarch64 | [DIVERGENCES §4](../../tools/difftest/DIVERGENCES.md#4-la-correction-majeure--le-mot-de-contrôle-x87) |
| Out-of-range or NaN float-to-int casts give `0x7FFFFFFF` on ARM64 but `0x80000000` on x86 | The C cast is undefined; AArch64 saturates | Write the x86 "integer indefinite" values from the bounded helpers | [DIVERGENCES §4](../../tools/difftest/DIVERGENCES.md#4-la-correction-majeure--le-mot-de-contrôle-x87) |
| Jumps on overflow (`jo`, `jl`…) take wrong branches after shifts | Shift helpers never wrote OF (defined for a count of 1; `jo/jl/jg/jle/jge` read it) | Fixed in `runtime/cpu.h` and the generic pcrecomp `cpu.h` | [DIVERGENCES §5](../../tools/difftest/DIVERGENCES.md#5-of-des-décalages) |
| The oracle fails by the thousand on AF after `test`/`xor`/`or`/`and`/`shr` | Undefined-flag convention (SDM): the emulator writes 0, the lift leaves the old value | Mask the named families (counted in the report); expect harness artifacts too — simulated return frame, unresolved IAT | [DIVERGENCES §2](../../tools/difftest/DIVERGENCES.md#2-le-stock-m4-3-985--doù-venaient-les-échecs), [METHOD §6](../METHOD.md#6-the-oracle-per-function-differential-testing-this-toolkit) |
| Most functions are skipped by the oracle (64,977 of 76,650) | By design: flow leaving `.text` (imports, stubs) is not deterministically comparable | Read the named skip reasons; extending coverage means simulating imports on the reference side | [DIVERGENCES §7](../../tools/difftest/DIVERGENCES.md#7-ce-qui-reste--limites) |
| One oracle failure on ARM64 only: the top byte of a stored NaN differs | x87's "real indefinite" QNaN is negative; AArch64's default QNaN is positive | Known open limit; candidate fix is a sign-canonicalising helper on float writes under `__aarch64__` | [DIVERGENCES §6](../../tools/difftest/DIVERGENCES.md#6-re-certification-par-famille-run-du-jeu-arm64) |
| The runtime hits functions outside the lifted closure; a full re-lift is too slow and churns every file | A whole-program closure only covers the roots it was given | `tools/pcrecomp-closure/closure-plus.py` lifts only the missing functions and prints the new addresses | [pcrecomp-closure](../../tools/pcrecomp-closure/README.md#pcrecomp-closure--extend-an-existing-lifted-closure-without-a-global-relift) |
| After "guest registers as locals", the game freezes around frame 35, then SIGSEGV | The compiler may hoist guest memory accesses out of loops — legal C, wrong for guest memory | Barrier at loop head + volatile guest accesses; the control build compares every local copy against the CPU struct at each boundary (0 discrepancy over four missions) | [Rétrospective §3.11](../RETROSPECTIVE-RECOMPILATION.md#3-les-difficultés-qui-ont-coûté-le-plus-symptôme--mesure--cause--correctif) |

## Rendering (device, states, textures, presentation)

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| Depth and normals never written; depth-of-field blurs everything; scenes look flat | Only the first render target was bound while the game draws thousands of times to 2–4 targets | Attach, clear and mask every target; portraits and cinematics then come right | [CLIENT-5.0 §5](../CLIENT-5.0.md#5-rendering) |
| UI text unreadable or black, no error anywhere | `LockRect(pRect)` was ignored: every glyph of the atlas smashed into (0,0) | `pBits` must point at the sub-rectangle's top-left (containing 4×4 block for DXTn) | [RENDU §5.2](../RENDU-D3D9-VERS-VULKAN.md#52-lockrectprect--le-bug-invisible-dune-semaine) |
| The minimap is black without error, and units are missing from the mission view | Two independent causes: the D3DX layer accepted only DDS (the minimap is TGA), and `GetRenderTargetData` was a no-op (the engine composites unit sprites from its own targets) | Decode TGA into the DDS path in memory; make the readback real (proved A/B) | [Rétrospective §3.4](../RETROSPECTIVE-RECOMPILATION.md#3-les-difficultés-qui-ont-coûté-le-plus-symptôme--mesure--cause--correctif), [RENDU §5.3](../RENDU-D3D9-VERS-VULKAN.md#53-la-famille-relecturecomposition) |
| Title screen is a flat colour on Adreno or an emulator GPU | The GPU has no BC compression, so DXTn textures never make it up | Software DXTn decoder as a first-class path, tested against the spec and a GPU | [RENDU §5.1](../RENDU-D3D9-VERS-VULKAN.md#51-formats-et-bc-logiciel) |
| Refraction ghosts of a previous scene (a glass full of coloured swirls); visible on the device, invisible on the bench | Skipped draws leave a stale render target that later passes refract; the bench's fresh-zeroed memory hides what recycled memory shows | Count fallback draws — the counter led to this diagnosis — then capture the missing bytecodes (draws fell from 18,387 to 278) | [RENDU §6.6](../RENDU-D3D9-VERS-VULKAN.md#66-étude-de-cas--le-verre-de-la-cinématique-du-bar) |
| No shadows although a D32 depth surface exists and is bound | The engine never samples that surface itself — it expects the *sampler* to do the comparison | Comparison sampler (PCF) bound beside the raw view; a single PS binary serves both paths via specialization constants | [RENDU §6.4](../RENDU-D3D9-VERS-VULKAN.md#64-ombres--sampler-à-comparaison-sans-toucher-au-défaut) |
| Shadow acne once shadows render | The D3D9 depth-bias states were not carried to Vulkan | Map the slope-scaled bias and depth bias to the Vulkan depth-bias states | [RENDU §6.4](../RENDU-D3D9-VERS-VULKAN.md#64-ombres--sampler-à-comparaison-sans-toucher-au-défaut) |
| Looks like a saturated GPU (~14 ms/frame) but GPU timestamps show under 40% busy | The staging ring overflowed mid-frame: a forced submit waited on the previous frame (1.93 submits/frame, 9.2 ms of fence wait) | Size the ring per frame slot (256 MiB) and watch the forced-submit counter (drops to ~1.0) | [Rétrospective §3.8](../RETROSPECTIVE-RECOMPILATION.md#3-les-difficultés-qui-ont-coûté-le-plus-symptôme--mesure--cause--correctif), [RENDU §8](../RENDU-D3D9-VERS-VULKAN.md#8-transferts--lanneau-les-époques-les-générations) |
| Android: `VK_SUBOPTIMAL_KHR` on *every* present; thousands of swapchain creations; perpetual black screen | Recreating on SUBOPTIMAL starves the compositor (one creation ≈ one frame); native-portrait-held-landscape makes it fire each frame | Recreate only on `VK_OUT_OF_DATE`, SUBOPTIMAL **with** a size change, or a platform resize; bounded fallback to CPU copy | [RENDU §7.1](../RENDU-D3D9-VERS-VULKAN.md#71-la-discipline-suboptimal-la-bataille-de-lécran-noir) |
| Emulator: the engine runs and presents, the screen stays black, no crash | Neither emulator GPU exposes `VK_KHR_push_descriptor` | Classic-descriptor fallback (pool per frame slot); every binding must be valid — a null view SIGSEGVs on Lavapipe, so unused samplers get white dummy textures | [android-porting, push_descriptor](../../tools/android-porting/README.md#note-vk_khr_push_descriptor-on-emulators), [RENDU §4](../RENDU-D3D9-VERS-VULKAN.md#4-descripteurs--un-chemin-rapide-un-repli-honnête) |
| The whole UI is offset by one pixel; alpha-tested edges show fringes | D3D9's half-pixel offset and the engine's software alpha-test were not passed to the shaders | Half-pixel (VS) and alpha-test (PS) travel as push constants, never as recompile-time flags | [RENDU §6.2](../RENDU-D3D9-VERS-VULKAN.md#62-linterface-verrouillée-avec-le-backend-le-contrat) |
| Adreno rejects the VS/PS interface | Varyings declared as arrays; Adreno refuses array-to-scalar matching between stages | Freeze the interface contract first: 16 distinct `vec4` varyings | [RENDU §6.2](../RENDU-D3D9-VERS-VULKAN.md#62-linterface-verrouillée-avec-le-backend-le-contrat) |
| MoltenVK: triangle fans missing, sampler LOD bias clamped to 0 | `VK_KHR_portability_subset` limits | Log the portability limits at load; convert fans to lists; clamp the bias | [RENDU §7.3](../RENDU-D3D9-VERS-VULKAN.md#73-moltenvk-au-banc) |
| Second and later render targets receive nothing once MRT is enabled | D3D9 per-target write masks have CreateDevice defaults that must be set (at CreateDevice **and** Reset) before the engine reads them | Set the defaults; `INDEPENDENTWRITEMASKS` is announced, so the engine will read them | [CAPS, Pièges](../CAPS.md#pièges) |
| AUTOGENMIPMAP textures are served at mip level 0 only; `GenerateMipSubLevels` is a counted no-op | Mip generation is not implemented; the `CANAUTOGENMIPMAP` bit was withdrawn accordingly | Accept the traced limit, or implement generation and restore the bit | [CAPS, Caps2](../CAPS.md#caps2-12-lu--0xffffffff--0x20120000) |

## Shaders

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| A draw is silently skipped, leaving a hole in the scene | Never allowed: a draw without a translated shader falls back to the real "light" pipeline (degenerate off-screen triangle) and is **counted** | Watch the light-draw counter, not the screen | [RENDU §6.3](../RENDU-D3D9-VERS-VULKAN.md#63-la-table-embarquée-et-le-mode--lumière-) |
| The translator refuses some bytecodes (predicates, `call`/`label`, `oFog`/`oPts`, `t` registers in ps_3) | Unsupported SM2/3 constructs | Each refusal motif is a work ticket listed by the generation report; those draws fall back until translated | [RENDU §6.1](../RENDU-D3D9-VERS-VULKAN.md#61-chaîne-hors-ligne) |
| Millions of light draws the first time a new scene plays | The embedded table covers only previously played scenes | Build the mmap pack (`SC2_SPIRV_PACK`, format in PACK-SPIRV.md): 264,063 fallback draws dropped to 0 | [PACK-SPIRV](../PACK-SPIRV.md#pack-spir-v-sc2-spirvpak-format-v1-passe-10t) |
| Switching quality in the in-game options gives a black world (1.6 million draws without a shader) | The pack covered only the quality levels that had been played | Harvest each quality level (pack grew from 8,215 to 13,398); a 0-missing-shader bench proves coverage **for its own settings only** | [CLIENT-5.0 §5](../CLIENT-5.0.md#5-rendering) |
| Runtime and table disagree about a shader's identity, or duplicates pile up | Identity must be one hash everywhere | FNV-1a 64 of the bytecode, token by token, comments skipped — same hash at runtime, in the translator and in the pack; shader object addresses are never reused | [RENDU §6.2](../RENDU-D3D9-VERS-VULKAN.md#62-linterface-verrouillée-avec-le-backend-le-contrat) |
| A translated shader renders wrong or crashes the backend | A broken shader shipped silently | Only `spirv-val`-passing SPIR-V enters the table; the generation report lists every refusal | [D3D9 guide §6](../D3D9-PORTING-GUIDE.md#6-shaders--translate-the-cache-never-the-source-day-2-5) |

## Audio

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| FMOD finds no devices, or init fails | The enumeration callback ignores a device with a null GUID | Announce a real device too | [CLIENT-5.0 §6](../CLIENT-5.0.md#6-audio) |
| The mixer thread stalls or starves | It needs wall-clock time; the scheduler's virtual time starved it | Give the mixer wall-clock time | [CLIENT-5.0 §6](../CLIENT-5.0.md#6-audio) |
| Output stays silent until the next buffer — one silent run among dozens | After a stall longer than half the buffer the write cursor must re-sync to FMOD's own phase; after a near-whole-buffer stall it must catch up one full turn | The resync is a pure function with a host test that replays both recorded silences and 100,000 random mixer steps | [CLIENT-5.0 §6](../CLIENT-5.0.md#6-audio) |
| The engine runs, frames advance, captures are black and identical; no render error | Audio init failed and re-looped thousands of times (logcat decides), interleaved with swapchain re-creations | Link the software mixer: an engine without its sound has no screen | [Rétrospective §3.2](../RETROSPECTIVE-RECOMPILATION.md#3-les-difficultés-qui-ont-coûté-le-plus-symptôme--mesure--cause--correctif) |
| Sound effects absent; trigger errors on the bridge | The asset archives expose effects only under a suffixed name while the data requests the plain name | Runtime alias table at archive mount (`SC2_MPQ_ALIAS_WAV`; `[MPQ] alias .wav` log lines) | [VARIABLES](../VARIABLES.md) |
| Mixer output is silence or noise — no ground truth yet | You are debugging blind | Dump the mix to WAV (`SC2_AUDIO_DUMP`) and check duration, entropy, RMS: a real soundtrack is neither flat nor uniform noise | [D3D9 guide §8](../D3D9-PORTING-GUIDE.md#8-audio-and-video-parallel-tracks) |

## Video

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| The video quad never draws; cinematics stay black | The decoder was starved by an unimplemented audio-DSP clock; the texture-upload path was sound | Implement the engine's DSP callbacks (`SC2_DSP`) | [MILESTONES, entry "Video path"](../MILESTONES.md) |
| Phone CPU pegged by cinematic decode — reach for hardware decoding? | Not before measuring: after lift optimization the lifted Theora decoder ran real time (24.9 fps for a 24 fps video) | Keep the hardware path optional (`scripts/video-transcode.sh` + `SC2_VIDEO_AMC=1`) | [D3D9 guide §8](../D3D9-PORTING-GUIDE.md#8-audio-and-video-parallel-tracks) |
| With hardware decode on, a cinematic sticks or silently changes path | No sibling `.mp4`, unsupported stream, or decoder open failure | Automatic fallback to the engine's software Theora path; a mid-play failure keeps the last picture (the game never stops) | [BRICKS, video brick](../BRICKS.md#scriptsvideo-transcodesh--h264-video-brick--brique-vidéo-h264) |

## Input

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| Frame rate stuck low and steady while nothing looks busy; the loop sleeps 10 ms per iteration | The game believed it was in the background: no `WM_ACTIVATEAPP` was ever delivered | Send activation from the input module (`SC2_ACTIVATION`); confirmed by wait instrumentation (`SC2_ATTENTES=1`); +60 % on the phone (35 → 56 FPS) | [BANC-SCENARIOS §5](../BANC-SCENARIOS.md#5-ce-que-le-banc-a-trouvé-exemples-mesurés) |
| The first minimap click after minutes without input does not move the camera | Engine-side idle behaviour, not an input bug | Double each such click in the scenario | [BANC-MAC, Escarmouche](../BANC-MAC.md#escarmouche-v410-2310) |
| Scenario clicks land nowhere; one line of the scenario is cut in half | A button that reacts only after hover needs `MOVE` then `CLIC`; a panel opening late under load needs repeated clicks in a window; `;` ends a comment line | Follow the scenario writing rules; each scenario states its expectations (rc, log lines) in its header | [BANC-SCENARIOS §1](../BANC-SCENARIOS.md#1-le-module-dentrées-du-banc-runtimesc2_entrees_bancc) |

## Performance

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| Everything at 4-FPS-class numbers | The first builds were `-O0` | Check the compile flags first — before any clever lever | [CLIENT-5.0 §7](../CLIENT-5.0.md#7-performance--from-1324-to-3643-fps-on-the-heaviest-missions) |
| Dispatch is ~50% of the main thread (binary search over ~600k entries per computed call and jump) | Every indirect target searched a sorted table | Direct-mapped dispatch cache: 27 → 45 FPS on the first mission, boot 90 → 58 s | [CLIENT-5.0 §7](../CLIENT-5.0.md#7-performance--from-1324-to-3643-fps-on-the-heaviest-missions) |
| MMX-heavy scenes crawl (bar scene at single-digit FPS) | MMX resolved by name comparison per instruction | Resolve at decode, then translate to direct C (247 instructions checked against the interpreter): 3.5 → 19, then 17 → 79 FPS | [CLIENT-5.0 §7](../CLIENT-5.0.md#7-performance--from-1324-to-3643-fps-on-the-heaviest-missions) |
| Heavy missions stuck in the 12–24 FPS band | The 2010 lift passes had never been applied to this build | Direct calls (~1.73M constant sites) and guest registers/flags in local variables (607,809 functions): about +50% | [CLIENT-5.0 §7](../CLIENT-5.0.md#7-performance--from-1324-to-3643-fps-on-the-heaviest-missions) |
| Android: the dynamic TLS resolver eats ~7% of a `dlopen`ed library, though another architecture's bench called the change a win | Every `__thread` access goes through the resolver in a `dlopen`ed `.so` | Carry state on the CPU pointer; mark cold paths `noinline`; measure on the target — a bench of a different architecture lies about memory costs | [Rétrospective §3.7](../RETROSPECTIVE-RECOMPILATION.md#3-les-difficultés-qui-ont-coûté-le-plus-symptôme--mesure--cause--correctif) |
| Direct calls alone made the phone *slower* (−15%, +31 MB of code) | They only win after registers are locals; the order of transformations matters as much as the transformations | Apply the passes in the measured order | [Rétrospective §5](../RETROSPECTIVE-RECOMPILATION.md#5-les-impasses-autant-de-temps-gagné-pour-la-suite) |
| Lowering the render resolution changed nothing (960×540 = 720p FPS) | The frame is CPU-bound | Work on the lifted code, not the resolution | [Rétrospective §5](../RETROSPECTIVE-RECOMPILATION.md#5-les-impasses-autant-de-temps-gagné-pour-la-suite) |
| Static-buffer caching made it slower on Adreno | The engine regenerates "static" buffers more than you think | Volatility guard: more than two replacements reclassifies the buffer as volatile | [Rétrospective §5](../RETROSPECTIVE-RECOMPILATION.md#5-les-impasses-autant-de-temps-gagné-pour-la-suite), [RENDU §8](../RENDU-D3D9-VERS-VULKAN.md#8-transferts--lanneau-les-époques-les-générations) |
| First run of a scene stalls tens of seconds creating pipelines (18.6 s measured on a cold cache) | Cold `VkPipelineCache` | Persistent on-disk cache (`SC2_VK_CACHE_PIPELINES`), written atomically; under 1 s on later runs | [CAPS, Adaptateur](../CAPS.md#adaptateur--mémoire-vidéo-par-directdraw-v25a-2210), [RENDU §3](../RENDU-D3D9-VERS-VULKAN.md#3-modèle-détat--brut-au-set-traduit-au-draw) |
| macOS: the present path takes ~10% of the game thread | MoltenVK encodes everything inside `vkQueueSubmit` | Asynchronous queue submits: 10.6% → 2.1% | [CLIENT-5.0 §5](../CLIENT-5.0.md#5-rendering) |
| `memmove` dominates the profile on the phone (5–8 ms/frame) but was cheap on the Mac (0.3 ms) | Whole constant blocks copied per draw | Copy only the register range the shader reads (+5.6% FPS, A/B/A); a translation-layer host lies about memory costs — measure on the target | [D3D9 guide §12](../D3D9-PORTING-GUIDE.md#12-pitfall-checklist), [RENDU §8](../RENDU-D3D9-VERS-VULKAN.md#8-transferts--lanneau-les-époques-les-générations) |
| The profiler says the threads are 100% inside libc | `ITIMER_PROF` artifact: a whole tick is charged to the thread woken at the deadline | Cross-check any sampler against the kernel scheduler counters (`/proc/<pid>/task/<tid>/schedstat`) | [Rétrospective §3.10](../RETROSPECTIVE-RECOMPILATION.md#3-les-difficultés-qui-ont-coûté-le-plus-symptôme--mesure--cause--correctif) |

## Bench and measurement

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| Frame 10's fingerprint alternates between two accepted values across runs | A thread race in the loading screen (proven by CPU pinning) | Tolerated variant in the canon; judge on stable frames only (1, 100–300), never the frame queue | [BANC-MAC, Empreintes](../BANC-MAC.md#empreintes-de-référence-run--défaut--600-frames) |
| Two runs of the same build differ by more than noise | Game content alone is worth ±15 ms of CPU per frame | A/B/A: replay the reference *after* the candidate, ≥ 3 interleaved pairs, thermals controlled | [Rétrospective §4](../RETROSPECTIVE-RECOMPILATION.md#4-mesurer-juste-le-protocole-duplicable) |
| Per-mission frame rates swing −9% to +38% on a shared bench | Shared-machine variance | Judge FPS on the aggregate of the missions (−10% threshold); fine measurement uses `banc-ab.sh` on a calm machine | [BANC-SCENARIOS §3](../BANC-SCENARIOS.md#3-non-régression-scriptsnonregsh-scriptsnonregpy) |
| A bench run lost the real profile, or two benches wiped each other's data | A profile that is a symbolic link is not copied by a naive copy; the benches then share — and can wipe — the real one | Bench isolation: per-run profile copy and pipeline cache | [CLIENT-5.0 §8](../CLIENT-5.0.md#8-bench) |
| Non-regression is always green — can the judge be trusted? | A judge that cannot fail is not a judge | The judge is itself tested: a sleeping game (`SC2_ACTIVATION=0`) and a SPIR-V-less binary must both fail; `scripts/injecter-bugs-v22.sh` mutants must all be caught | [BANC-SCENARIOS §3](../BANC-SCENARIOS.md#3-non-régression-scriptsnonregsh-scriptsnonregpy) |
| Fingerprints moved by one pixel after a correctness fix | Expected: a coordinate now truncated as on Windows instead of rounded | Verify visually, then re-baseline; the change must reproduce across runs | [DIVERGENCES §6](../../tools/difftest/DIVERGENCES.md#6-re-certification-par-famille-run-du-jeu-arm64) |
| A worker's test killed every shell on the machine; a fake game silently queued every bench behind the real lock | `pkill -x sh` in a test; tests taking the real bench lock | Briefs forbid `pkill`/`killall`; tests take their own lock | [CLIENT-5.0 §9](../CLIENT-5.0.md#9-pitfalls-worth-sharing) |
| Profiling a renamed copy of the binary: every sample lands in "other libraries" | The Mac profile analyzer only recognizes the image named `recomp-sc2` | Rename the image in the sample output before analysis | [BANC-MAC, Mesures](../BANC-MAC.md#mesures) |

## macOS host

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| Crash in the engine's Ogg decode path only on the Mac (around frame 40) | The guest zone was not 4 GB-aligned, so stubs returning host pointers by low-32-bit truncation were wrong | Align the guest zone (`zone_hote_init`) | [BANC-MAC, correctifs runtime](../BANC-MAC.md#ce-quil-a-fallu-corriger-dans-le-runtime-jamais-exécuté-nativement-depuis-la-7u) |
| Windows thread identifiers look like pointers (high bit set) | `GetCurrentThreadId` returned a truncated `pthread_self()` | Use `pthread_threadid_np` | [BANC-MAC, correctifs runtime](../BANC-MAC.md#ce-quil-a-fallu-corriger-dans-le-runtime-jamais-exécuté-nativement-depuis-la-7u) |
| SIGSEGV inside Metal's `backtrace()` at device or queue creation, with fibers | The frame chain runs past the top of the fiber's stack, outside the mapping | Leave 64 null bytes above the fiber's initial SP (`fiber_ctx_init`) | [BANC-MAC, correctifs runtime](../BANC-MAC.md#ce-quil-a-fallu-corriger-dans-le-runtime-jamais-exécuté-nativement-depuis-la-7u), [Rétrospective §3.12](../RETROSPECTIVE-RECOMPILATION.md#3-les-difficultés-qui-ont-coûté-le-plus-symptôme--mesure--cause--correctif) |
| Video pacing drifts on macOS | No `clock_nanosleep` on this platform | Relative wait up to the same deadline | [BANC-MAC, correctifs runtime](../BANC-MAC.md#ce-quil-a-fallu-corriger-dans-le-runtime-jamais-exécuté-nativement-depuis-la-7u) |
| An app copied by Finder or USB fails to load its framework | The quarantine attribute breaks framework loading | `xattr -dr com.apple.quarantine SC2Partout.app` on the copy | [BANC-MAC, Binaire unique](../BANC-MAC.md#binaire-unique--banc-et-app-v3-2210) |
| Campaign progression and saves disappear between bench runs | The bench wipes the emulated drive at start and end | Point `SC2_DOCUMENTS` at a folder outside the run directory; `Banks/` and `Saves/` are symlinked and merged (newest wins) | [BANC-MAC, Construire et lancer](../BANC-MAC.md#construire-et-lancer) |

## Android host

| Symptom | Cause | Fix or first check | Reference |
|---|---|---|---|
| `adb install -r` sent while the emulator was still booting "succeeded", and the app's pushed data were wiped | The package was reinstalled under a new uid and Android wiped the app's files | Wait for `sys.boot_completed` and check `pm path` before installing | [CLIENT-5.0 §9](../CLIENT-5.0.md#9-pitfalls-worth-sharing) |
| Guest atomic operations crash or corrupt memory only on Android | The arena is relocated (delta mode); the stock atomics bypassed the guest-pointer macro | `tools/android-porting/cpu_delta.cmake` patches the three atomic sites (self-checking); fixed low mappings are impossible in an app process | [android-porting, delta mode](../../tools/android-porting/README.md#why-delta-mode) |
| The game suggests Ultra textures on a phone | The video-memory probe is served in the modern GPU profile, and the device has not been measured at those settings | DirectDraw stays refused on Android by default (`SC2_DDRAW`); serve it only for a measurement pass | [CAPS, Adaptateur](../CAPS.md#adaptateur--mémoire-vidéo-par-directdraw-v25a-2210) |
| After trimming the install: "unable to open map", a fallback world, or a black screen | A needed dataset was removed | Prune A/B: each removal breaks exactly the expected feature; the minimal offline set is proven complete by this method | [MILESTONES, entry "Minimal campaign data set"](../MILESTONES.md), [D3D9 guide §1](../D3D9-PORTING-GUIDE.md#1-inventory-day-0) |

---

**Adding an entry.** One row in the table of its subsystem: the symptom as you would grep it, the
measured cause, the fix or first check, and a reference link to the section that grounds it. No
row without a reference; no symptom nobody has actually seen.
