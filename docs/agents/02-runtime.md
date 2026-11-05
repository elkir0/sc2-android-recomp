# 02 — The host runtime: map, principles, and adding a host test

The host runtime is the "fake Win32" that the statically recompiled game code runs
on: fibers and memory, the import table, files and saves, a software FMOD Ex, video
decode, and the presentation hooks. This guide maps `runtime/` file by file, states
the two rules that keep it debuggable, and shows how to add a test to the fast gate
in `tests-hote/`. The design method is [../D3D9-PORTING-GUIDE.md](../D3D9-PORTING-GUIDE.md)
§4 (Host runtime); the memory model and boot milestones are documented in
`runtime/sc2_host.h` itself.

## 1. Map of `runtime/`

Every file opens with a header comment stating what it does and, where relevant,
the measured facts it is built on — that comment is the entry point, and the table
below is only a paraphrase. Files whose role is rendering (the D3D9 device and the
Vulkan backend) are covered by [03-rendering.md](03-rendering.md).

| File or family | Role (from its header comment) |
|---|---|
| `runtime/cpu.h` | the CPU-struct x86-32 recompilation runtime model, paired with the lifter; several machine states can be live at once |
| `runtime/cpu_selftest.c` | hand-computed bit-operation pins (rotates that must not touch ZF/SF/PF, and the like) |
| `runtime/image_base.c` | defines `g_image_delta` declared extern by `cpu.h`; stays 0 while the image maps at its preferred base |
| `runtime/sc2_host.h` | the memory model (flat, guest VA == host address, under 4 GB), `sc2_dispatch` and the "stop on a named gap" contract |
| `runtime/sc2_runtime.c` | the runtime core; see `sc2_host.h` for the model |
| `runtime/sc2_lifted.h` | what the lifted code expects from the runtime, force-included before `cpu.h` in every `recomp_NN.c` |
| `runtime/win32_compat.c` + `runtime/win32_compat.h` | what the game expects from the system before a window exists, on libc/POSIX: Win32 fibers (`ucontext`), a shared handle table, TLS/TEB |
| `runtime/stubs_sc2.c` + `runtime/stubs_sc2.h` | the pre-window start-up stubs (single-instance IPC and similar), each stopping on a named gap |
| `runtime/sc2_iat.c` + `runtime/sc2_iat.h` | IAT dispatch: every slot gets a synthetic VA, calls are traced, implemented stubs run, the rest stop the run naming themselves |
| `runtime/sc2_iat_p4.c` | pass-4 stubs: CRT complements, USER32/GDI32 (fake window), and `Direct3DCreate9` |
| `runtime/sc2_mpq.c` + `runtime/sc2_mpq.h` | serves archive bytes to the engine's statically linked Storm and opens the same archives read-only with StormLib to name each raw read |
| `runtime/sc2_mpq_alias.h` | pure-logic MPQ hash-table augmentation (`X.wav_` → `X.wav` alias entries), header only |
| `runtime/sc2_documents.c` + `runtime/sc2_documents.h` | persistent `Banks/` and `Saves/` under `SC2_DOCUMENTS`, and the `Variables.txt` seeding of V1.5 |
| `runtime/sc2_input.h`, `runtime/sc2_input_sim.c` | the input state the game reads (message queue, cursor, keys) and the scripted-replay source |
| `runtime/sc2_entrees_banc.c` | the event-driven bench-input module (one env var, seconds-based instants), layered on `sc2_input_sim.c` |
| `runtime/sc2_escarmouche.c` + `runtime/sc2_escarmouche.h` | offline skirmish preparation: pick a melee map from the install's prefilled cache, rewrite its script, launch it |
| `runtime/sc2_icu_nombres.h` | fake `NumberFormat` objects so the UI formats numbers (credits, costs) |
| `runtime/sc2_ddraw.h` | decides whether DirectDraw is served and what `GetAvailableVidMem` answers, from the announced GPU profile |
| `runtime/sc2_horloge.h` | header-only monotonic clock shared by `GetTickCount` / `timeGetTime` / `MSG.time`, minus pause time |
| `runtime/sc2_attentes.c` + `runtime/sc2_attentes.h` | instrumentation of the wait primitives (blocked wall time per thread and cause), off unless `SC2_ATTENTES=1`, weak symbols |
| `runtime/sc2_audio.h` | the software FMOD Ex implementation contract (all sounds flow through it) |
| `runtime/sc2_audio_fmod.c` + `runtime/sc2_audio_fmod_table.h` | the 129 fmodex imports, each with a trampoline; unimplemented ones answered generically from the decoded MSVC signature |
| `runtime/sc2_audio_mix.c` | what playing a sound actually does: guest file callbacks, Ogg Vorbis decode (stb_vorbis), software stereo mixing at each `System::update` |
| `runtime/sc2_audio_int.h`, `runtime/sc2_audio_mpq.c`, `runtime/sc2_audio_sortie.c` + `runtime/sc2_audio_sortie.h`, `runtime/sc2_audio_sortie_compteur.c` | internal audio types; the `SC2_AUDIO_TEST` archive reader; the subscriber API for real-time outputs (AAudio, CoreAudio, bench); the proof-without-WAV counter |
| `runtime/sc2_audio_stb_vorbis.c` | vendored unchanged `stb_vorbis` v1.22 (public domain / MIT, licence kept in-file) |
| `runtime/sc2_video.c` + `runtime/sc2_video.h` | streaming Theora decode from an MPQ or a file (64 KiB blocks, never the whole file) |
| `runtime/video_amc.c` + `runtime/video_amc.h` | link-time (`--wrap`) interception of the game's statically linked libtheora, routing `.ogv` to hardware decode |
| `runtime/sc2_spv_pack.c` + `runtime/sc2_spv_pack.h` | the external SPIR-V pack (`SC2SPIV01` format, [../PACK-SPIRV.md](../PACK-SPIRV.md)): mmap read-only, consulted when the embedded table misses a shader |
| `runtime/shaders/` | the "light mode" stand-in shaders (degenerate triangle) that prove the Vulkan plumbing |
| `runtime/tests/` | the runtime's host tests — see §4 |

