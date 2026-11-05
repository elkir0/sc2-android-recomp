# 04 — Lift and oracle: pcrecomp, the CPU-struct model, and per-function certification

This guide covers the two tools that make a static recompilation trustworthy: the
lifter (pcrecomp patched with the generic fixes published here) and the per-function
differential oracle that certifies every lifted function against a
hardware-reference emulator. It is for anyone who must lift (or re-lift) a function,
apply or review a lifter patch, extend an existing closure, or classify and fix a
divergence. The pipeline overview is `docs/METHOD.md` §3 and §6; the full
divergence classification is `tools/difftest/DIVERGENCES.md`.

Input: the game executable from your own installation. Anything beyond pointing at
that input is out of scope for this repository — see
[AGENTS.md](../../AGENTS.md#scope).

1. [pcrecomp and the CPU-struct model](#1-pcrecomp-and-the-cpu-struct-model)
2. [The lifter patches](#2-the-lifter-patches-in-the-toolspcrecomp--directories)
3. [The tools in `tools/lift/`](#3-the-tools-in-toolslift)
4. [The lift-side transform scripts](#4-the-lift-side-transform-scripts)
5. [The oracle: what is compared](#5-the-oracle-what-is-compared)
6. [Running the oracle on one function](#6-running-the-oracle-on-one-function)
7. [Classifying a divergence](#7-classifying-a-divergence)
8. [Targeted difftests and self-checks](#8-targeted-difftests-and-self-checks)
9. [Known limits](#9-known-limits)
10. [Verify](#10-verify)

## 1. pcrecomp and the CPU-struct model

[pcrecomp](https://github.com/sp00nznet/pcrecomp) (MIT) is the external lifter this
project builds on; its pipeline is `pe_analyze` → `disasm32` (recursive descent) →
`lift32_cpu`. The generated C follows the **CPU-struct model** (`runtime/cpu.h`):
every lifted function is `void L_XXXXXXXX(CPU *c)`, all guest state lives in an
explicit `CPU` struct, and **every guest memory access goes through one pair of
`rd*/wr*` helpers**. Two consequences that shaped everything else
(`docs/METHOD.md` §3):

- The model is **reentrant and multi-thread friendly** — several CPU states can be
  live at once, which is what fibers and hybrid builds need.
- Funnelling all accesses through one pair of helpers is what makes hosts without
  fixed low mappings (macOS, Android) a ~30-line change: one `GPTR(va) = base +
  va` macro relocates the whole guest address space.

Expect roughly **one line of C per byte of machine code** for a large title (the
2010 client: 86,653 functions → 420 MB of C; the current 5.0.17 client: 609,384
functions, see `docs/CLIENT-5.0.md`). The same C builds on x86-64 Linux,
Apple-Silicon macOS and aarch64 Android. The lift closure facts of the 2010 client
are in `interfaces/LIFT-REPORT.json` and the IAT contract in
`interfaces/sc2_imports.h` — interface facts only (counters, slots, names), never the
lifted code itself, which each user generates locally.

The runtime side of the model is `runtime/cpu.h` (helpers) with
`runtime/cpu_selftest.c` pinning hand-computed values (rotates in particular do
NOT touch ZF/SF/PF on x86 — a helper that sets them looks fine until lifted code
branches on the wrong flag).

## 2. The lifter patches in the tools/pcrecomp-* directories

All patches are **generic by construction**: zero address, symbol or constant of
any particular binary. Read each README before applying — application differs
(`patch -p1`, `git apply`, `git am`).

| Directory | What it fixes | How to apply |
|---|---|---|
| `tools/pcrecomp-movhps/` | The mnemonic parser chose the SSE lane by testing the wrong character of the mnemonic, so `movhps`/`movhpd` (load into the **high** 64-bit lane) lifted exactly like `movlps`/`movlpd` (low lane). Symptom in any engine doing 4×4 matrix products with `movhps`-based SSE: degenerate view-projection matrices, every 3D triangle collapses — 3D black screen while 2D/UI still works. One-character fix. Detection: `grep -n 'm\[4\] == "h"' lift32_cpu.py` — any hit means the bug is present. | `git apply pcrecomp-movhps.patch` (`tools/pcrecomp-movhps/README.md`) |
| `tools/pcrecomp-movhlps/` | `movhlps`/`movlhps` (register to register) went to a TODO. Copies the source's high/low quadword into the destination's low/high one, keeping the other half; reads into a temporary first because `movhlps xmm0, xmm0` names one register twice. | `git am 0001-*.patch` (single commit in the directory) |
| `tools/pcrecomp-mmx/` | MMX instructions aborted the lift of a whole function (`not an xmm operand: mmN`). mm0–mm7 lift as their own `uint64_t` CPU fields (appended last in `cpu.h` so no existing offset moves); ~50 mnemonics as `cpu.h` helpers with lanes through a little-endian union (padd/psub + saturating, pmul*, pmax/pmin, pavg, psadbw, pcmpeq/pcmpgt, punpck, pack*, pand/por/pxor, shifts, pshufw, pmovmskb, pextrw/pinsrw, emms). | `git am 0001-base….patch 0002-lift32_cpu-MMX….patch` **in order** — `0001` is the shared local lifter base (movhps/movhpd high lane, FPU control word…), same base as `tools/pcrecomp-movhps/` |
| `tools/pcrecomp-atomics/` | Teaches the lifter to emit C atomics (`__atomic_*` builtins) where hardware semantics are atomic: `xchg` with a memory operand (8/16/32-bit, with or without `lock`), `lock add/sub/and/or/xor/inc/dec` (flags from the old value), `lock xadd` and `cmpxchg` 8/16-bit, `cmpxchg8b` (previously a TODO). Runtime side gains the single `GPTR(va)` macro. | `patch -p1 -d <your-pcrecomp-checkout> < pcrecomp-atomics.patch`, then re-lift and diff the generated C (only the listed sites should change) |
| `tools/pcrecomp-disasm32/` | Not a lifter patch: a patched copy of `disasm32.py` whose final `find_functions` loop crashed (MemoryError/SIGSEGV under `ulimit -v`) on >15 MB binaries — the alias-target loop forgot to release capstone operand lists (`release_operands=True` was honoured in the discovery rounds only), ~84 KB per alias staying live until the address space ran out. The fix releases them under the same condition; output is byte-for-byte identical (`cmp` on catalogue + meta). | Copy + `disasm32.patch` (+11 lines); `disasm32.py --selftest` on the copy; run from outside the pcrecomp tree it needs `PYTHONPATH` to find `pe_analyze` (`tools/pcrecomp-disasm32/PATCH.md`) |
| `tools/pcrecomp-closure/` | Not a fix: the generic packaging of `closure-plus.py` (see §3). | — |

Two more patches live with the difftest material because they were certified by it:
`tools/difftest/pcrecomp-7k.patch` (lifter + generic `cpu.h`: x87 control word,
bounded `fist(p)`, `fnstsw` TOP, OF of count-1 shifts) and
`tools/lift/pcrecomp-9w-rsqrt.patch` (`rsqrtss`/`rcpss`/`rsqrtps`/`rcpps` at full
precision, as QEMU computes them — the ~12-bit hardware approximation differs
between vendors).

**Every patch was certified by the oracle before use** (§5): per-function
equivalence against Unicorn on every touched function class, plus control runs over
unaffected families (`tools/difftest/DIVERGENCES.md` §6).

## 3. The tools in `tools/lift/`

These operate on a lift that already exists. Their headers are the interface;
arguments in angle brackets are required.

| Tool | Role |
|---|---|
| `lift-un.py <lift-boot.py> <tools/lift dir> <image> <va hex> [ceiling]` | Lifts **one** function to stdout (same PE reader, relocations and bounded-descent extent as the project's lift driver). The quick way to inspect what a patch changes on a given function. |
| `closure-plus.py <lift-boot.py> <tools/lift dir> <image> <list header> <out.c> <root-hex>…` | Extends an **existing** closure without a global re-lift: from the roots you give, lifts each function, scans the generated C for direct `dispatch(c, 0x…u)` targets, and recurses into every target in `.text` absent from the known list. A function the lifter refuses becomes a `RECOMP_TODO` stub instead of aborting the run. (Engine-agnostic repackaging with its own CLI — `--include`, `--known-re` — in `tools/pcrecomp-closure/`.) |
| `relift-garde.py <lift-boot.py> <tools/lift> <image> <lifted dir> <mnemonic regex> [--ecrire]` | **Guarded** re-lift of exactly the functions that carry a `RECOMP_TODO` of the given mnemonic(s): re-lifts on the original extent and accepts only if the line count is unchanged and every changed line was a TODO of those mnemonics — otherwise REFUS. Dry-run by default; `--ecrire` replaces the body in the file. This is how the rsqrt patch was rolled in without touching anything else. |
| `inventaire-mmx.py <image> <va hex>…` | Read-only disassembly inventory of a function (bounded-descent extent) with a per-mnemonic count of MMX operands — what to look at before deciding an MMX lift is needed. |
| `drapeaux-morts.py <file.c>… [--json out.json] [--top N]` / `--appliquer <src/lifted> <out>` | Static intra-procedural **backward liveness of the six x86 flags** over the generated C (one x86 instruction per line, `/* ADDRESS: asm */` comment). Analysis mode reports the share of dead flag writes — the decision basis for the rewriter; `--appliquer` rewrites: a fully dead helper call keeps only the arithmetic, a dead `cmp`/`test` becomes a comment (side effects of its arguments are kept). Conservative by construction: flags live at every exit, any call taking `c` reads and kills all six, unrecognized constructs read all six. |

## 4. The lift-side transform scripts

The transform scripts that rewrite the whole lift (compile-time levers, applied via
CMake options) live in `scripts/`: `appels-directs.py` (constant dispatch → direct
calls), `registres-locaux.py` (guest registers/flags in local variables with
flow-analyzed sync boundaries). Each has a `--verifier` mode that must restore the
input byte for byte, and a unit test (`scripts/test_appels_directs.py`,
`scripts/test_registres_locaux.py`).
They make the lifted C faster to compile and to run without changing its semantics.

## 5. The oracle: what is compared

`tools/difftest/difftest-sc2.py` is the harness; `tools/difftest/runner-difftest.c`
is the runner that links **every** lifted function (X-macro table) and executes
exactly one. Per function:

1. A seeded random initial state is generated: GPRs, arithmetic flags, guest memory
   windows. Both sides get the **same** bytes.
2. The **original bytes** of the function are replayed in **Unicorn** — the
   hardware reference. A sentinel return address is placed at `[esp0-4]` on both
   sides so the return is comparable without simulation; Unicorn stops when it
   jumps there, the lifted side returns from `fn()` after its `ret`.
3. The lifted function runs via the runner with the same state.
4. Every architectural field is diffed: registers, the arithmetic flags (CF PF AF
   ZF SF OF), and **every byte Unicorn wrote**. For each divergent field the
   harness reports the **last Unicorn writer** (address + disassembly) — the
   starting point of any bisection.
5. Functions whose Unicorn flow leaves `.text` (import calls, Win32 stubs) are
   **skipped by design** — only deterministic comparisons count. The full image
   (headers + all sections) is mapped on both sides and IAT slots hold out-of-text
   sentinels (`--disposition v2`, the default).

Conventions that are **never masked silently**: undefined flags (per the Intel SDM;
e.g. AF after `test`/`xor`) and known oracle-side quirks are named masks
(`--masques`), each masked divergence is kept in the report and counted. The x87
control word is the Windows one (0x027F) on the Unicorn side.

On the 2010 client this swept **76,650 functions**; the 3,985 initial failures of
the first campaign were all harness artifacts, and after fixes the run is
**0 true divergence** (`tools/difftest/DIVERGENCES.md` §0). Five real semantic
families were found and fixed on the way — the largest being the ignored `fldcw`
(MSVC truncation rounding instead of round-to-nearest on every float→int
conversion of the game), see §4 of that document.

## 6. Running the oracle on one function

Prerequisites (local, from your own install): the executable, the lift list header,
the built runner, and Python with `unicorn` + `capstone`. The full CLI is in the
header of `tools/difftest/difftest-sc2.py`:

```sh
python3 tools/difftest/difftest-sc2.py --exe <image> --liste <recomp_sc2_list.h> \
       --runner <runner-difftest> --adresses lot.txt [--seed 1] [--jobs 3] \
       [--json rapport.json]
```

- `lot.txt` (via `--adresses`) is one `ADDR [SEED]` per line — the unit of "one
  function (or one family) re-certified". `--debut`/`--n` select a slice of the
  whole list instead.
- The report JSON is what `classer-divergences.py` consumes (§7).
- Under a tight `ulimit -v`, launch through `tools/difftest/difftest-tcg64.py`
  (same CLI, smaller Unicorn TCG buffer per instance).
- Targeted wrappers that tailor the initial state to a function's calling
  convention (profiled objects, floats, nested fields, coverage counters):
  `difftest-cible-9q.py` (two fixed functions), `difftest-cible-9t.py`
  (profile-file driven), `difftest-cible-9w.py` (nested fields + coverage),
  `difftest-cible-c72900.py` (the YCbCr→BGRA MMX conversion, 11 coherent
  arguments).

## 7. Classifying a divergence

`tools/difftest/classer-divergences.py` reads one or more report JSONs and gives
each failing function **one family**, chosen from its most priority-divergent
field (runner stop > register > memory > flag). The family key names the field and
the **mnemonic of the last Unicorn writer** — that instruction is where the
semantics broke:

```sh
python3 tools/difftest/classer-divergences.py rapport.json [--top 30] [--exemples 3]
python3 tools/difftest/classer-divergences.py rapport.json --famille '<motif>' --adresses
```

`--adresses` emits an `ADDR SEED` list reusable directly by `difftest-sc2.py
--adresses` to re-certify a whole family. `tools/difftest/DIVERGENCES.md` is the
catalogue of every family met so far, with the fix for each; use it to match your
family against a known one before writing any code. Typical outcomes:

- **Convention (harness)** — simulated return frame, unresolved IAT, undefined
  flags per the SDM: masked, counted, never silent.
- **Oracle error** — QEMU/Unicorn itself diverges from the SDM (`fcomi` flags,
  signed `ret` imm16): mask `oracle-qemu`, documented.
- **Not comparable** — flow leaves `.text`, privileged instruction, `rdtsc`:
  skipped, named.
- **True semantic divergence** — the lifter or `cpu.h` is wrong; bisect to the
  instruction, fix (lifter patch or helper), then re-certify the whole family ×
  several seeds, then the campaign. Families 14–18 of `DIVERGENCES.md` are worked
  examples of exactly this loop.

## 8. Targeted difftests and self-checks

- `tools/difftest/test-x87-decalages.py <cpu.h>` — 147 instruction **sequences**
  (fldcw+fistp 16/32/64 × 4 rounding modes × out-of-range/NaN values, frndint,
  fnstcw/fnstsw TOP, OF of count-1 shifts), lifted by the real lifter, compiled
  with the given `cpu.h` (gcc x86-64 **and** aarch64 under qemu) and compared to
  Unicorn. This is the before/after proof of the 7k fix. Requires `PCRECOMP_FPU_CW=1`
  to test the fixed forms.
- `tools/difftest/difftest-mmx-aides.py <vectors.txt> [N] [seed]` + `cc …
  tools/difftest/test-mmx-aides.c` — generates random MMX operand pairs, executes
  each single instruction in Unicorn, then replays the vectors through the `mmx_*`
  helpers of `runtime/cpu.h` and compares.
- `croiser-stock-m4.py` — situates each failure of an old campaign in a new report
  (certified / skipped / still failing).

## 9. Known limits

From `tools/difftest/DIVERGENCES.md` §7: ~64,977 of 76,650 functions are skipped
by construction (flow leaves `.text`); x87 ST registers are not compared (only
memory/integer effects are — extended precision is memorized, not applied); `DF`
is not modelled; the default QNaN sign differs on aarch64 hosts (one function, NaN
sign only, documented and uncorrected).

## 10. Verify

Runnable without game data, from the repository root:

```sh
# 1) Unit tests of the dead-flag liveness analysis (hand-written lifted-style C)
python3 tools/lift/test_drapeaux_morts.py

# 2) cpu.h helper self-check (bit ops pinned to hand-computed values)
cc -O0 -o /tmp/cpu_selftest runtime/cpu_selftest.c -I runtime && /tmp/cpu_selftest

# 3) Transform scripts restore their input byte for byte
(cd scripts && python3 -m unittest test_appels_directs test_registres_locaux)

# 4) Argument contracts of the lift tools (each prints its usage)
python3 tools/lift/drapeaux-morts.py            # usage + mode list (exit 2 without args)
python3 tools/difftest/classer-divergences.py --help   # exit 0

# 5) bash syntax of the shell gate you may touch
bash -n scripts/verifier.sh
```

Expected: `Ran N tests … OK` for steps 1 and 3, `all checks passed` for step 2,
the usage texts for step 4, silence for step 5. Nothing here lifts, runs an
emulator or touches game data.

**Next:** [05 — Hosts](05-hosts.md) — the macOS window host and the Android APK
glue that carry this renderer.