Where a file needs a library that is never vendored (StormLib, libogg/libtheora),
the header says so and the build falls back or skips, never lies.

## 2. Two rules that keep the runtime debuggable

### Refuse, don't lie

From the recipe in [../D3D9-PORTING-GUIDE.md](../D3D9-PORTING-GUIDE.md) §4: an
unimplemented import or COM method must **stop the run naming itself** — a fake
success costs hours of debugging, a named refusal costs one log line. In this code
base that contract is `sc2_dispatch` (documented in `runtime/sc2_host.h`): a call
to an unknown point stops the run and prints a phantom guest call stack.

### Win32 answers must be true

The 5.0-client chapter [../CLIENT-5.0.md](../CLIENT-5.0.md) §2 ("Win32 answers must
be true — every lie surfaced later") lists the measured cost of plausible-looking
answers: a `FILETIME` counting from 1970 instead of 1601, `GetVersionExW` writing
past the caller's struct, a `VirtualQuery` that returns one allocation base for
everything. When the runtime cannot do something, return the documented Windows
error; when it can, return the true answer — sizes, offsets, ticks, key mappings.

§3 of the same chapter adds the arity rule: **an unknown stdcall arity must stop the
run immediately** (a left-over stack argument shifts every register restore of the
caller), and the end-of-run summary of calls per import is the place to spot a stub
of known arity that quietly returns 0.

## 3. `SC2_*` variables are part of the contract

Every `SC2_*` environment variable read by the code is documented in
[../VARIABLES.md](../VARIABLES.md) — one table per area, with the default and the
file that reads it — and the gate enforces it:

```sh
scripts/variables-sc2.sh liste       # names read by the code, sorted
scripts/variables-sc2.sh ou          # name -> every file:line that reads it
scripts/variables-sc2.sh squelette   # Markdown rows for the undocumented ones
scripts/variables-sc2.sh verifier    # exit 1 naming the missing ones (gate check 8)
```

So: when you add or rename a variable, add the row in the same change, and run
`scripts/verifier.sh --rapide` (it runs check 8). Variables used only by scripts are
documented in each script's header instead.

## 4. Add a host test in `tests-hote/`

The fast gate builds an **independent CMake project** (`tests-hote/CMakeLists.txt`)
that compiles the hand-written runtime and the pure-C headers of `android/jni/` and
`mac/`, at `-O0`, **without any lifted code** — nothing private is ever read. Two
ways to register a test:

1. **Core tests** go directly in `tests-hote/CMakeLists.txt`, via the `sc2_test()`
   function defined there (`SOURCES`, `INCLUDES`, `LIBS`, `OPTIONS`, `ARGS`). The
   call `sc2_test(test_horloge_9j SOURCES "${TR}/test_horloge_9j.c" INCLUDES
   "${RT}" ...)` is a minimal example: one test file under `runtime/tests/`,
   includes at `runtime/`, exit code 0 = pass.
2. **Wave-3 lots** add one fragment under `tests-hote/v3/`, globbed by the
   final block of `tests-hote/CMakeLists.txt` — never cross-edit another lot's
   fragment. `tests-hote/v3/escarmouche.cmake` (a three-line `sc2_test` call) is
   the simplest example; `tests-hote/v3/clavier_souris.cmake` shows a pair of
   tests that run on every host, and `tests-hote/v3/presentation.cmake` a
   macOS-gated lot (`if(APPLE …)`) with a SKIP branch when Vulkan headers are
   missing.

Steps, following `runtime/tests/test_documents.c` (a core test) as the model:

1. Write a new test under `runtime/tests/`. The conventions every test keeps:
   - a header comment stating what is verified and how to run it standalone
     (`cc -Wall -Wextra -I runtime runtime/tests/test_horloge_9j.c -o /tmp/t && /tmp/t`
     is a real example);
   - one `PASS`/`FAIL` (or OK/ÉCHEC) line per assertion, a final summary, and
     **exit 0 only when everything passed**;
   - a test that cannot run here (no Vulkan, no MoltenVK) prints `SKIP <reason>`
     and exits 77 — the ctest convention the gate reads (see
     `runtime/tests/test_d3d9v.c`).
2. Register it (core list, or a new fragment under `tests-hote/v3/`) with
   `sc2_test()`. `RACINE` is the repository root, `RT` is `runtime/`, `TR` is
   `runtime/tests/`, `JNI` is `android/jni/`, `MAC` is `mac/`, `VK_INC` the Vulkan
   headers (test that they exist before using them, and provide the SKIP branch —
   the CMakeLists shows both).
3. macOS specifics you may need: `-Wno-deprecated-declarations` for `ucontext`;
   weak symbols absent from a standalone link are left undefined with
   `target_link_options(... "LINKER:-U,_sym")` — `sc2_faibles_attentes()` and
   `tests-hote/v3/entrees_runtime.cmake` are the working examples.
4. Run it:

```sh
cmake -S tests-hote -B build-tests -G Ninja
cmake --build build-tests -j8
ctest --test-dir build-tests --output-on-failure -R "test_<nom>"   # your test name
```

5. Then the whole gate: `scripts/verifier.sh --rapide` (or the full gate when you
   hand back). A test that has known failures on one platform can be wrapped by
   `tests-hote/echecs-connus.sh` so that exactly those failures mean SKIP (77) and
   anything else stays a failure — `test_win32_compat` on macOS is the example,
   with its four named failures listed in `tests-hote/CMakeLists.txt`.

Tests live beside the code they test: `runtime/tests/` for the runtime,
`android/jni/tests/` for the pure-C Android glue, `mac/tests/` for the macOS host
(all three are built by the same gate), `android/src/test/` for the Java launcher
(run by `android/test-javac.sh`).

## 5. What `interfaces/` holds

Generated **interface facts** about the game build, kept versioned because they are
the contract the runtime implements — not code: `interfaces/sc2_imports.h` (the
755-slot IAT: VA, DLL, name or ordinal, feeding the stubs of `sc2_iat.c` /
`sc2_iat_p4.c`), `interfaces/sc2_image.h` (PE geometry: base, size, entry,
sections, TLS directory), `interfaces/LIFT-REPORT.json` (lift roots, bounds,
failures), and `interfaces/README.md` (what is tracked and what stays out of git).
Addresses in these files are uncopyrightable interface facts, valid for the build
they were measured on ([LEGAL.md](../../LEGAL.md) §2).

## Verify

```sh
scripts/verifier.sh --rapide                      # your new test appears and passes
ctest --test-dir build-tests --output-on-failure -R "test_<nom>"   # your test name
scripts/variables-sc2.sh verifier                 # still 0 if you added a variable
grep -n "SC2_" runtime/<yourfile>.c               # every literal has a doc row
```

**Next:** [03-rendering.md](03-rendering.md) for the D3D9 device and the Vulkan
backend, or jump to [05-hosts.md](05-hosts.md) for the macOS and Android hosts.
